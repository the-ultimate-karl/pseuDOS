#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

#define CLOCK_W 320
#define CLOCK_H 150

static ntfs_client_t g_client;

static void format_two_digits(char *buf, int val) {
    buf[0] = '0' + ((val / 10) % 10);
    buf[1] = '0' + (val % 10);
}

/* Scaled character drawing (scale 2x for large digital display) */
static void draw_large_char(int x, int y, char c, uint32_t fg) {
    if ((unsigned char)c < 32 || (unsigned char)c > 127) c = '?';
    const uint8_t *glyph = g_vga_font_8x16[(unsigned char)c - 32];

    for (int r = 0; r < 16; r++) {
        uint8_t bits = glyph[r];
        for (int b = 0; b < 8; b++) {
            if (bits & (0x80 >> b)) {
                /* 2x2 pixel block */
                gfx_fill_rect(g_client.pixels, CLOCK_W, CLOCK_H, x + b * 2, y + r * 2, 2, 2, fg);
            }
        }
    }
}

static void draw_large_string(int x, int y, const char *s, uint32_t fg) {
    int cx = x;
    while (*s) {
        draw_large_char(cx, y, *s, fg);
        cx += 18;
        s++;
    }
}

static void render_clock(void) {
    if (!g_client.pixels) return;

    gfx_fill_rect(g_client.pixels, CLOCK_W, CLOCK_H, 0, 0, CLOCK_W, CLOCK_H, COLOR_WIN_BG);

    /* Inner digital display screen (Sunken black bevel) */
    int sx = 12;
    int sy = 12;
    int sw = CLOCK_W - 24;
    int sh = CLOCK_H - 24;

    gfx_draw_bevel(g_client.pixels, CLOCK_W, CLOCK_H, sx, sy, sw, sh, 1);
    gfx_fill_rect(g_client.pixels, CLOCK_W, CLOCK_H, sx + 2, sy + 2, sw - 4, sh - 4, COLOR_BLACK);

    /* Time computation */
    uint64_t t = (uint64_t)syscall(SYS_TIME, 0, 0, 0, 0, 0);
    uint32_t sec = (uint32_t)(t % 60);
    uint32_t min = (uint32_t)((t / 60) % 60);
    uint32_t hour = (uint32_t)((t / 3600) % 24);

    char time_str[9];
    format_two_digits(&time_str[0], (int)hour);
    time_str[2] = ':';
    format_two_digits(&time_str[3], (int)min);
    time_str[5] = ':';
    format_two_digits(&time_str[6], (int)sec);
    time_str[8] = '\0';

    /* Glowing retro green/cyan digital time */
    uint32_t digit_color = 0x0000FFCC; /* Bright digital cyan */
    draw_large_string(sx + 34, sy + 28, time_str, digit_color);

    /* Subtitle text */
    gfx_draw_string(g_client.pixels, CLOCK_W, CLOCK_H, sx + 54, sy + 76, "Digital System Clock", COLOR_BTN_SHADOW, 0, 1);
    gfx_draw_string(g_client.pixels, CLOCK_W, CLOCK_H, sx + 74, sy + 94, "pseuDOS x86_64", 0x00008080, 0, 1);

    ntfs_client_damage(&g_client, 0, 0, CLOCK_W, CLOCK_H);
}

void clock_main(void) {
    if (ntfs_client_connect(&g_client, "Clock", CLOCK_W, CLOCK_H, NTFS_WIN_NORMAL | NTFS_WIN_FIXED_SIZE) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    render_clock();

    uint64_t last_time = 0;
    ntfs_msg_t msg;

    while (1) {
        while (ntfs_client_poll_event(&g_client, &msg)) {
            if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                ntfs_client_close(&g_client);
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                return;
            }
        }

        uint64_t t = (uint64_t)syscall(SYS_TIME, 0, 0, 0, 0, 0);
        if (t != last_time) {
            last_time = t;
            render_clock();
        }

        syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
    }
}
