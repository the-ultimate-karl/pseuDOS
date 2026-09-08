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

# Locate QEMU executable
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

New-Item -ItemType Directory -Force -Path $DisksDir | Out-Null

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

# Build ISO if needed
if (-not $IsNoIso -and -not (Test-Path $IsoPath)) {
    Write-Host "[INFO] ISO not found at $IsoPath. Building..." -ForegroundColor Yellow
    & (Join-Path $ScriptDir "build.ps1")
}

# Function to check partition
function Test-DiskPartitioned($diskPath) {
    if (-not (Test-Path $diskPath)) { return $false }
    $fileInfo = Get-Item $diskPath
    if ($fileInfo.Length -lt 512) { return $false }
    try {
        $stream = [System.IO.File]::OpenRead($diskPath)
        $mbr = New-Object byte[] 512
        [void]$stream.Read($mbr, 0, 512)
        if ($mbr[510] -ne 0x55 -or $mbr[511] -ne 0xAA) {
            $stream.Close()
            return $false
        }
        $gpt = New-Object byte[] 512
        $read = $stream.Read($gpt, 0, 512)
        $stream.Close()
        if ($read -ge 8) {
            $sig = [System.Text.Encoding]::ASCII.GetString($gpt, 0, 8)
            if ($sig -eq "EFI PART") { return $true }
        }
        foreach ($off in @(446, 462, 478, 494)) {
            if ($mbr[$off + 4] -ne 0) { return $true }
        }
        return $false
    } catch {
        return $false
    }
}

function Ensure-DiskImage($path, $sizeMB = 500) {
    if (-not (Test-Path $path)) {
        $stream = [System.IO.File]::Create($path)
        $stream.SetLength([int64]$sizeMB * 1024 * 1024)
        $stream.Close()
    }
}

if ($IsNoIso) {
    $TargetImg = ""
    switch ($BootFrom) {
        "sata" { $TargetImg = Join-Path $DisksDir "sata_disk.img" }
        "nvme" { $TargetImg = Join-Path $DisksDir "nvme_disk.img" }
        "usb"  { $TargetImg = Join-Path $DisksDir "usb_disk.img" }
    }
    if (-not (Test-DiskPartitioned $TargetImg)) {
        Write-Host "============================================================" -ForegroundColor Yellow
        Write-Host "[WARNING] Target boot disk ($TargetImg) is blank or unpartitioned!" -ForegroundColor Yellow
        Write-Host "[WARNING] pseuDOS has not been installed onto this disk yet." -ForegroundColor Yellow
        Write-Host "[WARNING] Tip: Boot with the ISO (without -NoIso) and run 'flash' to install." -ForegroundColor Yellow
        Write-Host "============================================================" -ForegroundColor Yellow
        $confirm = Read-Host "Attempt to boot anyway? (y/N)"
        if ($confirm -notmatch "^[yY]$") {
            Write-Host "[INFO] Aborting launch." -ForegroundColor Yellow
            exit 1
        }
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

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Starting pseuDOS (Normal Mode)" -ForegroundColor Cyan
if ($IsNoIso) {
    Write-Host " Media: Installed Disk ($($BootFrom.ToUpper()) Drive - Standalone Boot)" -ForegroundColor White
} else {
    Write-Host " ISO:   $IsoPath" -ForegroundColor White
}
Write-Host " BIOS:  $OvmfBios" -ForegroundColor White
if ($AttachSata) { Write-Host " Storage: [Attached] 500 MB AHCI SATA Disk" -ForegroundColor Gray }
if ($AttachNvme) { Write-Host " Storage: [Attached] 500 MB NVMe PCIe SSD" -ForegroundColor Gray }
if ($AttachUsb3) { Write-Host " Storage: [Attached] 500 MB USB 3.0/3.1 (xHCI) Drive" -ForegroundColor Gray }
if ($AttachUsb2) { Write-Host " Storage: [Attached] 500 MB USB 2.0 (EHCI) Drive" -ForegroundColor Gray }

if ($Mode -eq "gui") {
    Write-Host " Display: Graphical GUI Window" -ForegroundColor White
} elseif ($Mode -eq "curses") {
    Write-Host " Display: Curses Screen" -ForegroundColor White
} else {
    Write-Host " Display: Live Terminal Console (Press Ctrl+A then X to exit)" -ForegroundColor White
}
Write-Host "============================================================" -ForegroundColor Cyan

$QemuCommon = @(
    "-m", "512M",
    "-smp", "2",
    "-cpu", "max",
    "-net", "none"
)

if ($Mode -eq "gui") {
    $DisplayArgs = @("-serial", "mon:stdio")
} elseif ($Mode -eq "curses") {
    $DisplayArgs = @("-display", "curses")
} else {
    $DisplayArgs = @("-nographic")
}

$AllCmdArgs = $BiosArgs + $QemuBootArgs + $QemuCommon + $DisplayArgs + $StorageArgs + $FinalExtraArgs
& $QemuExe @AllCmdArgs
