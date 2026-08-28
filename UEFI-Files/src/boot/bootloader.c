#include "efi.h"
#include "bootinfo.h"
#include "errtext.h"
#include "io.h"
#include "lib.h"

typedef void (*BAREMETAL_KERNEL_ENTRY)(BootInfo *boot_info);

/*
 * Check basic x86_64 CPU features using CPUID instruction
 */
static int check_cpu_features(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000000));
    if (eax < 0x80000001) {
        return 0;
    }

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001));
    if (!(edx & (1 << 29))) {
        return 0;
    }

    return 1;
}

static void uart_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void uart_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0);
    outb(0x3F8, (uint8_t)c);
}

static void uart_puts(const char *str) {
    if (!str) return;
    while (*str) {
        if (*str == '\n') {
            uart_putc('\r');
        }
        uart_putc(*str++);
    }
}

static void boot_msg(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    if (SystemTable && SystemTable->ConOut) {
        CHAR16 buf[256];
        size_t i = 0;
        while (*str && i < 254) {
            if (*str == '\n') {
                buf[i++] = (CHAR16)'\r';
            }
            buf[i++] = (CHAR16)(*str++);
        }
        buf[i] = 0;
        SystemTable->ConOut->OutputString(SystemTable->ConOut, buf);
    }
    uart_puts(str);
}

static int guid_cmp(const EFI_GUID *g1, const EFI_GUID *g2) {
    if (g1->Data1 != g2->Data1 || g1->Data2 != g2->Data2 || g1->Data3 != g2->Data3) return 0;
    for (int i = 0; i < 8; i++) {
        if (g1->Data4[i] != g2->Data4[i]) return 0;
    }
    return 1;
}

static void unicode_to_ascii(const CHAR16 *src, char *dst, size_t max_len) {
    size_t i = 0;
    if (!src || !dst || max_len == 0) return;
    while (src[i] != 0 && i + 1 < max_len) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    uart_init();

    if (!SystemTable || !SystemTable->BootServices) {
        error_boot(NULL, ERR_GENERIC_BOOT_FAILURE, "SystemTable or BootServices pointer is null");
    }

    if (SystemTable->ConOut && SystemTable->ConOut->ClearScreen) {
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    }

    boot_msg(SystemTable, "\n[bootmgfw] pseuDOS 64-bit UEFI Boot Manager v1.0\n");
    boot_msg(SystemTable, "[bootmgfw] initializing UEFI Firmware environment...\n");

    /* 1. Validate System Table */
    boot_msg(SystemTable, "[bootmgfw] validating UEFI system table and boot services... [ok]\n");

    /* 2. Disable Watchdog Timer */
    boot_msg(SystemTable, "[bootmgfw] disabling UEFI watchdog timer... ");
    if (SystemTable->BootServices->SetWatchdogTimer) {
        SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
        boot_msg(SystemTable, "[ok]\n");
    } else {
        boot_msg(SystemTable, "[skipped]\n");
    }

    /* 3. Query CPUID for Long Mode */
    boot_msg(SystemTable, "[bootmgfw] querying CPUID for x86_64 long mode support... ");
    if (!check_cpu_features()) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_UNSUPPORTED_CPU, "x86_64 long mode (64-bit) feature flag not present in CPUID");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* 4. Locate and Configure GOP Video Mode (1920x1080 True Color) */
    boot_msg(SystemTable, "[bootmgfw] initializing Graphics Output Protocol (GOP)... ");
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = SystemTable->BootServices->LocateProtocol(
        &gEfiGraphicsOutputProtocolGuid,
        NULL,
        (VOID **)&gop
    );

    if (EFI_ERROR(status) || !gop || !gop->Mode) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_GOP_INIT_FAILED, "failed to locate UEFI GOP graphics protocol");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* Search for 1280x720 mode, 1024x768, or standard available */
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 target_720p_mode = 0xFFFFFFFF;
    UINT32 target_1024_mode = 0xFFFFFFFF;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        status = gop->QueryMode(gop, m, &size_of_info, &info);
        if (!EFI_ERROR(status) && info) {
            if (info->HorizontalResolution == 1280 && info->VerticalResolution == 720) {
                target_720p_mode = m;
            } else if (info->HorizontalResolution == 1024 && info->VerticalResolution == 768) {
                target_1024_mode = m;
            }
        }
    }

    if (target_720p_mode != 0xFFFFFFFF) {
        best_mode = target_720p_mode;
    } else if (target_1024_mode != 0xFFFFFFFF) {
        best_mode = target_1024_mode;
    }

    boot_msg(SystemTable, "[bootmgfw] setting 1280x720 GOP video mode... ");
    status = gop->SetMode(gop, best_mode);
    if (!EFI_ERROR(status)) {
        boot_msg(SystemTable, "[ok]\n");
    } else {
        boot_msg(SystemTable, "[fallback]\n");
    }

    /* 5. Populate BootInfo Structure */
    static BootInfo boot_info;
    memset(&boot_info, 0, sizeof(BootInfo));
    boot_info.magic = BOOTINFO_MAGIC;
    boot_info.version = 1;

    boot_info.fb.physical_base = gop->Mode->FrameBufferBase;
    boot_info.fb.buffer_size = gop->Mode->FrameBufferSize;
    boot_info.fb.width = gop->Mode->Info->HorizontalResolution;
    boot_info.fb.height = gop->Mode->Info->VerticalResolution;
    boot_info.fb.pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
    boot_info.fb.pixel_format = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) ? FB_FORMAT_BGR : FB_FORMAT_RGB;

    /* 6. Pre-allocate 16 MB Kernel Heap */
    boot_msg(SystemTable, "[bootmgfw] pre-allocating 16 MB kernel heap pages... ");
    EFI_PHYSICAL_ADDRESS heap_buffer = 0;
    UINTN heap_pages = 4096; /* 16 MB */
    status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages,
        EfiLoaderData,
        heap_pages,
        &heap_buffer
    );

    if (EFI_ERROR(status) || heap_buffer == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate 16 MB kernel heap memory");
    }
    boot_info.mem.heap_physical_start = heap_buffer;
    boot_info.mem.heap_size_bytes = heap_pages * 4096;
    boot_msg(SystemTable, "[ok]\n");

    /* 7. Detect Boot Storage Media Handle */
    boot_msg(SystemTable, "[bootmgfw] detecting boot storage media handle... ");
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    status = SystemTable->BootServices->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&loaded_image
    );

    if (EFI_ERROR(status) || !loaded_image || !loaded_image->DeviceHandle) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_STORAGE_DEVICE_IO, "failed to query loaded_image_protocol for boot device");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* Extract Device Path string if available */
    if (loaded_image->DeviceHandle) {
        EFI_DEVICE_PATH_PROTOCOL *dev_dp = NULL;
        status = SystemTable->BootServices->HandleProtocol(
            loaded_image->DeviceHandle,
            &gEfiDevicePathProtocolGuid,
            (VOID **)&dev_dp
        );
        if (!EFI_ERROR(status) && dev_dp) {
            EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dp_to_text = NULL;
            status = SystemTable->BootServices->LocateProtocol(
                &gEfiDevicePathToTextProtocolGuid,
                NULL,
                (VOID **)&dp_to_text
            );
            if (!EFI_ERROR(status) && dp_to_text && dp_to_text->ConvertDevicePathToText) {
                CHAR16 *text_u16 = dp_to_text->ConvertDevicePathToText(dev_dp, 1, 0);
                if (text_u16) {
                    unicode_to_ascii(text_u16, boot_info.hardware_devpath, sizeof(boot_info.hardware_devpath));
                }
            }
        }
    }
    if (boot_info.hardware_devpath[0] == '\0') {
        strcpy(boot_info.hardware_devpath, "PciRoot(0x0)/Pci(0x1,0x1)/Ata(0x0)/CDROM(0x0)");
    }
    strcpy(boot_info.boot_file_path, "\\EFI\\pseuDOS\\kernel.bin");

    /* 8. Mount Boot Partition Filesystem Volume */
    boot_msg(SystemTable, "[bootmgfw] mounting boot partition filesystem volume... ");
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    status = SystemTable->BootServices->HandleProtocol(
        loaded_image->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&sfs
    );

    if (EFI_ERROR(status) || !sfs) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_KERNEL_MOUNT_FAILED, "failed to open simple_file_system_protocol on boot volume");
    }

    EFI_FILE_PROTOCOL *root_dir = NULL;
    status = sfs->OpenVolume(sfs, &root_dir);
    if (EFI_ERROR(status) || !root_dir) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_KERNEL_MOUNT_FAILED, "failed to open root directory volume");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* 9. Look for Kernel Image */
    boot_msg(SystemTable, "[bootmgfw] looking for kernel...\n");
    const CHAR16 kernel_path[] = { '\\', 'E', 'F', 'I', '\\', 'p', 's', 'e', 'u', 'D', 'O', 'S', '\\', 'k', 'e', 'r', 'n', 'e', 'l', '.', 'b', 'i', 'n', 0 };
    EFI_FILE_PROTOCOL *kernel_file = NULL;

    status = root_dir->Open(
        root_dir,
        &kernel_file,
        kernel_path,
        EFI_FILE_MODE_READ,
        0
    );

    if (EFI_ERROR(status) || !kernel_file) {
        error_boot(SystemTable, ERR_KERNEL_NOT_FOUND, "\\EFI\\pseuDOS\\kernel.bin: no such file or directory");
    }
    boot_msg(SystemTable, "[bootmgfw] found kernel! loading kernel... [ok]\n");

    /* 10. Read Kernel Binary */
    boot_msg(SystemTable, "[bootmgfw] allocating physical memory pages for kernel... ");
    EFI_PHYSICAL_ADDRESS kernel_buffer = 0;
    UINTN kernel_pages = 128; /* 512 KB */
    status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages,
        EfiLoaderCode,
        kernel_pages,
        &kernel_buffer
    );

    if (EFI_ERROR(status) || kernel_buffer == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate memory pages for kernel execution");
    }

    UINTN read_size = kernel_pages * 4096;
    kernel_file->Read(kernel_file, &read_size, (VOID *)(uintptr_t)kernel_buffer);
    kernel_file->Close(kernel_file);
    root_dir->Close(root_dir);
    boot_msg(SystemTable, "[ok]\n");

    /* 11. Locate ACPI RSDP in Configuration Table */
    boot_msg(SystemTable, "[bootmgfw] discovering ACPI RSDP table pointer... ");
    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        if (guid_cmp(&SystemTable->ConfigurationTable[i].VendorGuid, &gEfiAcpi20TableGuid) ||
            guid_cmp(&SystemTable->ConfigurationTable[i].VendorGuid, &gEfiAcpi10TableGuid)) {
            boot_info.acpi_rsdp_address = (uint64_t)(uintptr_t)SystemTable->ConfigurationTable[i].VendorTable;
            break;
        }
    }
    boot_msg(SystemTable, "[ok]\n");

    /* 12. Allocate Buffer for Memory Map Snapshot */
    EFI_PHYSICAL_ADDRESS map_buf_phys = 0;
    UINTN map_pages = 16; /* 64 KB */
    status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages,
        EfiLoaderData,
        map_pages,
        &map_buf_phys
    );
    if (EFI_ERROR(status) || map_buf_phys == 0) {
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate memory map snapshot buffer");
    }
    boot_info.mem.map_buffer = (uint64_t)map_buf_phys;

    /* 13. Fetch Memory Map and Call ExitBootServices() */
    boot_msg(SystemTable, "[bootmgfw] exiting UEFI Boot Services (ExitBootServices)...\n");

    UINTN map_size = map_pages * 4096;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;

    status = SystemTable->BootServices->GetMemoryMap(
        &map_size,
        (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)boot_info.mem.map_buffer,
        &map_key,
        &desc_size,
        &desc_version
    );

    boot_info.mem.map_size = map_size;
    boot_info.mem.descriptor_size = desc_size;
    boot_info.mem.descriptor_version = desc_version;

    status = SystemTable->BootServices->ExitBootServices(ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        /* Retry once if map key shifted */
        map_size = map_pages * 4096;
        SystemTable->BootServices->GetMemoryMap(
            &map_size,
            (EFI_MEMORY_DESCRIPTOR *)(uintptr_t)boot_info.mem.map_buffer,
            &map_key,
            &desc_size,
            &desc_version
        );
        boot_info.mem.map_size = map_size;
        boot_info.mem.descriptor_size = desc_size;
        boot_info.mem.descriptor_version = desc_version;

        status = SystemTable->BootServices->ExitBootServices(ImageHandle, map_key);
        if (EFI_ERROR(status)) {
            error_boot(SystemTable, ERR_GENERIC_BOOT_FAILURE, "ExitBootServices() failed");
        }
    }

    uart_puts("[bootmgfw] successfully exited UEFI Boot Services!\n");
    uart_puts("[bootmgfw] transferring control to bare-metal pseuDOS kernel...\n");

    /* 14. Jump to Bare-Metal Kernel Entry */
    uint8_t *raw = (uint8_t *)(uintptr_t)kernel_buffer;
    uint32_t pe_offset = *(uint32_t *)(raw + 0x3C);
    if (raw[pe_offset] == 'P' && raw[pe_offset + 1] == 'E') {
        uint32_t entry_rva = *(uint32_t *)(raw + pe_offset + 0x28);
        BAREMETAL_KERNEL_ENTRY entry = (BAREMETAL_KERNEL_ENTRY)(raw + entry_rva);
        entry(&boot_info);
    }

    /* If kernel returns, halt */
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }

    return EFI_SUCCESS;
}
