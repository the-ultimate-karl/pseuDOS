#include "kernel.h"
#include "io.h"

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

void kernel_main(EFI_SYSTEM_TABLE *SystemTable) {
    uart_init();
    uart_puts("HELLO WORLD\n");

    if (SystemTable && SystemTable->ConOut) {
        CHAR16 msg[] = {
            (CHAR16)'H', (CHAR16)'E', (CHAR16)'L', (CHAR16)'L', (CHAR16)'O',
            (CHAR16)' ',
            (CHAR16)'W', (CHAR16)'O', (CHAR16)'R', (CHAR16)'L', (CHAR16)'D',
            (CHAR16)'\r', (CHAR16)'\n', 0
        };
        SystemTable->ConOut->OutputString(SystemTable->ConOut, msg);
    }

    while (1) {
        __asm__ volatile ("hlt");
    }
}
