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
    // GICv3+ fields omitted for brevity as v2 only uses up to here
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

static int parse_madt(ACPI_MADT *madt) {
    LOG_INFO_HEX("ACPI: Parsing MADT at: ", madt);

    uint8_t *ptr = (uint8_t *)madt + sizeof(ACPI_MADT);
    uint8_t *end = (uint8_t *)madt + madt->Header.Length;

    while (ptr < end) {
        ACPI_SUBTABLE_HEADER *sub = (ACPI_SUBTABLE_HEADER *)ptr;
        
        if (sub->Type == MADT_TYPE_GIC_DISTRIBUTOR) {
            ACPI_MADT_GICD *gicd = (ACPI_MADT_GICD *)sub;
            gicd_base = gicd->PhysicalBaseAddress;
            LOG_INFO_HEX("ACPI: Found GIC Distributor Base: ", gicd_base);
        } 
        else if (sub->Type == MADT_TYPE_GIC_CPU_INTERFACE) {
            ACPI_MADT_GICC *gicc = (ACPI_MADT_GICC *)sub;
            // The GICC base address is the same for all cores (as it's banked), 
            // so we can just read the first one we find.
            if (gicc_base == 0 && gicc->PhysicalBaseAddress != 0) {
                gicc_base = gicc->PhysicalBaseAddress;
                LOG_INFO_HEX("ACPI: Found GIC CPU Interface Base: ", gicc_base);
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
    
    ACPI_RSDP *rsdp = (ACPI_RSDP *)rsdp_ptr;
    LOG_INFO_HEX("ACPI: Found RSDP at: ", rsdp);

    // XSDT has 64-bit pointers. RSDT has 32-bit pointers. UEFI standard uses XSDT.
    if (rsdp->XsdtAddress == 0) {
        LOG_FAIL("ACPI: No XSDT found (only legacy RSDT). Not supported.");
        return -1;
    }

    ACPI_HEADER *xsdt = (ACPI_HEADER *)rsdp->XsdtAddress;
    LOG_INFO_HEX("ACPI: Parsing XSDT at: ", xsdt);

    int entries = (xsdt->Length - sizeof(ACPI_HEADER)) / 8;
    uint64_t *pointers = (uint64_t *)((uint8_t *)xsdt + sizeof(ACPI_HEADER));

    for (int i = 0; i < entries; i++) {
        ACPI_HEADER *header = (ACPI_HEADER *)pointers[i];
        if (header->Signature[0] == 'A' && header->Signature[1] == 'P' &&
            header->Signature[2] == 'I' && header->Signature[3] == 'C') {
            return parse_madt((ACPI_MADT *)header);
        }
    }

    LOG_FAIL("ACPI: Failed to find MADT table in XSDT.");
    return -1;
}

uint64_t acpi_get_gicd_base(void) {
    return gicd_base;
}

uint64_t acpi_get_gicc_base(void) {
    return gicc_base;
}
