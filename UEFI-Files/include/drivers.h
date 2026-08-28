#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

/* Framebuffer graphics */
void fb_init(const FramebufferInfo *fb_info);
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg_color, uint32_t bg_color);
void fb_scroll_up(uint32_t pixels, uint32_t bg_color);

/* Text Console Output */
void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_puts(const char *str);
void console_printf(const char *fmt, ...);

/* Keyboard Input Driver */
void keyboard_init(void);
char keyboard_getchar(void);
void keyboard_readline(char *buffer, size_t max_len, const char *prompt);
void keyboard_isr_handler(void);

/* Hardware Inspection */
void cpu_print_info(void);
void memory_print_info(const MemoryMapInfo *mem_info);
void pci_scan_bus(void);

/* Bare-metal Power Management */
void acpi_reboot(void);
void acpi_shutdown(void);

#endif /* DRIVERS_H */
