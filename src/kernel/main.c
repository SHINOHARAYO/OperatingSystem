#include "efi.h"
#include "uart.h"
#include "exceptions.h"
#include "mmu.h"
#include "pmm.h"
#include "frame.h"
#include "vmm.h"
#include "vm_object.h"
#include "page_cache.h"
#include "kmalloc.h"
#include "log.h"
#include "gic.h"
#include "timer.h"
#include "acpi.h"
#include "sched.h"
#include "smp.h"
#include "elf.h"
#include "initrd.h"

#define UART_MMIO_PA 0x09000000ULL
#define USER_UART_MMIO_VA 0xB0000000ULL
#define USER_VIRTIO_BLK_MMIO_VA 0xB0010000ULL
#define VIRTIO_BLK_MMIO_PA 0x0A000000ULL
#define VIRTIO_BLK_MMIO_SIZE 0x4000ULL

#define PANIC(msg)                                                             \
  do {                                                                         \
    SystemTable->ConOut->OutputString(SystemTable->ConOut,                     \
                                      (CHAR16 *)L"PANIC: ");                   \
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16 *)msg);     \
    while (1)                                                                  \
      ;                                                                        \
  } while (0)

#define BOOT_ARCHIVE_MAX_SIZE (8 * 1024 * 1024)
#define BOOT_FILE_COUNT 4
static uint8_t boot_archive_storage[BOOT_ARCHIVE_MAX_SIZE] __attribute__((aligned(4096)));

void neptune_kmain(void *memory_map, uint64_t map_size, uint64_t desc_size,
                   void *rsdp_ptr, void *initrd_data, uint64_t initrd_size) {
  uart_init();
  uart_puts("\n========================================\n");
  uart_puts("    Neptune Microkernel (AArch64)\n");
  uart_puts("========================================\n");
  LOG_OK("Serial Console Initialized.");
  LOG_OK("BootServices exited.");
  LOG_DEBUG_HEX("neptune_kmain loaded at: ", (uint64_t)neptune_kmain);
  uint64_t current_el;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
  current_el = (current_el >> 2) & 0x3;
  LOG_DEBUG_HEX("Current Exception Level (EL): ", current_el);

  if (current_el != 1) {
    LOG_WARN("Not in EL1!");
  }

  if (initrd_init(initrd_data, initrd_size) < 0) {
    LOG_FAIL("Cannot continue without boot archive.");
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  exceptions_init();
  LOG_OK("Exceptions Initialized (VBAR_EL1 set).");

  LOG_DEBUG("Enabling MMU...");
  mmu_init();
  LOG_OK("MMU is active. We are running with Virtual Memory mapped!");

  LOG_DEBUG("Starting Physical Memory Manager (PMM)...");
  pmm_init(memory_map, map_size, desc_size);

  LOG_DEBUG("Starting Frame Object Manager...");
  if (frame_init() < 0) {
    LOG_FAIL("Cannot continue without frame objects.");
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  LOG_DEBUG("Starting Virtual Memory Manager (VMM)...");
  vmm_init();

  LOG_DEBUG("Starting Page Cache...");
  page_cache_init();

  LOG_DEBUG("Starting VM Object Manager...");
  vm_object_init();

  LOG_DEBUG("Starting Kernel Heap (kmalloc)...");
  kmalloc_init();
  typedef struct {
    uint64_t a;
    uint64_t b;
  } TestStruct;

  TestStruct *ts = (TestStruct *)kmalloc(sizeof(TestStruct));
  if (ts) {
    ts->a = 0x1111222233334444ULL;
    ts->b = 0xAAAABBBBCCCCDDDDULL;
    LOG_DEBUG_HEX("Allocated kmalloc struct at VA: ", ts);
    LOG_DEBUG_HEX("Struct Field A: ", ts->a);
    LOG_DEBUG_HEX("Struct Field B: ", ts->b);
    LOG_DEBUG("kmalloc test passed.");
  } else {
    LOG_FAIL("kmalloc test FAILED!");
  }

  LOG_DEBUG("Starting ACPI Parsing for Hardware Discovery...");
  if (acpi_init(rsdp_ptr) < 0) {
    LOG_FAIL("Failed to parse ACPI tables!");
  }

  LOG_DEBUG("Starting Generic Interrupt Controller (GICv2)...");
  gic_init();

  LOG_DEBUG("Starting Task Scheduler...");
  sched_init();

  LOG_DEBUG("Starting secondary cores...");
  smp_init();

  const uint8_t *init_elf = 0;
  uint64_t init_elf_len = 0;
  if (initrd_find("init.elf", &init_elf, &init_elf_len) < 0) {
    LOG_FAIL("BOOTFS: Missing init.elf.");
    while (1) __asm__ volatile("wfi");
  }

  LOG_DEBUG_HEX("Creating Init ELF Blob Size: ", init_elf_len);
  sched_create_user_task(init_elf, init_elf_len, 10);

  LOG_INFO("Starting ARM Generic Timer (1kHz)...");
  timer_init();

  LOG_OK("Entering Idle Loop (Task 0)...");
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void ascii_copy(char *dst, const char *src, uint64_t cap) {
  uint64_t i = 0;
  if (!dst || cap == 0) {
    return;
  }
  for (; i + 1 < cap && src && src[i]; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

static EFI_STATUS open_boot_volume(EFI_HANDLE ImageHandle,
                                   EFI_SYSTEM_TABLE *SystemTable,
                                   EFI_FILE_PROTOCOL **root_out) {
  EFI_GUID loaded_image_guid = {
      0x5b1b31a1,
      0x9562,
      0x11d2,
      {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
  EFI_GUID simple_fs_guid = {
      0x964e5b22,
      0x6459,
      0x11d2,
      {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
  if (!root_out) {
    return 1;
  }

  EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
  EFI_STATUS status = SystemTable->BootServices->HandleProtocol(
      ImageHandle, &loaded_image_guid, (void **)&loaded);
  if (status != EFI_SUCCESS) {
    return status;
  }

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
  status = SystemTable->BootServices->HandleProtocol(
      loaded->DeviceHandle, &simple_fs_guid, (void **)&fs);
  if (status != EFI_SUCCESS) {
    return status;
  }

  return fs->OpenVolume(fs, root_out);
}

static EFI_STATUS read_boot_file(EFI_FILE_PROTOCOL *root, CHAR16 *path,
                                 void *buffer, uint64_t *size) {
  if (!root || !path || !buffer || !size || *size == 0) {
    return 1;
  }

  EFI_FILE_PROTOCOL *file = 0;
  EFI_STATUS status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
  if (status != EFI_SUCCESS) {
    return status;
  }
  status = file->Read(file, size, buffer);
  file->Close(file);
  return status;
}

static EFI_STATUS load_boot_archive(EFI_HANDLE ImageHandle,
                                    EFI_SYSTEM_TABLE *SystemTable,
                                    void *buffer,
                                    uint64_t *buffer_size) {
  typedef struct {
    CHAR16 *path;
    const char *name;
  } boot_file_t;

  static boot_file_t files[BOOT_FILE_COUNT] = {
      {(CHAR16 *)L"\\boot\\init.elf", "init.elf"},
      {(CHAR16 *)L"\\boot\\ns.elf", "ns.elf"},
      {(CHAR16 *)L"\\boot\\block.elf", "block.elf"},
      {(CHAR16 *)L"\\boot\\fs.elf", "fs.elf"},
  };

  if (!buffer || !buffer_size || *buffer_size < sizeof(initrd_header_t) +
                                                BOOT_FILE_COUNT * sizeof(initrd_entry_t)) {
    return 1;
  }

  EFI_FILE_PROTOCOL *root = 0;
  EFI_STATUS status = open_boot_volume(ImageHandle, SystemTable, &root);
  if (status != EFI_SUCCESS) {
    return status;
  }

  initrd_header_t *header = (initrd_header_t *)buffer;
  initrd_entry_t *entries =
      (initrd_entry_t *)((uint8_t *)buffer + sizeof(initrd_header_t));
  uint64_t header_size = sizeof(initrd_header_t) +
                         BOOT_FILE_COUNT * sizeof(initrd_entry_t);
  uint64_t offset = header_size;

  for (uint32_t i = 0; i < BOOT_FILE_COUNT; i++) {
    if (offset >= *buffer_size) {
      root->Close(root);
      return 1;
    }
    uint64_t remaining = *buffer_size - offset;
    uint64_t read_size = remaining;
    status = read_boot_file(root, files[i].path, (uint8_t *)buffer + offset,
                            &read_size);
    if (status != EFI_SUCCESS || read_size == 0) {
      root->Close(root);
      return status != EFI_SUCCESS ? status : 1;
    }

    ascii_copy(entries[i].name, files[i].name, INITRD_NAME_LEN);
    entries[i].offset = offset;
    entries[i].size = read_size;
    offset += read_size;
  }

  root->Close(root);

  header->magic = INITRD_MAGIC;
  header->version = INITRD_VERSION;
  header->file_count = BOOT_FILE_COUNT;
  header->header_size = (uint32_t)header_size;
  *buffer_size = offset;
  return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  SystemTable->ConOut->Reset(SystemTable->ConOut, 0);
  SystemTable->ConOut->OutputString(
      SystemTable->ConOut, (CHAR16 *)L"UEFI Booting Neptune OS...\r\n");
  void *rsdp_ptr = NULL;
  EFI_GUID acpi2_guid = {0x8868e871,
                         0xe4f1,
                         0x11d3,
                         {0xbc, 0x22, 0x0, 0x80, 0xc7, 0x3c, 0x88, 0x81}};

  for (uint64_t i = 0; i < SystemTable->NumberOfTableEntries; i++) {
    EFI_GUID *g = &SystemTable->ConfigurationTable[i].VendorGuid;
    if (g->Data1 == acpi2_guid.Data1 && g->Data2 == acpi2_guid.Data2 &&
        g->Data3 == acpi2_guid.Data3 && g->Data4[0] == acpi2_guid.Data4[0] &&
        g->Data4[1] == acpi2_guid.Data4[1] &&
        g->Data4[2] == acpi2_guid.Data4[2] &&
        g->Data4[3] == acpi2_guid.Data4[3] &&
        g->Data4[4] == acpi2_guid.Data4[4] &&
        g->Data4[5] == acpi2_guid.Data4[5] &&
        g->Data4[6] == acpi2_guid.Data4[6] &&
        g->Data4[7] == acpi2_guid.Data4[7]) {
      rsdp_ptr = SystemTable->ConfigurationTable[i].VendorTable;
      break;
    }
  }

  if (!rsdp_ptr) {
    PANIC(L"Failed to find ACPI 2.0 RSDP!\r\n");
  }

  uint64_t boot_archive_size = BOOT_ARCHIVE_MAX_SIZE;
  EFI_STATUS boot_status =
      load_boot_archive(ImageHandle, SystemTable, boot_archive_storage,
                        &boot_archive_size);
  if (boot_status != EFI_SUCCESS) {
    PANIC(L"Failed to load /boot seed files!\r\n");
  }
  uint64_t MemoryMapSize = 0;
  EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
  uint64_t MapKey = 0;
  uint64_t DescriptorSize = 0;
  uint32_t DescriptorVersion = 0;
  EFI_STATUS STATUS = SystemTable->BootServices->GetMemoryMap(
      &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

  // Keep a static buffer large enough for the UEFI memory map snapshot.
  static uint8_t mmap_buf[8192];
  MemoryMapSize = sizeof(mmap_buf);
  MemoryMap = (EFI_MEMORY_DESCRIPTOR *)mmap_buf;

  STATUS = SystemTable->BootServices->GetMemoryMap(
      &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

  if (STATUS != EFI_SUCCESS) {
    PANIC(L"Failed to get memory map!\r\n");
  }
  STATUS = SystemTable->BootServices->ExitBootServices(ImageHandle, MapKey);
  if (STATUS != EFI_SUCCESS) {
    // Retry once in case the memory map changed before ExitBootServices.
    MemoryMapSize = sizeof(mmap_buf);
    STATUS = SystemTable->BootServices->GetMemoryMap(&MemoryMapSize, MemoryMap,
                                                     &MapKey, &DescriptorSize,
                                                     &DescriptorVersion);
    STATUS = SystemTable->BootServices->ExitBootServices(ImageHandle, MapKey);

    if (STATUS != EFI_SUCCESS) {
      PANIC(L"Failed to ExitBootServices!\r\n");
    }
  }
  neptune_kmain(MemoryMap, MemoryMapSize, DescriptorSize, rsdp_ptr,
                boot_archive_storage, boot_archive_size);

  return EFI_SUCCESS;
}
