#ifndef DRIVERS_H
#define DRIVERS_H

#include "efi.h"

/* Console & UART */
void console_init(EFI_SYSTEM_TABLE *SystemTable);
void console_puts(const char *str);
void console_putc(char c);
void console_clear(void);
void console_printf(const char *fmt, ...);

/* Keyboard Input */
void keyboard_init(EFI_SYSTEM_TABLE *SystemTable);
char keyboard_getc(void);
int keyboard_readline(char *buffer, size_t max_len, const char *prompt);

/* Hardware Diagnostics */
void cpu_print_info(void);
void memory_print_info(EFI_SYSTEM_TABLE *SystemTable);
void pci_scan_bus(void);

#endif /* DRIVERS_H */
