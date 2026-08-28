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
echo " Starting pseuDOS (Normal Mode)"
echo " ISO:  ${ISO_PATH}"
echo " BIOS: ${OVMF_BIOS}"
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
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -display gtk \
        -serial mon:stdio \
        "${EXTRA_ARGS[@]}"
elif [ "$MODE" = "curses" ]; then
    exec qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -display curses \
        "${EXTRA_ARGS[@]}"
else
    exec qemu-system-x86_64 \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -m 512M \
        -smp 2 \
        -cpu max \
        -net none \
        -nographic \
        "${EXTRA_ARGS[@]}"
fi
