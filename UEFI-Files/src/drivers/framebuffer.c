#include "bootinfo.h"
#include "drivers.h"
#include "font.h"
#include "io.h"
#include "lib.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID     0
#define VBE_DISPI_INDEX_XRES   1
#define VBE_DISPI_INDEX_YRES   2
#define VBE_DISPI_INDEX_BPP    3
#define VBE_DISPI_INDEX_ENABLE 4

#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

static FramebufferInfo g_fb;
static int g_fb_initialized = 0;

static uint32_t g_last_good_width = 1280;
static uint32_t g_last_good_height = 720;

static int g_is_vmware_svga = 0;
static uint16_t g_svga_io_base = 0;
static uint64_t g_svga_fb_base = 0;

static void svga_detect(void) {
    g_is_vmware_svga = 0;
    for (uint16_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_read_config_16((uint8_t)bus, slot, func, 0);
                if (vendor == 0x15AD) {
                    uint16_t device = pci_read_config_16((uint8_t)bus, slot, func, 2);
                    if (device == 0x0405) {
                        uint32_t bar0 = pci_read_config_32((uint8_t)bus, slot, func, 0x10);
                        uint32_t bar1 = pci_read_config_32((uint8_t)bus, slot, func, 0x14);
                        if (bar0 & 1) {
                            uint16_t io_base = (uint16_t)(bar0 & ~3);
                            outl(io_base + 0, 0); /* SVGA_REG_ID */
                            outl(io_base + 1, 0x90000002);
                            outl(io_base + 0, 0);
                            if (inl(io_base + 1) == 0x90000002) {
                                g_is_vmware_svga = 1;
                                g_svga_io_base = io_base;
                                g_svga_fb_base = (uint64_t)(bar1 & ~0xF);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void vbe_write(uint16_t index, uint16_t data) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, data);
}

static uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

void fb_init(const FramebufferInfo *fb_info) {
    if (!fb_info || fb_info->physical_base == 0) return;
    memcpy(&g_fb, fb_info, sizeof(FramebufferInfo));
    g_fb_initialized = 1;

    g_last_good_width = g_fb.width;
    g_last_good_height = g_fb.height;

    svga_detect();
}

int fb_is_runtime_switch_supported(void) {
    if (g_is_vmware_svga) return 1;
    uint16_t vbe_id = vbe_read(VBE_DISPI_INDEX_ID);
    if (vbe_id >= 0xB0C0 && vbe_id <= 0xB0CF) return 1;
    return 0;
}

uint32_t fb_get_width(void) {
    return g_fb_initialized ? g_fb.width : 0;
}

uint32_t fb_get_height(void) {
    return g_fb_initialized ? g_fb.height : 0;
}

uint32_t fb_get_last_good_width(void) {
    return g_last_good_width;
}

uint32_t fb_get_last_good_height(void) {
    return g_last_good_height;
}

int fb_set_resolution(uint32_t width, uint32_t height) {
    if (!g_fb_initialized) return -1;

    /* Validate safety bounds (640x480 to 3840x2160) */
    if (width < 640 || width > 3840 || height < 480 || height > 2160) {
        return -1;
    }

    int mode_switched = 0;
    uint32_t new_stride = width;

    /* Tier 1: VMware SVGA II Hardware Switch */
    if (g_is_vmware_svga && g_svga_io_base != 0) {
        outl(g_svga_io_base + 0, 2); /* SVGA_REG_WIDTH */
        outl(g_svga_io_base + 1, width);
        outl(g_svga_io_base + 0, 3); /* SVGA_REG_HEIGHT */
        outl(g_svga_io_base + 1, height);
        outl(g_svga_io_base + 0, 7); /* SVGA_REG_BITS_PER_PIXEL */
        outl(g_svga_io_base + 1, 32);
        outl(g_svga_io_base + 0, 1); /* SVGA_REG_ENABLE */
        outl(g_svga_io_base + 1, 1);

        outl(g_svga_io_base + 0, 14); /* SVGA_REG_BYTES_PER_LINE */
        uint32_t bpl = inl(g_svga_io_base + 1);
        if (bpl >= width * 4) {
            new_stride = bpl / 4;
        } else {
            new_stride = width;
        }

        if (g_svga_fb_base) {
            g_fb.physical_base = g_svga_fb_base;
        }
        mode_switched = 1;
    }

    /* Tier 2: Bochs / QEMU VBE Dispi Hardware Switch */
    if (!mode_switched) {
        uint16_t vbe_id = vbe_read(VBE_DISPI_INDEX_ID);
        if (vbe_id >= 0xB0C0 && vbe_id <= 0xB0CF) {
            vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
            vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
            vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
            vbe_write(VBE_DISPI_INDEX_BPP, 32);
            vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

            uint16_t cur_x = vbe_read(VBE_DISPI_INDEX_XRES);
            uint16_t cur_y = vbe_read(VBE_DISPI_INDEX_YRES);
            if (cur_x == (uint16_t)width && cur_y == (uint16_t)height) {
                new_stride = width;
                mode_switched = 1;
            }
        }
    }

    /* Tier 3: Real Hardware (Intel/AMD/Nvidia) without SVGA hardware mode-switching */
    if (!mode_switched) {
        /* Fail immediately without altering software geometry or scanline stride */
        return -1;
    }

    /* Update active framebuffer coordinates and scanline pitch */
    g_fb.width = width;
    g_fb.height = height;
    g_fb.pixels_per_scanline = new_stride;

    /* Recalibrate text console matrix, redraw preserved history */
    console_rebuild_layout();

    /* Record last known good resolution */
    g_last_good_width = width;
    g_last_good_height = height;

    return 0;
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
    if (w > g_fb.width - x) w = g_fb.width - x;
    if (h > g_fb.height - y) h = g_fb.height - y;

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
