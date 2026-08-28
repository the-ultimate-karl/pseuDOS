#include "drivers.h"
#include "io.h"
#include "lib.h"

static EFI_SYSTEM_TABLE *g_st = NULL;

static void uart_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

void console_init(EFI_SYSTEM_TABLE *SystemTable) {
    g_st = SystemTable;
    uart_init();
}

void console_putc(char c) {
    if (g_st && g_st->ConOut) {
        CHAR16 buf[3];
        int idx = 0;
        if (c == '\n') {
            buf[idx++] = (CHAR16)'\r';
        }
        buf[idx++] = (CHAR16)(uint8_t)c;
        buf[idx] = 0;
        g_st->ConOut->OutputString(g_st->ConOut, buf);
    } else {
        /* Fallback to direct UART port I/O if ConOut is absent */
        while ((inb(0x3F8 + 5) & 0x20) == 0);
        if (c == '\n') {
            outb(0x3F8, '\r');
        }
        outb(0x3F8, (uint8_t)c);
    }
}

void console_puts(const char *str) {
    if (!str) return;
    while (*str) {
        console_putc(*str++);
    }
}

void console_clear(void) {
    if (g_st && g_st->ConOut && g_st->ConOut->ClearScreen) {
        g_st->ConOut->ClearScreen(g_st->ConOut);
    } else {
        console_puts("\033[2J\033[H");
    }
}

void console_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_puts(buf);
}
