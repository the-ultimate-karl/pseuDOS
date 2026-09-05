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

    /* Switch ConOut to matching high-resolution text mode for 1280x720 */
    if (SystemTable->ConOut && SystemTable->ConOut->QueryMode && SystemTable->ConOut->SetMode && SystemTable->ConOut->Mode) {
        INT32 best_text_mode = SystemTable->ConOut->Mode->Mode;
        UINTN max_text_cols = 0;
        for (INT32 tm = 0; tm < SystemTable->ConOut->Mode->MaxMode; tm++) {
            UINTN cols = 0, rows = 0;
            if (!EFI_ERROR(SystemTable->ConOut->QueryMode(SystemTable->ConOut, (UINTN)tm, &cols, &rows))) {
                if (cols > max_text_cols) {
                    max_text_cols = cols;
                    best_text_mode = tm;
                }
            }
        }
        if (max_text_cols > 0 && best_text_mode != SystemTable->ConOut->Mode->Mode) {
            SystemTable->ConOut->SetMode(SystemTable->ConOut, (UINTN)best_text_mode);
        }
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
        strcpy(boot_info.hardware_devpath, "Unknown / Direct Boot");
    }
    boot_info.boot_file_path[0] = '\0';

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

    /* 9. Check for Boot Manager Configuration */
    const CHAR16 bootcfg_path1[] = { '\\', 'p', 'r', 'o', 't', 'e', 'c', 't', 'e', 'd', '\\', 'b', 'o', 'o', 't', 'm', 'g', 'r', '\\', 'b', 'o', 'o', 't', '.', 'c', 'f', 'g', 0 };
    const CHAR16 bootcfg_path2[] = { '\\', 'p', 'r', 'o', 't', 'e', 'c', 't', '\\', 'b', 'o', 'o', 't', 'm', 'g', 'r', '\\', 'b', 'o', 'o', 't', '.', 'c', 'f', 'g', 0 };
    EFI_FILE_PROTOCOL *cfg_file = NULL;
    status = root_dir->Open(root_dir, &cfg_file, bootcfg_path1, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !cfg_file) {
        status = root_dir->Open(root_dir, &cfg_file, bootcfg_path2, EFI_FILE_MODE_READ, 0);
    }
    if (!EFI_ERROR(status) && cfg_file) {
        boot_msg(SystemTable, "[bootmgfw] reading boot configuration \\protected\\bootmgr\\boot.cfg... [ok]\n");
        cfg_file->Close(cfg_file);
    }

    /* 10. Look for Kernel Image */
    boot_msg(SystemTable, "[bootmgfw] looking for kernel image...\n");
    const CHAR16 kernel_path_win1[] = { '\\', 'p', 'r', 'o', 't', 'e', 'c', 't', 'e', 'd', '\\', 'k', 'r', 'n', 'l', '\\', 'k', 'e', 'r', 'n', 'e', 'l', '.', 'b', 'i', 'n', 0 };
    const CHAR16 kernel_path_win2[] = { '\\', 'p', 'r', 'o', 't', 'e', 'c', 't', '\\', 'k', 'r', 'n', 'l', '\\', 'k', 'e', 'r', 'n', 'e', 'l', '.', 'b', 'i', 'n', 0 };
    const CHAR16 kernel_path_legacy[] = { '\\', 'E', 'F', 'I', '\\', 'p', 's', 'e', 'u', 'D', 'O', 'S', '\\', 'k', 'e', 'r', 'n', 'e', 'l', '.', 'b', 'i', 'n', 0 };
    EFI_FILE_PROTOCOL *kernel_file = NULL;

    status = root_dir->Open(root_dir, &kernel_file, kernel_path_win1, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !kernel_file) {
        status = root_dir->Open(root_dir, &kernel_file, kernel_path_win2, EFI_FILE_MODE_READ, 0);
    }

    if (!EFI_ERROR(status) && kernel_file) {
        strcpy(boot_info.boot_file_path, "\\protected\\krnl\\kernel.bin");
        boot_msg(SystemTable, "[bootmgfw] found kernel at \\protected\\krnl\\kernel.bin [ok]\n");
    } else {
        status = root_dir->Open(root_dir, &kernel_file, kernel_path_legacy, EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status) && kernel_file) {
            strcpy(boot_info.boot_file_path, "\\EFI\\pseuDOS\\kernel.bin");
            boot_msg(SystemTable, "[bootmgfw] found kernel at \\EFI\\pseuDOS\\kernel.bin (fallback) [ok]\n");
        } else {
            error_boot(SystemTable, ERR_KERNEL_NOT_FOUND, "kernel image not found (checked \\protected\\krnl and \\EFI\\pseuDOS)");
        }
    }

    /* 11. Dynamically Query Kernel File Size */
    boot_msg(SystemTable, "[bootmgfw] querying kernel image file size... ");
    UINT8 info_buf[sizeof(EFI_FILE_INFO) + 256];
    UINTN info_size = sizeof(info_buf);
    UINT64 actual_file_size = 0;

    status = kernel_file->GetInfo(
        kernel_file,
        &gEfiFileInfoGuid,
        &info_size,
        info_buf
    );

    if (!EFI_ERROR(status)) {
        EFI_FILE_INFO *file_info = (EFI_FILE_INFO *)info_buf;
        actual_file_size = file_info->FileSize;
    }

    if (actual_file_size == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "kernel image file size is 0 bytes or unreadable");
    }

    if (actual_file_size < 512) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "kernel file size is too small to contain valid executable code");
    }

    UINTN kernel_file_pages = (actual_file_size + 4095) / 4096;
    if (kernel_file_pages < 16) kernel_file_pages = 16;
    boot_msg(SystemTable, "[ok]\n");

    /* 12. Read Kernel Binary into Temporary Buffer */
    boot_msg(SystemTable, "[bootmgfw] allocating memory for kernel loading... ");
    EFI_PHYSICAL_ADDRESS temp_file_buffer = 0;
    status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages,
        EfiLoaderData,
        kernel_file_pages,
        &temp_file_buffer
    );

    if (EFI_ERROR(status) || temp_file_buffer == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate memory pages for kernel loading");
    }

    UINTN read_size = actual_file_size;
    status = kernel_file->Read(kernel_file, &read_size, (VOID *)(uintptr_t)temp_file_buffer);
    kernel_file->Close(kernel_file);
    root_dir->Close(root_dir);

    if (EFI_ERROR(status)) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_STORAGE_DEVICE_IO, "failed to read kernel binary from storage media");
    }

    if (read_size < actual_file_size) {
        boot_msg(SystemTable, "[truncated]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "kernel binary truncated on storage media (read fewer bytes than file size)");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* 13. Map PE Sections and Allocate Execution Memory */
    boot_msg(SystemTable, "[bootmgfw] mapping kernel PE sections and allocating memory... ");
    EFI_PHYSICAL_ADDRESS kernel_buffer = 0;
    UINT8 *raw_file = (UINT8 *)(uintptr_t)temp_file_buffer;

    /* Validate DOS MZ Header */
    if (read_size < 0x40 || raw_file[0] != 'M' || raw_file[1] != 'Z') {
        boot_msg(SystemTable, "[invalid]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "invalid executable format: missing 'MZ' DOS header");
    }

    UINT32 pe_offset = *(UINT32 *)(raw_file + 0x3C);
    if (pe_offset + 0x100 > read_size ||
        raw_file[pe_offset] != 'P' || raw_file[pe_offset + 1] != 'E' ||
        raw_file[pe_offset + 2] != '\0' || raw_file[pe_offset + 3] != '\0') {
        boot_msg(SystemTable, "[corrupted]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "invalid PE signature: kernel executable header is corrupted");
    }

    UINT16 machine = *(UINT16 *)(raw_file + pe_offset + 4);
    if (machine != 0x8664) {
        boot_msg(SystemTable, "[unsupported]\n");
        error_boot(SystemTable, ERR_UNSUPPORTED_CPU, "kernel target machine architecture is not x86_64");
    }

    UINT16 num_sections = *(UINT16 *)(raw_file + pe_offset + 6);
    UINT16 opt_hdr_size = *(UINT16 *)(raw_file + pe_offset + 20);
    UINT32 opt_offset = pe_offset + 24;

    if (opt_offset + opt_hdr_size > read_size || num_sections == 0) {
        boot_msg(SystemTable, "[corrupted]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "corrupted PE optional header or zero sections");
    }

    UINT32 kernel_entry_rva = *(UINT32 *)(raw_file + opt_offset + 16);
    UINT32 size_of_image = *(UINT32 *)(raw_file + opt_offset + 56);
    UINT32 size_of_headers = *(UINT32 *)(raw_file + opt_offset + 60);

    if (size_of_image < 4096 || kernel_entry_rva == 0 || kernel_entry_rva >= size_of_image) {
        boot_msg(SystemTable, "[invalid]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "invalid kernel entry point RVA or SizeOfImage");
    }

    UINT32 sec_table_offset = opt_offset + opt_hdr_size;
    if (sec_table_offset + (UINT32)num_sections * 40 > read_size) {
        boot_msg(SystemTable, "[corrupted]\n");
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "PE section table extends past end of file");
    }

    UINTN kernel_image_pages = (size_of_image + 4095) / 4096;
    if (kernel_image_pages < 16) kernel_image_pages = 16;

    status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages,
        EfiLoaderCode,
        kernel_image_pages,
        &kernel_buffer
    );

    if (EFI_ERROR(status) || kernel_buffer == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate execution pages for kernel image");
    }

    /* Cleanly zero out all allocated pages (clears BSS and padding) */
    memset((void *)(uintptr_t)kernel_buffer, 0, kernel_image_pages * 4096);

    /* Copy PE headers */
    if (size_of_headers > read_size) size_of_headers = (UINT32)read_size;
    memcpy((void *)(uintptr_t)kernel_buffer, raw_file, size_of_headers);

    /* Copy each section to its virtual address */
    UINT8 *sec_hdr = raw_file + sec_table_offset;
    for (UINT16 i = 0; i < num_sections; i++) {
        UINT32 vaddr = *(UINT32 *)(sec_hdr + 12);
        UINT32 raw_size = *(UINT32 *)(sec_hdr + 16);
        UINT32 raw_ptr = *(UINT32 *)(sec_hdr + 20);
        if (raw_size > 0 && raw_ptr > 0) {
            if (raw_ptr + raw_size > read_size) {
                boot_msg(SystemTable, "[truncated]\n");
                error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "kernel section data extends past physical read bytes (truncated file)");
            }
            if (vaddr + raw_size > size_of_image) {
                boot_msg(SystemTable, "[corrupted]\n");
                error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "kernel section virtual address exceeds SizeOfImage");
            }
            memcpy((UINT8 *)(uintptr_t)kernel_buffer + vaddr, raw_file + raw_ptr, raw_size);
        }
        sec_hdr += 40;
    }

    boot_info.kernel_physical_base = kernel_buffer;
    boot_info.kernel_image_size = size_of_image;

    /* Free temporary load buffer */
    SystemTable->BootServices->FreePages(temp_file_buffer, kernel_file_pages);
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
            error_boot(SystemTable, ERR_EXIT_BOOT_SERVICES_FAILED, "memory map key mismatch on retry");
        }
    }

    uart_puts("[bootmgfw] successfully exited UEFI Boot Services!\n");
    uart_puts("[bootmgfw] transferring control to bare-metal pseuDOS kernel...\n");

    /* 14. Jump to Bare-Metal Kernel Entry */
    uint8_t *raw = (uint8_t *)(uintptr_t)kernel_buffer;
    if (kernel_entry_rva != 0) {
        BAREMETAL_KERNEL_ENTRY entry = (BAREMETAL_KERNEL_ENTRY)(raw + kernel_entry_rva);
        entry(&boot_info);
    } else {
        uint32_t pe_hdr_off = *(uint32_t *)(raw + 0x3C);
        if (raw[pe_hdr_off] == 'P' && raw[pe_hdr_off + 1] == 'E') {
            uint32_t rva = *(uint32_t *)(raw + pe_hdr_off + 0x28);
            BAREMETAL_KERNEL_ENTRY entry = (BAREMETAL_KERNEL_ENTRY)(raw + rva);
            entry(&boot_info);
        } else {
            BAREMETAL_KERNEL_ENTRY entry = (BAREMETAL_KERNEL_ENTRY)raw;
            entry(&boot_info);
        }
    }

    /* If kernel returns, halt */
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }

    return EFI_SUCCESS;
}
