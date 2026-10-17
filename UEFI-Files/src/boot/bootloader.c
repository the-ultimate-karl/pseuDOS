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

/*
 * Detect if running inside a virtual machine (QEMU, KVM, VMware, VirtualBox, Hyper-V, etc.)
 */
static int is_running_in_vm(void) {
    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 1: ECX bit 31 indicates hypervisor present */
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1));
    if (ecx & (1U << 31)) {
        return 1;
    }

    return 0;
}

static int g_boot_uart_checked = 0;
static int g_boot_uart_present = 0;

static int is_uart_present(void) {
    if (!g_boot_uart_checked) {
        /* Test scratch register with "hi lol" to detect UART presence */
        const char *sig = "hi lol";
        int ok = 1;
        for (int i = 0; sig[i] != '\0'; i++) {
            outb(0x3F8 + 7, (uint8_t)sig[i]);
            if (inb(0x3F8 + 7) != (uint8_t)sig[i]) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            g_boot_uart_present = 1;
            /* Initialize UART 115200 8N1 */
            outb(0x3F8 + 1, 0x00);
            outb(0x3F8 + 3, 0x80);
            outb(0x3F8 + 0, 0x01);
            outb(0x3F8 + 1, 0x00);
            outb(0x3F8 + 3, 0x03);
            outb(0x3F8 + 2, 0xC7);
            outb(0x3F8 + 4, 0x0B);
        }
        g_boot_uart_checked = 1;
    }
    return g_boot_uart_present;
}

static void uart_init(void) {
    (void)is_uart_present();
}

static void uart_putc(char c) {
    if (!is_uart_present()) return;
    int timeout = 50000;
    while ((inb(0x3F8 + 5) & 0x20) == 0 && --timeout > 0);
    if (timeout > 0) {
        outb(0x3F8, (uint8_t)c);
    }
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

static void ascii_to_unicode(const char *src, CHAR16 *dst, size_t max_len) {
    size_t i = 0;
    if (!src || !dst || max_len == 0) return;
    while (src[i] != '\0' && i + 1 < max_len) {
        dst[i] = (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

static void cmdline_append(char *dst, const char *src, size_t max_len) {
    if (!dst || !src || max_len == 0) return;
    size_t dlen = strlen(dst);
    if (dlen >= max_len - 1) return;
    if (dlen > 0 && dst[dlen - 1] != ' ') {
        dst[dlen++] = ' ';
        dst[dlen] = '\0';
    }
    while (*src && dlen < max_len - 1) {
        dst[dlen++] = *src++;
    }
    dst[dlen] = '\0';
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

    /* Search for optimal GOP video mode: 1080p, 720p, 800p (1280x800), 1024x768 */
    int in_vm = is_running_in_vm();
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 target_1080p_mode = 0xFFFFFFFF;
    UINT32 target_720p_mode = 0xFFFFFFFF;
    UINT32 target_800p_mode = 0xFFFFFFFF;
    UINT32 target_1024_mode = 0xFFFFFFFF;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        status = gop->QueryMode(gop, m, &size_of_info, &info);
        if (!EFI_ERROR(status) && info) {
            if (info->HorizontalResolution == 1920 && info->VerticalResolution == 1080) {
                target_1080p_mode = m;
            } else if (info->HorizontalResolution == 1280 && info->VerticalResolution == 720) {
                target_720p_mode = m;
            } else if (info->HorizontalResolution == 1280 && info->VerticalResolution == 800) {
                target_800p_mode = m;
            } else if (info->HorizontalResolution == 1024 && info->VerticalResolution == 768) {
                target_1024_mode = m;
            }
        }
    }

    if (!in_vm) {
        if (target_1080p_mode != 0xFFFFFFFF) best_mode = target_1080p_mode;
        else if (target_720p_mode != 0xFFFFFFFF) best_mode = target_720p_mode;
        else if (target_800p_mode != 0xFFFFFFFF) best_mode = target_800p_mode;
        else if (target_1024_mode != 0xFFFFFFFF) best_mode = target_1024_mode;
    } else {
        if (target_720p_mode != 0xFFFFFFFF) best_mode = target_720p_mode;
        else if (target_1080p_mode != 0xFFFFFFFF) best_mode = target_1080p_mode;
        else if (target_800p_mode != 0xFFFFFFFF) best_mode = target_800p_mode;
        else if (target_1024_mode != 0xFFFFFFFF) best_mode = target_1024_mode;
    }

    if (in_vm) {
        boot_msg(SystemTable, "[bootmgfw] hypervisor detected: configuring optimal GOP video mode... ");
    } else {
        boot_msg(SystemTable, "[bootmgfw] bare metal detected: configuring optimal 1080p GOP video mode... ");
    }
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

    /* 6. Pre-allocate 128 MB Kernel Heap (prefer < 4GB for DMA safety) */
    boot_msg(SystemTable, "[bootmgfw] pre-allocating 128 MB kernel heap pages... ");
    EFI_PHYSICAL_ADDRESS heap_buffer = 0xFFFFFFFFULL;
    UINTN heap_pages = 32768; /* 128 MB */
    status = SystemTable->BootServices->AllocatePages(
        AllocateMaxAddress,
        EfiLoaderData,
        heap_pages,
        &heap_buffer
    );
    if (EFI_ERROR(status) || heap_buffer == 0) {
        heap_buffer = 0;
        status = SystemTable->BootServices->AllocatePages(
            AllocateAnyPages,
            EfiLoaderData,
            heap_pages,
            &heap_buffer
        );
    }

    if (EFI_ERROR(status) || heap_buffer == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate 128 MB kernel heap memory");
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

    /* Extract Device Path string and walk raw binary Device Path nodes */
    int dp_has_cdrom = 0;
    int dp_has_usb   = 0;
    int dp_has_nvme  = 0;
    int dp_has_sata  = 0;
    if (loaded_image->DeviceHandle) {
        EFI_DEVICE_PATH_PROTOCOL *dev_dp = NULL;
        status = SystemTable->BootServices->HandleProtocol(
            loaded_image->DeviceHandle,
            &gEfiDevicePathProtocolGuid,
            (VOID **)&dev_dp
        );
        if (!EFI_ERROR(status) && dev_dp) {
            /* Walk raw binary EFI_DEVICE_PATH_PROTOCOL nodes (works even when DevicePathToText is absent in OEM BIOS) */
            const EFI_DEVICE_PATH_PROTOCOL *node = dev_dp;
            for (int iter = 0; iter < 64 && node; iter++) {
                UINT16 node_len = (UINT16)node->Length[0] | ((UINT16)node->Length[1] << 8);
                if (node->Type == EFI_DEVICE_PATH_TYPE_END || node_len < 4) {
                    break;
                }
                if (node->Type == EFI_DEVICE_PATH_TYPE_MEDIA) {
                    /* SubType 2 = MEDIA_CDROM_DP (El Torito), SubType 9 = MEDIA_RAM_DISK_DP */
                    if (node->SubType == 2 || node->SubType == 9) {
                        dp_has_cdrom = 1;
                    }
                } else if (node->Type == EFI_DEVICE_PATH_TYPE_MESSAGING) {
                    /* SubType 5 = USB, 15 = USB Class, 16 = USB WWID */
                    if (node->SubType == 5 || node->SubType == 15 || node->SubType == 16) {
                        dp_has_usb = 1;
                    } else if (node->SubType == 23) { /* NVMe Namespace */
                        dp_has_nvme = 1;
                    } else if (node->SubType == 18 || node->SubType == 1) { /* SATA / ATAPI */
                        dp_has_sata = 1;
                    }
                }
                node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + node_len);
            }

            EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dp_to_text = NULL;
            status = SystemTable->BootServices->LocateProtocol(
                &gEfiDevicePathToTextProtocolGuid,
                NULL,
                (VOID **)&dp_to_text
            );
            if (!EFI_ERROR(status) && dp_to_text && dp_to_text->ConvertDevicePathToText) {
                CHAR16 *text_u16 = dp_to_text->ConvertDevicePathToText(dev_dp, FALSE, FALSE);
                if (text_u16) {
                    unicode_to_ascii(text_u16, boot_info.hardware_devpath, sizeof(boot_info.hardware_devpath));
                }
            }
        }
    }
    if (boot_info.hardware_devpath[0] == '\0') {
        if (dp_has_cdrom) {
            strcpy(boot_info.hardware_devpath, "PciRoot(0x0)/CDROM(0x1)");
        } else if (dp_has_usb) {
            strcpy(boot_info.hardware_devpath, "PciRoot(0x0)/USB(0x0,0x0)");
        } else if (dp_has_nvme) {
            strcpy(boot_info.hardware_devpath, "PciRoot(0x0)/NVMe(0x1)");
        } else if (dp_has_sata) {
            strcpy(boot_info.hardware_devpath, "PciRoot(0x0)/Sata(0x0,0x0,0x0)");
        } else {
            strcpy(boot_info.hardware_devpath, "Unknown / Direct Boot");
        }
    } else if (dp_has_cdrom && strstr(boot_info.hardware_devpath, "CDROM") == NULL &&
               strstr(boot_info.hardware_devpath, "CdRom") == NULL) {
        cmdline_append(boot_info.hardware_devpath, "/CDROM(0x1)", sizeof(boot_info.hardware_devpath));
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

    /* 9. Check and Parse Boot Manager Configuration */
    boot_msg(SystemTable, "[bootmgfw] checking boot configuration... ");

    char cfg_kernel_path[128] = "";
    char cfg_initramfs_path[128] = "";
    boot_info.autoinit_path[0] = '\0';
    boot_info.shell_path[0] = '\0';
    boot_info.cmdline[0] = '\0';

    const CHAR16 bootcfg_path1[] = { '\\', 'p', 'r', 'o', 't', 'e', 'c', 't', 'e', 'd', '\\', 'b', 'o', 'o', 't', 'm', 'g', 'r', '\\', 'b', 'o', 'o', 't', '.', 'c', 'f', 'g', 0 };
    const CHAR16 bootcfg_path2[] = { '\\', 'b', 'o', 'o', 't', '\\', 'b', 'o', 'o', 't', '.', 'c', 'f', 'g', 0 };
    const CHAR16 bootcfg_path3[] = { '\\', 'b', 'o', 'o', 't', '.', 'c', 'f', 'g', 0 };

    EFI_FILE_PROTOCOL *cfg_file = NULL;
    status = root_dir->Open(root_dir, &cfg_file, (CHAR16 *)bootcfg_path1, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !cfg_file) {
        status = root_dir->Open(root_dir, &cfg_file, (CHAR16 *)bootcfg_path2, EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(status) || !cfg_file) {
        status = root_dir->Open(root_dir, &cfg_file, (CHAR16 *)bootcfg_path3, EFI_FILE_MODE_READ, 0);
    }

    if (EFI_ERROR(status) || !cfg_file) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_BOOTCFG_NOT_FOUND, "boot configuration not found (\\protected\\bootmgr\\boot.cfg required)");
    }

    char cfg_buf[2048];
    UINTN cfg_read_size = sizeof(cfg_buf) - 1;
    status = cfg_file->Read(cfg_file, &cfg_read_size, cfg_buf);
    cfg_file->Close(cfg_file);

    if (EFI_ERROR(status) || cfg_read_size == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_BOOTCFG_NOT_FOUND, "boot.cfg is empty or unreadable");
    }

    cfg_buf[cfg_read_size] = '\0';
    char *line = cfg_buf;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
        if (*line == '\0') break;
        char *line_end = line;
        while (*line_end && *line_end != '\r' && *line_end != '\n') line_end++;
        char orig_end = *line_end;
        *line_end = '\0';

        if (*line != '#') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim(line);
                char *val = trim(eq + 1);
                if (strcmp(key, "kernel") == 0) {
                    strncpy(cfg_kernel_path, val, sizeof(cfg_kernel_path) - 1);
                } else if (strcmp(key, "initramfs") == 0) {
                    strncpy(cfg_initramfs_path, val, sizeof(cfg_initramfs_path) - 1);
                } else if (strcmp(key, "autoinit") == 0) {
                    strncpy(boot_info.autoinit_path, val, sizeof(boot_info.autoinit_path) - 1);
                } else if (strcmp(key, "shell") == 0) {
                    strncpy(boot_info.shell_path, val, sizeof(boot_info.shell_path) - 1);
                } else if (strcmp(key, "cmdline") == 0) {
                    strncpy(boot_info.cmdline, val, sizeof(boot_info.cmdline) - 1);
                }
            }
        }

        if (orig_end == '\0') break;
        line = line_end + 1;
    }
    boot_msg(SystemTable, "[ok]\n");

    /* boot.cfg MUST specify kernel= */
    if (cfg_kernel_path[0] == '\0') {
        error_boot(SystemTable, ERR_BOOTCFG_NOT_FOUND, "boot.cfg missing required 'kernel=' directive");
    }

    if (dp_has_cdrom && strstr(boot_info.cmdline, "boot=live") == NULL) {
        cmdline_append(boot_info.cmdline, "boot=live", sizeof(boot_info.cmdline));
    }

    /* 9b. Parse UEFI LoadedImage LoadOptions (passed from EFI Shell or boot manager) */
    if (loaded_image && loaded_image->ImageLoadOptions && loaded_image->ImageLoadOptionsSize > 0) {
        char opt_buf[256];
        size_t opt_len = 0;
        const CHAR16 *u16 = (const CHAR16 *)loaded_image->ImageLoadOptions;
        UINTN chars = loaded_image->ImageLoadOptionsSize / sizeof(CHAR16);
        if (chars > 0 && u16[0] != 0 && (u16[0] & 0xFF00) == 0) {
            while (opt_len < chars && opt_len < sizeof(opt_buf) - 1 && u16[opt_len] != 0) {
                opt_buf[opt_len] = (char)(u16[opt_len] & 0x7F);
                opt_len++;
            }
            opt_buf[opt_len] = '\0';
        } else {
            const char *asc = (const char *)loaded_image->ImageLoadOptions;
            size_t bytes = (loaded_image->ImageLoadOptionsSize < sizeof(opt_buf) - 1)
                           ? loaded_image->ImageLoadOptionsSize : (sizeof(opt_buf) - 1);
            memcpy(opt_buf, asc, bytes);
            opt_buf[bytes] = '\0';
        }
        if (opt_buf[0] != '\0') {
            cmdline_append(boot_info.cmdline, opt_buf, sizeof(boot_info.cmdline));
            boot_msg(SystemTable, "[bootmgfw] appended UEFI LoadOptions: ");
            boot_msg(SystemTable, opt_buf);
            boot_msg(SystemTable, "\n");
        }
    }

    /* 9c. Probe QEMU fw_cfg command-line options */
    if (in_vm) {
        outw(0x510, 0x0000); /* FW_CFG_SIGNATURE */
        char sig[4];
        for (int k = 0; k < 4; k++) sig[k] = (char)inb(0x511);
        if (memcmp(sig, "QEMU", 4) == 0) {
            /* Try standard FW_CFG_CMDLINE_SIZE / DATA (selector 4 & 5) */
            outw(0x510, 0x0004);
            uint32_t sz = (uint32_t)inb(0x511) | ((uint32_t)inb(0x511) << 8) |
                          ((uint32_t)inb(0x511) << 16) | ((uint32_t)inb(0x511) << 24);
            if (sz > 0 && sz < 512) {
                outw(0x510, 0x0005);
                char fw_buf[256];
                size_t r = sz < sizeof(fw_buf) - 1 ? sz : sizeof(fw_buf) - 1;
                for (size_t k = 0; k < r; k++) fw_buf[k] = (char)inb(0x511);
                fw_buf[r] = '\0';
                if (fw_buf[0] != '\0') {
                    cmdline_append(boot_info.cmdline, fw_buf, sizeof(boot_info.cmdline));
                    boot_msg(SystemTable, "[bootmgfw] appended QEMU fw_cfg cmdline: ");
                    boot_msg(SystemTable, fw_buf);
                    boot_msg(SystemTable, "\n");
                }
            }

            /* Also check named fw_cfg files for "opt/cmdline" */
            outw(0x510, 0x0019); /* FW_CFG_FILE_DIR */
            uint32_t file_count = ((uint32_t)inb(0x511) << 24) | ((uint32_t)inb(0x511) << 16) |
                                  ((uint32_t)inb(0x511) << 8) | (uint32_t)inb(0x511);
            if (file_count > 0 && file_count < 1024) {
                for (uint32_t f = 0; f < file_count; f++) {
                    uint32_t fsize = ((uint32_t)inb(0x511) << 24) | ((uint32_t)inb(0x511) << 16) |
                                     ((uint32_t)inb(0x511) << 8) | (uint32_t)inb(0x511);
                    uint16_t fsel = ((uint16_t)inb(0x511) << 8) | (uint16_t)inb(0x511);
                    (void)inb(0x511); (void)inb(0x511); /* reserved */
                    char fname[56];
                    for (int k = 0; k < 56; k++) fname[k] = (char)inb(0x511);
                    fname[55] = '\0';
                    if (strcmp(fname, "opt/cmdline") == 0 && fsize > 0 && fsize < 256) {
                        outw(0x510, fsel);
                        char fbuf[256];
                        for (uint32_t k = 0; k < fsize; k++) fbuf[k] = (char)inb(0x511);
                        fbuf[fsize] = '\0';
                        cmdline_append(boot_info.cmdline, fbuf, sizeof(boot_info.cmdline));
                        boot_msg(SystemTable, "[bootmgfw] appended QEMU fw_cfg opt/cmdline: ");
                        boot_msg(SystemTable, fbuf);
                        boot_msg(SystemTable, "\n");
                        break;
                    }
                }
            }
        }
    }

    /* 10. Load Kernel Image specified in boot.cfg */
    boot_msg(SystemTable, "[bootmgfw] looking for kernel image...\n");
    boot_msg(SystemTable, "[bootmgfw] loading kernel from boot.cfg: ");
    boot_msg(SystemTable, cfg_kernel_path);
    boot_msg(SystemTable, "... ");

    CHAR16 cfg_kernel_path16[128];
    ascii_to_unicode(cfg_kernel_path, cfg_kernel_path16, sizeof(cfg_kernel_path16) / sizeof(CHAR16));
    EFI_FILE_PROTOCOL *kernel_file = NULL;
    status = root_dir->Open(root_dir, &kernel_file, cfg_kernel_path16, EFI_FILE_MODE_READ, 0);

    if (EFI_ERROR(status) || !kernel_file) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_KERNEL_NOT_FOUND, "kernel image specified in boot.cfg not found");
    }
    strcpy(boot_info.boot_file_path, cfg_kernel_path);
    boot_msg(SystemTable, "[ok]\n");

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
    EFI_PHYSICAL_ADDRESS temp_file_buffer = 0xFFFFFFFFULL;
    status = SystemTable->BootServices->AllocatePages(
        AllocateMaxAddress,
        EfiLoaderData,
        kernel_file_pages,
        &temp_file_buffer
    );
    if (EFI_ERROR(status) || temp_file_buffer == 0) {
        temp_file_buffer = 0;
        status = SystemTable->BootServices->AllocatePages(
            AllocateAnyPages,
            EfiLoaderData,
            kernel_file_pages,
            &temp_file_buffer
        );
    }

    if (EFI_ERROR(status) || temp_file_buffer == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate memory pages for kernel loading");
    }

    UINTN read_size = actual_file_size;
    status = kernel_file->Read(kernel_file, &read_size, (VOID *)(uintptr_t)temp_file_buffer);
    kernel_file->Close(kernel_file);

    /* 12b. Attempt to load initramfs from boot.cfg if specified */
    if (cfg_initramfs_path[0] != '\0') {
        boot_msg(SystemTable, "[bootmgfw] loading initramfs from boot.cfg: ");
        boot_msg(SystemTable, cfg_initramfs_path);
        boot_msg(SystemTable, "... ");
        CHAR16 cfg_initramfs_path16[128];
        ascii_to_unicode(cfg_initramfs_path, cfg_initramfs_path16, sizeof(cfg_initramfs_path16) / sizeof(CHAR16));
        EFI_FILE_PROTOCOL *ramfs_file = NULL;
        status = root_dir->Open(root_dir, &ramfs_file, cfg_initramfs_path16, EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status) && ramfs_file) {
            boot_msg(SystemTable, "[ok]\n");
            UINTN r_info_size = sizeof(info_buf);
            UINTN ramfs_file_size = 0;
            if (!EFI_ERROR(ramfs_file->GetInfo(ramfs_file, &gEfiFileInfoGuid, &r_info_size, info_buf))) {
                ramfs_file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
            }
            if (ramfs_file_size > 0) {
                UINTN r_pages = (ramfs_file_size + 4095) / 4096;
                EFI_PHYSICAL_ADDRESS ramfs_buf = 0;
                if (!EFI_ERROR(SystemTable->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, r_pages, &ramfs_buf)) && ramfs_buf != 0) {
                    UINTN r_read = ramfs_file_size;
                    if (!EFI_ERROR(ramfs_file->Read(ramfs_file, &r_read, (VOID *)(uintptr_t)ramfs_buf))) {
                        boot_info.initramfs_base = (uint64_t)ramfs_buf;
                        boot_info.initramfs_size = (uint64_t)r_read;
                        boot_msg(SystemTable, "[bootmgfw] loaded initramfs into memory [ok]\n");
                    }
                }
            }
            ramfs_file->Close(ramfs_file);
        } else {
            boot_msg(SystemTable, "[failed]\n");
            error_boot(SystemTable, ERR_KERNEL_NOT_FOUND, "initramfs image specified in boot.cfg not found");
        }
    }

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

    UINT64 image_base = *(UINT64 *)(raw_file + opt_offset + 24);

    status = EFI_UNSUPPORTED;
    if (image_base != 0) {
        kernel_buffer = (EFI_PHYSICAL_ADDRESS)image_base;
        status = SystemTable->BootServices->AllocatePages(
            AllocateAddress,
            EfiLoaderCode,
            kernel_image_pages,
            &kernel_buffer
        );
    }

    if (EFI_ERROR(status) || kernel_buffer == 0) {
        kernel_buffer = 0xFFFFFFFFULL;
        status = SystemTable->BootServices->AllocatePages(
            AllocateMaxAddress,
            EfiLoaderCode,
            kernel_image_pages,
            &kernel_buffer
        );
    }

    if (EFI_ERROR(status) || kernel_buffer == 0) {
        kernel_buffer = 0;
        status = SystemTable->BootServices->AllocatePages(
            AllocateAnyPages,
            EfiLoaderCode,
            kernel_image_pages,
            &kernel_buffer
        );
    }

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

    /* Apply PE Base Relocations if loaded at non-preferred address */
    INT64 delta = (INT64)kernel_buffer - (INT64)image_base;
    if (delta != 0 && opt_hdr_size >= 112 + 6 * 8) {
        UINT32 reloc_rva = *(UINT32 *)(raw_file + opt_offset + 112 + 5 * 8);
        UINT32 reloc_size = *(UINT32 *)(raw_file + opt_offset + 112 + 5 * 8 + 4);

        if (reloc_rva != 0 && reloc_size != 0 && (reloc_rva + reloc_size <= size_of_image)) {
            UINT8 *reloc_ptr = (UINT8 *)(uintptr_t)kernel_buffer + reloc_rva;
            UINT8 *reloc_end = reloc_ptr + reloc_size;

            while (reloc_ptr + 8 <= reloc_end) {
                UINT32 page_rva = *(UINT32 *)reloc_ptr;
                UINT32 block_sz = *(UINT32 *)(reloc_ptr + 4);
                if (block_sz < 8 || reloc_ptr + block_sz > reloc_end) break;

                UINT16 *entries = (UINT16 *)(reloc_ptr + 8);
                UINTN count = (block_sz - 8) / 2;
                for (UINTN j = 0; j < count; j++) {
                    UINT16 type = entries[j] >> 12;
                    UINT16 off = entries[j] & 0xFFF;
                    if (type == 10) { /* IMAGE_REL_BASED_DIR64 */
                        if (page_rva + off + 8 <= size_of_image) {
                            UINT64 *target = (UINT64 *)((UINT8 *)(uintptr_t)kernel_buffer + page_rva + off);
                            *target += (UINT64)delta;
                        }
                    } else if (type == 3) { /* IMAGE_REL_BASED_HIGHLOW */
                        if (page_rva + off + 4 <= size_of_image) {
                            UINT32 *target = (UINT32 *)((UINT8 *)(uintptr_t)kernel_buffer + page_rva + off);
                            *target += (UINT32)delta;
                        }
                    }
                }
                reloc_ptr += block_sz;
            }
        }
    }

    boot_info.kernel_physical_base = kernel_buffer;
    boot_info.kernel_image_size = size_of_image;
    boot_info.kernel_raw_file_base = temp_file_buffer;
    boot_info.kernel_raw_file_size = read_size;

    /* Keep temp_file_buffer preserved in memory as the pristine raw kernel PE payload for self-installation */
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

    /* 11b. Wake UEFI Pointer Protocols (powers on OEM I2C-HID / PS/2 trackpads before ExitBootServices) */
    {
        typedef struct _EFI_SIMPLE_POINTER_PROTOCOL {
            EFI_STATUS (EFIAPI *Reset)(struct _EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN ExtendedVerification);
            VOID *GetState;
            VOID *WaitForInput;
            VOID *Mode;
        } EFI_SIMPLE_POINTER_PROTOCOL;

        typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER_FN)(
            UINT32 SearchType,
            const EFI_GUID *Protocol,
            VOID *SearchKey,
            UINTN *NoHandles,
            EFI_HANDLE **Buffer
        );

        static const EFI_GUID simple_ptr_guid = {
            0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d }
        };
        static const EFI_GUID abs_ptr_guid = {
            0x8d59d32b, 0xc655, 0x4ae9, { 0x9b, 0x15, 0xf2, 0x59, 0x04, 0x99, 0x2a, 0x43 }
        };

        EFI_SIMPLE_POINTER_PROTOCOL *sptr = NULL;
        if (SystemTable->BootServices->LocateProtocol(&simple_ptr_guid, NULL, (VOID **)&sptr) == EFI_SUCCESS && sptr && sptr->Reset) {
            sptr->Reset(sptr, FALSE);
        }
        if (SystemTable->BootServices->LocateProtocol(&abs_ptr_guid, NULL, (VOID **)&sptr) == EFI_SUCCESS && sptr && sptr->Reset) {
            sptr->Reset(sptr, FALSE);
        }

        if (SystemTable->BootServices->LocateHandleBuffer) {
            EFI_LOCATE_HANDLE_BUFFER_FN locate_hb = (EFI_LOCATE_HANDLE_BUFFER_FN)SystemTable->BootServices->LocateHandleBuffer;
            UINTN handle_count = 0;
            EFI_HANDLE *handles = NULL;
            if (locate_hb(2 /* ByProtocol */, &simple_ptr_guid, NULL, &handle_count, &handles) == EFI_SUCCESS && handles) {
                for (UINTN h = 0; h < handle_count; h++) {
                    EFI_SIMPLE_POINTER_PROTOCOL *p = NULL;
                    if (SystemTable->BootServices->HandleProtocol(handles[h], &simple_ptr_guid, (VOID **)&p) == EFI_SUCCESS && p && p->Reset) {
                        p->Reset(p, FALSE);
                    }
                }
                SystemTable->BootServices->FreePool(handles);
            }
        }
    }

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
