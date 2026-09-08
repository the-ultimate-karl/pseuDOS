[CmdletBinding(PositionalBinding = $false)]
param(
    [Alias("g")][switch]$Gui,
    [Alias("c")][switch]$Curses,
    [Alias("t", "nographic")][switch]$Terminal,
    [switch]$Sata,
    [switch]$Nvme,
    [Alias("scsi")][switch]$Usb,
    [switch]$Usb2,
    [switch]$All,
    [Alias("no-iso", "no-cdrom", "disk-boot")][switch]$NoIso,
    [Alias("boot-from", "boot")][string]$BootFrom,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"
$ScriptDir = $PSScriptRoot
$IsoPath = Join-Path $ScriptDir "UEFI-Files\build\pseuDOS.iso"
$DisksDir = Join-Path $ScriptDir "UEFI-Files\build\disks"
$LogDir = Join-Path $ScriptDir "UEFI-Bootlogs"

# Timestamp
$BootDate = [DateTime]::UtcNow.ToString("yyyy-MM-dd")
$BootTime = [DateTime]::UtcNow.ToString("HH-mm-ss'(GMT)'")
$LogFile = Join-Path $LogDir "UEFIBoot-$BootDate-$BootTime.log"

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $DisksDir | Out-Null

# Locate QEMU
$QemuExe = "qemu-system-x86_64"
if (-not (Get-Command $QemuExe -ErrorAction SilentlyContinue)) {
    $CommonQemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (Test-Path $CommonQemu) {
        $QemuExe = $CommonQemu
    } else {
        Write-Error "[ERROR] qemu-system-x86_64 not found in PATH or 'C:\Program Files\qemu'."
        exit 1
    }
}

# Locate EDK2 / OVMF UEFI Firmware
$OvmfCandidates = @(
    "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
    "C:\Program Files\qemu\share\OVMF.fd",
    "C:\Program Files\qemu\OVMF.fd",
    "/usr/share/ovmf/OVMF.fd"
)

$OvmfBios = ""
foreach ($cand in $OvmfCandidates) {
    if (Test-Path $cand) {
        $OvmfBios = $cand
        break
    }
}

if (-not $OvmfBios) {
    Write-Error "[ERROR] OVMF/EDK2 UEFI firmware not found on system."
    exit 1
}

$Mode = "nographic"
if ($Gui) { $Mode = "gui" }
elseif ($Curses) { $Mode = "curses" }
elseif ($Terminal) { $Mode = "nographic" }

$AttachSata = [bool]$Sata
$AttachNvme = [bool]$Nvme
$AttachUsb3 = [bool]$Usb
$AttachUsb2 = [bool]$Usb2
$IsNoIso = [bool]$NoIso
if ($All) {
    $AttachSata = $true
    $AttachNvme = $true
    $AttachUsb3 = $true
}
$ResolvedBootFrom = if ($BootFrom) { $BootFrom.ToLower() } else { "" }

# Process any remaining/extra arguments (e.g. Linux double-dash flags or QEMU args)
$FinalExtraArgs = [System.Collections.Generic.List[string]]::new()
$i = 0
[string[]]$rawArgs = if ($ExtraArgs) { @($ExtraArgs) } else { @() }

while ($i -lt $rawArgs.Count) {
    $arg = $rawArgs[$i]
    switch -Regex ($arg) {
        "^--?gui$|^-g$" {
            $Mode = "gui"
        }
        "^--?curses$|^-c$" {
            $Mode = "curses"
        }
        "^--?(terminal|nographic)$|^-t$" {
            $Mode = "nographic"
        }
        "^--?sata$" {
            $AttachSata = $true
        }
        "^--?nvme$" {
            $AttachNvme = $true
        }
        "^--?(usb|scsi)$" {
            $AttachUsb3 = $true
        }
        "^--?usb2$" {
            $AttachUsb2 = $true
        }
        "^--?all$" {
            $AttachSata = $true
            $AttachNvme = $true
            $AttachUsb3 = $true
        }
        "^--?(no-iso|no-cdrom|disk-boot)$" {
            $IsNoIso = $true
        }
        "^--?boot-from=(.+)$" {
            $IsNoIso = $true
            $ResolvedBootFrom = $Matches[1].ToLower()
        }
        "^--?boot-from$" {
            $IsNoIso = $true
            if ($i + 1 -lt $rawArgs.Count) {
                $i++
                $ResolvedBootFrom = $rawArgs[$i].ToLower()
            } else {
                Write-Error "[ERROR] --boot-from requires an argument: sata, nvme, or usb"
                exit 1
            }
        }
        default {
            $FinalExtraArgs.Add($arg)
        }
    }
    $i++
}

# Validate boot-from if specified
if ($ResolvedBootFrom) {
    $IsNoIso = $true
    switch ($ResolvedBootFrom) {
        "sata" { $AttachSata = $true }
        "nvme" { $AttachNvme = $true }
        "usb"  { $AttachUsb3 = $true }
        default {
            Write-Error "[ERROR] Invalid option for --boot-from: '$ResolvedBootFrom'. Supported options: sata, nvme, usb"
            exit 1
        }
    }
}

if ($IsNoIso -and -not $ResolvedBootFrom) {
    if ($AttachNvme -and -not $AttachSata) {
        $ResolvedBootFrom = "nvme"
    } elseif ($AttachUsb3 -and -not $AttachSata) {
        $ResolvedBootFrom = "usb"
    } else {
        $ResolvedBootFrom = "sata"
        $AttachSata = $true
    }
}

$BootFrom = $ResolvedBootFrom

if (-not $IsNoIso -and -not (Test-Path $IsoPath)) {
    Write-Host "[INFO] ISO not found at $IsoPath. Building..." -ForegroundColor Yellow
    & (Join-Path $ScriptDir "build.ps1")
}

function Ensure-DiskImage($path, $sizeMB = 500) {
    if (-not (Test-Path $path)) {
        $stream = [System.IO.File]::Create($path)
        $stream.SetLength([int64]$sizeMB * 1024 * 1024)
        $stream.Close()
    }
}

$StorageArgs = @()

if ($AttachSata) {
    $sataImg = Join-Path $DisksDir "sata_disk.img"
    Ensure-DiskImage $sataImg 500
    $bootProp = ""
    if ($IsNoIso -and $BootFrom -eq "sata") { $bootProp = ",bootindex=1" }
    $StorageArgs += "-drive", "file=$sataImg,if=none,id=sata0,format=raw"
    $StorageArgs += "-device", "ich9-ahci,id=ahci"
    $StorageArgs += "-device", "ide-hd,drive=sata0,bus=ahci.0$bootProp"
}

if ($AttachNvme) {
    $nvmeImg = Join-Path $DisksDir "nvme_disk.img"
    Ensure-DiskImage $nvmeImg 500
    $bootProp = ""
    if ($IsNoIso -and $BootFrom -eq "nvme") { $bootProp = ",bootindex=1" }
    $StorageArgs += "-drive", "file=$nvmeImg,if=none,id=nvm0,format=raw"
    $StorageArgs += "-device", "nvme,serial=970EVO500M,drive=nvm0$bootProp"
}

if ($AttachUsb3) {
    $usbImg = Join-Path $DisksDir "usb_disk.img"
    Ensure-DiskImage $usbImg 500
    $bootProp = ""
    if ($IsNoIso -and $BootFrom -eq "usb") { $bootProp = ",bootindex=1" }
    $StorageArgs += "-device", "qemu-xhci,id=xhci"
    $StorageArgs += "-drive", "file=$usbImg,if=none,id=usb0,format=raw"
    $StorageArgs += "-device", "usb-storage,bus=xhci.0,drive=usb0$bootProp"
}

if ($AttachUsb2) {
    $usb2Img = Join-Path $DisksDir "usb2_disk.img"
    Ensure-DiskImage $usb2Img 500
    $StorageArgs += "-device", "usb-ehci,id=ehci"
    $StorageArgs += "-drive", "file=$usb2Img,if=none,id=usb2_0,format=raw"
    $StorageArgs += "-device", "usb-storage,bus=ehci.0,drive=usb2_0"
}

$QemuBootArgs = @()
if ($IsNoIso) {
    $QemuBootArgs += "-boot", "order=c,menu=off"
} else {
    $QemuBootArgs += "-cdrom", "$IsoPath", "-boot", "order=d,menu=off"
}

$BiosArgs = @()
if ($OvmfBios.EndsWith("edk2-x86_64-code.fd")) {
    $BiosArgs += "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=$OvmfBios"
} else {
    $BiosArgs += "-bios", "$OvmfBios"
}

$DebugconLog = [System.IO.Path]::GetTempFileName()
$StderrLog = [System.IO.Path]::GetTempFileName()

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Starting pseuDOS (Debug Logging Mode)" -ForegroundColor Cyan
Write-Host " Log File: $LogFile" -ForegroundColor Yellow
Write-Host "============================================================" -ForegroundColor Cyan

$QemuCommon = @(
    "-m", "512M",
    "-smp", "2",
    "-cpu", "max",
    "-net", "none",
    "-debugcon", "file:$DebugconLog",
    "-global", "isa-debugcon.iobase=0x402"
)

if ($Mode -eq "gui") {
    $DisplayArgs = @("-serial", "mon:stdio")
} elseif ($Mode -eq "curses") {
    $DisplayArgs = @("-display", "curses")
} else {
    $DisplayArgs = @("-nographic")
}

$AllCmdArgs = $BiosArgs + $QemuBootArgs + $QemuCommon + $DisplayArgs + $StorageArgs + $FinalExtraArgs

try {
    & $QemuExe @AllCmdArgs 2>$StderrLog | Tee-Object -FilePath $LogFile
} finally {
    Add-Content -Path $LogFile -Value "`n--- FIRMWARE DEBUG OUTPUT (Port 0x402) ---"
    if (Test-Path $DebugconLog) {
        Get-Content $DebugconLog -ErrorAction SilentlyContinue | Add-Content -Path $LogFile
        Remove-Item $DebugconLog -ErrorAction SilentlyContinue
    }
    Add-Content -Path $LogFile -Value "`n--- QEMU STDERR / HOST LOGS ---"
    if (Test-Path $StderrLog) {
        Get-Content $StderrLog -ErrorAction SilentlyContinue | Add-Content -Path $LogFile
        Remove-Item $StderrLog -ErrorAction SilentlyContinue
    }
    Add-Content -Path $LogFile -Value "`n============================================================"
    Add-Content -Path $LogFile -Value " Session Ended: $([DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm:ss')) GMT"
    Add-Content -Path $LogFile -Value "============================================================"
    Write-Host "`n[INFO] Debug log saved to: $LogFile" -ForegroundColor Green
}
