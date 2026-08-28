#!/usr/bin/env bash
set -e

# ==============================================================================
# pseuDOS UEFI Launcher - Safe Mode with Debug Logging
# Runs pseuDOS.iso in Safe Mode with live display and saves complete
# diagnostic logs to /UEFI-Bootlogs/UEFIBoot-[bootdate]-[boottime(GMT)].log.
#
# Usage:
#   ./run_debug.sh          (Live terminal console + debug logging)
#   ./run_debug.sh --gui    (Graphical GTK/SDL window + debug logging)
#   ./run_debug.sh --curses (Full terminal curses screen + debug logging)
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

mkdir -p "${LOGDIR}"

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
    echo " Execution Mode:  Safe Mode (TCG, 1 CPU, 256MB RAM, std VGA)"
    echo " Log Destination: ${LOGFILE}"
    echo "============================================================"
    echo ""
} > "$LOGFILE"

echo "============================================================"
echo " Starting pseuDOS (Safe Mode with Debug Logging)"
echo " ISO:     ${ISO_PATH}"
echo " BIOS:    ${OVMF_BIOS}"
echo " Logfile: ${LOGFILE}"
if [ "$MODE" = "nographic" ]; then
    echo " Display: Live Terminal Console (Press Ctrl+A then X to exit)"
elif [ "$MODE" = "curses" ]; then
    echo " Display: Curses Terminal Screen (Press Esc+2 for monitor, Esc+1 for screen)"
else
    echo " Display: Graphical GUI Window (GTK/SDL)"
fi
echo "============================================================"

DEBUGCON_LOG=$(mktemp)
SERIAL_LOG=$(mktemp)
STDERR_LOG=$(mktemp)

cleanup() {
    EXIT_CODE=$?
    {
        echo ""
        echo "--- SERIAL PORT OUTPUT (COM1 / 0x3F8) ---"
        if [ -f "$SERIAL_LOG" ]; then
            cat "$SERIAL_LOG" 2>/dev/null || true
            rm -f "$SERIAL_LOG"
        fi
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
        -machine pc,accel=tcg \
        -cpu qemu64 \
        -smp 1 \
        -m 256M \
        -vga std \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -net none \
        -display gtk \
        -chardev file,id=char0,path="$SERIAL_LOG" \
        -serial chardev:char0 \
        -debugcon file:"$DEBUGCON_LOG" \
        -global isa-debugcon.iobase=0x402 \
        "${EXTRA_ARGS[@]}" 2> "$STDERR_LOG"
elif [ "$MODE" = "curses" ]; then
    qemu-system-x86_64 \
        -machine pc,accel=tcg \
        -cpu qemu64 \
        -smp 1 \
        -m 256M \
        -vga std \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -net none \
        -display curses \
        -chardev file,id=char0,path="$SERIAL_LOG" \
        -serial chardev:char0 \
        -debugcon file:"$DEBUGCON_LOG" \
        -global isa-debugcon.iobase=0x402 \
        "${EXTRA_ARGS[@]}" 2> "$STDERR_LOG"
else
    qemu-system-x86_64 \
        -machine pc,accel=tcg \
        -cpu qemu64 \
        -smp 1 \
        -m 256M \
        -vga std \
        -bios "$OVMF_BIOS" \
        -cdrom "$ISO_PATH" \
        -net none \
        -nographic \
        -chardev file,id=char0,path="$SERIAL_LOG" \
        -serial chardev:char0 \
        -debugcon file:"$DEBUGCON_LOG" \
        -global isa-debugcon.iobase=0x402 \
        "${EXTRA_ARGS[@]}" 2> "$STDERR_LOG"
fi
