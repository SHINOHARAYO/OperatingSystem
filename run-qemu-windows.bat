@echo off
setlocal

set "ROOT=%~dp0"
set "BIOS=%ROOT%..\QEMU_EFI.fd"
set "FAT_IMG=%ROOT%build\fat.img"
set "STORAGE_IMG=%ROOT%build\storage.fat"

if not defined QEMU_ACCEL set "QEMU_ACCEL=tcg"
if not defined QEMU_CPU set "QEMU_CPU=cortex-a57"
if not defined QEMU_VIDEO set "QEMU_VIDEO=virtio-gpu-only"
if not defined QEMU_SERIAL set "QEMU_SERIAL=stdio"
if not defined QEMU_GPU_WIDTH set "QEMU_GPU_WIDTH=2160"
if not defined QEMU_GPU_HEIGHT set "QEMU_GPU_HEIGHT=1440"
if not defined QEMU_DISPLAY set "QEMU_DISPLAY=gtk,zoom-to-fit=off,show-tabs=off,show-menubar=on"

if /I "%QEMU_ACCEL%"=="tcg" (
    if /I "%QEMU_CPU%"=="host" set "QEMU_CPU=cortex-a57"
    set "QEMU_ACCEL_ARGS=tcg,thread=multi"
) else (
    set "QEMU_ACCEL_ARGS=%QEMU_ACCEL%"
)

if /I "%QEMU_VIDEO%"=="virtio-gpu" (
    set "QEMU_VIDEO_ARGS=-device ramfb -device virtio-gpu-pci,disable-legacy=on,xres=%QEMU_GPU_WIDTH%,yres=%QEMU_GPU_HEIGHT%"
) else if /I "%QEMU_VIDEO%"=="virtio-gpu-only" (
    set "QEMU_VIDEO_ARGS=-device virtio-gpu-pci,disable-legacy=on,xres=%QEMU_GPU_WIDTH%,yres=%QEMU_GPU_HEIGHT%"
) else (
    set "QEMU_VIDEO_ARGS=-device ramfb"
)

if not exist "%BIOS%" (
    echo Missing BIOS: "%BIOS%"
    exit /b 1
)

if not exist "%FAT_IMG%" (
    echo Missing boot image: "%FAT_IMG%"
    echo Build it from WSL first with: make image
    exit /b 1
)

if not exist "%STORAGE_IMG%" (
    echo Missing storage image: "%STORAGE_IMG%"
    echo Build it from WSL first with: make image
    exit /b 1
)

qemu-system-aarch64 ^
    -M virt ^
    -global virtio-mmio.force-legacy=false ^
    -smp 4 ^
    -cpu "%QEMU_CPU%" ^
    -m 2048M ^
    -accel "%QEMU_ACCEL_ARGS%" ^
    -bios "%BIOS%" ^
    -drive file="%FAT_IMG%",format=raw,if=none,id=bootfat ^
    -device virtio-blk-device,drive=bootfat ^
    -drive file="%STORAGE_IMG%",format=raw,if=none,id=storage ^
    -device virtio-blk-pci,disable-legacy=on,drive=storage ^
    -display "%QEMU_DISPLAY%" ^
    %QEMU_VIDEO_ARGS% ^
    -device virtio-keyboard-device ^
    -device virtio-mouse-device ^
    -device virtio-tablet-device ^
    -serial "%QEMU_SERIAL%"

endlocal
