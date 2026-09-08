[CmdletBinding()]
param(
    [switch]$Clean,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"
$ScriptDir = $PSScriptRoot
$UefiDir = Join-Path $ScriptDir "UEFI-Files"

# Ensure MinGW64 toolchain is on PATH if installed
if (Test-Path "C:\mingw64\bin") {
    $env:PATH = "C:\mingw64\bin;" + $env:PATH
}

if ($Clean -or ($ExtraArgs -contains "clean")) {
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host " Cleaning pseuDOS Build Artifacts" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan
    make -C "$UefiDir" clean
    Write-Host "============================================================" -ForegroundColor Green
    Write-Host " Clean Complete" -ForegroundColor Green
    Write-Host "============================================================" -ForegroundColor Green
    exit 0
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Building pseuDOS UEFI System" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

if ($ExtraArgs) {
    make -C "$UefiDir" @ExtraArgs
} else {
    make -C "$UefiDir"
}

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "============================================================" -ForegroundColor Green
Write-Host " Build Complete: UEFI-Files/build/pseuDOS.iso" -ForegroundColor Green
Write-Host " Run with: .\run_normal.ps1 or .\run_debug.ps1" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
