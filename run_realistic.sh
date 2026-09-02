#!/usr/bin/env bash
set -e

# ==============================================================================
# pseuDOS UEFI Launcher - Strict Bare-Metal Hardware Simulation Mode
#
# Simulates unforgiving bare-metal hardware conditions:
# - Strict modern Q35 PCIe machine type with System Management Mode (SMM)
# - Intel IOMMU (VT-d) with DMA remapping & Device-IOTLB enabled
# - Fragmented non-contiguous physical RAM with NUMA nodes & ACPI memory holes
# - Downstream PCIe Root Ports topology for high-speed storage buses
# - Hardware RTC timer clock tracking with host drift
# - Full parameter parity with run_normal.sh
#
# Usage:
#   ./run_realistic.sh          (Live terminal console output)
#   ./run_realistic.sh --gui    (Graphical GTK/SDL window)
#   ./run_realistic.sh --curses (Full terminal curses screen)
#
# Storage Drive Options (500 MB persistent virtual disks):
#   --sata                      (Attach internal AHCI SATA hard disk on PCIe)
#   --nvme                      (Attach internal NVMe PCIe SSD on PCIe Root Port)
#   --usb / --scsi              (Attach external USB 3.0/3.1 Mass Storage drive)
#   --usb2                      (Attach external USB 2.0 Mass Storage drive)
#   --all                       (Attach SATA, NVMe, and USB 3.0 drives simultaneously)
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

if [ ! -f "$ISO_PATH" ]; then
    echo "[INFO] ISO not found at ${ISO_PATH}. Building..."
    make -C "${SCRIPT_DIR}/UEFI-Files"
fi

mkdir -p "$DISKS_DIR"

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
ROOT_PORTS=()

# Setup PCIe Root Ports
ROOT_PORTS+=(
    -device "pcie-root-port,id=rp1,slot=1,chassis=1"
    -device "pcie-root-port,id=rp2,slot=2,chassis=2"
)

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

# 2. NVMe PCIe SSD on dedicated PCIe Root Port (500 MB)
if [ "$ATTACH_NVME" -eq 1 ]; then
    NVME_IMG="${DISKS_DIR}/nvme_disk.img"
    if [ ! -f "$NVME_IMG" ]; then
        truncate -s 500M "$NVME_IMG"
    fi
    STORAGE_ARGS+=(
        -drive "file=${NVME_IMG},if=none,id=nvm0,format=raw"
        -device "nvme,bus=rp1,serial=970EVO500M,drive=nvm0"
    )
fi

# 3. USB 3.0/3.1 xHCI Mass Storage on PCIe Root Port (500 MB)
if [ "$ATTACH_USB3" -eq 1 ]; then
    USB_IMG="${DISKS_DIR}/usb_disk.img"
    if [ ! -f "$USB_IMG" ]; then
        truncate -s 500M "$USB_IMG"
    fi
    STORAGE_ARGS+=(
        -device "qemu-xhci,id=xhci,bus=rp2"
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

# Multi-Node NUMA Memory Topology (forces non-contiguous physical RAM & ACPI memory holes)
NUMA_ARGS=(
    -m 1024M
    -smp cpus=2,sockets=2,cores=1,threads=1
    -object memory-backend-ram,id=mem0,size=512M
    -object memory-backend-ram,id=mem1,size=512M
    -numa node,nodeid=0,cpus=0,memdev=mem0
    -numa node,nodeid=1,cpus=1,memdev=mem1
)

# Hardware Simulation Flags (IOMMU, SMM, Host RTC, Strict Hardware Reset)
HARDWARE_SIM_ARGS=(
    -machine q35,smm=on
    -global ICH9-LPC.disable_s3=1
    -global ICH9-LPC.disable_s4=1
    -device intel-iommu,intremap=on,caching-mode=on,device-iotlb=on
    -rtc base=localtime,clock=host,driftfix=slew
    -boot menu=off,splash-time=1500
    -cpu max
    -net none
)

echo "============================================================"
echo " Starting pseuDOS (Strict Bare-Metal Hardware Simulation)"
echo " ISO:     ${ISO_PATH}"
echo " BIOS:    ${OVMF_BIOS}"
echo " Chipset: Intel Q35 with System Management Mode (SMM)"
echo " IOMMU:   Intel VT-d DMA Remapping & Device-IOTLB Active"
echo " Memory:  Multi-Node NUMA (1024MB split with physical RAM holes)"
if [ "$ATTACH_SATA" -eq 1 ]; then
    echo " Storage: [Attached] 500 MB AHCI SATA Disk"
fi
if [ "$ATTACH_NVME" -eq 1 ]; then
    echo " Storage: [Attached] 500 MB NVMe SSD on PCIe Root Port 1"
fi
if [ "$ATTACH_USB3" -eq 1 ]; then
    echo " Storage: [Attached] 500 MB USB 3.0/3.1 on PCIe Root Port 2"
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
        -cdrom "$ISO_PATH" \
        -display gtk \
        -serial mon:stdio \
        "${HARDWARE_SIM_ARGS[@]}" \
        "${NUMA_ARGS[@]}" \
        "${ROOT_PORTS[@]}" \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}"
elif [ "$MODE" = "curses" ]; then
    exec qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -display curses \
        "${HARDWARE_SIM_ARGS[@]}" \
        "${NUMA_ARGS[@]}" \
        "${ROOT_PORTS[@]}" \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}"
else
    exec qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -nographic \
        "${HARDWARE_SIM_ARGS[@]}" \
        "${NUMA_ARGS[@]}" \
        "${ROOT_PORTS[@]}" \
        "${STORAGE_ARGS[@]}" \
        "${EXTRA_ARGS[@]}"
fi
