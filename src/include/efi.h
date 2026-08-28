/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_EFI_H
#define GEMIBOOT_EFI_H 1

#include "types.h"

#if defined(_MSC_VER) || defined(__CYGWIN__) || defined(__MINGW32__)
#define EFIAPI __cdecl
#elif defined(__x86_64__) || defined(__amd64__)
#define EFIAPI __attribute__((ms_abi))
#elif defined(__i386__)
#define EFIAPI __attribute__((cdecl))
#else
#define EFIAPI
#endif

typedef void* efi_handle;
typedef uintptr_t efi_status;
typedef uint16_t utf16;

#define EFI_SUCCESS                 0
#define EFI_LOAD_ERROR              (1 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))
#define EFI_INVALID_PARAMETER       (2 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))
#define EFI_UNSUPPORTED             (3 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))
#define EFI_BAD_BUFFER_SIZE         (4 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))
#define EFI_BUFFER_TOO_SMALL        (5 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))
#define EFI_NOT_READY               (6 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))
#define EFI_DEVICE_ERROR            (7 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))
#define EFI_NOT_FOUND               (14 | ((uintptr_t)1 << (sizeof(uintptr_t)*8 - 1)))

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} efi_guid;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9B62, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Memory Types */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

#pragma pack(push, 1)
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;
#pragma pack(pop)

/* Graphics Output Protocol */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    uintptr_t SizeOfInfo;
    uint64_t FrameBufferBase;
    uintptr_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef efi_status (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    uint32_t ModeNumber,
    uintptr_t *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

typedef efi_status (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    uint32_t ModeNumber);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
};

/* File Protocol */
#define EFI_FILE_MODE_READ      0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE     0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE    0x8000000000000000ULL

#define EFI_FILE_READ_ONLY      0x0000000000000001ULL
#define EFI_FILE_HIDDEN         0x0000000000000002ULL
#define EFI_FILE_SYSTEM         0x0000000000000004ULL
#define EFI_FILE_RESERVED       0x0000000000000008ULL
#define EFI_FILE_DIRECTORY      0x0000000000000010ULL
#define EFI_FILE_ARCHIVE        0x0000000000000020ULL

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef efi_status (EFIAPI *EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL *This,
    EFI_FILE_PROTOCOL **NewHandle,
    utf16 *FileName,
    uint64_t OpenMode,
    uint64_t Attributes);

typedef efi_status (EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);
typedef efi_status (EFIAPI *EFI_FILE_DELETE)(EFI_FILE_PROTOCOL *This);
typedef efi_status (EFIAPI *EFI_FILE_READ)(EFI_FILE_PROTOCOL *This, uintptr_t *BufferSize, void *Buffer);
typedef efi_status (EFIAPI *EFI_FILE_WRITE)(EFI_FILE_PROTOCOL *This, uintptr_t *BufferSize, const void *Buffer);
typedef efi_status (EFIAPI *EFI_FILE_GET_POSITION)(EFI_FILE_PROTOCOL *This, uint64_t *Position);
typedef efi_status (EFIAPI *EFI_FILE_SET_POSITION)(EFI_FILE_PROTOCOL *This, uint64_t Position);
typedef efi_status (EFIAPI *EFI_FILE_GET_INFO)(EFI_FILE_PROTOCOL *This, const efi_guid *InformationType, uintptr_t *BufferSize, void *Buffer);
typedef efi_status (EFIAPI *EFI_FILE_SET_INFO)(EFI_FILE_PROTOCOL *This, const efi_guid *InformationType, uintptr_t BufferSize, const void *Buffer);
typedef efi_status (EFIAPI *EFI_FILE_FLUSH)(EFI_FILE_PROTOCOL *This);

struct _EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    EFI_FILE_DELETE       Delete;
    EFI_FILE_READ         Read;
    EFI_FILE_WRITE        Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    EFI_FILE_SET_INFO     SetInfo;
    EFI_FILE_FLUSH        Flush;
};

typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint8_t  CreateTime[16];
    uint8_t  LastAccessTime[16];
    uint8_t  ModificationTime[16];
    uint64_t Attribute;
    utf16    FileName[1];
} EFI_FILE_INFO;

/* Simple File System Protocol */
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef efi_status (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
};

/* Loaded Image Protocol */
typedef struct {
    uint32_t   Revision;
    efi_handle ParentHandle;
    void       *SystemTable;
    efi_handle DeviceHandle;
    void       *FilePath;
    void       *Reserved;
    uint32_t   ImageDataBase;
    uint64_t   ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void       *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* Simple Text Output Protocol */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef efi_status (EFIAPI *EFI_TEXT_RESET)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, bool ExtendedVerification);
typedef efi_status (EFIAPI *EFI_TEXT_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, const utf16 *String);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET  Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

/* Boot Services Table */
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef efi_status (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    uintptr_t Pages,
    uint64_t *Memory);

typedef efi_status (EFIAPI *EFI_FREE_PAGES)(
    uint64_t Memory,
    uintptr_t Pages);

typedef efi_status (EFIAPI *EFI_GET_MEMORY_MAP)(
    uintptr_t *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    uintptr_t *MapKey,
    uintptr_t *DescriptorSize,
    uint32_t *DescriptorVersion);

typedef efi_status (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType,
    uintptr_t Size,
    void **Buffer);

typedef efi_status (EFIAPI *EFI_FREE_POOL)(
    void *Buffer);

typedef efi_status (EFIAPI *EFI_HANDLE_PROTOCOL)(
    efi_handle Handle,
    const efi_guid *Protocol,
    void **Interface);

typedef efi_status (EFIAPI *EFI_LOCATE_PROTOCOL)(
    const efi_guid *Protocol,
    void *Registration,
    void **Interface);

typedef efi_status (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    efi_handle ImageHandle,
    uintptr_t MapKey);

typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef efi_status (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    EFI_LOCATE_SEARCH_TYPE SearchType,
    const efi_guid *Protocol,
    void *SearchKey,
    uintptr_t *NoHandles,
    efi_handle **Buffer);

typedef efi_status (EFIAPI *EFI_STALL)(
    uintptr_t Microseconds);

typedef struct {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    EFI_ALLOCATE_PAGES AllocatePages;
    EFI_FREE_PAGES     FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL  AllocatePool;
    EFI_FREE_POOL      FreePool;

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

    void *GetNextMonotonicCount;
    EFI_STALL Stall;
    void *SetWatchdogTimer;

    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    efi_guid VendorGuid;
    void     *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    utf16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    efi_handle ConsoleInHandle;
    void *ConIn;
    efi_handle ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    efi_handle StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uintptr_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* GEMIBOOT_EFI_H */
