param(
    [Parameter(Mandatory = $true)] [string] $FirmwareDir,
    [Parameter(Mandatory = $true)] [string] $Destination
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bootTree = Join-Path $root 'build\pi4\pi4-esp'
$requiredFirmware = @('RPI_EFI.fd', 'start4.elf', 'fixup4.dat', 'bcm2711-rpi-4-b.dtb')

if (-not (Test-Path (Join-Path $bootTree 'EFI\BOOT\BOOTAA64.EFI'))) {
    throw "Missing Pi 4 build output. Run 'wsl make PLATFORM=pi4 pi4-image' first."
}
foreach ($name in $requiredFirmware) {
    if (-not (Test-Path (Join-Path $FirmwareDir $name))) {
        throw "Firmware directory is missing $name. Extract a Raspberry Pi 4 UEFI release first."
    }
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Copy-Item -Recurse -Force (Join-Path $FirmwareDir '*') $Destination
Copy-Item -Recurse -Force (Join-Path $bootTree 'EFI') $Destination
$configPath = Join-Path $Destination 'config.txt'
Add-Content -Path $configPath -Value "`n# Neptune serial bring-up`n$(Get-Content -Raw (Join-Path $bootTree 'config.txt'))"

Write-Host "Pi 4 UEFI boot tree prepared at $Destination"
Write-Host "Serial: 115200 8N1, GPIO14/pin 8 TX, GPIO15/pin 10 RX, pin 6 ground."
