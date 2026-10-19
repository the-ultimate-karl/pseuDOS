#include "errtext.h"
#include "io.h"
#include "lib.h"

static EFI_HANDLE g_boot_image_handle = NULL;

void set_boot_image_handle(EFI_HANDLE image_handle) {
    g_boot_image_handle = image_handle;
}

static int g_err_uart_checked = 0;
static int g_err_uart_present = 0;

static int is_err_uart_present(void) {
    if (!g_err_uart_checked) {
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
            g_err_uart_present = 1;
        }
        g_err_uart_checked = 1;
    }
    return g_err_uart_present;
}

static void uart_putc(char c) {
    if (!is_err_uart_present()) return;
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

static void uefi_puts(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    if (!SystemTable || !SystemTable->ConOut || !str) return;
    CHAR16 buf[256];
    while (*str) {
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

/*
 * Chainload recovery.efi from the boot filesystem
 */
static EFI_STATUS launch_recovery_console(EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable || !SystemTable->BootServices || !g_boot_image_handle) {
        return EFI_INVALID_PARAMETER;
    }

    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_STATUS st = SystemTable->BootServices->HandleProtocol(
        g_boot_image_handle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&loaded
    );
    if (EFI_ERROR(st) || !loaded || !loaded->DeviceHandle) {
        return EFI_NOT_FOUND;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    st = SystemTable->BootServices->HandleProtocol(
        loaded->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&sfs
    );
    if (EFI_ERROR(st) || !sfs) {
        return EFI_NOT_FOUND;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    st = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(st) || !root) {
        return EFI_NOT_FOUND;
    }

    /* Check candidate recovery application paths on the ESP */
    const char *paths[] = {
        "\\EFI\\BOOT\\recovery.efi",
        "\\EFI\\pseuDOS\\recovery.efi",
        "\\protected\\bootmgr\\recovery.efi",
        "\\recovery.efi",
        NULL
    };

    EFI_FILE_PROTOCOL *rec_file = NULL;
    for (int i = 0; paths[i]; i++) {
        CHAR16 u16[128];
        ascii_to_unicode(paths[i], u16, sizeof(u16)/sizeof(CHAR16));
        st = root->Open(root, &rec_file, u16, EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(st) && rec_file) {
            break;
        }
    }

    if (!rec_file) {
        root->Close(root);
        return EFI_NOT_FOUND;
    }

    /* Read recovery.efi into memory buffer */
    UINT8 info_buf[sizeof(EFI_FILE_INFO) + 256];
    UINTN sz = sizeof(info_buf);
    st = rec_file->GetInfo(rec_file, &gEfiFileInfoGuid, &sz, info_buf);
    if (EFI_ERROR(st)) {
        rec_file->Close(rec_file);
        root->Close(root);
        return st;
    }

    UINTN fsize = (UINTN)((EFI_FILE_INFO *)info_buf)->FileSize;
    if (fsize == 0) {
        rec_file->Close(rec_file);
        root->Close(root);
        return EFI_LOAD_ERROR;
    }

    VOID *file_buf = NULL;
    st = SystemTable->BootServices->AllocatePool(EfiLoaderData, fsize, &file_buf);
    if (EFI_ERROR(st) || !file_buf) {
        rec_file->Close(rec_file);
        root->Close(root);
        return st;
    }

    UINTN r_bytes = fsize;
    st = rec_file->Read(rec_file, &r_bytes, file_buf);
    rec_file->Close(rec_file);
    root->Close(root);

    if (EFI_ERROR(st) || r_bytes != fsize) {
        SystemTable->BootServices->FreePool(file_buf);
        return EFI_LOAD_ERROR;
    }

    /* Load and start image from memory buffer */
    EFI_HANDLE rec_handle = NULL;
    st = SystemTable->BootServices->LoadImage(
        FALSE,
        g_boot_image_handle,
        NULL,
        file_buf,
        fsize,
        &rec_handle
    );

    if (!EFI_ERROR(st) && rec_handle) {
        UINTN exit_data_size = 0;
        CHAR16 *exit_data = NULL;
        st = SystemTable->BootServices->StartImage(rec_handle, &exit_data_size, &exit_data);
    }

    SystemTable->BootServices->FreePool(file_buf);
    return st;
}

static void print_both(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    if (SystemTable && SystemTable->ConOut) {
        uefi_puts(SystemTable, str);
    } else {
        uart_puts(str);
    }
}

static void get_screen_geometry(EFI_SYSTEM_TABLE *SystemTable, UINTN *out_cols, UINTN *out_rows) {
    UINTN cols = 80;
    UINTN rows = 25;
    if (SystemTable && SystemTable->ConOut && SystemTable->ConOut->QueryMode && SystemTable->ConOut->Mode) {
        UINTN c = 0, r = 0;
        if (!EFI_ERROR(SystemTable->ConOut->QueryMode(SystemTable->ConOut, SystemTable->ConOut->Mode->Mode, &c, &r))) {
            if (c > 0) cols = c;
            if (r > 0) rows = r;
        }
    }
    if (out_cols) *out_cols = cols;
    if (out_rows) *out_rows = rows;
}

static void print_spaces(EFI_SYSTEM_TABLE *SystemTable, UINTN count) {
    char buf[128];
    while (count > 0) {
        size_t n = count > (sizeof(buf) - 1) ? (sizeof(buf) - 1) : count;
        memset(buf, ' ', n);
        buf[n] = '\0';
        print_both(SystemTable, buf);
        count -= n;
    }
}

static void draw_white_bar(EFI_SYSTEM_TABLE *SystemTable, UINTN row, const char *left_text, const char *center_text, const char *right_text, UINTN cols) {
    char bar[256];
    if (cols > sizeof(bar) - 1) cols = sizeof(bar) - 1;
    memset(bar, ' ', cols);
    bar[cols] = '\0';

    if (center_text) {
        size_t len = strlen(center_text);
        if (len < cols) {
            size_t start = (cols - len) / 2;
            memcpy(bar + start, center_text, len);
        }
    }
    if (left_text) {
        size_t len = strlen(left_text);
        if (len + 2 < cols) {
            memcpy(bar + 2, left_text, len);
        }
    }
    if (right_text) {
        size_t len = strlen(right_text);
        if (len + 2 < cols) {
            size_t start = cols - len - 2;
            memcpy(bar + start, right_text, len);
        }
    }

    if (SystemTable && SystemTable->ConOut) {
        if (SystemTable->ConOut->SetCursorPosition) {
            SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, 0, row);
        }
        if (SystemTable->ConOut->SetAttribute) {
            /* 0x70 = Black text on White / Light Gray background */
            SystemTable->ConOut->SetAttribute(SystemTable->ConOut, 0x70);
        }
        uefi_puts(SystemTable, bar);
        if (SystemTable->ConOut->SetAttribute) {
            /* 0x0F = Bright White text on Black background */
            SystemTable->ConOut->SetAttribute(SystemTable->ConOut, 0x0F);
        }
    }

    uart_puts("\033[30;47m");
    uart_puts(bar);
    uart_puts("\033[37;40m\r\n");
}

static void print_padded_line(EFI_SYSTEM_TABLE *SystemTable, UINTN left_pad, const char *str) {
    if (left_pad > 0) print_spaces(SystemTable, left_pad);
    if (str) print_both(SystemTable, str);
    print_both(SystemTable, "\n");
}

static void clear_screen(EFI_SYSTEM_TABLE *SystemTable) {
    /* 1. Direct hardware GOP framebuffer wipe: zeroes out all pixels on the graphical display */
    if (SystemTable && SystemTable->BootServices && SystemTable->BootServices->LocateProtocol) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        EFI_STATUS st = SystemTable->BootServices->LocateProtocol(
            &gEfiGraphicsOutputProtocolGuid,
            NULL,
            (VOID **)&gop
        );
        if (!EFI_ERROR(st) && gop && gop->Mode) {
            if (gop->Mode->FrameBufferBase && gop->Mode->FrameBufferSize > 0) {
                memset((void *)gop->Mode->FrameBufferBase, 0, gop->Mode->FrameBufferSize);
            }
        }
    }

    /* 2. Switch ConOut to matching high-resolution text mode for 1280x720 */
    if (SystemTable && SystemTable->ConOut && SystemTable->ConOut->QueryMode && SystemTable->ConOut->SetMode && SystemTable->ConOut->Mode) {
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

    /* 3. Reset UEFI text console state and position cursor to top-left (0, 0) */
    if (SystemTable && SystemTable->ConOut) {
        if (SystemTable->ConOut->SetAttribute) {
            SystemTable->ConOut->SetAttribute(SystemTable->ConOut, 0x0F);
        }
        if (SystemTable->ConOut->ClearScreen) {
            SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
        }
        if (SystemTable->ConOut->SetCursorPosition) {
            SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, 0, 0);
        }
        if (SystemTable->ConOut->EnableCursor) {
            SystemTable->ConOut->EnableCursor(SystemTable->ConOut, FALSE);
        }
    }

    /* 4. Clear ANSI serial terminal */
    uart_puts("\033[2J\033[H");
}

static void print_wrapped_padded(EFI_SYSTEM_TABLE *SystemTable, UINTN left_pad, const char *str, int max_cols) {
    if (!str) return;
    int col = 0;
    const char *p = str;

    if (left_pad > 0) print_spaces(SystemTable, left_pad);

    while (*p) {
        if (*p == '\n') {
            print_both(SystemTable, "\n");
            if (left_pad > 0) print_spaces(SystemTable, left_pad);
            col = 0;
            p++;
            continue;
        }

        /* Skip leading spaces on wrapped line */
        if (col == 0 && *p == ' ') {
            p++;
            continue;
        }

        /* Find length of current word */
        const char *w_end = p;
        while (*w_end && *w_end != ' ' && *w_end != '\n') {
            w_end++;
        }
        int wlen = (int)(w_end - p);

        if (wlen > 0) {
            /* If adding this word exceeds max_cols, wrap to next line */
            if (col > 0 && (col + 1 + wlen > max_cols)) {
                print_both(SystemTable, "\n");
                if (left_pad > 0) print_spaces(SystemTable, left_pad);
                col = 0;
            } else if (col > 0) {
                print_both(SystemTable, " ");
                col++;
            }

            char buf[128];
            while (p < w_end) {
                size_t chunk = (size_t)(w_end - p);
                if (chunk >= sizeof(buf)) chunk = sizeof(buf) - 1;
                memcpy(buf, p, chunk);
                buf[chunk] = '\0';
                print_both(SystemTable, buf);
                col += (int)chunk;
                p += chunk;
            }
        } else if (*p == ' ') {
            p++;
        }
    }
}

static const char *get_error_diagnostic(BootErrorCode code) {
    switch (code) {
        case ERR_KERNEL_MOUNT_FAILED:
            return "silo unable to mount boot device filesystem";
        case ERR_BOOTCONFIG:
            return "silo encountered an error with the boot configuration data";
        case ERR_BOOTCFG_NOT_FOUND:
            return "silo failed to find or parse boot configuration (boot.cfg)";
        case ERR_KERNEL_INTEGRITY:
            return "silo cannot verify integrity of the kernel";
        case ERR_MEMORY_MAP_EXHAUSTED:
            return "silo physical memory allocation failed or memory map exhausted";
        case ERR_GOP_INIT_FAILED:
            return "silo failed to initialize Graphics Output Protocol (GOP) display mode";
        case ERR_UNSUPPORTED_CPU:
            return "silo CPU does not meet minimum architecture requirements";
        case ERR_ACPI_RSDP_NOT_FOUND:
            return "silo ACPI RSDP descriptor table not found in UEFI configuration tables";
        case ERR_EXIT_BOOT_SERVICES_FAILED:
            return "silo failed to exit UEFI Boot Services";
        case ERR_SECURE_BOOT_VIOLATION:
            return "silo UEFI Secure Boot signature verification failed for boot image";
        case ERR_STORAGE_DEVICE_IO:
            return "silo fatal read error encountered from block storage device controller";
        case ERR_GENERIC_BOOT_FAILURE:
        default:
            return "silo unhandled fatal bootloader exception";
    }
}

static const char *get_default_detail(BootErrorCode code) {
    switch (code) {
        case ERR_KERNEL_NOT_FOUND:
        case ERR_BOOTCFG_NOT_FOUND:
            return "No such file or directory";
        case ERR_UNSUPPORTED_CPU:
            return "x86_64 Long Mode missing";
        case ERR_EXIT_BOOT_SERVICES_FAILED:
            return "memory map key mismatch";
        default:
            return NULL;
    }
}

static const char *get_error_status_string(BootErrorCode code) {
    switch (code) {
        case ERR_KERNEL_MOUNT_FAILED:       return "ERR_KERNEL_MOUNT_FAILED";
        case ERR_BOOTCONFIG:                return "ERR_BOOTCONFIG";
        case ERR_BOOTCFG_NOT_FOUND:         return "ERR_BOOTCFG_NOT_FOUND";
        case ERR_KERNEL_INTEGRITY:          return "ERR_KERNEL_INTEGRITY";
        case ERR_MEMORY_MAP_EXHAUSTED:      return "ERR_MEMORY_MAP_EXHAUSTED";
        case ERR_GOP_INIT_FAILED:           return "ERR_GOP_INIT_FAILED";
        case ERR_UNSUPPORTED_CPU:           return "ERR_UNSUPPORTED_CPU";
        case ERR_ACPI_RSDP_NOT_FOUND:       return "ERR_ACPI_RSDP_NOT_FOUND";
        case ERR_EXIT_BOOT_SERVICES_FAILED: return "ERR_EXIT_BOOT_SERVICES_FAILED";
        case ERR_SECURE_BOOT_VIOLATION:     return "ERR_SECURE_BOOT_VIOLATION";
        case ERR_STORAGE_DEVICE_IO:         return "ERR_STORAGE_DEVICE_IO";
        case ERR_GENERIC_BOOT_FAILURE:
        default:                            return "ERR_GENERIC_BOOT_FAILURE";
    }
}

static void exit_to_uefi_firmware(EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable) return;

    /* Request UEFI Firmware UI (BIOS/Setup configuration) on reboot via OsIndications */
    if (SystemTable->RuntimeServices && SystemTable->RuntimeServices->SetVariable) {
        EFI_GUID global_guid = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
        UINT64 os_ind = 1; /* EFI_OS_INDICATIONS_BOOT_TO_FW_UI */
        const CHAR16 var_name[] = { 'O', 's', 'I', 'n', 'd', 'i', 'c', 'a', 't', 'i', 'o', 'n', 's', 0 };
        UINT32 attrs = 0x00000001 | 0x00000002 | 0x00000004; /* NV | BS | RT */
        SystemTable->RuntimeServices->SetVariable(
            (CHAR16 *)var_name,
            &global_guid,
            attrs,
            sizeof(UINT64),
            &os_ind
        );
    }

    /* Restore normal console attribute and clear screen */
    if (SystemTable->ConOut) {
        SystemTable->ConOut->SetAttribute(SystemTable->ConOut, 0x07);
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    }

    /* Try exiting cleanly back to the UEFI Boot Manager / firmware */
    if (SystemTable->BootServices && SystemTable->BootServices->Exit && g_boot_image_handle) {
        SystemTable->BootServices->Exit(g_boot_image_handle, EFI_ABORTED, 0, NULL);
    }

    /* Fallback: cold reboot into firmware UI */
    if (SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
        SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    }
}

void error_boot(EFI_SYSTEM_TABLE *SystemTable, BootErrorCode code, const char *details) {
    while (1) {
        /* Clear full screen so only the error screen appears */
        clear_screen(SystemTable);

        UINTN cols = 80, rows = 25;
        get_screen_geometry(SystemTable, &cols, &rows);

        UINTN left_padding = 4;
        if (cols > 90) {
            left_padding = 6;
        }

        /* 1. Draw Top White Bar across row 1 */
        draw_white_bar(SystemTable, 1, NULL, "[silo]", NULL, cols);

        /* 2. Position cursor for body text at row 3 */
        if (SystemTable && SystemTable->ConOut && SystemTable->ConOut->SetCursorPosition) {
            SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, 0, 3);
        }
        print_both(SystemTable, "\n");

        /* Explanatory text */
        print_padded_line(SystemTable, left_padding, "pseuDOS failed to start. A recent hardware or software change might be the");
        print_padded_line(SystemTable, left_padding, "cause. To fix the problem:");
        print_both(SystemTable, "\n");

        /* Step-by-step recovery instructions (indented by 4 spaces) */
        print_padded_line(SystemTable, left_padding + 4, "1. Press 'R' to launch the SiLo Recovery Console to inspect and repair settings.");
        print_padded_line(SystemTable, left_padding + 4, "2. Or insert installation media, restart, and run 'flash' to reinstall.");
        print_both(SystemTable, "\n");

        /* Info diagnostic section */
        char status_line[64];
        snprintf(status_line, sizeof(status_line), "Status: %s", get_error_status_string(code));
        print_padded_line(SystemTable, left_padding, status_line);

        UINTN info_indent = left_padding + 6; /* 6 characters for "Info: " */

        if (code == ERR_BOOTCONFIG) {
            print_padded_line(SystemTable, left_padding, "Info: silo encountered an error with the boot configuration data:");
            print_padded_line(SystemTable, info_indent, (details && details[0]) ? details : "No such file or directory");
            print_both(SystemTable, "\n");
        } else if (code == ERR_BOOTCFG_NOT_FOUND) {
            print_padded_line(SystemTable, left_padding, "Info: silo failed to locate boot configuration (\\protected\\bootmgr\\boot.cfg):");
            print_padded_line(SystemTable, info_indent, (details && details[0]) ? details : "No such file or directory");
            print_both(SystemTable, "\n");
        } else if (code == ERR_KERNEL_INTEGRITY) {
            print_padded_line(SystemTable, left_padding, "Info: silo cannot verify integrity of the kernel:");
            print_padded_line(SystemTable, info_indent, (details && details[0]) ? details : "corrupted executable image");
            print_both(SystemTable, "\n");
        } else {
            if (left_padding > 0) print_spaces(SystemTable, left_padding);
            print_both(SystemTable, "Info: ");
            const char *diag = get_error_diagnostic(code);
            print_both(SystemTable, diag);
            if (details && details[0] != '\0') {
                print_both(SystemTable, ":\n");
                print_wrapped_padded(SystemTable, info_indent, details, (int)(cols - info_indent - left_padding));
                print_both(SystemTable, "\n\n");
            } else {
                const char *def_det = get_default_detail(code);
                if (def_det) {
                    print_both(SystemTable, " (");
                    print_both(SystemTable, def_det);
                    print_both(SystemTable, ")\n\n");
                } else {
                    print_both(SystemTable, "\n\n");
                }
            }
        }

        /* Footer message */
        print_padded_line(SystemTable, left_padding, "Please re-check your hardware configuration, boot media, and firmware settings.");
        print_both(SystemTable, "\n");

        /* 3. Draw Bottom White Bar at row (rows - 2) */
        UINTN bottom_row = (rows > 2) ? (rows - 2) : (rows - 1);
        draw_white_bar(SystemTable, bottom_row, "ENTER=Continue", "R=Recovery", "ESC=Exit", cols);

        /* 4. Interactive Key Handling: Wait for ENTER, R, or ESC */
        int should_redraw = 0;
        if (SystemTable && SystemTable->ConIn && SystemTable->ConIn->ReadKeyStroke) {
            EFI_INPUT_KEY key;
            while (1) {
                EFI_STATUS st = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &key);
                if (!EFI_ERROR(st)) {
                    if (key.UnicodeChar == (CHAR16)'r' || key.UnicodeChar == (CHAR16)'R') {
                        EFI_STATUS rec_st = launch_recovery_console(SystemTable);
                        if (rec_st == EFI_ALREADY_STARTED) {
                            if (SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
                                SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                            }
                        }
                        should_redraw = 1;
                        break;
                    } else if (key.UnicodeChar == (CHAR16)'\r' || key.UnicodeChar == (CHAR16)'\n') {
                        if (SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
                            SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                        }
                        break;
                    } else if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B) {
                        /* ESC: Exit to UEFI Firmware Settings / Boot Manager */
                        exit_to_uefi_firmware(SystemTable);
                        break;
                    }
                }
                if (is_err_uart_present() && (inb(0x3F8 + 5) & 0x01)) {
                    uint8_t c = inb(0x3F8);
                    if (c == 'r' || c == 'R') {
                        EFI_STATUS rec_st = launch_recovery_console(SystemTable);
                        if (rec_st == EFI_ALREADY_STARTED) {
                            if (SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
                                SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                            }
                        }
                        should_redraw = 1;
                        break;
                    } else if (c == '\r' || c == '\n') {
                        if (SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
                            SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                        }
                        break;
                    } else if (c == 0x1B) {
                        /* ESC: Exit to UEFI Firmware Settings / Boot Manager */
                        exit_to_uefi_firmware(SystemTable);
                        break;
                    }
                }
            }
        }
        if (should_redraw) {
            continue;
        }

        /* Halt CPU execution fallback */
        __asm__ volatile ("cli");
        while (1) {
            __asm__ volatile ("hlt");
        }
    }
}
