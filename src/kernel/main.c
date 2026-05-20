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
#include "elf.h"
#include "initrd.h"

#define UART_MMIO_PA 0x09000000ULL
#define USER_UART_MMIO_VA 0xB0000000ULL

#define PANIC(msg)                                                             \
  do {                                                                         \
    SystemTable->ConOut->OutputString(SystemTable->ConOut,                     \
                                      (CHAR16 *)L"PANIC: ");                   \
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16 *)msg);     \
    while (1)                                                                  \
      ;                                                                        \
  } while (0)

#define INITRD_MAX_SIZE (2 * 1024 * 1024)
static uint8_t initrd_storage[INITRD_MAX_SIZE];

void neptune_kmain(void *memory_map, uint64_t map_size, uint64_t desc_size,
                   void *rsdp_ptr, void *initrd_data, uint64_t initrd_size) {
  uart_init();
  uart_puts("\n========================================\n");
  uart_puts("    Neptune Microkernel (AArch64)\n");
  uart_puts("========================================\n");
  LOG_OK("Serial Console Initialized.");
  LOG_INFO("BootServices Exited. Kernel in full control.");
  LOG_INFO_HEX("neptune_kmain loaded at: ", (uint64_t)neptune_kmain);
  uint64_t current_el;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
  current_el = (current_el >> 2) & 0x3;
  LOG_INFO_HEX("Current Exception Level (EL): ", current_el);

  if (current_el != 1) {
    LOG_WARN("Not in EL1!");
  }

  if (initrd_init(initrd_data, initrd_size) < 0) {
    LOG_FAIL("Cannot continue without initrd.");
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  exceptions_init();
  LOG_OK("Exceptions Initialized (VBAR_EL1 set).");

  LOG_INFO("Enabling MMU...");
  mmu_init();
  LOG_OK("MMU is active. We are running with Virtual Memory mapped!");

  LOG_INFO("Starting Physical Memory Manager (PMM)...");
  pmm_init(memory_map, map_size, desc_size);

  LOG_INFO("Starting Frame Object Manager...");
  if (frame_init() < 0) {
    LOG_FAIL("Cannot continue without frame objects.");
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  LOG_INFO("Starting Virtual Memory Manager (VMM)...");
  vmm_init();

  LOG_INFO("Starting Page Cache...");
  page_cache_init();

  LOG_INFO("Starting VM Object Manager...");
  vm_object_init();

  LOG_INFO("Starting Kernel Heap (kmalloc)...");
  kmalloc_init();
  typedef struct {
    uint64_t a;
    uint64_t b;
  } TestStruct;

  TestStruct *ts = (TestStruct *)kmalloc(sizeof(TestStruct));
  if (ts) {
    ts->a = 0x1111222233334444ULL;
    ts->b = 0xAAAABBBBCCCCDDDDULL;
    LOG_OK_HEX("Allocated kmalloc struct at VA: ", ts);
    LOG_INFO_HEX("Struct Field A: ", ts->a);
    LOG_INFO_HEX("Struct Field B: ", ts->b);
    LOG_OK("kmalloc test PASSED!");
  } else {
    LOG_FAIL("kmalloc test FAILED!");
  }

  LOG_INFO("Starting ACPI Parsing for Hardware Discovery...");
  if (acpi_init(rsdp_ptr) < 0) {
    LOG_FAIL("Failed to parse ACPI tables!");
  }

  LOG_INFO("Starting Generic Interrupt Controller (GICv2)...");
  gic_init();

  LOG_INFO("Starting Task Scheduler...");
  sched_init();

  // Extract these fucking user service ELF blobs and instance them inside Ring 3.
  // TID assignment order matters for bootstrap: name server=1, don't fuck this up!

  const uint8_t *ns_elf = 0;
  uint64_t ns_elf_len = 0;
  if (initrd_find("ns.elf", &ns_elf, &ns_elf_len) < 0) {
    LOG_FAIL("INITRD: Missing ns.elf.");
    while (1) __asm__ volatile("wfi");
  }

  LOG_INFO_HEX("Creating Name Server ELF Blob Size: ", ns_elf_len);
  sched_create_user_task(ns_elf, ns_elf_len, 5);

  const uint8_t *uart_elf = 0;
  uint64_t uart_elf_len = 0;
  if (initrd_find("uart.elf", &uart_elf, &uart_elf_len) < 0) {
    LOG_FAIL("INITRD: Missing uart.elf.");
    while (1) __asm__ volatile("wfi");
  }

  LOG_INFO_HEX("Creating UART Server ELF Blob Size: ", uart_elf_len);
  int uart_tid = sched_create_user_task(uart_elf, uart_elf_len, 5);

  // Grant the UART server EL0 access to the PL011 UART MMIO page.
  uint64_t *uart_pgd = sched_get_task_pgd(uart_tid);
  uint16_t uart_asid = sched_get_task_asid(uart_tid);
  if (uart_pgd) {
    if (vmm_map_page_asid(uart_pgd, uart_asid, USER_UART_MMIO_VA, UART_MMIO_PA,
                          VMM_FLAG_USER_DEVICE) < 0) {
      LOG_FAIL("Failed to map UART MMIO page for UART server.");
    }
  }

  const uint8_t *keyboard_elf = 0;
  uint64_t keyboard_elf_len = 0;
  if (initrd_find("keyboard.elf", &keyboard_elf, &keyboard_elf_len) < 0) {
    LOG_FAIL("INITRD: Missing keyboard.elf.");
    while (1) __asm__ volatile("wfi");
  }

  LOG_INFO_HEX("Creating Keyboard Server ELF Blob Size: ", keyboard_elf_len);
  int kb_tid = sched_create_user_task(keyboard_elf, keyboard_elf_len, 5);

  // Grant the Keyboard server EL0 access to the PL011 UART MMIO page.
  uint64_t *kb_pgd = sched_get_task_pgd(kb_tid);
  uint16_t kb_asid = sched_get_task_asid(kb_tid);
  if (kb_pgd) {
    if (vmm_map_page_asid(kb_pgd, kb_asid, USER_UART_MMIO_VA, UART_MMIO_PA,
                          VMM_FLAG_USER_DEVICE) < 0) {
      LOG_FAIL("Failed to map UART MMIO page for keyboard server.");
    }
  }

  const uint8_t *fs_elf = 0;
  uint64_t fs_elf_len = 0;
  if (initrd_find("fs.elf", &fs_elf, &fs_elf_len) < 0) {
    LOG_FAIL("INITRD: Missing fs.elf.");
    while (1) __asm__ volatile("wfi");
  }

  LOG_INFO_HEX("Creating FS Server ELF Blob Size: ", fs_elf_len);
  int fs_tid = sched_create_user_task(fs_elf, fs_elf_len, 5);
  if (fs_tid < 0) {
    LOG_FAIL("Failed to create FS server.");
    while (1) __asm__ volatile("wfi");
  }

  uint64_t initrd_count = initrd_file_count();
  for (uint32_t i = 0; i < initrd_count; i++) {
    if (sched_install_exec_cap_at((uint32_t)fs_tid,
                                  FS_BOOT_EXEC_CAP_BASE + i,
                                  i) < 0) {
      LOG_FAIL("Failed to bootstrap FS executable capability.");
      while (1) __asm__ volatile("wfi");
    }
    if (sched_install_file_cap_at((uint32_t)fs_tid,
                                  FS_BOOT_FILE_CAP_BASE + i,
                                  i) < 0) {
      LOG_FAIL("Failed to bootstrap FS file capability.");
      while (1) __asm__ volatile("wfi");
    }
  }

  const uint8_t *shell_elf = 0;
  uint64_t shell_elf_len = 0;
  if (initrd_find("shell.elf", &shell_elf, &shell_elf_len) < 0) {
    LOG_FAIL("INITRD: Missing shell.elf.");
    while (1) __asm__ volatile("wfi");
  }

  LOG_INFO_HEX("Creating Shell ELF Blob Size: ", shell_elf_len);
  sched_create_user_task(shell_elf, shell_elf_len, 10);

  LOG_INFO("Starting ARM Generic Timer (10Hz)...");
  timer_init();

  LOG_OK("Entering Idle Loop (Task 0)...");
  while (1) {
    __asm__ volatile("wfi");
  }
}

static EFI_STATUS load_initrd(EFI_HANDLE ImageHandle,
                              EFI_SYSTEM_TABLE *SystemTable,
                              void *buffer,
                              uint64_t *buffer_size) {
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

  EFI_FILE_PROTOCOL *root = 0;
  status = fs->OpenVolume(fs, &root);
  if (status != EFI_SUCCESS) {
    return status;
  }

  EFI_FILE_PROTOCOL *file = 0;
  status = root->Open(root, &file, (CHAR16 *)L"initrd.bin",
                      EFI_FILE_MODE_READ, 0);
  if (status != EFI_SUCCESS) {
    root->Close(root);
    return status;
  }

  status = file->Read(file, buffer_size, buffer);
  file->Close(file);
  root->Close(root);
  return status;
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

  uint64_t initrd_size = INITRD_MAX_SIZE;
  EFI_STATUS initrd_status =
      load_initrd(ImageHandle, SystemTable, initrd_storage, &initrd_size);
  if (initrd_status != EFI_SUCCESS) {
    PANIC(L"Failed to load initrd.bin!\r\n");
  }
  uint64_t MemoryMapSize = 0;
  EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
  uint64_t MapKey = 0;
  uint64_t DescriptorSize = 0;
  uint32_t DescriptorVersion = 0;
  EFI_STATUS STATUS = SystemTable->BootServices->GetMemoryMap(
      &MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

  // Normally we should allocate `MemoryMapSize + buffer` to ensure it fucking fits
  // after alloc changes the memory map. Let's allocate a static buffer for
  // simplicity because we are fucking lazy.
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
    // A fucking common issue: some shit triggered a timer and changed the memory map
    // between GetMemoryMap and ExitBootServices.
    // Try one more time.
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
                initrd_storage, initrd_size);

  return EFI_SUCCESS;
}
