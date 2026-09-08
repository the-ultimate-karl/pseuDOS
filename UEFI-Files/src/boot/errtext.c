#include "errtext.h"
#include "io.h"
#include "lib.h"

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

static void print_divider(EFI_SYSTEM_TABLE *SystemTable, UINTN left_pad, UINTN width) {
    if (left_pad > 0) print_spaces(SystemTable, left_pad);
    char buf[128];
    while (width > 0) {
        size_t n = width > (sizeof(buf) - 1) ? (sizeof(buf) - 1) : width;
        memset(buf, '=', n);
        buf[n] = '\0';
        print_both(SystemTable, buf);
        width -= n;
    }
    print_both(SystemTable, "\n");
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
        if (SystemTable->ConOut->SetCursorPosition) {
            SystemTable->ConOut->SetCursorPosition(SystemTable->ConOut, 0, 0);
        }
        if (SystemTable->ConOut->ClearScreen) {
            SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
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
            return "bootmgfw unable to mount boot device filesystem";
        case ERR_KERNEL_NOT_FOUND:
            return "bootmgfw failed to find kernel binary (\\KERNEL.BIN or EFI\\BOOT\\KERNEL.ELF)";
        case ERR_KERNEL_INTEGRITY:
            return "bootmgfw cannot verify integrity of the kernel";
        case ERR_MEMORY_MAP_EXHAUSTED:
            return "bootmgfw physical memory allocation failed or memory map exhausted";
        case ERR_GOP_INIT_FAILED:
            return "bootmgfw failed to initialize Graphics Output Protocol (GOP) display mode";
        case ERR_UNSUPPORTED_CPU:
            return "bootmgfw CPU does not meet minimum architecture requirements";
        case ERR_ACPI_RSDP_NOT_FOUND:
            return "bootmgfw ACPI RSDP descriptor table not found in UEFI configuration tables";
        case ERR_EXIT_BOOT_SERVICES_FAILED:
            return "bootmgfw failed to exit UEFI Boot Services";
        case ERR_SECURE_BOOT_VIOLATION:
            return "bootmgfw UEFI Secure Boot signature verification failed for boot image";
        case ERR_STORAGE_DEVICE_IO:
            return "bootmgfw fatal read error encountered from block storage device controller";
        case ERR_GENERIC_BOOT_FAILURE:
        default:
            return "bootmgfw unhandled fatal bootloader exception";
    }
}

static const char *get_default_detail(BootErrorCode code) {
    switch (code) {
        case ERR_KERNEL_NOT_FOUND:
            return "No such file or directory";
        case ERR_KERNEL_INTEGRITY:
            return "might be corrupted";
        case ERR_UNSUPPORTED_CPU:
            return "x86_64 Long Mode missing";
        case ERR_EXIT_BOOT_SERVICES_FAILED:
            return "memory map key mismatch";
        default:
            return NULL;
    }
}

void error_boot(EFI_SYSTEM_TABLE *SystemTable, BootErrorCode code, const char *details) {
    /* Clear full screen so only the error screen appears */
    clear_screen(SystemTable);

    UINTN cols = 80, rows = 25;
    get_screen_geometry(SystemTable, &cols, &rows);

    UINTN box_width = 86;
    if (cols < box_width) {
        box_width = cols > 4 ? cols - 2 : cols;
    }
    UINTN left_padding = (cols > box_width) ? (cols - box_width) / 2 : 0;

    /* Total vertical lines in error block: ~17 lines */
    UINTN total_lines = 17;
    UINTN top_padding = (rows > total_lines) ? (rows - total_lines) / 2 : 0;

    for (UINTN i = 0; i < top_padding; i++) {
        print_both(SystemTable, "\n");
    }

    /* Centered [bootmgfw] header */
    UINTN header_pad = left_padding + ((box_width > 10) ? (box_width - 10) / 2 : 0);
    print_spaces(SystemTable, header_pad);
    print_both(SystemTable, "[bootmgfw]\n");
    print_divider(SystemTable, left_padding, box_width);

    /* Explanatory text */
    print_padded_line(SystemTable, left_padding, "pseuDOS Boot Loader failed to start. A recent hardware or software change might be the");
    print_padded_line(SystemTable, left_padding, "cause. To fix the problem:");
    print_both(SystemTable, "\n");

    /* Step-by-step recovery instructions */
    print_padded_line(SystemTable, left_padding, "1. Insert your pseuDOS installation media and restart your computer.");
    print_padded_line(SystemTable, left_padding, "2. Type 'flash' into the command line and hit enter.");
    print_padded_line(SystemTable, left_padding, "3. Reinstall the entire operating system.");
    print_both(SystemTable, "\n");

    /* Info diagnostic section */
    if (code == ERR_KERNEL_NOT_FOUND) {
        print_padded_line(SystemTable, left_padding, "Info: bootmgfw failed to find kernel binary (\\KERNEL.BIN or EFI\\BOOT\\KERNEL.ELF):");
        print_padded_line(SystemTable, left_padding, "No such file or directory");
        print_both(SystemTable, "\n");
    } else {
        if (left_padding > 0) print_spaces(SystemTable, left_padding);
        print_both(SystemTable, "Info: ");
        const char *diag = get_error_diagnostic(code);
        print_both(SystemTable, diag);
        if (details && details[0] != '\0') {
            print_both(SystemTable, ":\n");
            print_wrapped_padded(SystemTable, left_padding, details, (int)box_width);
            print_both(SystemTable, "\n\n");
        } else {
            const char *def_det = get_default_detail(code);
            if (def_det) {
                if (code == ERR_KERNEL_INTEGRITY) {
                    print_both(SystemTable, ". ");
                    print_both(SystemTable, def_det);
                    print_both(SystemTable, "\n\n");
                } else {
                    print_both(SystemTable, " (");
                    print_both(SystemTable, def_det);
                    print_both(SystemTable, ")\n\n");
                }
            } else {
                print_both(SystemTable, "\n\n");
            }
        }
    }

    /* Footer message */
    print_padded_line(SystemTable, left_padding, "Please re-check your hardware configuration, boot media, and firmware settings.");
    print_padded_line(SystemTable, left_padding, "System halted!");
    print_both(SystemTable, "\n");
    print_divider(SystemTable, left_padding, box_width);
    print_both(SystemTable, "\n");

    /* Halt CPU execution */
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}
