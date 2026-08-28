#include "bootinfo.h"
#include "font.h"
#include "lib.h"

static FramebufferInfo g_fb;
static int g_fb_initialized = 0;

void fb_init(const FramebufferInfo *fb_info) {
    if (!fb_info || fb_info->physical_base == 0) return;
    memcpy(&g_fb, fb_info, sizeof(FramebufferInfo));
    g_fb_initialized = 1;
}

uint32_t fb_get_width(void) {
    return g_fb_initialized ? g_fb.width : 0;
}

uint32_t fb_get_height(void) {
    return g_fb_initialized ? g_fb.height : 0;
}

static inline uint32_t color_to_raw(uint32_t rgb) {
    if (g_fb.pixel_format == FB_FORMAT_BGR) {
        uint32_t r = (rgb >> 16) & 0xFF;
        uint32_t g = (rgb >> 8) & 0xFF;
        uint32_t b = rgb & 0xFF;
        return (b << 16) | (g << 8) | r;
    }
    return rgb;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_fb_initialized || x >= g_fb.width || y >= g_fb.height) return;
    volatile uint32_t *fb = (volatile uint32_t *)g_fb.physical_base;
    fb[y * g_fb.pixels_per_scanline + x] = color_to_raw(color);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!g_fb_initialized) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (x + w > g_fb.width) w = g_fb.width - x;
    if (y + h > g_fb.height) h = g_fb.height - y;

    uint32_t raw_color = color_to_raw(color);
    volatile uint32_t *fb = (volatile uint32_t *)g_fb.physical_base;

    for (uint32_t row = y; row < y + h; row++) {
        uint32_t offset = row * g_fb.pixels_per_scanline + x;
        for (uint32_t col = 0; col < w; col++) {
            fb[offset + col] = raw_color;
        }
    }
}

void fb_clear(uint32_t color) {
    if (!g_fb_initialized) return;
    fb_fill_rect(0, 0, g_fb.width, g_fb.height, color);
}

void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg_color, uint32_t bg_color) {
    if (!g_fb_initialized) return;
    if (x + FONT_WIDTH > g_fb.width || y + FONT_HEIGHT > g_fb.height) return;

    uint8_t glyph_idx = 0;
    unsigned char uc = (unsigned char)c;
    if (uc >= 32 && uc <= 127) {
        glyph_idx = (uint8_t)(uc - 32);
    } else {
        glyph_idx = (uint8_t)('?' - 32);
    }

    const uint8_t *glyph = g_font_8x16[glyph_idx];
    uint32_t raw_fg = color_to_raw(fg_color);
    uint32_t raw_bg = color_to_raw(bg_color);
    volatile uint32_t *fb = (volatile uint32_t *)g_fb.physical_base;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t line = glyph[row];
        uint32_t offset = (y + row) * g_fb.pixels_per_scanline + x;
        for (int col = 0; col < FONT_WIDTH; col++) {
            if ((line >> (7 - col)) & 1) {
                fb[offset + col] = raw_fg;
            } else {
                fb[offset + col] = raw_bg;
            }
        }
    }
}

void fb_scroll_up(uint32_t pixels, uint32_t bg_color) {
    if (!g_fb_initialized || pixels == 0 || pixels >= g_fb.height) return;

    volatile uint8_t *dest = (volatile uint8_t *)g_fb.physical_base;
    volatile uint8_t *src  = (volatile uint8_t *)(g_fb.physical_base + (pixels * g_fb.pixels_per_scanline * 4));
    size_t copy_bytes = (size_t)(g_fb.height - pixels) * g_fb.pixels_per_scanline * 4;

    memmove((void *)dest, (const void *)src, copy_bytes);

    /* Clear bottom rows */
    fb_fill_rect(0, g_fb.height - pixels, g_fb.width, pixels, bg_color);
}
