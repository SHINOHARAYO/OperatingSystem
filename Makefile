BUILD  = build
TARGET = $(BUILD)/BOOTAA64.EFI
QEMU_MEM ?= 2048M
QEMU_CPU ?= cortex-a57
QEMU_WIN_CPU ?= host
LOG_LEVEL ?= 1

CC = clang
LD = lld-link

CFLAGS   = -target aarch64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone -Wall -Wextra -O2 -Iincludes -DLOG_LEVEL=$(LOG_LEVEL)
LDFLAGS  = -subsystem:efi_application -entry:efi_main

OBJ = $(BUILD)/main.o $(BUILD)/uart.o $(BUILD)/exceptions.o $(BUILD)/mmu.o \
      $(BUILD)/pmm.o $(BUILD)/frame.o $(BUILD)/vmm.o $(BUILD)/vma.o $(BUILD)/vm_object.o $(BUILD)/page_cache.o $(BUILD)/kmalloc.o $(BUILD)/vectors.o \
      $(BUILD)/gic.o $(BUILD)/timer.o $(BUILD)/acpi.o $(BUILD)/orange_cat.o $(BUILD)/ipc.o $(BUILD)/memcap.o $(BUILD)/vfs.o $(BUILD)/sched.o \
      $(BUILD)/elf.o $(BUILD)/initrd.o $(BUILD)/usercopy.o

KERNEL_HEADERS := $(wildcard includes/*.h)
USER_HEADERS   := $(wildcard user/*.h)
USER_ELFS      := $(BUILD)/shell.elf $(BUILD)/pong.elf $(BUILD)/fault.elf $(BUILD)/spin.elf $(BUILD)/badptr.elf \
                  $(BUILD)/stackgrow.elf $(BUILD)/memshare.elf $(BUILD)/memxfer.elf $(BUILD)/memrevoke.elf \
                  $(BUILD)/ipcfast.elf $(BUILD)/ipccap.elf $(BUILD)/ipckill.elf $(BUILD)/speed.elf $(BUILD)/speedipc.elf \
                  $(BUILD)/uart.elf $(BUILD)/keyboard.elf $(BUILD)/ns.elf $(BUILD)/fs.elf

USER_CFLAGS = -target aarch64-linux-gnu -ffreestanding -nostdlib -static -fno-builtin \
              -Ithird_party/fatfs -Wl,-Ttext=0x80000000 -Wl,--entry=_start

# ── Top-level targets ────────────────────────────────────────────────────────

all: $(TARGET) $(BUILD)/initrd.bin
	@echo "Build successful: $(TARGET)"

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

$(BUILD)/uart.elf: user/uart.c user/lib.h user/lib.c user/ns_proto.h user/ipc_proto.h | $(BUILD)
	clang $(USER_CFLAGS) user/uart.c user/lib.c -o $(BUILD)/uart.elf

$(BUILD)/keyboard.elf: user/keyboard.c user/lib.h user/lib.c user/ns_proto.h user/ipc_proto.h | $(BUILD)
	clang $(USER_CFLAGS) user/keyboard.c user/lib.c -o $(BUILD)/keyboard.elf

$(BUILD)/ns.elf: user/ns.c user/ns_proto.h user/ipc_proto.h user/lib.h user/lib.c | $(BUILD)
	clang $(USER_CFLAGS) user/ns.c user/lib.c -o $(BUILD)/ns.elf

$(BUILD)/fs.elf: user/fs.c user/fat_diskio.c user/fat_diskio.h user/fat_unicode.c user/fs_proto.h user/ns_proto.h user/ipc_proto.h user/lib.h user/lib.c user/malloc.h user/malloc.c third_party/fatfs/ff.c third_party/fatfs/ff.h third_party/fatfs/diskio.h third_party/fatfs/ffconf.h | $(BUILD)
	clang $(USER_CFLAGS) user/fs.c user/fat_diskio.c user/fat_unicode.c user/lib.c user/malloc.c third_party/fatfs/ff.c -o $(BUILD)/fs.elf

$(BUILD)/mkinitrd: tools/mkinitrd.c | $(BUILD)
	cc tools/mkinitrd.c -o $(BUILD)/mkinitrd

$(BUILD)/apps.fat: user_apps | $(BUILD)
	rm -f $(BUILD)/apps.fat
	dd if=/dev/zero of=$(BUILD)/apps.fat bs=1M count=4
	mkfs.fat -F 12 $(BUILD)/apps.fat
	mcopy -i $(BUILD)/apps.fat $(USER_ELFS) ::/

$(BUILD)/initrd.bin: $(BUILD)/mkinitrd user_apps $(BUILD)/apps.fat | $(BUILD)
	$(BUILD)/mkinitrd $(BUILD)/initrd.bin \
		$(BUILD)/shell.elf \
		$(BUILD)/uart.elf \
		$(BUILD)/keyboard.elf \
		$(BUILD)/ns.elf \
		$(BUILD)/fs.elf \
		$(BUILD)/pong.elf \
		$(BUILD)/fault.elf \
		$(BUILD)/spin.elf \
		$(BUILD)/badptr.elf \
		$(BUILD)/stackgrow.elf \
		$(BUILD)/memshare.elf \
		$(BUILD)/memxfer.elf \
		$(BUILD)/memrevoke.elf \
		$(BUILD)/ipcfast.elf \
		$(BUILD)/ipccap.elf \
		$(BUILD)/ipckill.elf \
		$(BUILD)/speed.elf \
		$(BUILD)/speedipc.elf \
		$(BUILD)/apps.fat

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

image: $(TARGET) $(BUILD)/initrd.bin
	@echo "Creating FAT32 image..."
	dd if=/dev/zero of=$(BUILD)/fat.img bs=1M count=64
	mkfs.fat -F 32 $(BUILD)/fat.img
	mmd -i $(BUILD)/fat.img ::/EFI
	mmd -i $(BUILD)/fat.img ::/EFI/BOOT
	mcopy -i $(BUILD)/fat.img $(TARGET) ::/EFI/BOOT/BOOTAA64.EFI
	mcopy -i $(BUILD)/fat.img $(BUILD)/initrd.bin ::/initrd.bin
	@echo "Image created: $(BUILD)/fat.img"

run: image
	qemu-system-aarch64 \
		-M virt \
		-cpu $(QEMU_CPU) \
		-m $(QEMU_MEM) \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-drive file=$(BUILD)/fat.img,format=raw,if=virtio \
		-nographic

run_g: image
	qemu-system-aarch64 \
		-M virt \
		-cpu $(QEMU_CPU) \
		-m $(QEMU_MEM) \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-drive file=$(BUILD)/fat.img,format=raw,if=virtio

runwin: image
	mingw-w64-clang-aarch64-qemu \
		-M virt,accel=whpx \
		-cpu $(QEMU_WIN_CPU) \
		-m $(QEMU_MEM) \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-drive file=$(BUILD)/fat.img,format=raw,if=virtio

clean:
	rm -rf $(BUILD) user/*_elf.h
