BUILD  = build
TARGET = $(BUILD)/BOOTAA64.EFI
QEMU_MEM ?= 2048M
QEMU_SMP ?= 4
QEMU_CPU ?= cortex-a57
QEMU_WIN_CPU ?= host
LOG_LEVEL ?= 1

CC = clang
LD = lld-link

CFLAGS   = -target aarch64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone -mgeneral-regs-only -Wall -Wextra -O2 -Iincludes -DLOG_LEVEL=$(LOG_LEVEL)
LDFLAGS  = -subsystem:efi_application -entry:efi_main

OBJ = $(BUILD)/main.o $(BUILD)/uart.o $(BUILD)/exceptions.o $(BUILD)/mmu.o \
      $(BUILD)/pmm.o $(BUILD)/frame.o $(BUILD)/vmm.o $(BUILD)/vma.o $(BUILD)/vm_object.o $(BUILD)/page_cache.o $(BUILD)/kmalloc.o $(BUILD)/vectors.o \
      $(BUILD)/gic.o $(BUILD)/timer.o $(BUILD)/acpi.o $(BUILD)/orange_cat.o $(BUILD)/ipc.o $(BUILD)/memcap.o $(BUILD)/vfs.o $(BUILD)/smp.o $(BUILD)/sched.o \
      $(BUILD)/elf.o $(BUILD)/initrd.o $(BUILD)/usercopy.o

KERNEL_HEADERS := $(wildcard includes/*.h)
USER_HEADERS   := $(wildcard user/*.h)
BIN_ELFS       := $(BUILD)/shell.elf $(BUILD)/pong.elf $(BUILD)/fault.elf $(BUILD)/spin.elf $(BUILD)/badptr.elf \
                  $(BUILD)/stackgrow.elf $(BUILD)/memshare.elf $(BUILD)/memxfer.elf $(BUILD)/memrevoke.elf \
                  $(BUILD)/ipcfast.elf $(BUILD)/ipccap.elf $(BUILD)/ipckill.elf $(BUILD)/speed.elf $(BUILD)/speedipc.elf
SBIN_ELFS      := $(BUILD)/init.elf $(BUILD)/uart.elf $(BUILD)/keyboard.elf $(BUILD)/ns.elf $(BUILD)/block.elf $(BUILD)/fs.elf
APP_ELFS       := $(BIN_ELFS) $(SBIN_ELFS)
BOOT_ELFS      := $(BUILD)/init.elf $(BUILD)/ns.elf $(BUILD)/block.elf $(BUILD)/fs.elf
USER_ELFS      := $(APP_ELFS)
BOOT_MANIFEST  := etc/boot.txt

USER_CFLAGS = -target aarch64-linux-gnu -ffreestanding -nostdlib -static -fno-builtin \
              -mgeneral-regs-only -Ithird_party/fatfs -Iincludes \
              -Wl,-Ttext=0x80000000 -Wl,--entry=_start

# ── Top-level targets ────────────────────────────────────────────────────────

all: $(TARGET)
	@echo "Build successful: $(TARGET)"

.PHONY: all user_apps update-storage-apps image run run_g runwin reset-storage clean distclean

$(BUILD):
	mkdir -p $(BUILD)

# ── User-space ELF blobs ─────────────────────────────────────────────────────

user_apps: $(USER_ELFS)

$(OBJ): $(KERNEL_HEADERS)

$(USER_ELFS): $(USER_HEADERS)

$(BUILD)/pong.elf: user/pong.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/pong.c user/lib.c user/malloc.c -o $(BUILD)/pong.elf

$(BUILD)/fault.elf: user/fault.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/fault.c user/lib.c user/malloc.c -o $(BUILD)/fault.elf

$(BUILD)/spin.elf: user/spin.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/spin.c user/lib.c user/malloc.c -o $(BUILD)/spin.elf

$(BUILD)/badptr.elf: user/badptr.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/badptr.c user/lib.c user/malloc.c -o $(BUILD)/badptr.elf

$(BUILD)/stackgrow.elf: user/stackgrow.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/stackgrow.c user/lib.c user/malloc.c -o $(BUILD)/stackgrow.elf

$(BUILD)/memshare.elf: user/memshare.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/memshare.c user/lib.c user/malloc.c -o $(BUILD)/memshare.elf

$(BUILD)/memxfer.elf: user/memxfer.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/memxfer.c user/lib.c user/malloc.c -o $(BUILD)/memxfer.elf

$(BUILD)/memrevoke.elf: user/memrevoke.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/memrevoke.c user/lib.c user/malloc.c -o $(BUILD)/memrevoke.elf

$(BUILD)/ipcfast.elf: user/ipcfast.c user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/ipcfast.c user/lib.c user/malloc.c -o $(BUILD)/ipcfast.elf

$(BUILD)/ipccap.elf: user/ipccap.c user/ipc_proto.h user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/ipccap.c user/lib.c user/malloc.c -o $(BUILD)/ipccap.elf

$(BUILD)/ipckill.elf: user/ipckill.c user/ipc_proto.h user/lib.h user/lib.c user/ns_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/ipckill.c user/lib.c user/malloc.c -o $(BUILD)/ipckill.elf

$(BUILD)/speed.elf: user/speed.c user/lib.h user/lib.c user/ns_proto.h user/ipc_proto.h user/malloc.h user/malloc.c | $(BUILD)
	clang $(USER_CFLAGS) user/speed.c user/lib.c user/malloc.c -o $(BUILD)/speed.elf

$(BUILD)/speedipc.elf: user/speedipc.c user/lib.h user/lib.c | $(BUILD)
	clang $(USER_CFLAGS) user/speedipc.c user/lib.c -o $(BUILD)/speedipc.elf

$(BUILD)/shell.elf: user/shell.c user/lib.h user/lib.c user/malloc.h user/malloc.c user/fs_proto.h user/ns_proto.h user/ipc_proto.h | $(BUILD)
	clang $(USER_CFLAGS) user/shell.c user/lib.c user/malloc.c -o $(BUILD)/shell.elf

$(BUILD)/init.elf: user/init.c user/lib.h user/lib.c user/fs_proto.h user/ns_proto.h user/ipc_proto.h | $(BUILD)
	clang $(USER_CFLAGS) user/init.c user/lib.c -o $(BUILD)/init.elf

$(BUILD)/uart.elf: user/uart.c user/lib.h user/lib.c user/ns_proto.h user/ipc_proto.h | $(BUILD)
	clang $(USER_CFLAGS) user/uart.c user/lib.c -o $(BUILD)/uart.elf

$(BUILD)/keyboard.elf: user/keyboard.c user/lib.h user/lib.c user/ns_proto.h user/ipc_proto.h | $(BUILD)
	clang $(USER_CFLAGS) user/keyboard.c user/lib.c -o $(BUILD)/keyboard.elf

$(BUILD)/ns.elf: user/ns.c user/ns_proto.h user/ipc_proto.h user/lib.h user/lib.c | $(BUILD)
	clang $(USER_CFLAGS) user/ns.c user/lib.c -o $(BUILD)/ns.elf

$(BUILD)/block.elf: user/block.c user/block_proto.h user/fs_proto.h user/ns_proto.h user/ipc_proto.h user/lib.h user/lib.c | $(BUILD)
	clang $(USER_CFLAGS) user/block.c user/lib.c -o $(BUILD)/block.elf

$(BUILD)/fs.elf: user/fs.c user/fat_diskio.c user/fat_diskio.h user/fat_unicode.c user/fs_proto.h user/ns_proto.h user/ipc_proto.h user/lib.h user/lib.c user/malloc.h user/malloc.c third_party/fatfs/ff.c third_party/fatfs/ff.h third_party/fatfs/diskio.h third_party/fatfs/ffconf.h | $(BUILD)
	clang $(USER_CFLAGS) user/fs.c user/fat_diskio.c user/fat_unicode.c user/lib.c user/malloc.c third_party/fatfs/ff.c -o $(BUILD)/fs.elf

$(BUILD)/apps.fat: user_apps $(BOOT_MANIFEST) | $(BUILD)
	rm -f $(BUILD)/apps.fat
	dd if=/dev/zero of=$(BUILD)/apps.fat bs=1M count=4
	mkfs.fat -F 12 $(BUILD)/apps.fat
	mmd -i $(BUILD)/apps.fat ::/bin
	mmd -i $(BUILD)/apps.fat ::/sbin
	mmd -i $(BUILD)/apps.fat ::/etc
	mcopy -i $(BUILD)/apps.fat $(BIN_ELFS) ::/bin/
	mcopy -i $(BUILD)/apps.fat $(SBIN_ELFS) ::/sbin/
	mcopy -i $(BUILD)/apps.fat $(BOOT_MANIFEST) ::/etc/boot.txt

# ── Kernel object files ───────────────────────────────────────────────────────

$(BUILD)/main.o: src/kernel/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/uart.o: src/drivers/uart.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sched.o: src/kernel/sched.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/orange_cat.o: src/kernel/orange_cat.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ipc.o: src/kernel/ipc.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/memcap.o: src/kernel/memcap.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vfs.o: src/kernel/vfs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/smp.o: src/kernel/smp.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf.o: src/kernel/elf.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/initrd.o: src/kernel/initrd.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/usercopy.o: src/kernel/usercopy.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/gic.o: src/drivers/gic.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/timer.o: src/drivers/timer.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/exceptions.o: src/arch/aarch64/exceptions.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vectors.o: src/arch/aarch64/vectors.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/acpi.o: src/arch/aarch64/acpi.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mmu.o: src/arch/aarch64/mmu.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pmm.o: src/mm/pmm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/frame.o: src/mm/frame.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vmm.o: src/mm/vmm.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vma.o: src/mm/vma.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vm_object.o: src/mm/vm_object.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/page_cache.o: src/mm/page_cache.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kmalloc.o: src/mm/kmalloc.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Link ─────────────────────────────────────────────────────────────────────

$(TARGET): $(OBJ) | $(BUILD)
	$(LD) $(LDFLAGS) $(OBJ) -out:$@

# ── Image & Run ───────────────────────────────────────────────────────────────

$(BUILD)/storage.fat: | $(BUILD)/apps.fat
	cp $(BUILD)/apps.fat $(BUILD)/storage.fat

update-storage-apps: user_apps $(BOOT_MANIFEST) $(BUILD)/storage.fat
	@mdir -i $(BUILD)/storage.fat ::/bin >/dev/null 2>&1 || mmd -i $(BUILD)/storage.fat ::/bin
	@mdir -i $(BUILD)/storage.fat ::/sbin >/dev/null 2>&1 || mmd -i $(BUILD)/storage.fat ::/sbin
	@mdir -i $(BUILD)/storage.fat ::/etc >/dev/null 2>&1 || mmd -i $(BUILD)/storage.fat ::/etc
	@for f in $(notdir $(APP_ELFS)); do mdel -i $(BUILD)/storage.fat ::/$$f >/dev/null 2>&1 || true; done
	mcopy -o -i $(BUILD)/storage.fat $(BIN_ELFS) ::/bin/
	mcopy -o -i $(BUILD)/storage.fat $(SBIN_ELFS) ::/sbin/
	mcopy -o -i $(BUILD)/storage.fat $(BOOT_MANIFEST) ::/etc/boot.txt

image: $(TARGET) $(BOOT_ELFS) update-storage-apps
	@echo "Creating FAT32 image..."
	rm -f $(BUILD)/initrd.bin
	dd if=/dev/zero of=$(BUILD)/fat.img bs=1M count=64
	mkfs.fat -F 32 $(BUILD)/fat.img
	mmd -i $(BUILD)/fat.img ::/EFI
	mmd -i $(BUILD)/fat.img ::/EFI/BOOT
	mmd -i $(BUILD)/fat.img ::/boot
	mcopy -i $(BUILD)/fat.img $(TARGET) ::/EFI/BOOT/BOOTAA64.EFI
	mcopy -i $(BUILD)/fat.img $(BOOT_ELFS) ::/boot/
	@echo "Image created: $(BUILD)/fat.img"

reset-storage:
	rm -f $(BUILD)/storage.fat
	$(MAKE) $(BUILD)/storage.fat

run: image
	qemu-system-aarch64 \
		-M virt \
		-smp $(QEMU_SMP) \
		-cpu $(QEMU_CPU) \
		-m $(QEMU_MEM) \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-drive file=$(BUILD)/fat.img,format=raw,if=none,id=bootfat \
		-device virtio-blk-device,drive=bootfat \
		-drive file=$(BUILD)/storage.fat,format=raw,if=none,id=storage \
		-device virtio-blk-pci,disable-legacy=on,drive=storage \
		-nographic

run_g: image
	qemu-system-aarch64 \
		-M virt \
		-smp $(QEMU_SMP) \
		-cpu $(QEMU_CPU) \
		-m $(QEMU_MEM) \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-drive file=$(BUILD)/fat.img,format=raw,if=none,id=bootfat \
		-device virtio-blk-device,drive=bootfat \
		-drive file=$(BUILD)/storage.fat,format=raw,if=none,id=storage \
		-device virtio-blk-pci,disable-legacy=on,drive=storage

runwin: image
	mingw-w64-clang-aarch64-qemu \
		-M virt,accel=whpx \
		-smp $(QEMU_SMP) \
		-cpu $(QEMU_WIN_CPU) \
		-m $(QEMU_MEM) \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-drive file=$(BUILD)/fat.img,format=raw,if=none,id=bootfat \
		-device virtio-blk-device,drive=bootfat \
		-drive file=$(BUILD)/storage.fat,format=raw,if=none,id=storage \
		-device virtio-blk-pci,disable-legacy=on,drive=storage

clean:
	@tmp=$$(mktemp); \
	if [ -f $(BUILD)/storage.fat ]; then cp $(BUILD)/storage.fat $$tmp; else rm -f $$tmp; fi; \
	rm -rf $(BUILD) user/*_elf.h; \
	if [ -f $$tmp ]; then mkdir -p $(BUILD); mv $$tmp $(BUILD)/storage.fat; fi

distclean:
	rm -rf $(BUILD) user/*_elf.h
