#!/usr/bin/env bash
set -e

# ==============================================================================
# pseuDOS UEFI Launcher - Normal Mode
# Displays the live boot process directly in your terminal by default,
# or in a GUI window if requested with --gui.
#
# Usage:
#   ./run_normal.sh          (Live terminal console output)
#   ./run_normal.sh --gui    (Graphical GTK/SDL window)
#   ./run_normal.sh --curses (Full terminal curses screen)
#
# Storage Drive Options (500 MB persistent virtual disks):
#   --sata                   (Attach internal AHCI SATA hard disk)
#   --nvme                   (Attach internal NVMe PCIe SSD)
#   --usb / --scsi           (Attach external USB 3.0/3.1 Mass Storage drive)
#   --usb2                   (Attach external USB 2.0 Mass Storage drive)
#   --all                    (Attach SATA, NVMe, and USB 3.0 drives simultaneously)
#
# Boot Media Options:
#   --no-iso / --disk-boot      (Boot directly from installed disk without ISO attached)
#   --boot-from <sata|nvme|usb> (Select boot drive: sata, nvme, or usb; implies --no-iso)
# ==============================================================================

# Sanitize Snap environment variables that cause GTK/glibc library version mismatches
unset GTK_MODULES GTK_PATH GIO_MODULE_DIR SNAP SNAP_LIBRARY_PATH

# Ensure a proper terminal capability exists for curses
if [ -z "$TERM" ] || [ "$TERM" = "dumb" ]; then
    export TERM="xterm-256color"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO_PATH="${SCRIPT_DIR}/UEFI-Files/build/pseuDOS.iso"
DISKS_DIR="${SCRIPT_DIR}/UEFI-Files/build/disks"

# Locate OVMF firmware
OVMF_BIOS=""
for candidate in \
    "/usr/share/ovmf/OVMF.fd" \
    "/usr/share/OVMF/OVMF_CODE_4M.fd" \
    "/usr/share/OVMF/OVMF_CODE.fd" \
    "/usr/share/qemu/OVMF.fd"; do
    if [ -f "$candidate" ]; then
        OVMF_BIOS="$candidate"
        break
    fi
done

if [ -z "$OVMF_BIOS" ]; then
    echo "[ERROR] OVMF UEFI firmware not found on system." >&2
    exit 1
fi

mkdir -p "$DISKS_DIR"

MODE="nographic"
ATTACH_SATA=0
ATTACH_NVME=0
ATTACH_USB3=0
ATTACH_USB2=0
NO_ISO=0
BOOT_FROM=""
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --gui|-g)
            MODE="gui"
            ;;
        --curses|-c)
            MODE="curses"
            ;;
        --terminal|-t|--nographic)
            MODE="nographic"
            ;;
        --sata)
            ATTACH_SATA=1
            ;;
        --nvme)
            ATTACH_NVME=1
            ;;
        --usb|--scsi)
            ATTACH_USB3=1
            ;;
        --usb2)
            ATTACH_USB2=1
            ;;
        --all)
            ATTACH_SATA=1
            ATTACH_NVME=1
            ATTACH_USB3=1
            ;;
        --no-iso|--no-cdrom|--disk-boot)
            NO_ISO=1
            ;;
        --boot-from)
            NO_ISO=1
            shift
            if [ $# -gt 0 ]; then
                BOOT_FROM="$(echo "$1" | tr '[:upper:]' '[:lower:]')"
            else
                echo "[ERROR] --boot-from requires an argument: sata, nvme, or usb" >&2
                exit 1
            fi
            ;;
        --boot-from=*)
            NO_ISO=1
            BOOT_FROM="$(echo "${1#*=}" | tr '[:upper:]' '[:lower:]')"
            ;;
        *)
            EXTRA_ARGS+=("$1")
            ;;
    esac
    shift
done

# Validate --boot-from if provided
if [ -n "$BOOT_FROM" ]; then
    case "$BOOT_FROM" in
        sata)
            ATTACH_SATA=1
            ;;
        nvme)
            ATTACH_NVME=1
            ;;
        usb)
            ATTACH_USB3=1
            ;;
        *)
            echo "[ERROR] Invalid option for --boot-from: '$BOOT_FROM'. Supported options: sata, nvme, usb" >&2
            exit 1
            ;;
    esac
fi

# If booting from disk without explicit target, resolve default boot drive
if [ "$NO_ISO" -eq 1 ] && [ -z "$BOOT_FROM" ]; then
    if [ "$ATTACH_NVME" -eq 1 ] && [ "$ATTACH_SATA" -eq 0 ]; then
        BOOT_FROM="nvme"
    elif [ "$ATTACH_USB3" -eq 1 ] && [ "$ATTACH_SATA" -eq 0 ]; then
        BOOT_FROM="usb"
    else
        BOOT_FROM="sata"
        ATTACH_SATA=1
    fi
fi

# Build ISO only when ISO media is required
if [ "$NO_ISO" -ne 1 ] && [ ! -f "$ISO_PATH" ]; then
    echo "[INFO] ISO not found at ${ISO_PATH}. Building..."
    make -C "${SCRIPT_DIR}/UEFI-Files"
fi

# Function to inspect whether a disk image has a valid partition table
is_disk_partitioned() {
    local disk_file="$1"
    if [ ! -f "$disk_file" ] || [ ! -s "$disk_file" ]; then
        return 1
    fi
    python3 -c '
import sys
try:
    with open(sys.argv[1], "rb") as f:
        mbr = f.read(512)
        if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
            sys.exit(1)
        gpt = f.read(512)
        if len(gpt) >= 8 and gpt[:8] == b"EFI PART":
            sys.exit(0)
        for offset in (446, 462, 478, 494):
            if mbr[offset + 4] != 0:
                sys.exit(0)
        sys.exit(1)
except Exception:
    sys.exit(1)
' "$disk_file" 2>/dev/null
}

# Warn if target boot disk is blank/unpartitioned when booting standalone
if [ "$NO_ISO" -eq 1 ]; then
    TARGET_BOOT_IMG=""
    case "$BOOT_FROM" in
        sata) TARGET_BOOT_IMG="${DISKS_DIR}/sata_disk.img" ;;
        nvme) TARGET_BOOT_IMG="${DISKS_DIR}/nvme_disk.img" ;;
        usb)  TARGET_BOOT_IMG="${DISKS_DIR}/usb_disk.img" ;;
    esac

    if ! is_disk_partitioned "$TARGET_BOOT_IMG"; then
        echo "============================================================"
        echo "[WARNING] Target boot disk ($TARGET_BOOT_IMG) is blank or unpartitioned!"
        echo "[WARNING] pseuDOS has not been installed onto this disk yet."
        echo "[WARNING] Tip: Boot with the ISO (without --no-iso) and run 'flash' to install."
        echo "============================================================"
        read -r -p "Attempt to boot anyway? (y/N) " confirm
        if [[ ! "$confirm" =~ ^[yY]$ ]]; then
            echo "[INFO] Aborting launch."
            exit 1
        fi
    fi
fi

STORAGE_ARGS=()

# 1. AHCI SATA Drive (500 MB)
if [ "$ATTACH_SATA" -eq 1 ]; then
    SATA_IMG="${DISKS_DIR}/sata_disk.img"
    if [ ! -f "$SATA_IMG" ]; then
        truncate -s 500M "$SATA_IMG"
    fi
    BOOT_PROP=""
    if [ "$NO_ISO" -eq 1 ] && [ "$BOOT_FROM" = "sata" ]; then
        BOOT_PROP=",bootindex=1"
    fi
    STORAGE_ARGS+=(
        -drive "file=${SATA_IMG},if=none,id=sata0,format=raw"
        -device "ich9-ahci,id=ahci"
        -device "ide-hd,drive=sata0,bus=ahci.0${BOOT_PROP}"
    )
fi

# 2. NVMe PCIe SSD (500 MB)
if [ "$ATTACH_NVME" -eq 1 ]; then
    NVME_IMG="${DISKS_DIR}/nvme_disk.img"
    if [ ! -f "$NVME_IMG" ]; then
        truncate -s 500M "$NVME_IMG"
    fi
    BOOT_PROP=""
    if [ "$NO_ISO" -eq 1 ] && [ "$BOOT_FROM" = "nvme" ]; then
        BOOT_PROP=",bootindex=1"
    fi
    STORAGE_ARGS+=(
        -drive "file=${NVME_IMG},if=none,id=nvm0,format=raw"
        -device "nvme,serial=970EVO500M,drive=nvm0${BOOT_PROP}"
    )
fi

# 3. USB 3.0/3.1 xHCI Mass Storage (500 MB)
if [ "$ATTACH_USB3" -eq 1 ]; then
    USB_IMG="${DISKS_DIR}/usb_disk.img"
    if [ ! -f "$USB_IMG" ]; then
        truncate -s 500M "$USB_IMG"
    fi
    BOOT_PROP=""
    if [ "$NO_ISO" -eq 1 ] && [ "$BOOT_FROM" = "usb" ]; then
        BOOT_PROP=",bootindex=1"
    fi
    STORAGE_ARGS+=(
        -device "qemu-xhci,id=xhci"
        -drive "file=${USB_IMG},if=none,id=usb0,format=raw"
        -device "usb-storage,bus=xhci.0,drive=usb0${BOOT_PROP}"
    )
fi

# 4. USB 2.0 EHCI Mass Storage (500 MB)
if [ "$ATTACH_USB2" -eq 1 ]; then
    USB2_IMG="${DISKS_DIR}/usb2_disk.img"
    if [ ! -f "$USB2_IMG" ]; then
        truncate -s 500M "$USB2_IMG"
    fi
    STORAGE_ARGS+=(
        -device "usb-ehci,id=ehci"
        -drive "file=${USB2_IMG},if=none,id=usb2_0,format=raw"
        -device "usb-storage,bus=ehci.0,drive=usb2_0"
    )
fi

QEMU_BOOT_ARGS=()
if [ "$NO_ISO" -eq 1 ]; then
    QEMU_BOOT_ARGS=(-boot order=c,menu=off)
else
    QEMU_BOOT_ARGS=(-cdrom "$ISO_PATH" -boot order=d,menu=off)
fi

echo "============================================================"
echo " Starting pseuDOS (Normal Mode)"
if [ "$NO_ISO" -eq 1 ]; then
    echo " Media: Installed Disk (${BOOT_FROM^^} Drive - Standalone Boot)"
else
    echo " ISO:  ${ISO_PATH}"
fi
echo " BIOS: ${OVMF_BIOS}"
if [ "$ATTACH_SATA" -eq 1 ]; then
    echo " Storage: [Attached] 500 MB AHCI SATA Disk"
fi
if [ "$ATTACH_NVME" -eq 1 ]; then
    echo " Storage: [Attached] 500 MB NVMe PCIe SSD"
fi
if [ "$ATTACH_USB3" -eq 1 ]; then
    echo " Storage: [Attached] 500 MB USB 3.0/3.1 (xHCI) Drive"
fi
if [ "$ATTACH_USB2" -eq 1 ]; then
    echo " Storage: [Attached] 500 MB USB 2.0 (EHCI) Drive"
fi
if [ "$MODE" = "nographic" ]; then
    echo " Display: Live Terminal Console (Press Ctrl+A then X to exit)"
elif [ "$MODE" = "curses" ]; then
    echo " Display: Curses Terminal Screen (Press Esc+2 for monitor, Esc+1 for screen)"
else
    echo " Display: Graphical GUI Window (GTK/SDL)"
fi
echo "============================================================"

if [ "$MODE" = "gui" ]; then
    exec qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        "${QEMU_BOOT_ARGS[@]}" \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -display gtk \
        -serial mon:stdio \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}"
elif [ "$MODE" = "curses" ]; then
    exec qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        "${QEMU_BOOT_ARGS[@]}" \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -display curses \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}"
else
    exec qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        "${QEMU_BOOT_ARGS[@]}" \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -nographic \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}"
fi
