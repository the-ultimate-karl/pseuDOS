#ifndef PSEUDOS_EFI_H
#define PSEUDOS_EFI_H

#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef uint16_t  CHAR16;
typedef uint8_t   CHAR8;
typedef uint8_t   BOOLEAN;
typedef void      VOID;

typedef UINT64    EFI_STATUS;
typedef VOID*     EFI_HANDLE;
typedef VOID*     EFI_EVENT;
typedef UINT64    EFI_LBA;
typedef UINT64    EFI_TPL;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#define EFI_SUCCESS               0ULL
#define EFI_LOAD_ERROR            (1ULL | (1ULL << 63))
#define EFI_INVALID_PARAMETER     (2ULL | (1ULL << 63))
#define EFI_UNSUPPORTED           (3ULL | (1ULL << 63))
#define EFI_BAD_BUFFER_SIZE       (4ULL | (1ULL << 63))
#define EFI_BUFFER_TOO_SMALL      (5ULL | (1ULL << 63))
#define EFI_NOT_READY             (6ULL | (1ULL << 63))
#define EFI_DEVICE_ERROR          (7ULL | (1ULL << 63))
#define EFI_WRITE_PROTECTED       (8ULL | (1ULL << 63))
#define EFI_OUT_OF_RESOURCES      (9ULL | (1ULL << 63))
#define EFI_VOLUME_CORRUPTED      (10ULL | (1ULL << 63))
#define EFI_VOLUME_FULL           (11ULL | (1ULL << 63))
#define EFI_NO_MEDIA              (12ULL | (1ULL << 63))
#define EFI_MEDIA_CHANGED         (13ULL | (1ULL << 63))
#define EFI_NOT_FOUND             (14ULL | (1ULL << 63))
#define EFI_ACCESS_DENIED         (15ULL | (1ULL << 63))
#define EFI_NO_RESPONSE           (16ULL | (1ULL << 63))
#define EFI_NO_MAPPING            (17ULL | (1ULL << 63))
#define EFI_TIMEOUT               (18ULL | (1ULL << 63))
#define EFI_NOT_STARTED           (19ULL | (1ULL << 63))
#define EFI_ALREADY_STARTED       (20ULL | (1ULL << 63))
#define EFI_ABORTED               (21ULL | (1ULL << 63))
#define EFI_ICMP_ERROR            (22ULL | (1ULL << 63))
#define EFI_TFTP_ERROR            (23ULL | (1ULL << 63))
#define EFI_PROTOCOL_ERROR        (24ULL | (1ULL << 63))
#define EFI_INCOMPATIBLE_VERSION  (25ULL | (1ULL << 63))
#define EFI_SECURITY_VIOLATION    (26ULL | (1ULL << 63))
#define EFI_CRC_ERROR             (27ULL | (1ULL << 63))

#define EFI_ERROR(status) (((INT64)(status)) < 0)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* Forward declarations */
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct _EFI_BOOT_SERVICES;
struct _EFI_RUNTIME_SERVICES;

/* Simple Text Output Protocol */
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    BOOLEAN ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    const CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    const CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINT64 ModeNumber,
    UINT64 *Columns,
    UINT64 *Rows
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINT64 ModeNumber
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINT64 Attribute
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINT64 Column,
    UINT64 Row
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    BOOLEAN Visible
);

typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET                Reset;
    EFI_TEXT_STRING               OutputString;
    EFI_TEXT_TEST_STRING          TestString;
    EFI_TEXT_QUERY_MODE           QueryMode;
    EFI_TEXT_SET_MODE             SetMode;
    EFI_TEXT_SET_ATTRIBUTE        SetAttribute;
    EFI_TEXT_CLEAR_SCREEN         ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION  SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR        EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE       *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Memory types */
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

typedef struct {
    UINT32               Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* Graphics Output Protocol */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                               MaxMode;
    UINT32                               Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINT64                               SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                 FrameBufferBase;
    UINT64                               FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber,
    UINT64 *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber
);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    VOID                                    *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Boot Services */
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    UINT64 Pages,
    EFI_PHYSICAL_ADDRESS *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory,
    UINT64 Pages
);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINT64 *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINT64 *MapKey,
    UINT64 *DescriptorSize,
    UINT32 *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType,
    UINT64 Size,
    VOID **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    VOID *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    EFI_GUID *Protocol,
    VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    VOID *Registration,
    VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    UINT64 MapKey
);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINT64 Timeout,
    UINT64 WatchdogCode,
    UINT64 DataSize,
    CHAR16 *WatchdogData
);

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER        Hdr;
    VOID                    *RaiseTPL;
    VOID                    *RestoreTPL;
    EFI_ALLOCATE_PAGES      AllocatePages;
    EFI_FREE_PAGES          FreePages;
    EFI_GET_MEMORY_MAP      GetMemoryMap;
    EFI_ALLOCATE_POOL       AllocatePool;
    EFI_FREE_POOL           FreePool;
    VOID                    *CreateEvent;
    VOID                    *SetTimer;
    VOID                    *WaitForEvent;
    VOID                    *SignalEvent;
    VOID                    *CloseEvent;
    VOID                    *CheckEvent;
    VOID                    *InstallProtocolInterface;
    VOID                    *ReinstallProtocolInterface;
    VOID                    *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL     HandleProtocol;
    VOID                    *Void;
    VOID                    *RegisterProtocolNotify;
    VOID                    *LocateHandle;
    VOID                    *LocateDevicePath;
    VOID                    *InstallConfigurationTable;
    VOID                    *LoadImage;
    VOID                    *StartImage;
    VOID                    *Exit;
    VOID                    *UnloadImage;
    EFI_EXIT_BOOT_SERVICES  ExitBootServices;
    VOID                    *GetNextMonotonicCount;
    VOID                    *Stall;
    EFI_SET_WATCHDOG_TIMER  SetWatchdogTimer;
    VOID                    *ConnectController;
    VOID                    *DisconnectController;
    VOID                    *OpenProtocol;
    VOID                    *CloseProtocol;
    VOID                    *OpenProtocolInformation;
    VOID                    *ProtocolsPerHandle;
    VOID                    *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL     LocateProtocol;
} EFI_BOOT_SERVICES;

/* Runtime Services */
typedef EFI_STATUS (EFIAPI *EFI_RESET_SYSTEM)(
    UINT32 ResetType,
    EFI_STATUS ResetStatus,
    UINT64 DataSize,
    VOID *ResetData
);

typedef struct _EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER    Hdr;
    VOID                *GetTime;
    VOID                *SetTime;
    VOID                *GetWakeupTime;
    VOID                *SetWakeupTime;
    VOID                *SetVirtualAddressMap;
    VOID                *ConvertPointer;
    VOID                *GetVariable;
    VOID                *GetNextVariableName;
    VOID                *SetVariable;
    VOID                *GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM    ResetSystem;
} EFI_RUNTIME_SERVICES;

/* Configuration Table */
typedef struct {
    EFI_GUID VendorGuid;
    VOID     *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* System Table */
typedef struct {
    EFI_TABLE_HEADER                Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                          FirmwareRevision;
    EFI_HANDLE                      ConsoleInHandle;
    VOID                            *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                      StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINT64                          NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* PSEUDOS_EFI_H */
