#!/usr/bin/env bash
set -e

# ==============================================================================
# pseuDOS UEFI Launcher - Safe Mode
# Runs pseuDOS.iso using conservative, highly-compatible QEMU parameters:
# - Single core CPU (-smp 1)
# - Base generic x86_64 CPU model (-cpu qemu64)
# - Conservative memory (-m 256M)
# - Standard VGA graphics (-vga std)
# - Pure software emulation (-machine pc,accel=tcg)
# - No network devices (-net none)
#
# Usage:
#   ./run_safemode.sh          (Live terminal console output)
#   ./run_safemode.sh --gui    (Graphical GTK/SDL window)
#   ./run_safemode.sh --curses (Full terminal curses screen)
# ==============================================================================

# Sanitize Snap environment variables that cause GTK/glibc library version mismatches
unset GTK_MODULES GTK_PATH GIO_MODULE_DIR SNAP SNAP_LIBRARY_PATH

# Ensure a proper terminal capability exists for curses
if [ -z "$TERM" ] || [ "$TERM" = "dumb" ]; then
    export TERM="xterm-256color"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO_PATH="${SCRIPT_DIR}/UEFI-Files/build/pseuDOS.iso"

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
        *)
            EXTRA_ARGS+=("$arg")
            ;;
    esac
done

echo "============================================================"
echo " Starting pseuDOS (Safe Mode - Maximum Compatibility)"
echo " ISO:  ${ISO_PATH}"
echo " BIOS: ${OVMF_BIOS}"
echo " Mode: 1 CPU, 256MB RAM, Standard VGA, TCG Software Emulation"
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
        -machine pc,accel=tcg \
        -cpu qemu64 \
        -smp 1 \
        -m 256M \
        -vga std \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -net none \
        -display gtk \
        -serial mon:stdio \
        "${EXTRA_ARGS[@]}"
elif [ "$MODE" = "curses" ]; then
    exec qemu-system-x86_64 \
        -machine pc,accel=tcg \
        -cpu qemu64 \
        -smp 1 \
        -m 256M \
        -vga std \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -net none \
        -display curses \
        "${EXTRA_ARGS[@]}"
else
    exec qemu-system-x86_64 \
        -machine pc,accel=tcg \
        -cpu qemu64 \
        -smp 1 \
        -m 256M \
        -vga std \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -net none \
        -nographic \
        "${EXTRA_ARGS[@]}"
fi
