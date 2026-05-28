#pragma once

#include <stdint.h>

#define BLOCK_BOOT_MAGIC 0x4B4C4256U
#define BLOCK_BOOT_VERSION 1U

#define BLOCK_TRANSPORT_NONE 0U
#define BLOCK_TRANSPORT_MMIO 1U
#define BLOCK_TRANSPORT_PCI_MODERN 2U

#define BLOCK_DEVICE_MMIO_VA 0xB0010000ULL
#define BLOCK_BOOT_INFO_VA   0xB0020000ULL
#define BLOCK_DEVICE_MMIO_SIZE 0x4000ULL

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t transport;
    uint32_t reserved;
    uint64_t bar_va;
    uint64_t bar_size;
    uint32_t common_off;
    uint32_t notify_off;
    uint32_t isr_off;
    uint32_t device_off;
    uint32_t notify_multiplier;
    uint32_t pci_segment;
    uint32_t pci_bdf;
    uint32_t reserved2;
} block_boot_info_t;
