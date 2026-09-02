#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$1" = "clean" ]; then
    echo "============================================================"
    echo " Cleaning pseuDOS Build Artifacts"
    echo "============================================================"
    make -C "${SCRIPT_DIR}/UEFI-Files" clean
    echo "============================================================"
    echo " Clean Complete"
    echo "============================================================"
    exit 0
fi

echo "============================================================"
echo " Building pseuDOS UEFI System"
echo "============================================================"

make -C "${SCRIPT_DIR}/UEFI-Files" "$@"

echo "============================================================"
echo " Build Complete: UEFI-Files/build/pseuDOS.iso"
echo " Run with: ./run_normal.sh or ./run_debug.sh"
echo "============================================================"
