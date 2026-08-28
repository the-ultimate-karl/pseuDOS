#ifndef EFI_H
#define EFI_H

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
typedef uint64_t  UINTN;
typedef int64_t   INTN;
typedef uint16_t  CHAR16;
typedef uint8_t   CHAR8;
typedef uint8_t   BOOLEAN;
typedef void      VOID;

#ifndef TRUE
#define TRUE  1
#endif

#ifndef FALSE
#define FALSE 0
#endif

typedef UINT64    EFI_STATUS;
typedef VOID*     EFI_HANDLE;
typedef VOID*     EFI_EVENT;
typedef UINT64    EFI_LBA;
typedef UINT64    EFI_TPL;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#define EFI_SUCCESS                 0ULL
#define EFI_LOAD_ERROR              (1ULL  | (1ULL << 63))
#define EFI_INVALID_PARAMETER       (2ULL  | (1ULL << 63))
#define EFI_UNSUPPORTED             (3ULL  | (1ULL << 63))
#define EFI_BAD_BUFFER_SIZE         (4ULL  | (1ULL << 63))
#define EFI_BUFFER_TOO_SMALL        (5ULL  | (1ULL << 63))
#define EFI_NOT_READY               (6ULL  | (1ULL << 63))
#define EFI_DEVICE_ERROR            (7ULL  | (1ULL << 63))
#define EFI_WRITE_PROTECTED         (8ULL  | (1ULL << 63))
#define EFI_OUT_OF_RESOURCES        (9ULL  | (1ULL << 63))
#define EFI_VOLUME_CORRUPTED        (10ULL | (1ULL << 63))
#define EFI_VOLUME_FULL             (11ULL | (1ULL << 63))
#define EFI_NO_MEDIA                (12ULL | (1ULL << 63))
#define EFI_MEDIA_CHANGED           (13ULL | (1ULL << 63))
#define EFI_NOT_FOUND               (14ULL | (1ULL << 63))
#define EFI_ACCESS_DENIED           (15ULL | (1ULL << 63))
#define EFI_NO_RESPONSE             (16ULL | (1ULL << 63))
#define EFI_NO_MAPPING              (17ULL | (1ULL << 63))
#define EFI_TIMEOUT                 (18ULL | (1ULL << 63))
#define EFI_NOT_STARTED             (19ULL | (1ULL << 63))
#define EFI_ALREADY_STARTED         (20ULL | (1ULL << 63))
#define EFI_ABORTED                 (21ULL | (1ULL << 63))
#define EFI_SECURITY_VIOLATION      (26ULL | (1ULL << 63))

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
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct _EFI_BOOT_SERVICES;
struct _EFI_RUNTIME_SERVICES;
struct _EFI_DEVICE_PATH_PROTOCOL;
struct _EFI_FILE_PROTOCOL;
struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* Input Key */
typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    BOOLEAN ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    EFI_INPUT_KEY *Key
);

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET         Reset;
    EFI_INPUT_READ_KEY      ReadKeyStroke;
    EFI_EVENT               WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

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
    UINTN ModeNumber,
    UINTN *Columns,
    UINTN *Rows
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN ModeNumber
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN Attribute
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN Column,
    UINTN Row
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
    EFI_TEXT_RESET                  Reset;
    EFI_TEXT_STRING                 OutputString;
    EFI_TEXT_TEST_STRING            TestString;
    EFI_TEXT_QUERY_MODE             QueryMode;
    EFI_TEXT_SET_MODE               SetMode;
    EFI_TEXT_SET_ATTRIBUTE          SetAttribute;
    EFI_TEXT_CLEAR_SCREEN           ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION    SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR          EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE         *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

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

typedef struct {
    UINT32                  Type;
    EFI_PHYSICAL_ADDRESS    PhysicalStart;
    EFI_VIRTUAL_ADDRESS     VirtualStart;
    UINT64                  NumberOfPages;
    UINT64                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* Reset Types */
typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific
} EFI_RESET_TYPE;

typedef VOID (EFIAPI *EFI_RESET_SYSTEM)(
    EFI_RESET_TYPE ResetType,
    EFI_STATUS ResetStatus,
    UINTN DataSize,
    VOID *ResetData
);

typedef struct _EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER        Hdr;
    VOID                    *GetTime;
    VOID                    *SetTime;
    VOID                    *GetWakeupTime;
    VOID                    *SetWakeupTime;
    VOID                    *SetVirtualAddressMap;
    VOID                    *ConvertPointer;
    VOID                    *GetVariable;
    VOID                    *GetNextVariableName;
    VOID                    *SetVariable;
    VOID                    *GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM        ResetSystem;
} EFI_RUNTIME_SERVICES;

/* Device Path Protocol */
typedef struct _EFI_DEVICE_PATH_PROTOCOL {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define EFI_DEVICE_PATH_TYPE_HARDWARE 0x01
#define EFI_DEVICE_PATH_TYPE_ACPI     0x02
#define EFI_DEVICE_PATH_TYPE_MESSAGING 0x03
#define EFI_DEVICE_PATH_TYPE_MEDIA    0x04
#define EFI_DEVICE_PATH_TYPE_BIOS     0x05
#define EFI_DEVICE_PATH_TYPE_END      0x7F

/* Loaded Image Protocol */
typedef struct {
    UINT32                  Revision;
    EFI_HANDLE              ParentHandle;
    VOID                    *SystemTable;
    EFI_HANDLE              DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL *FilePath;
    VOID                    *Reserved;
    UINT32                  ImageLoadOptionsSize;
    VOID                    *ImageLoadOptions;
    VOID                    *ImageBase;
    UINT64                  ImageSize;
    EFI_MEMORY_TYPE         ImageCodeType;
    EFI_MEMORY_TYPE         ImageDataType;
    VOID                    *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* File Protocols */
#define EFI_FILE_MODE_READ      0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE     0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE    0x8000000000000000ULL

#define EFI_FILE_READ_ONLY      0x0000000000000001ULL
#define EFI_FILE_HIDDEN         0x0000000000000002ULL
#define EFI_FILE_SYSTEM         0x0000000000000004ULL
#define EFI_FILE_RESERVED       0x0000000000000008ULL
#define EFI_FILE_DIRECTORY      0x0000000000000010ULL
#define EFI_FILE_ARCHIVE        0x0000000000000020ULL

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT8  CreateTime[16];
    UINT8  LastAccessTime[16];
    UINT8  ModificationTime[16];
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(
        struct _EFI_FILE_PROTOCOL *This,
        struct _EFI_FILE_PROTOCOL **NewHandle,
        const CHAR16 *FileName,
        UINT64 OpenMode,
        UINT64 Attributes
    );
    EFI_STATUS (EFIAPI *Close)(struct _EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(struct _EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(
        struct _EFI_FILE_PROTOCOL *This,
        UINTN *BufferSize,
        VOID *Buffer
    );
    EFI_STATUS (EFIAPI *Write)(
        struct _EFI_FILE_PROTOCOL *This,
        UINTN *BufferSize,
        const VOID *Buffer
    );
    EFI_STATUS (EFIAPI *GetPosition)(struct _EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(struct _EFI_FILE_PROTOCOL *This, UINT64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(
        struct _EFI_FILE_PROTOCOL *This,
        const EFI_GUID *InformationType,
        UINTN *BufferSize,
        VOID *Buffer
    );
    EFI_STATUS (EFIAPI *SetInfo)(
        struct _EFI_FILE_PROTOCOL *This,
        const EFI_GUID *InformationType,
        UINTN BufferSize,
        const VOID *Buffer
    );
    EFI_STATUS (EFIAPI *Flush)(struct _EFI_FILE_PROTOCOL *This);
} EFI_FILE_PROTOCOL;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(
        struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
        EFI_FILE_PROTOCOL **Root
    );
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* Device Path To Text Protocol */
typedef CHAR16* (EFIAPI *EFI_DEVICE_PATH_TO_TEXT_NODE)(
    const EFI_DEVICE_PATH_PROTOCOL *DeviceNode,
    BOOLEAN DisplayOnly,
    BOOLEAN AllowShortcuts
);

typedef CHAR16* (EFIAPI *EFI_DEVICE_PATH_TO_TEXT_PATH)(
    const EFI_DEVICE_PATH_PROTOCOL *DevicePath,
    BOOLEAN DisplayOnly,
    BOOLEAN AllowShortcuts
);

typedef struct {
    EFI_DEVICE_PATH_TO_TEXT_NODE ConvertDeviceNodeToText;
    EFI_DEVICE_PATH_TO_TEXT_PATH ConvertDevicePathToText;
} EFI_DEVICE_PATH_TO_TEXT_PROTOCOL;

/* Boot Services function pointers */
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle,
    const EFI_GUID *Protocol,
    VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    const EFI_GUID *Protocol,
    VOID *Registration,
    VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory,
    UINTN Pages
);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType,
    UINTN Size,
    VOID **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    VOID *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(
    BOOLEAN BootPolicy,
    EFI_HANDLE ParentImageHandle,
    EFI_DEVICE_PATH_PROTOCOL *FilePath,
    VOID *SourceBuffer,
    UINTN SourceSize,
    EFI_HANDLE *ImageHandle
);

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_START)(
    EFI_HANDLE ImageHandle,
    UINTN *ExitDataSize,
    CHAR16 **ExitData
);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINTN Timeout,
    UINT64 WatchdogCode,
    UINTN DataSize,
    const CHAR16 *WatchdogData
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    UINTN MapKey
);

typedef struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER                Hdr;
    VOID                            *RaiseTPL;
    VOID                            *RestoreTPL;
    EFI_ALLOCATE_PAGES              AllocatePages;
    EFI_FREE_PAGES                  FreePages;
    EFI_GET_MEMORY_MAP              GetMemoryMap;
    EFI_ALLOCATE_POOL               AllocatePool;
    EFI_FREE_POOL                   FreePool;
    VOID                            *CreateEvent;
    VOID                            *SetTimer;
    VOID                            *WaitForEvent;
    VOID                            *SignalEvent;
    VOID                            *CloseEvent;
    VOID                            *CheckEvent;
    VOID                            *InstallProtocolInterface;
    VOID                            *ReinstallProtocolInterface;
    VOID                            *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL             HandleProtocol;
    VOID                            *VoidReserved;
    VOID                            *RegisterProtocolNotify;
    VOID                            *LocateHandle;
    VOID                            *LocateDevicePath;
    VOID                            *InstallConfigurationTable;
    EFI_IMAGE_LOAD                  LoadImage;
    EFI_IMAGE_START                 StartImage;
    VOID                            *Exit;
    VOID                            *UnloadImage;
    EFI_EXIT_BOOT_SERVICES          ExitBootServices;
    VOID                            *GetNextMonotonicCount;
    VOID                            *Stall;
    EFI_SET_WATCHDOG_TIMER          SetWatchdogTimer;
    VOID                            *ConnectController;
    VOID                            *DisconnectController;
    VOID                            *OpenProtocol;
    VOID                            *CloseProtocol;
    VOID                            *OpenProtocolInformation;
    VOID                            *ProtocolsPerHandle;
    VOID                            *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL             LocateProtocol;
    VOID                            *InstallMultipleProtocolInterfaces;
    VOID                            *UninstallMultipleProtocolInterfaces;
    VOID                            *CalculateCrc32;
    VOID                            *CopyMem;
    VOID                            *SetMem;
    VOID                            *CreateEventEx;
} EFI_BOOT_SERVICES;

/* System Table */
typedef struct {
    EFI_TABLE_HEADER                Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                          FirmwareRevision;
    EFI_HANDLE                      ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                      StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                           NumberOfTableEntries;
    VOID                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Protocol GUID Constants */
extern const EFI_GUID gEfiLoadedImageProtocolGuid;
extern const EFI_GUID gEfiDevicePathProtocolGuid;
extern const EFI_GUID gEfiDevicePathToTextProtocolGuid;
extern const EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern const EFI_GUID gEfiFileInfoGuid;

#endif /* EFI_H */
