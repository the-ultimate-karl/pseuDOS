#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

/* Framebuffer graphics */
void fb_init(const FramebufferInfo *fb_info);
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t fb_get_last_good_width(void);
uint32_t fb_get_last_good_height(void);
int fb_set_resolution(uint32_t width, uint32_t height);
int fb_is_runtime_switch_supported(void);
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg_color, uint32_t bg_color);
void fb_scroll_up(uint32_t pixels, uint32_t bg_color);

/* Text Console Output */
void console_init(void);
void console_clear(void);
void console_rebuild_layout(void);
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

/* PCI Configuration Space Access */
uint8_t pci_read_config_8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read_config_16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_config_16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
void pci_write_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

/* Bare-metal Power Management */
void acpi_reboot(void);
void acpi_shutdown(void);

#endif /* DRIVERS_H */
