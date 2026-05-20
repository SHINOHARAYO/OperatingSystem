#pragma once

#include <stdint.h>
#include <stddef.h>

#define EFI_SUCCESS 0
#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef uint16_t CHAR16;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef struct _EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (*EFI_TEXT_RESET)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    uint8_t ExtendedVerification
);

typedef EFI_STATUS (*EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String
);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET Reset;
    EFI_TEXT_STRING OutputString;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(
    uint64_t *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    uint64_t *MapKey,
    uint64_t *DescriptorSize,
    uint32_t *DescriptorVersion
);

typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    uint64_t MapKey
);

typedef EFI_STATUS (*EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    EFI_GUID *Protocol,
    void **Interface
);

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    void *AllocatePages;
    void *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
} EFI_BOOT_SERVICES;

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

struct _EFI_FILE_PROTOCOL;

typedef EFI_STATUS (*EFI_FILE_OPEN)(
    struct _EFI_FILE_PROTOCOL *This,
    struct _EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName,
    uint64_t OpenMode,
    uint64_t Attributes
);

typedef EFI_STATUS (*EFI_FILE_CLOSE)(struct _EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (*EFI_FILE_READ)(
    struct _EFI_FILE_PROTOCOL *This,
    uint64_t *BufferSize,
    void *Buffer
);

typedef struct _EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    void *Delete;
    EFI_FILE_READ Read;
} EFI_FILE_PROTOCOL;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (*OpenVolume)(
        struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
        EFI_FILE_PROTOCOL **Root
    );
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uint64_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;
