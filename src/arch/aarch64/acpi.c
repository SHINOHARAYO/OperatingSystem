#include "acpi.h"
#include "log.h"
#include "uart.h"

typedef struct {
    char Signature[4];
    uint32_t Length;
    uint8_t Revision;
    uint8_t Checksum;
    char OEMID[6];
    char OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} __attribute__((packed)) ACPI_HEADER;

typedef struct {
    char Signature[8];
    uint8_t Checksum;
    char OEMID[6];
    uint8_t Revision;
    uint32_t RsdtAddress;
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t ExtendedChecksum;
    uint8_t reserved[3];
} __attribute__((packed)) ACPI_RSDP;

typedef struct {
    ACPI_HEADER Header;
    uint32_t LocalInterruptControllerAddress;
    uint32_t Flags;
} __attribute__((packed)) ACPI_MADT;

typedef struct {
    ACPI_HEADER Header;
    uint64_t Reserved;
} __attribute__((packed)) ACPI_MCFG;

typedef struct {
    uint64_t BaseAddress;
    uint16_t SegmentGroup;
    uint8_t StartBus;
    uint8_t EndBus;
    uint32_t Reserved;
} __attribute__((packed)) ACPI_MCFG_ALLOCATION;

typedef struct {
    uint8_t Type;
    uint8_t Length;
} __attribute__((packed)) ACPI_SUBTABLE_HEADER;

#define MADT_TYPE_GIC_CPU_INTERFACE 11
#define MADT_TYPE_GIC_DISTRIBUTOR   12

typedef struct {
    ACPI_SUBTABLE_HEADER Header;
    uint16_t Reserved;
    uint32_t CPUInterfaceNumber;
    uint32_t ProcessorUID;
    uint32_t Flags;
    uint32_t ParkingProtocolVersion;
    uint32_t PerformanceInterruptGsiv;
    uint64_t ParkedAddress;
    uint64_t PhysicalBaseAddress;
    uint64_t GICV;
    uint64_t GICH;
    uint32_t VGICMaintenanceInterrupt;
    uint64_t GICRBaseAddress;
    uint64_t MPIDR;
    uint8_t ProcessorPowerEfficiencyClass;
    uint8_t Reserved2;
    uint16_t SPEOverflowInterrupt;
} __attribute__((packed)) ACPI_MADT_GICC;

typedef struct {
    ACPI_SUBTABLE_HEADER Header;
    uint16_t Reserved;
    uint32_t GicID;
    uint64_t PhysicalBaseAddress;
    uint32_t SystemVectorBase;
    uint8_t GicVersion;
    uint8_t Reserved2[3];
} __attribute__((packed)) ACPI_MADT_GICD;

static uint64_t gicd_base = 0;
static uint64_t gicc_base = 0;
static acpi_virtio_blk_info_t virtio_blk_info;
static uint64_t cpu_mpidrs[8];
static uint32_t cpu_count = 0;

#define PCI_VENDOR_ID              0x00
#define PCI_DEVICE_ID              0x02
#define PCI_COMMAND                0x04
#define PCI_STATUS                 0x06
#define PCI_HEADER_TYPE            0x0E
#define PCI_CAPABILITY_POINTER     0x34
#define PCI_BAR0                   0x10
#define PCI_STATUS_CAP_LIST        (1U << 4)
#define PCI_COMMAND_MEMORY         (1U << 1)
#define PCI_COMMAND_BUS_MASTER     (1U << 2)
#define PCI_CAP_ID_VENDOR_SPECIFIC 0x09
#define PCI_VENDOR_VIRTIO          0x1AF4
#define PCI_DEVICE_VIRTIO_BLK      0x1042
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

static int signature_is(const ACPI_HEADER *header, const char *sig) {
    return header->Signature[0] == sig[0] && header->Signature[1] == sig[1] &&
           header->Signature[2] == sig[2] && header->Signature[3] == sig[3];
}

static volatile uint8_t *pci_cfg_ptr(const ACPI_MCFG_ALLOCATION *alloc,
                                     uint8_t bus, uint8_t device,
                                     uint8_t function, uint16_t offset) {
    uint64_t bus_offset = (uint64_t)(bus - alloc->StartBus) << 20;
    uint64_t dev_offset = (uint64_t)device << 15;
    uint64_t fn_offset = (uint64_t)function << 12;
    return (volatile uint8_t *)(alloc->BaseAddress + bus_offset + dev_offset +
                                fn_offset + offset);
}

static uint8_t pci_read8(const ACPI_MCFG_ALLOCATION *alloc, uint8_t bus,
                         uint8_t device, uint8_t function, uint16_t offset) {
    return *pci_cfg_ptr(alloc, bus, device, function, offset);
}

static uint16_t pci_read16(const ACPI_MCFG_ALLOCATION *alloc, uint8_t bus,
                           uint8_t device, uint8_t function, uint16_t offset) {
    return *(volatile uint16_t *)pci_cfg_ptr(alloc, bus, device, function, offset);
}

static uint32_t pci_read32(const ACPI_MCFG_ALLOCATION *alloc, uint8_t bus,
                           uint8_t device, uint8_t function, uint16_t offset) {
    return *(volatile uint32_t *)pci_cfg_ptr(alloc, bus, device, function, offset);
}

static void pci_write16(const ACPI_MCFG_ALLOCATION *alloc, uint8_t bus,
                        uint8_t device, uint8_t function, uint16_t offset,
                        uint16_t value) {
    *(volatile uint16_t *)pci_cfg_ptr(alloc, bus, device, function, offset) = value;
}

static uint64_t pci_bar_address(const ACPI_MCFG_ALLOCATION *alloc, uint8_t bus,
                                uint8_t device, uint8_t function, uint8_t bar) {
    if (bar >= 6) {
        return 0;
    }

    uint16_t off = PCI_BAR0 + ((uint16_t)bar * 4);
    uint32_t lo = pci_read32(alloc, bus, device, function, off);
    if (lo & 0x1) {
        return (uint64_t)(lo & ~0x3U);
    }

    uint64_t addr = lo & ~0xFULL;
    uint32_t type = (lo >> 1) & 0x3;
    if (type == 0x2 && bar < 5) {
        uint32_t hi = pci_read32(alloc, bus, device, function, off + 4);
        addr |= (uint64_t)hi << 32;
    }
    return addr;
}

static uint64_t round_up_page(uint64_t value) {
    return (value + 4095ULL) & ~4095ULL;
}

static int parse_virtio_pci_caps(const ACPI_MCFG_ALLOCATION *alloc, uint8_t bus,
                                 uint8_t device, uint8_t function) {
    uint16_t status = pci_read16(alloc, bus, device, function, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        return -1;
    }

    uint8_t cap = pci_read8(alloc, bus, device, function, PCI_CAPABILITY_POINTER) & ~0x3U;
    uint8_t common_bar = 0xFF;
    uint8_t notify_bar = 0xFF;
    uint8_t isr_bar = 0xFF;
    uint8_t device_bar = 0xFF;
    uint32_t common_off = 0;
    uint32_t notify_off = 0;
    uint32_t isr_off = 0;
    uint32_t device_off = 0;
    uint32_t notify_multiplier = 0;
    uint64_t required_size = 0;

    for (uint32_t guard = 0; cap && guard < 48; guard++) {
        uint8_t cap_id = pci_read8(alloc, bus, device, function, cap);
        uint8_t next = pci_read8(alloc, bus, device, function, cap + 1) & ~0x3U;
        uint8_t cap_len = pci_read8(alloc, bus, device, function, cap + 2);

        if (cap_id == PCI_CAP_ID_VENDOR_SPECIFIC && cap_len >= 16) {
            uint8_t cfg_type = pci_read8(alloc, bus, device, function, cap + 3);
            uint8_t bar = pci_read8(alloc, bus, device, function, cap + 4);
            uint32_t offset = pci_read32(alloc, bus, device, function, cap + 8);
            uint32_t length = pci_read32(alloc, bus, device, function, cap + 12);
            uint64_t end = (uint64_t)offset + (length ? length : 1);

            if (end > required_size) {
                required_size = end;
            }

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                common_bar = bar;
                common_off = offset;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && cap_len >= 20) {
                notify_bar = bar;
                notify_off = offset;
                notify_multiplier = pci_read32(alloc, bus, device, function, cap + 16);
            } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
                isr_bar = bar;
                isr_off = offset;
            } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                device_bar = bar;
                device_off = offset;
            }
        }

        cap = next;
    }

    if (common_bar == 0xFF || notify_bar == 0xFF ||
        isr_bar == 0xFF || device_bar == 0xFF) {
        return -1;
    }
    if (common_bar != notify_bar || common_bar != isr_bar ||
        common_bar != device_bar) {
        LOG_WARN("ACPI: virtio-pci block uses split BARs; unsupported in this build.");
        return -1;
    }

    uint64_t bar_pa = pci_bar_address(alloc, bus, device, function, common_bar);
    if (!bar_pa) {
        return -1;
    }

    uint16_t command = pci_read16(alloc, bus, device, function, PCI_COMMAND);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    pci_write16(alloc, bus, device, function, PCI_COMMAND, command);

    virtio_blk_info.present = 1;
    virtio_blk_info.segment = alloc->SegmentGroup;
    virtio_blk_info.bus = bus;
    virtio_blk_info.device = device;
    virtio_blk_info.function = function;
    virtio_blk_info.bar_pa = bar_pa;
    virtio_blk_info.bar_size = round_up_page(required_size ? required_size : 4096);
    virtio_blk_info.common_off = common_off;
    virtio_blk_info.notify_off = notify_off;
    virtio_blk_info.isr_off = isr_off;
    virtio_blk_info.device_off = device_off;
    virtio_blk_info.notify_multiplier = notify_multiplier;

    LOG_OK("ACPI: Discovered modern virtio-blk PCI device.");
    LOG_DEBUG_HEX("ACPI: virtio-blk BAR PA: ", virtio_blk_info.bar_pa);
    return 0;
}

static void discover_pci_device(const ACPI_MCFG_ALLOCATION *alloc, uint8_t bus,
                                uint8_t device, uint8_t function) {
    if (virtio_blk_info.present) {
        return;
    }

    uint16_t vendor = pci_read16(alloc, bus, device, function, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) {
        return;
    }

    uint16_t device_id = pci_read16(alloc, bus, device, function, PCI_DEVICE_ID);
    if (vendor == PCI_VENDOR_VIRTIO && device_id == PCI_DEVICE_VIRTIO_BLK) {
        parse_virtio_pci_caps(alloc, bus, device, function);
    }
}

static void parse_mcfg(ACPI_MCFG *mcfg) {
    LOG_DEBUG_HEX("ACPI: Parsing MCFG at: ", mcfg);

    uint8_t *ptr = (uint8_t *)mcfg + sizeof(ACPI_MCFG);
    uint8_t *end = (uint8_t *)mcfg + mcfg->Header.Length;

    while (ptr + sizeof(ACPI_MCFG_ALLOCATION) <= end) {
        ACPI_MCFG_ALLOCATION *alloc = (ACPI_MCFG_ALLOCATION *)ptr;
        LOG_DEBUG_HEX("ACPI: PCI ECAM base: ", alloc->BaseAddress);

        for (uint16_t bus = alloc->StartBus; bus <= alloc->EndBus && !virtio_blk_info.present; bus++) {
            for (uint8_t dev = 0; dev < 32 && !virtio_blk_info.present; dev++) {
                uint16_t vendor = pci_read16(alloc, (uint8_t)bus, dev, 0, PCI_VENDOR_ID);
                if (vendor == 0xFFFF) {
                    continue;
                }

                uint8_t header = pci_read8(alloc, (uint8_t)bus, dev, 0, PCI_HEADER_TYPE);
                uint8_t functions = (header & 0x80) ? 8 : 1;
                for (uint8_t fn = 0; fn < functions && !virtio_blk_info.present; fn++) {
                    discover_pci_device(alloc, (uint8_t)bus, dev, fn);
                }
            }
        }

        ptr += sizeof(ACPI_MCFG_ALLOCATION);
    }

    if (!virtio_blk_info.present) {
        LOG_WARN("ACPI: No modern virtio-blk PCI device found.");
    }
}

static int parse_madt(ACPI_MADT *madt) {
    LOG_DEBUG_HEX("ACPI: Parsing MADT at: ", madt);

    uint8_t *ptr = (uint8_t *)madt + sizeof(ACPI_MADT);
    uint8_t *end = (uint8_t *)madt + madt->Header.Length;

    while (ptr < end) {
        ACPI_SUBTABLE_HEADER *sub = (ACPI_SUBTABLE_HEADER *)ptr;
        if (sub->Length == 0) {
            break;
        }
        
        if (sub->Type == MADT_TYPE_GIC_DISTRIBUTOR) {
            ACPI_MADT_GICD *gicd = (ACPI_MADT_GICD *)sub;
            gicd_base = gicd->PhysicalBaseAddress;
            LOG_DEBUG_HEX("ACPI: Found GIC Distributor Base: ", gicd_base);
        } 
        else if (sub->Type == MADT_TYPE_GIC_CPU_INTERFACE) {
            ACPI_MADT_GICC *gicc = (ACPI_MADT_GICC *)sub;
            if (gicc_base == 0 && gicc->PhysicalBaseAddress != 0) {
                gicc_base = gicc->PhysicalBaseAddress;
                LOG_DEBUG_HEX("ACPI: Found GIC CPU Interface Base: ", gicc_base);
            }
            if ((gicc->Flags & 1U) && cpu_count < 8) {
                uint64_t mpidr = 0;
                if (sub->Length >= 76) {
                    mpidr = gicc->MPIDR;
                } else {
                    mpidr = gicc->CPUInterfaceNumber;
                }
                cpu_mpidrs[cpu_count++] = mpidr;
            }
        }

        ptr += sub->Length;
    }

    if (gicd_base != 0 && gicc_base != 0) {
        LOG_OK("ACPI: Discovered GIC hardware successfully.");
        return 0;
    } else {
        LOG_FAIL("ACPI: Missing GIC Distributor or CPU Interface in MADT.");
        return -1;
    }
}

int acpi_init(void *rsdp_ptr) {
    if (!rsdp_ptr) return -1;
    virtio_blk_info.present = 0;
    cpu_count = 0;
    
    ACPI_RSDP *rsdp = (ACPI_RSDP *)rsdp_ptr;
    LOG_DEBUG_HEX("ACPI: Found RSDP at: ", rsdp);

    // XSDT has 64-bit pointers. RSDT has 32-bit pointers. UEFI standard uses XSDT.
    if (rsdp->XsdtAddress == 0) {
        LOG_FAIL("ACPI: No XSDT found (only legacy RSDT). Not supported.");
        return -1;
    }

    ACPI_HEADER *xsdt = (ACPI_HEADER *)rsdp->XsdtAddress;
    LOG_DEBUG_HEX("ACPI: Parsing XSDT at: ", xsdt);

    int entries = (xsdt->Length - sizeof(ACPI_HEADER)) / 8;
    uint64_t *pointers = (uint64_t *)((uint8_t *)xsdt + sizeof(ACPI_HEADER));

    int madt_status = -1;
    for (int i = 0; i < entries; i++) {
        ACPI_HEADER *header = (ACPI_HEADER *)pointers[i];
        if (signature_is(header, "APIC")) {
            madt_status = parse_madt((ACPI_MADT *)header);
        } else if (signature_is(header, "MCFG")) {
            parse_mcfg((ACPI_MCFG *)header);
        }
    }

    if (madt_status < 0) {
        LOG_FAIL("ACPI: Failed to find MADT table in XSDT.");
        return -1;
    }
    return 0;
}

uint64_t acpi_get_gicd_base(void) {
    return gicd_base;
}

uint64_t acpi_get_gicc_base(void) {
    return gicc_base;
}

uint32_t acpi_get_cpu_count(void) {
    return cpu_count;
}

uint64_t acpi_get_cpu_mpidr(uint32_t index) {
    if (index >= cpu_count) {
        return 0;
    }
    return cpu_mpidrs[index];
}

int acpi_get_virtio_blk_info(acpi_virtio_blk_info_t *out) {
    if (!out || !virtio_blk_info.present) {
        return -1;
    }
    *out = virtio_blk_info;
    return 0;
}
