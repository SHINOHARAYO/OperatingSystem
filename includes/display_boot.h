#pragma once

#include <stdint.h>

#define DISPLAY_BOOT_MAGIC 0x594C5053U
#define DISPLAY_BOOT_VERSION 1U

#define DISPLAY_PIXEL_RGB_RESERVED_8BIT 0U
#define DISPLAY_PIXEL_BGR_RESERVED_8BIT 1U
#define DISPLAY_PIXEL_BIT_MASK 2U

#define DISPLAY_FRAMEBUFFER_VA 0x100000000ULL
#define DISPLAY_BOOT_INFO_VA   0x110000000ULL
#define DISPLAY_GPU_MMIO_VA    0xB0040000ULL

#define DISPLAY_GPU_TRANSPORT_NONE       0U
#define DISPLAY_GPU_TRANSPORT_PCI_MODERN 2U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scan_line;
    uint32_t pixel_format;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
    uint64_t framebuffer_pa;
    uint64_t framebuffer_size;
    uint64_t framebuffer_va;
    uint32_t gpu_present;
    uint32_t gpu_transport;
    uint64_t gpu_bar_va;
    uint64_t gpu_bar_size;
    uint32_t gpu_common_off;
    uint32_t gpu_notify_off;
    uint32_t gpu_isr_off;
    uint32_t gpu_device_off;
    uint32_t gpu_notify_multiplier;
    uint32_t gpu_pci_segment;
    uint32_t gpu_pci_bdf;
    uint32_t reserved2;
} display_boot_info_t;
