#!/usr/bin/env bash
set -e

# ==============================================================================
# pseuDOS UEFI Launcher - Debug Logging Mode
# Runs pseuDOS with live display and saves complete diagnostic logs to
# /UEFI-Bootlogs/UEFIBoot-[bootdate]-[boottime(GMT)].log.
#
# Usage:
#   ./run_debug.sh          (Live terminal console + debug logging)
#   ./run_debug.sh --gui    (Graphical GTK/SDL window + debug logging)
#   ./run_debug.sh --curses (Full terminal curses screen + debug logging)
#
# Storage Drive Options (500 MB persistent virtual disks):
#   --sata                  (Attach internal AHCI SATA hard disk)
#   --nvme                  (Attach internal NVMe PCIe SSD)
#   --usb / --scsi          (Attach external USB 3.0/3.1 Mass Storage drive)
#   --usb2                  (Attach external USB 2.0 Mass Storage drive)
#   --all                   (Attach SATA, NVMe, and USB 3.0 drives simultaneously)
# ==============================================================================

# Sanitize Snap environment variables that cause GTK/glibc library version mismatches
unset GTK_MODULES GTK_PATH GIO_MODULE_DIR SNAP SNAP_LIBRARY_PATH

# Ensure a proper terminal capability exists for curses
if [ -z "$TERM" ] || [ "$TERM" = "dumb" ]; then
    export TERM="xterm-256color"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO_PATH="${SCRIPT_DIR}/UEFI-Files/build/pseuDOS.iso"
LOGDIR="${SCRIPT_DIR}/UEFI-Bootlogs"
DISKS_DIR="${SCRIPT_DIR}/UEFI-Files/build/disks"

mkdir -p "${LOGDIR}"
mkdir -p "${DISKS_DIR}"

# Generate timestamp in GMT
BOOT_DATE=$(date -u +"%Y-%m-%d")
BOOT_TIME=$(date -u +"%H-%M-%S(GMT)")
LOGFILE="${LOGDIR}/UEFIBoot-${BOOT_DATE}-${BOOT_TIME}.log"

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

if [ ! -f "$ISO_PATH" ]; then
    echo "[INFO] ISO not found at ${ISO_PATH}. Building..."
    make -C "${SCRIPT_DIR}/UEFI-Files"
fi

MODE="nographic"
ATTACH_SATA=0
ATTACH_NVME=0
ATTACH_USB3=0
ATTACH_USB2=0
EXTRA_ARGS=()

for arg in "$@"; do
    case "$arg" in
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
        *)
            EXTRA_ARGS+=("$arg")
            ;;
    esac
done

STORAGE_ARGS=()

# 1. AHCI SATA Drive (500 MB)
if [ "$ATTACH_SATA" -eq 1 ]; then
    SATA_IMG="${DISKS_DIR}/sata_disk.img"
    if [ ! -f "$SATA_IMG" ]; then
        truncate -s 500M "$SATA_IMG"
    fi
    STORAGE_ARGS+=(
        -drive "file=${SATA_IMG},if=none,id=sata0,format=raw"
        -device "ich9-ahci,id=ahci"
        -device "ide-hd,drive=sata0,bus=ahci.0"
    )
fi

# 2. NVMe PCIe SSD (500 MB)
if [ "$ATTACH_NVME" -eq 1 ]; then
    NVME_IMG="${DISKS_DIR}/nvme_disk.img"
    if [ ! -f "$NVME_IMG" ]; then
        truncate -s 500M "$NVME_IMG"
    fi
    STORAGE_ARGS+=(
        -drive "file=${NVME_IMG},if=none,id=nvm0,format=raw"
        -device "nvme,serial=970EVO500M,drive=nvm0"
    )
fi

# 3. USB 3.0/3.1 xHCI Mass Storage (500 MB)
if [ "$ATTACH_USB3" -eq 1 ]; then
    USB_IMG="${DISKS_DIR}/usb_disk.img"
    if [ ! -f "$USB_IMG" ]; then
        truncate -s 500M "$USB_IMG"
    fi
    STORAGE_ARGS+=(
        -device "qemu-xhci,id=xhci"
        -drive "file=${USB_IMG},if=none,id=usb0,format=raw"
        -device "usb-storage,bus=xhci.0,drive=usb0"
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

# Write initial log header
{
    echo "============================================================"
    echo " pseuDOS UEFI Boot Log"
    echo " Date (GMT):      ${BOOT_DATE}"
    echo " Time (GMT):      ${BOOT_TIME}"
    echo " Host System:     $(uname -s) $(uname -r) $(uname -m)"
    echo " Display Mode:    ${MODE}"
    echo " Display Env:     DISPLAY=${DISPLAY:-<unset>}, WAYLAND=${WAYLAND_DISPLAY:-<unset>}"
    echo " QEMU Version:    $(qemu-system-x86_64 --version | head -n 1)"
    echo " OVMF Firmware:   ${OVMF_BIOS}"
    echo " Boot Image:      ${ISO_PATH} ($(stat -c%s "${ISO_PATH}" 2>/dev/null || stat -f%z "${ISO_PATH}" 2>/dev/null || echo "N/A") bytes)"
    if [ "$ATTACH_SATA" -eq 1 ]; then
        echo " Attached Disk:   SATA (500MB) -> ${DISKS_DIR}/sata_disk.img"
    fi
    if [ "$ATTACH_NVME" -eq 1 ]; then
        echo " Attached Disk:   NVMe (500MB) -> ${DISKS_DIR}/nvme_disk.img"
    fi
    if [ "$ATTACH_USB3" -eq 1 ]; then
        echo " Attached Disk:   USB 3.0 (500MB) -> ${DISKS_DIR}/usb_disk.img"
    fi
    if [ "$ATTACH_USB2" -eq 1 ]; then
        echo " Attached Disk:   USB 2.0 (500MB) -> ${DISKS_DIR}/usb2_disk.img"
    fi
    echo " Log Destination: ${LOGFILE}"
    echo "============================================================"
    echo ""
    echo "--- LIVE TERMINAL / SERIAL CONSOLE OUTPUT ---"
} > "$LOGFILE"

echo "============================================================"
echo " Starting pseuDOS (Debug Logging Mode)"
echo " ISO:     ${ISO_PATH}"
echo " BIOS:    ${OVMF_BIOS}"
echo " Logfile: ${LOGFILE}"
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

DEBUGCON_LOG=$(mktemp)
STDERR_LOG=$(mktemp)

cleanup() {
    EXIT_CODE=$?
    {
        echo ""
        echo "--- FIRMWARE DEBUG OUTPUT (Port 0x402) ---"
        if [ -f "$DEBUGCON_LOG" ]; then
            cat "$DEBUGCON_LOG" 2>/dev/null || true
            rm -f "$DEBUGCON_LOG"
        fi
        echo ""
        echo "--- QEMU STDERR / HOST LOGS ---"
        if [ -f "$STDERR_LOG" ]; then
            cat "$STDERR_LOG" 2>/dev/null || true
            rm -f "$STDERR_LOG"
        fi
        echo ""
        echo "============================================================"
        echo " Session Ended: $(date -u +"%Y-%m-%d %H:%M:%S GMT") (Exit Code: ${EXIT_CODE})"
        echo "============================================================"
    } >> "$LOGFILE"

    echo ""
    echo "[INFO] Debug log saved to: ${LOGFILE}"
}
trap cleanup EXIT

if [ "$MODE" = "gui" ]; then
    qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -boot order=d,menu=off \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -display gtk \
        -serial mon:stdio \
        -debugcon file:"$DEBUGCON_LOG" \
        -global isa-debugcon.iobase=0x402 \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}" 2> "$STDERR_LOG" | tee -a "$LOGFILE"
elif [ "$MODE" = "curses" ]; then
    qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -boot order=d,menu=off \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -display curses \
        -debugcon file:"$DEBUGCON_LOG" \
        -global isa-debugcon.iobase=0x402 \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}" 2> "$STDERR_LOG"
else
    qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -boot order=d,menu=off \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -nographic \
        -debugcon file:"$DEBUGCON_LOG" \
        -global isa-debugcon.iobase=0x402 \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}" 2> "$STDERR_LOG" | tee -a "$LOGFILE"
fi
