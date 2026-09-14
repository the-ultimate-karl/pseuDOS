#!/usr/bin/env bash
# ==============================================================================
# pseuDOS Host Environment Checker & Installer (Bash / Shell)
# Verifies all required compiler and emulation tools are installed.
# If tools are missing, attempts automatic installation via system package manager.
# Supports Linux (apt, dnf, pacman, apk, zypper), macOS (brew), and Windows (Git Bash/MSYS2).
# ==============================================================================

set -e

COLOR_CYAN='\033[0;36m'
COLOR_GREEN='\033[0;32m'
COLOR_RED='\033[0;31m'
COLOR_YELLOW='\033[1;33m'
COLOR_RESET='\033[0m'

echo -e "${COLOR_CYAN}============================================================${COLOR_RESET}"
echo -e "${COLOR_CYAN} pseuDOS Host Environment Diagnostic & Setup (Shell)${COLOR_RESET}"
echo -e "${COLOR_CYAN}============================================================${COLOR_RESET}"

# Expand PATH with standard and known tool locations if present
for p in \
    "/c/mingw64/bin" \
    "/c/Program Files/mingw64/bin" \
    "/c/msys64/mingw64/bin" \
    "/c/msys64/usr/bin" \
    "/c/Program Files/qemu"; do
    if [ -d "$p" ]; then
        export PATH="$p:$PATH"
    fi
done

ALL_READY=true
MISSING_CC=false
MISSING_LD=false
MISSING_MAKE=false
MISSING_PYTHON=false
MISSING_QEMU=false
MISSING_OVMF=false

# 1. Check C Compiler
CC_PATH=""
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    CC_PATH="$(command -v x86_64-w64-mingw32-gcc)"
    CC_VER="$("$CC_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] C Compiler:      $CC_PATH ($CC_VER)${COLOR_RESET}"
elif command -v gcc >/dev/null 2>&1; then
    CC_PATH="$(command -v gcc)"
    CC_VER="$("$CC_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] C Compiler:      $CC_PATH ($CC_VER)${COLOR_RESET}"
else
    echo -e "${COLOR_RED}[MISSING] C Compiler: x86_64-w64-mingw32-gcc / gcc not found in PATH${COLOR_RESET}"
    ALL_READY=false
    MISSING_CC=true
fi

# 2. Check Linker
LD_PATH=""
if command -v x86_64-w64-mingw32-ld >/dev/null 2>&1; then
    LD_PATH="$(command -v x86_64-w64-mingw32-ld)"
    LD_VER="$("$LD_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] Linker:          $LD_PATH ($LD_VER)${COLOR_RESET}"
elif command -v ld >/dev/null 2>&1; then
    LD_PATH="$(command -v ld)"
    LD_VER="$("$LD_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] Linker:          $LD_PATH ($LD_VER)${COLOR_RESET}"
else
    echo -e "${COLOR_RED}[MISSING] Linker:     x86_64-w64-mingw32-ld / ld not found in PATH${COLOR_RESET}"
    ALL_READY=false
    MISSING_LD=true
fi

# 3. Check GNU Make
MAKE_PATH=""
if command -v make >/dev/null 2>&1; then
    MAKE_PATH="$(command -v make)"
    MAKE_VER="$("$MAKE_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] Build System:    $MAKE_PATH ($MAKE_VER)${COLOR_RESET}"
elif command -v gmake >/dev/null 2>&1; then
    MAKE_PATH="$(command -v gmake)"
    MAKE_VER="$("$MAKE_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] Build System:    $MAKE_PATH ($MAKE_VER)${COLOR_RESET}"
elif command -v mingw32-make >/dev/null 2>&1; then
    MAKE_PATH="$(command -v mingw32-make)"
    MAKE_VER="$("$MAKE_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] Build System:    $MAKE_PATH ($MAKE_VER)${COLOR_RESET}"
else
    echo -e "${COLOR_RED}[MISSING] Build System: GNU make not found in PATH${COLOR_RESET}"
    ALL_READY=false
    MISSING_MAKE=true
fi

# 4. Check Python 3
PYTHON_PATH=""
for py_cand in python3 python /c/Python310/python.exe /c/Python311/python.exe /c/Python312/python.exe; do
    if command -v "$py_cand" >/dev/null 2>&1; then
        if "$py_cand" -c "import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)" >/dev/null 2>&1; then
            PYTHON_PATH="$(command -v "$py_cand")"
            PY_VER="$("$PYTHON_PATH" --version 2>&1 | head -n 1)"
            echo -e "${COLOR_GREEN}[OK] Python:          $PYTHON_PATH ($PY_VER)${COLOR_RESET}"
            break
        fi
    fi
done

if [ -z "$PYTHON_PATH" ]; then
    echo -e "${COLOR_RED}[MISSING] Python:     Python 3 not found in PATH${COLOR_RESET}"
    ALL_READY=false
    MISSING_PYTHON=true
fi

# 5. Check QEMU x86_64
QEMU_PATH=""
if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    QEMU_PATH="$(command -v qemu-system-x86_64)"
    QEMU_VER="$("$QEMU_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] Emulator:        $QEMU_PATH ($QEMU_VER)${COLOR_RESET}"
elif [ -f "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then
    QEMU_PATH="/c/Program Files/qemu/qemu-system-x86_64.exe"
    QEMU_VER="$("$QEMU_PATH" --version | head -n 1)"
    echo -e "${COLOR_GREEN}[OK] Emulator:        $QEMU_PATH ($QEMU_VER)${COLOR_RESET}"
else
    echo -e "${COLOR_RED}[MISSING] Emulator:   qemu-system-x86_64 not found in PATH${COLOR_RESET}"
    ALL_READY=false
    MISSING_QEMU=true
fi

# 6. Check EDK2 / OVMF UEFI Firmware
OVMF_PATH=""
for cand in \
    "/c/Program Files/qemu/share/edk2-x86_64-code.fd" \
    "/c/Program Files/qemu/share/OVMF.fd" \
    "/c/Program Files/qemu/OVMF.fd" \
    "/c/msys64/mingw64/share/qemu/edk2-x86_64-code.fd" \
    "/usr/share/ovmf/OVMF.fd" \
    "/usr/share/OVMF/OVMF_CODE_4M.fd" \
    "/usr/share/OVMF/OVMF_CODE.fd" \
    "/usr/share/qemu/OVMF.fd" \
    "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd" \
    "/usr/share/edk2/ovmf/OVMF_CODE.fd" \
    "/opt/homebrew/share/qemu/edk2-x86_64-code.fd" \
    "/usr/local/share/qemu/edk2-x86_64-code.fd"; do
    if [ -f "$cand" ]; then
        OVMF_PATH="$cand"
        break
    fi
done

if [ -n "$OVMF_PATH" ]; then
    echo -e "${COLOR_GREEN}[OK] UEFI Firmware:   $OVMF_PATH${COLOR_RESET}"
else
    echo -e "${COLOR_RED}[MISSING] UEFI Firmware: EDK2/OVMF (.fd) not found${COLOR_RESET}"
    ALL_READY=false
    MISSING_OVMF=true
fi

# 7. Automatic Installation if dependencies are missing
if [ "$ALL_READY" = false ]; then
    echo ""
    echo -e "${COLOR_YELLOW}============================================================${COLOR_RESET}"
    echo -e "${COLOR_YELLOW} Missing dependencies detected. Attempting installation...${COLOR_RESET}"
    echo -e "${COLOR_YELLOW}============================================================${COLOR_RESET}"

    SUDO=""
    if [ "$(id -u 2>/dev/null || echo 1000)" -ne 0 ]; then
        if command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        fi
    fi

    if command -v apt-get >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via apt-get (Debian/Ubuntu)...${COLOR_RESET}"
        $SUDO apt-get update
        $SUDO apt-get install -y gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 make python3 qemu-system-x86 ovmf
    elif command -v dnf >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via dnf (Fedora/RHEL)...${COLOR_RESET}"
        $SUDO dnf install -y mingw64-gcc mingw64-binutils make python3 qemu-system-x86 edk2-ovmf
    elif command -v pacman >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via pacman...${COLOR_RESET}"
        $SUDO pacman -Sy --noconfirm mingw-w64-gcc make python qemu-system-x86 edk2-ovmf 2>/dev/null || \
        $SUDO pacman -Sy --noconfirm mingw-w64-x86_64-toolchain make python qemu
    elif command -v apk >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via apk (Alpine Linux)...${COLOR_RESET}"
        $SUDO apk add --no-cache mingw-w64-gcc make python3 qemu-system-x86_64 ovmf
    elif command -v zypper >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via zypper (openSUSE)...${COLOR_RESET}"
        $SUDO zypper install -y cross-x86_64-gcc-bootstrap make python3 qemu-x86 qemu-ovmf-x86_64
    elif command -v brew >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via Homebrew (macOS)...${COLOR_RESET}"
        brew install mingw-w64 make python3 qemu
    elif command -v winget.exe >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via winget (Windows)...${COLOR_RESET}"
        if [ "$MISSING_CC" = true ]; then winget.exe install -e --id BrechtSanders.WinLibs.POSIX.UCRT --accept-source-agreements --accept-package-agreements; fi
        if [ "$MISSING_MAKE" = true ]; then winget.exe install -e --id GnuWin32.Make --accept-source-agreements --accept-package-agreements; fi
        if [ "$MISSING_PYTHON" = true ]; then winget.exe install -e --id Python.Python.3.11 --accept-source-agreements --accept-package-agreements; fi
        if [ "$MISSING_QEMU" = true ]; then winget.exe install -e --id SoftwareFreedomConservancy.QEMU --accept-source-agreements --accept-package-agreements; fi
    elif command -v choco.exe >/dev/null 2>&1; then
        echo -e "${COLOR_CYAN}Installing dependencies via chocolatey (Windows)...${COLOR_RESET}"
        if [ "$MISSING_CC" = true ]; then choco.exe install -y mingw; fi
        if [ "$MISSING_MAKE" = true ]; then choco.exe install -y make; fi
        if [ "$MISSING_PYTHON" = true ]; then choco.exe install -y python; fi
        if [ "$MISSING_QEMU" = true ]; then choco.exe install -y qemu; fi
    else
        echo -e "${COLOR_RED}[ERROR] Unsupported package manager. Please install dependencies manually:${COLOR_RESET}"
        echo "  - C Compiler: x86_64-w64-mingw32-gcc"
        echo "  - Linker:     x86_64-w64-mingw32-ld"
        echo "  - Build tool: make"
        echo "  - Python:     python3"
        echo "  - Emulator:   qemu-system-x86_64 and ovmf"
        exit 1
    fi
fi

echo ""
echo -e "${COLOR_CYAN}============================================================${COLOR_RESET}"
echo -e "${COLOR_CYAN} Diagnostic Summary${COLOR_RESET}"
echo -e "${COLOR_CYAN}============================================================${COLOR_RESET}"

FINAL_READY=true
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then FINAL_READY=false; fi
if ! command -v make >/dev/null 2>&1 && ! command -v gmake >/dev/null 2>&1 && ! command -v mingw32-make >/dev/null 2>&1; then FINAL_READY=false; fi
if [ -z "$PYTHON_PATH" ]; then FINAL_READY=false; fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1 && [ ! -f "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then FINAL_READY=false; fi

if [ "$FINAL_READY" = true ]; then
    echo -e "${COLOR_GREEN}[SUCCESS] All required tools are installed and ready!${COLOR_RESET}"
    echo -e "${COLOR_GREEN}  - Compile: ./build.sh${COLOR_RESET}"
    echo -e "${COLOR_GREEN}  - Run:     ./run_normal.sh${COLOR_RESET}"
    exit 0
else
    echo -e "${COLOR_RED}[ERROR] One or more required tools are still missing.${COLOR_RESET}"
    exit 1
fi
