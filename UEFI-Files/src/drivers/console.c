#include "drivers.h"
#include "font.h"
#include "io.h"
#include "lib.h"

#define COLOR_WHITE 0xFFFFFF
#define COLOR_BLACK 0x000000

static uint32_t g_cursor_x = 0;
static uint32_t g_cursor_y = 0;

static uint32_t g_fg_color = COLOR_WHITE;
static uint32_t g_bg_color = COLOR_BLACK;

static uint32_t g_max_cols = 80;
static uint32_t g_max_rows = 25;

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

void console_init(void) {
    uart_init();

    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    if (w > 0 && h > 0) {
        g_max_cols = w / FONT_WIDTH;
        g_max_rows = h / FONT_HEIGHT;
    } else {
        g_max_cols = 80;
        g_max_rows = 25;
    }

    g_cursor_x = 0;
    g_cursor_y = 0;
    fb_clear(g_bg_color);
    uart_puts("\033[2J\033[H");
}

void console_clear(void) {
    fb_clear(g_bg_color);
    g_cursor_x = 0;
    g_cursor_y = 0;
    uart_puts("\033[2J\033[H");
}

void console_putc(char c) {
    if (c == '\r') {
        g_cursor_x = 0;
        uart_putc('\r');
        return;
    }

    if (c == '\n') {
        g_cursor_x = 0;
        g_cursor_y++;
        if (g_cursor_y >= g_max_rows) {
            fb_scroll_up(FONT_HEIGHT, g_bg_color);
            g_cursor_y = g_max_rows - 1;
        }
        uart_putc('\r');
        uart_putc('\n');
        return;
    }

    if (c == '\b') {
        if (g_cursor_x > 0) {
            g_cursor_x--;
            fb_draw_char(g_cursor_x * FONT_WIDTH, g_cursor_y * FONT_HEIGHT, ' ', g_fg_color, g_bg_color);
            uart_putc('\b');
            uart_putc(' ');
            uart_putc('\b');
        }
        return;
    }

    if (c == '\t') {
        uint32_t tab_stop = (g_cursor_x + 4) & ~3;
        while (g_cursor_x < tab_stop && g_cursor_x < g_max_cols) {
            console_putc(' ');
        }
        return;
    }

    fb_draw_char(g_cursor_x * FONT_WIDTH, g_cursor_y * FONT_HEIGHT, c, g_fg_color, g_bg_color);
    uart_putc(c);

    g_cursor_x++;
    if (g_cursor_x >= g_max_cols) {
        g_cursor_x = 0;
        g_cursor_y++;
        if (g_cursor_y >= g_max_rows) {
            fb_scroll_up(FONT_HEIGHT, g_bg_color);
            g_cursor_y = g_max_rows - 1;
        }
    }
}

void console_puts(const char *str) {
    if (!str) return;
    while (*str) {
        console_putc(*str++);
    }
}

void console_printf(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    console_puts(buf);
}
