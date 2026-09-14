# ==============================================================================
# pseuDOS Host Environment Checker & Installer (PowerShell)
# Verifies all required compiler and emulation tools are installed.
# If tools are missing, attempts automatic installation via winget or choco.
# ==============================================================================

[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$Force
)

$ErrorActionPreference = "Continue"

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " pseuDOS Host Environment Diagnostic & Setup" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# 1. Expand PATH with known tool directories if present
$KnownPaths = @(
    "C:\mingw64\bin",
    "C:\Program Files\mingw64\bin",
    "C:\msys64\mingw64\bin",
    "C:\msys64\usr\bin",
    "C:\Program Files\qemu"
)

foreach ($p in $KnownPaths) {
    if (Test-Path $p) {
        if ($env:PATH -notlike "*$p*") {
            $env:PATH = "$p;" + $env:PATH
        }
    }
}

$AllReady = $true
$MissingCompiler = $false
$MissingMake = $false
$MissingPython = $false
$MissingQemu = $false
$MissingOvmf = $false

# 2. Check GCC (MinGW x86_64 PE32+ capable)
$GccCmd = Get-Command gcc -ErrorAction SilentlyContinue
if ($GccCmd) {
    $GccVer = (& gcc --version | Select-Object -First 1)
    Write-Host "[OK] C Compiler:      $($GccCmd.Source) ($GccVer)" -ForegroundColor Green
} else {
    Write-Host "[MISSING] C Compiler: GCC / MinGW-w64 not found in PATH" -ForegroundColor Red
    $AllReady = $false
    $MissingCompiler = $true
}

# 3. Check GNU Make
$MakeCmd = Get-Command make, mingw32-make -ErrorAction SilentlyContinue | Select-Object -First 1
if ($MakeCmd) {
    $MakeVer = (& $MakeCmd.Source --version | Select-Object -First 1)
    Write-Host "[OK] Build System:    $($MakeCmd.Source) ($MakeVer)" -ForegroundColor Green
} else {
    Write-Host "[MISSING] Build System: GNU Make not found in PATH" -ForegroundColor Red
    $AllReady = $false
    $MissingMake = $true
}

# 4. Check Python 3
$PythonCmd = Get-Command python, python3 -ErrorAction SilentlyContinue | Select-Object -First 1
if ($PythonCmd) {
    $PyVer = (& $PythonCmd.Source --version 2>&1 | Select-Object -First 1)
    Write-Host "[OK] Python:          $($PythonCmd.Source) ($PyVer)" -ForegroundColor Green
} else {
    Write-Host "[MISSING] Python:     Python 3 not found in PATH" -ForegroundColor Red
    $AllReady = $false
    $MissingPython = $true
}

# 5. Check QEMU x86_64
$QemuCmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $QemuCmd) {
    if (Test-Path "C:\Program Files\qemu\qemu-system-x86_64.exe") {
        $QemuCmd = [PSCustomObject]@{ Source = "C:\Program Files\qemu\qemu-system-x86_64.exe" }
    }
}

if ($QemuCmd) {
    $QemuVer = (& $QemuCmd.Source --version | Select-Object -First 1)
    Write-Host "[OK] Emulator:        $($QemuCmd.Source) ($QemuVer)" -ForegroundColor Green
} else {
    Write-Host "[MISSING] Emulator:   qemu-system-x86_64 not found" -ForegroundColor Red
    $AllReady = $false
    $MissingQemu = $true
}

# 6. Check EDK2 / OVMF UEFI Firmware
$OvmfCandidates = @(
    "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
    "C:\Program Files\qemu\share\OVMF.fd",
    "C:\Program Files\qemu\OVMF.fd",
    "C:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd",
    "/usr/share/ovmf/OVMF.fd"
)

$FoundOvmf = $null
foreach ($cand in $OvmfCandidates) {
    if (Test-Path $cand) {
        $FoundOvmf = $cand
        break
    }
}

if ($FoundOvmf) {
    Write-Host "[OK] UEFI Firmware:   $FoundOvmf" -ForegroundColor Green
} else {
    Write-Host "[MISSING] UEFI Firmware: EDK2/OVMF (.fd) not found" -ForegroundColor Red
    $AllReady = $false
    $MissingOvmf = $true
}

# 7. Install missing components if needed
if (-not $AllReady) {
    Write-Host "`n============================================================" -ForegroundColor Yellow
    Write-Host " Missing dependencies detected. Attempting installation..." -ForegroundColor Yellow
    Write-Host "============================================================" -ForegroundColor Yellow

    $HasWinget = (Get-Command winget -ErrorAction SilentlyContinue) -ne $null
    $HasChoco = (Get-Command choco -ErrorAction SilentlyContinue) -ne $null

    if ($HasWinget) {
        Write-Host "Using Windows Package Manager (winget)..." -ForegroundColor Cyan
        if ($MissingCompiler) {
            Write-Host "Installing MinGW-w64 (WinLibs)..." -ForegroundColor Cyan
            & winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT --accept-source-agreements --accept-package-agreements
        }
        if ($MissingMake) {
            Write-Host "Installing GNU Make..." -ForegroundColor Cyan
            & winget install -e --id GnuWin32.Make --accept-source-agreements --accept-package-agreements
        }
        if ($MissingPython) {
            Write-Host "Installing Python 3..." -ForegroundColor Cyan
            & winget install -e --id Python.Python.3.11 --accept-source-agreements --accept-package-agreements
        }
        if ($MissingQemu) {
            Write-Host "Installing QEMU..." -ForegroundColor Cyan
            & winget install -e --id SoftwareFreedomConservancy.QEMU --accept-source-agreements --accept-package-agreements
        }
    } elseif ($HasChoco) {
        Write-Host "Using Chocolatey (choco)..." -ForegroundColor Cyan
        if ($MissingCompiler) {
            Write-Host "Installing MinGW..." -ForegroundColor Cyan
            & choco install -y mingw
        }
        if ($MissingMake) {
            Write-Host "Installing Make..." -ForegroundColor Cyan
            & choco install -y make
        }
        if ($MissingPython) {
            Write-Host "Installing Python..." -ForegroundColor Cyan
            & choco install -y python
        }
        if ($MissingQemu) {
            Write-Host "Installing QEMU..." -ForegroundColor Cyan
            & choco install -y qemu
        }
    } else {
        Write-Host "[WARN] Neither winget nor chocolatey was found on this system." -ForegroundColor Yellow
        Write-Host "Please install the missing tools manually or install winget/chocolatey:" -ForegroundColor Yellow
        Write-Host "  - GCC:   https://winlibs.com or MSYS2"
        Write-Host "  - Make:  GnuWin32 Make or choco install make"
        Write-Host "  - Python: https://python.org"
        Write-Host "  - QEMU:  https://www.qemu.org/download/#windows"
        exit 1
    }

    # Re-evaluate environment after installation
    Write-Host "`nRefreshing environment and re-checking..." -ForegroundColor Cyan
    $env:PATH = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
    foreach ($p in $KnownPaths) {
        if (Test-Path $p) {
            if ($env:PATH -notlike "*$p*") {
                $env:PATH = "$p;" + $env:PATH
            }
        }
    }
}

Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host " Diagnostic Summary" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$FinalGcc = Get-Command gcc -ErrorAction SilentlyContinue
$FinalMake = Get-Command make, mingw32-make -ErrorAction SilentlyContinue | Select-Object -First 1
$FinalPy = Get-Command python, python3 -ErrorAction SilentlyContinue | Select-Object -First 1
$FinalQemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if (-not $FinalQemu -and (Test-Path "C:\Program Files\qemu\qemu-system-x86_64.exe")) {
    $FinalQemu = [PSCustomObject]@{ Source = "C:\Program Files\qemu\qemu-system-x86_64.exe" }
}

if ($FinalGcc -and $FinalMake -and $FinalPy -and $FinalQemu) {
    Write-Host "[SUCCESS] All required tools are installed and ready!" -ForegroundColor Green
    Write-Host "  - Compile: powershell -File .\build.ps1" -ForegroundColor Green
    Write-Host "  - Run:     powershell -File .\run_normal.ps1" -ForegroundColor Green
    exit 0
} else {
    Write-Host "[ERROR] One or more required tools are still missing." -ForegroundColor Red
    exit 1
}
