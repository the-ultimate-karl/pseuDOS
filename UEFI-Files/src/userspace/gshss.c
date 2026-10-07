#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

static size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void puts(const char *str) {
    if (!str) return;
    syscall(SYS_WRITE, 1, (uint64_t)(uintptr_t)str, strlen(str), 0, 0);
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int abs(int x) {
    return (x < 0) ? -x : x;
}

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return (int)((unsigned char)c1 - (unsigned char)c2);
        s1++;
        s2++;
    }
    return (int)((unsigned char)*s1 - (unsigned char)*s2);
}

/* Memory arena for image decoding */
#define ARENA_SIZE (16 * 1024 * 1024)
static uint8_t s_arena[ARENA_SIZE];
static size_t s_arena_offset = 0;

/* Dedicated static file buffer for reading wallpaper files from VFS */
#define MAX_WALLPAPER_FILE_BYTES (1024 * 1024)
static uint8_t s_file_buf[MAX_WALLPAPER_FILE_BYTES];

typedef struct {
    size_t size;
} alloc_hdr_t;

static void *my_malloc(size_t sz) {
    size_t total = sizeof(alloc_hdr_t) + ((sz + 15) & ~15);
    if (s_arena_offset + total > ARENA_SIZE) return NULL;
    alloc_hdr_t *hdr = (alloc_hdr_t *)(s_arena + s_arena_offset);
    hdr->size = sz;
    s_arena_offset += total;
    return (void *)(hdr + 1);
}

static void *my_realloc(void *ptr, size_t sz) {
    if (!ptr) return my_malloc(sz);
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    void *new_ptr = my_malloc(sz);
    if (!new_ptr) return NULL;
    size_t copy_sz = (hdr->size < sz) ? hdr->size : sz;
    memcpy(new_ptr, ptr, copy_sz);
    return new_ptr;
}

static void my_free(void *ptr) {
    (void)ptr;
}

static void arena_reset(void) {
    s_arena_offset = 0;
}

#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC(sz)           my_malloc(sz)
#define STBI_REALLOC(p,newsz)     my_realloc(p,newsz)
#define STBI_FREE(p)              my_free(p)
#define STBI_ASSERT(x)            ((void)0)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* Indestructible baked fallback wallpaper compiled into gshss.exe */
extern const uint8_t g_fallback_wallpaper_png[];
extern const uint64_t g_fallback_wallpaper_png_size;

static const char * const g_wallpaper_list[] = {
    "/home/user/Wallpapers/Under Construction.png",
    "/home/user/Wallpapers/fallback.png",
    "/home/user/Wallpapers/accurate.png",
    "/home/user/Wallpapers/bluescreen.png",
    "/home/user/Wallpapers/meow.png",
    "/home/user/Wallpapers/sparklymeow.png",
    "/home/user/Wallpapers/sparklymeow_alt.png",
    "/home/user/Wallpapers/sparklymeow_altcircles.png",
    "/home/user/Wallpapers/sparklymeow_altlines.png",
    "/home/user/Wallpapers/sparklymeow_altlines2.png"
};
#define NUM_WALLPAPERS 10

static uint32_t g_screen_width = 1280;
static uint32_t g_screen_height = 720;

static ntfs_client_t g_bg_client;
static ntfs_client_t g_tb_client;
static ntfs_client_t g_menu_client;
static ntfs_client_t g_ctx_client;
static ntfs_client_t g_err_client;
static ntfs_client_t g_wall_dlg_client;

static int g_menu_open = 0;
static int g_ctx_open = 0;
static int g_err_open = 0;
static int g_wall_dlg_open = 0;
static uint8_t g_prev_tb_left = 0;
static uint8_t g_prev_menu_left = 0;
static uint8_t g_prev_bg_left = 0;
static uint8_t g_prev_bg_right = 0;
static uint8_t g_prev_ctx_left = 0;
static uint8_t g_prev_err_left = 0;
static uint8_t g_prev_dlg_left = 0;
static int g_ctx_hover = -1;

#define MAX_WALLPAPER_FILES 24
#define WALL_DLG_W 380
#define WALL_DLG_H 280
static char g_dlg_files[MAX_WALLPAPER_FILES][64];
static int g_dlg_count = 0;
static int g_dlg_selected = 0;
static int g_dlg_scroll = 0;

static const char *g_ctx_items[] = {
    "Terminal",
    "File Manager",
    "System Monitor",
    "Calculator",
    "Notepad",
    "Paint",
    "New Folder",
    "Refresh Desktop",
    "Change Wallpaper"
};
#define CTX_ITEM_COUNT 9
#define CTX_WIDTH 160
#define CTX_HEIGHT 197

static const char *g_menu_items[] = {
    "Terminal",
    "File Manager",
    "SysMon",
    "Calculator",
    "Notepad",
    "Paint",
    "Digital Clock",
    "Reboot",
    "Shutdown"
};
#define MENU_ITEM_COUNT 9
#define MENU_WIDTH 160
#define MENU_HEIGHT 202

static void close_desktop_error(void);
static void close_context_menu(void);
static void toggle_start_menu(void);

static int render_decoded_image(const uint8_t *file_data, size_t file_size) {
    if (!file_data || file_size == 0 || !g_bg_client.pixels) return -1;
    arena_reset();
    int img_w = 0, img_h = 0, channels = 0;
    unsigned char *decoded = stbi_load_from_memory(file_data, (int)file_size, &img_w, &img_h, &channels, 4);
    if (!decoded || img_w <= 0 || img_h <= 0) {
        arena_reset();
        return -1;
    }

    if (img_w <= 500 && img_h <= 400) {
        /* Tile / repeat pattern across entire desktop (e.g. Under Construction 320x160) */
        for (uint32_t y = 0; y < g_screen_height; y++) {
            int sy = (int)(y % (uint32_t)img_h);
            const uint8_t *src_row = decoded + (sy * img_w) * 4;
            uint32_t *dst_row = &g_bg_client.pixels[y * g_screen_width];
            for (uint32_t x = 0; x < g_screen_width; x++) {
                int sx = (int)(x % (uint32_t)img_w);
                const uint8_t *px = src_row + sx * 4;
                if (px[3] >= 128) {
                    dst_row[x] = COLOR_RGB(px[0], px[1], px[2]);
                } else {
                    dst_row[x] = COLOR_DESKTOP;
                }
            }
        }
    } else if (img_w == (int)g_screen_width && img_h == (int)g_screen_height) {
        for (uint32_t i = 0; i < g_screen_width * g_screen_height; i++) {
            const uint8_t *px = decoded + i * 4;
            g_bg_client.pixels[i] = COLOR_RGB(px[0], px[1], px[2]);
        }
    } else {
        /* Scale image to fit desktop resolution */
        for (uint32_t y = 0; y < g_screen_height; y++) {
            int sy = ((int)y * img_h) / (int)g_screen_height;
            if (sy >= img_h) sy = img_h - 1;
            const uint8_t *src_row = decoded + (sy * img_w) * 4;
            uint32_t *dst_row = &g_bg_client.pixels[y * g_screen_width];
            for (uint32_t x = 0; x < g_screen_width; x++) {
                int sx = ((int)x * img_w) / (int)g_screen_width;
                if (sx >= img_w) sx = img_w - 1;
                const uint8_t *px = src_row + sx * 4;
                if (px[3] >= 128) {
                    dst_row[x] = COLOR_RGB(px[0], px[1], px[2]);
                } else {
                    dst_row[x] = COLOR_DESKTOP;
                }
            }
        }
    }
    arena_reset();
    return 0;
}

static int load_and_render_file(const char *path) {
    if (!path || !path[0]) return -1;
    vfs_stat_t st;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0) {
        return -1;
    }
    if (st.size == 0 || st.size > sizeof(s_file_buf)) return -1;

    int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)s_file_buf, st.size, 0, 0);
    if (rd <= 0) {
        return -1;
    }
    return render_decoded_image(s_file_buf, (size_t)rd);
}


static void draw_background(void) {
    if (!g_bg_client.pixels) return;

    int rendered = -1;

    /* 1. Try currently configured wallpaper from /home/user/wallpaper.cfg */
    char cfg_path[128];
    memset(cfg_path, 0, sizeof(cfg_path));
    int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)"/home/user/wallpaper.cfg", (uint64_t)(uintptr_t)cfg_path, sizeof(cfg_path) - 1, 0, 0);
    if (rd > 0) {
        for (int i = 0; i < (int)sizeof(cfg_path); i++) {
            if (cfg_path[i] == '\r' || cfg_path[i] == '\n') {
                cfg_path[i] = '\0';
                break;
            }
        }
        if (cfg_path[0] != '\0') {
            rendered = load_and_render_file(cfg_path);
        }
    }

    /* 2. Try default /home/user/Wallpapers/Under Construction.png */
    if (rendered < 0) {
        rendered = load_and_render_file("/home/user/Wallpapers/Under Construction.png");
    }

    /* 3. Try fallback /home/user/Wallpapers/fallback.png */
    if (rendered < 0) {
        rendered = load_and_render_file("/home/user/Wallpapers/fallback.png");
    }

    /* 4. Try protected /protected/wallpapers/fallback.png */
    if (rendered < 0) {
        rendered = load_and_render_file("/protected/wallpapers/fallback.png");
    }

    /* 5. Indestructible in-memory .rodata fallback baked into gshss.exe */
    if (rendered < 0 && g_fallback_wallpaper_png_size > 0) {
        rendered = render_decoded_image(g_fallback_wallpaper_png, (size_t)g_fallback_wallpaper_png_size);
    }

    /* 5. Last resort: Solid retro teal color + watermark */
    if (rendered < 0) {
        for (uint32_t i = 0; i < g_screen_width * g_screen_height; i++) {
            g_bg_client.pixels[i] = COLOR_DESKTOP;
        }
        int wx = (int)g_screen_width - 320;
        int wy = (int)g_screen_height - 80;
        gfx_draw_string(g_bg_client.pixels, g_screen_width, g_screen_height, wx, wy, "pseuDOS v0.7.0", COLOR_WHITE, 0, 1);
        gfx_draw_string(g_bg_client.pixels, g_screen_width, g_screen_height, wx, wy + 20, "Graphical Shell Subsystem", COLOR_BTN_LIGHT, 0, 1);
    }

    ntfs_client_damage(&g_bg_client, 0, 0, g_screen_width, g_screen_height);
}

static void draw_wallpaper_dialog(void);

static void scan_wallpaper_files(void) {
    g_dlg_count = 0;
    g_dlg_selected = 0;
    g_dlg_scroll = 0;

    char list_buf[2048];
    int64_t n = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)"/home/user/Wallpapers", (uint64_t)(uintptr_t)list_buf, sizeof(list_buf), 0, 0);
    if (n > 0) {
        char *tok = list_buf;
        for (int64_t i = 0; i <= n; i++) {
            if (list_buf[i] == '\n' || list_buf[i] == '\0') {
                list_buf[i] = '\0';
                if (tok[0] != '\0' && g_dlg_count < MAX_WALLPAPER_FILES) {
                    size_t tlen = strlen(tok);
                    if (tlen > 4) {
                        const char *ext = tok + tlen - 4;
                        if (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".jpg") == 0) {
                            ntfs_strncpy(g_dlg_files[g_dlg_count++], tok, 63);
                        }
                    }
                }
                tok = &list_buf[i + 1];
            }
        }
    }

    if (g_dlg_count == 0) {
        for (int i = 0; i < NUM_WALLPAPERS && i < MAX_WALLPAPER_FILES; i++) {
            const char *src = g_wallpaper_list[i];
            const char *fname = src;
            for (int k = 0; src[k]; k++) {
                if (src[k] == '/') fname = &src[k + 1];
            }
            ntfs_strncpy(g_dlg_files[g_dlg_count++], fname, 63);
        }
    }

    char cfg_path[128];
    memset(cfg_path, 0, sizeof(cfg_path));
    int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)"/home/user/wallpaper.cfg", (uint64_t)(uintptr_t)cfg_path, sizeof(cfg_path) - 1, 0, 0);
    if (rd > 0) {
        for (int i = 0; i < (int)sizeof(cfg_path); i++) {
            if (cfg_path[i] == '\r' || cfg_path[i] == '\n') {
                cfg_path[i] = '\0';
                break;
            }
        }
        const char *cur_fname = cfg_path;
        for (int k = 0; cfg_path[k]; k++) {
            if (cfg_path[k] == '/') cur_fname = &cfg_path[k + 1];
        }
        for (int i = 0; i < g_dlg_count; i++) {
            if (strcmp(cur_fname, g_dlg_files[i]) == 0) {
                g_dlg_selected = i;
                break;
            }
        }
    }
}

static void draw_wallpaper_dialog(void) {
    if (!g_wall_dlg_client.pixels) return;
    int w = WALL_DLG_W;
    int h = WALL_DLG_H;

    gfx_fill_rect(g_wall_dlg_client.pixels, w, h, 0, 0, w, h, COLOR_WIN_BG);
    gfx_draw_bevel(g_wall_dlg_client.pixels, w, h, 0, 0, w, h, 0);

    /* Title bar */
    gfx_fill_rect(g_wall_dlg_client.pixels, w, h, 2, 2, w - 4, 18, COLOR_WIN_TITLE);
    gfx_draw_string(g_wall_dlg_client.pixels, w, h, 6, 4, "Select Wallpaper", COLOR_TITLE_TEXT, 0, 1);

    /* [X] close button */
    gfx_fill_rect(g_wall_dlg_client.pixels, w, h, w - 18, 3, 15, 14, COLOR_BTN_FACE);
    gfx_draw_bevel(g_wall_dlg_client.pixels, w, h, w - 18, 3, 15, 14, 0);
    gfx_draw_string(g_wall_dlg_client.pixels, w, h, w - 14, 4, "X", COLOR_TEXT, 0, 1);

    /* Directory label */
    gfx_draw_string(g_wall_dlg_client.pixels, w, h, 14, 26, "Wallpaper (/home/user/Wallpapers):", COLOR_TEXT, 0, 1);

    /* Listbox */
    int lx = 12;
    int ly = 44;
    int lw = w - 24;
    int lh = 180;
    gfx_draw_bevel(g_wall_dlg_client.pixels, w, h, lx, ly, lw, lh, 1);
    gfx_fill_rect(g_wall_dlg_client.pixels, w, h, lx + 1, ly + 1, lw - 2, lh - 2, COLOR_WHITE);

    int max_visible = 10;
    for (int i = 0; i < max_visible && (i + g_dlg_scroll) < g_dlg_count; i++) {
        int idx = i + g_dlg_scroll;
        int iy = ly + 2 + i * 17;
        if (idx == g_dlg_selected) {
            gfx_fill_rect(g_wall_dlg_client.pixels, w, h, lx + 2, iy, lw - 4, 17, 0x00000080);
            gfx_draw_string(g_wall_dlg_client.pixels, w, h, lx + 8, iy + 2, g_dlg_files[idx], COLOR_WHITE, 0, 1);
        } else {
            gfx_draw_string(g_wall_dlg_client.pixels, w, h, lx + 8, iy + 2, g_dlg_files[idx], COLOR_BLACK, 0, 1);
        }
    }

    /* Buttons at bottom */
    int btn_y = ly + lh + 14;

    /* [ Apply ] button */
    gfx_fill_rect(g_wall_dlg_client.pixels, w, h, w - 165, btn_y, 70, 24, COLOR_BTN_FACE);
    gfx_draw_bevel(g_wall_dlg_client.pixels, w, h, w - 165, btn_y, 70, 24, 0);
    gfx_draw_string(g_wall_dlg_client.pixels, w, h, w - 150, btn_y + 5, "Apply", COLOR_TEXT, 0, 1);

    /* [ Cancel ] button */
    gfx_fill_rect(g_wall_dlg_client.pixels, w, h, w - 85, btn_y, 70, 24, COLOR_BTN_FACE);
    gfx_draw_bevel(g_wall_dlg_client.pixels, w, h, w - 85, btn_y, 70, 24, 0);
    gfx_draw_string(g_wall_dlg_client.pixels, w, h, w - 73, btn_y + 5, "Cancel", COLOR_TEXT, 0, 1);

    ntfs_client_damage(&g_wall_dlg_client, 0, 0, w, h);
}

static void close_wallpaper_dialog(void) {
    if (g_wall_dlg_open) {
        ntfs_client_close(&g_wall_dlg_client);
        g_wall_dlg_open = 0;
        g_prev_dlg_left = 0;
    }
}

static void apply_selected_wallpaper(void) {
    if (g_dlg_selected >= 0 && g_dlg_selected < g_dlg_count) {
        char full_path[128];
        full_path[0] = '\0';
        int pos = 0;
        const char *prefix = "/home/user/Wallpapers/";
        while (prefix[pos]) {
            full_path[pos] = prefix[pos];
            pos++;
        }
        const char *fname = g_dlg_files[g_dlg_selected];
        int k = 0;
        while (fname[k] && pos < 126) {
            full_path[pos++] = fname[k++];
        }
        full_path[pos] = '\0';

        syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)"/home/user/wallpaper.cfg", (uint64_t)(uintptr_t)full_path, pos, 0, 0);
        draw_background();
    }
    close_wallpaper_dialog();
}

static void open_wallpaper_dialog(void) {
    close_wallpaper_dialog();
    if (g_err_open) close_desktop_error();
    if (g_menu_open) toggle_start_menu();
    if (g_ctx_open) close_context_menu();

    scan_wallpaper_files();

    int dw = WALL_DLG_W;
    int dh = WALL_DLG_H;
    int dx = ((int)g_screen_width - dw) / 2;
    int dy = ((int)g_screen_height - dh) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;

    if (ntfs_client_connect(&g_wall_dlg_client, "SelectWallpaper", dw, dh, NTFS_WIN_FRAMELESS) == 0) {
        ntfs_msg_t smsg;
        memset(&smsg, 0, sizeof(smsg));
        smsg.type = NTFS_MSG_SET_GEOMETRY;
        smsg.window_id = g_wall_dlg_client.window_id;
        smsg.x = dx;
        smsg.y = dy;
        smsg.width = dw;
        smsg.height = dh;
        smsg.flags = 2200; /* topmost z-order modal */
        syscall(SYS_SEND, g_wall_dlg_client.sock, (uint64_t)(uintptr_t)&smsg, sizeof(smsg), 0, 0);

        ntfs_msg_t fmsg;
        memset(&fmsg, 0, sizeof(fmsg));
        fmsg.type = NTFS_MSG_FOCUS_WINDOW;
        fmsg.window_id = g_wall_dlg_client.window_id;
        syscall(SYS_SEND, g_wall_dlg_client.sock, (uint64_t)(uintptr_t)&fmsg, sizeof(fmsg), 0, 0);

        draw_wallpaper_dialog();
        g_wall_dlg_open = 1;
        g_prev_dlg_left = 0;
    }
}

static void format_two_digits(char *buf, int val) {
    buf[0] = '0' + ((val / 10) % 10);
    buf[1] = '0' + (val % 10);
}

#define COLOR_PSEUDOS_BLUE COLOR_RGB(0, 140, 255)

/* 13x11 Greek letter Psi (pseuDOS brand logo) */
static const uint16_t g_psi_bitmap[11] = {
    0x01F0, /* ....#####.... row 0: top cap serif */
    0x0040, /* ......#...... row 1: upper stem */
    0x0E4E, /* .###..#..###. row 2: left & right outer wing serifs */
    0x0248, /* ...#..#..#... row 3: prongs & center stem */
    0x0248, /* ...#..#..#... row 4: prongs & center stem */
    0x0150, /* ....#.#.#.... row 5: lower curve */
    0x00E0, /* .....###..... row 6: bottom of bowl */
    0x0040, /* ......#...... row 7: lower stem */
    0x0040, /* ......#...... row 8: lower stem */
    0x0040, /* ......#...... row 9: lower stem */
    0x01F0  /* ....#####.... row 10: bottom base serif */
};

static void draw_psi_logo(uint32_t *buf, uint32_t buf_w, uint32_t buf_h, int x0, int y0, uint32_t color) {
    if (!buf) return;
    for (int r = 0; r < 11; r++) {
        int y = y0 + r;
        if (y < 0 || (uint32_t)y >= buf_h) continue;
        uint16_t row_bits = g_psi_bitmap[r];
        for (int c = 0; c < 13; c++) {
            int x = x0 + c;
            if (x < 0 || (uint32_t)x >= buf_w) continue;
            if (row_bits & (1 << (12 - c))) {
                buf[y * buf_w + x] = color;
            }
        }
    }
}

static void draw_taskbar(void) {
    if (!g_tb_client.pixels) return;

    uint32_t w = g_screen_width;
    uint32_t h = TASKBAR_HEIGHT;

    /* Taskbar surface background */
    gfx_fill_rect(g_tb_client.pixels, w, h, 0, 0, w, h, COLOR_WIN_BG);

    /* 3D Top highlight */
    gfx_fill_rect(g_tb_client.pixels, w, h, 0, 0, w, 1, COLOR_WHITE);
    gfx_fill_rect(g_tb_client.pixels, w, h, 0, 1, w, 1, COLOR_BTN_LIGHT);

    /* Start Button at (3, 3, 62, 22) */
    int sb_x = 3;
    int sb_y = 3;
    int sb_w = 62;
    int sb_h = 22;

    gfx_fill_rect(g_tb_client.pixels, w, h, sb_x, sb_y, sb_w, sb_h, COLOR_BTN_FACE);
    gfx_draw_bevel(g_tb_client.pixels, w, h, sb_x, sb_y, sb_w, sb_h, g_menu_open ? 1 : 0);

    /* pseuDOS Psi logo in #008cff */
    draw_psi_logo(g_tb_client.pixels, w, h, sb_x + 4, sb_y + 5, COLOR_PSEUDOS_BLUE);

    /* "Start" text */
    gfx_draw_string(g_tb_client.pixels, w, h, sb_x + 19, sb_y + 3, "Start", COLOR_BLACK, 0, 1);

    /* Quick status info in middle */
    gfx_draw_string(g_tb_client.pixels, w, h, 80, 6, "pseuDOS", COLOR_BTN_SHADOW, 0, 1);

    /* System Tray at right: (w - 78, 3, 74, 22) */
    int tray_x = (int)w - 78;
    int tray_y = 3;
    int tray_w = 74;
    int tray_h = 22;

    gfx_draw_bevel(g_tb_client.pixels, w, h, tray_x, tray_y, tray_w, tray_h, 1);

    /* Query time via SYS_TIME */
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

    gfx_draw_string(g_tb_client.pixels, w, h, tray_x + 6, tray_y + 3, time_str, COLOR_TEXT, 0, 1);

    ntfs_client_damage(&g_tb_client, 0, 0, w, h);
}

static void draw_start_menu(void) {
    if (!g_menu_client.pixels) return;

    int w = MENU_WIDTH;
    int h = MENU_HEIGHT;

    gfx_fill_rect(g_menu_client.pixels, w, h, 0, 0, w, h, COLOR_WIN_BG);
    gfx_draw_bevel(g_menu_client.pixels, w, h, 0, 0, w, h, 0);

    /* Left banner strip */
    gfx_fill_rect(g_menu_client.pixels, w, h, 2, 2, 22, h - 4, COLOR_WIN_TITLE);
    /* pseuDOS Psi brand logo in white */
    draw_psi_logo(g_menu_client.pixels, w, h, 6, h - 24, COLOR_WHITE);

    /* Menu items */
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int iy = 8 + i * 21;
        gfx_draw_string(g_menu_client.pixels, w, h, 30, iy, g_menu_items[i], COLOR_TEXT, 0, 1);
        if (i == 6) {
            /* Separator */
            gfx_fill_rect(g_menu_client.pixels, w, h, 26, iy + 18, w - 30, 1, COLOR_BTN_SHADOW);
            gfx_fill_rect(g_menu_client.pixels, w, h, 26, iy + 19, w - 30, 1, COLOR_WHITE);
        }
    }

    ntfs_client_damage(&g_menu_client, 0, 0, w, h);
}

static void close_desktop_error(void) {
    if (g_err_open) {
        ntfs_client_close(&g_err_client);
        g_err_open = 0;
        g_prev_err_left = 0;
    }
}

static void show_desktop_error(const char *l1, const char *l2) {
    close_desktop_error();
    int ew = 340, eh = 140;
    int ex = ((int)g_screen_width - ew) / 2;
    int ey = ((int)g_screen_height - eh) / 2;
    if (ex < 0) ex = 0;
    if (ey < 0) ey = 0;

    if (ntfs_client_connect(&g_err_client, "ErrorDialog", ew, eh, NTFS_WIN_FRAMELESS) == 0) {
        ntfs_msg_t smsg;
        memset(&smsg, 0, sizeof(smsg));
        smsg.type = NTFS_MSG_SET_GEOMETRY;
        smsg.window_id = g_err_client.window_id;
        smsg.x = ex;
        smsg.y = ey;
        smsg.width = ew;
        smsg.height = eh;
        smsg.flags = 2200; /* topmost z-order */
        syscall(SYS_SEND, g_err_client.sock, (uint64_t)(uintptr_t)&smsg, sizeof(smsg), 0, 0);

        ntfs_msg_t fmsg;
        memset(&fmsg, 0, sizeof(fmsg));
        fmsg.type = NTFS_MSG_FOCUS_WINDOW;
        fmsg.window_id = g_err_client.window_id;
        syscall(SYS_SEND, g_err_client.sock, (uint64_t)(uintptr_t)&fmsg, sizeof(fmsg), 0, 0);

        gfx_draw_error_dialog(g_err_client.pixels, ew, eh, 0, 0, ew, eh, "pseuDOS", l1, l2);
        ntfs_client_damage(&g_err_client, 0, 0, ew, eh);

        g_err_open = 1;
        g_prev_err_left = 0;
    }
}

static void handle_exec_result(int64_t ret) {
    if (ret == -ENOMEM) {
        show_desktop_error("Out of kernel", "heap!");
    } else if (ret == -ENOENT) {
        show_desktop_error("Application binary", "not found!");
    } else if (ret < 0) {
        show_desktop_error("Could not start", "application!");
    }
}

static void launch_app(int item_idx) {
    int64_t ret = 0;
    switch (item_idx) {
        case 0:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/shell.exe", 0, 0, 0, 0);
            break;
        case 1:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/filemgr.exe", 0, 0, 0, 0);
            break;
        case 2:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/sysmon.exe", 0, 0, 0, 0);
            break;
        case 3:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/calc.exe", 0, 0, 0, 0);
            break;
        case 4:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/notepad.exe", 0, 0, 0, 0);
            break;
        case 5:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/paint.exe", 0, 0, 0, 0);
            break;
        case 6:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/clock.exe", 0, 0, 0, 0);
            break;
        case 7:
            syscall(SYS_REBOOT, 0, 0, 0, 0, 0);
            return;
        case 8:
            syscall(SYS_SHUTDOWN, 0, 0, 0, 0, 0);
            return;
        default:
            return;
    }
    handle_exec_result(ret);
}

static void draw_context_menu(int hover_idx) {
    if (!g_ctx_client.pixels) return;
    int w = CTX_WIDTH;
    int h = CTX_HEIGHT;

    gfx_fill_rect(g_ctx_client.pixels, w, h, 0, 0, w, h, COLOR_WIN_BG);
    gfx_draw_bevel(g_ctx_client.pixels, w, h, 0, 0, w, h, 0);

    for (int i = 0; i < CTX_ITEM_COUNT; i++) {
        int iy = 4 + i * 21;
        if (i == hover_idx) {
            gfx_fill_rect(g_ctx_client.pixels, w, h, 2, iy - 1, w - 4, 20, 0x00000080);
            gfx_draw_string(g_ctx_client.pixels, w, h, 14, iy + 2, g_ctx_items[i], COLOR_WHITE, 0, 1);
        } else {
            gfx_draw_string(g_ctx_client.pixels, w, h, 14, iy + 2, g_ctx_items[i], COLOR_TEXT, 0, 1);
        }
        if (i == 5 || i == 7) {
            /* Separator between Paint & New Folder, and Refresh & Change Wallpaper */
            gfx_fill_rect(g_ctx_client.pixels, w, h, 6, iy + 20, w - 12, 1, COLOR_BTN_SHADOW);
            gfx_fill_rect(g_ctx_client.pixels, w, h, 6, iy + 21, w - 12, 1, COLOR_WHITE);
        }
    }
    ntfs_client_damage(&g_ctx_client, 0, 0, w, h);
}

static void close_context_menu(void) {
    if (g_ctx_open) {
        ntfs_client_close(&g_ctx_client);
        g_ctx_open = 0;
        g_ctx_hover = -1;
    }
}

static void launch_ctx_action(int idx) {
    int64_t ret = 0;
    switch (idx) {
        case 0:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/shell.exe", 0, 0, 0, 0);
            break;
        case 1:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/filemgr.exe", 0, 0, 0, 0);
            break;
        case 2:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/sysmon.exe", 0, 0, 0, 0);
            break;
        case 3:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/calc.exe", 0, 0, 0, 0);
            break;
        case 4:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/notepad.exe", 0, 0, 0, 0);
            break;
        case 5:
            ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/paint.exe", 0, 0, 0, 0);
            break;
        case 6: {
            static int folder_cnt = 1;
            char path[64];
            path[0] = '/'; path[1] = 'h'; path[2] = 'o'; path[3] = 'm'; path[4] = 'e';
            path[5] = '/'; path[6] = 'u'; path[7] = 's'; path[8] = 'e'; path[9] = 'r';
            path[10] = '/'; path[11] = 'N'; path[12] = 'e'; path[13] = 'w';
            path[14] = '_'; path[15] = 'F'; path[16] = 'o'; path[17] = 'l'; path[18] = 'd'; path[19] = 'e'; path[20] = 'r';
            path[21] = '_'; path[22] = '0' + (folder_cnt++ % 10); path[23] = '\0';
            syscall(SYS_MKDIR, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
            return;
        }
        case 7:
            draw_background();
            return;
        case 8:
            open_wallpaper_dialog();
            return;
        default:
            return;
    }
    handle_exec_result(ret);
}

static void open_context_menu(int screen_x, int screen_y) {
    close_context_menu();
    if (g_menu_open) toggle_start_menu();

    int px = screen_x;
    int py = screen_y;
    if (px + CTX_WIDTH > (int)g_screen_width) px = (int)g_screen_width - CTX_WIDTH;
    if (py + CTX_HEIGHT > (int)g_screen_height - (int)TASKBAR_HEIGHT) py = (int)g_screen_height - (int)TASKBAR_HEIGHT - CTX_HEIGHT;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    if (ntfs_client_connect(&g_ctx_client, "DesktopMenu", CTX_WIDTH, CTX_HEIGHT, NTFS_WIN_FRAMELESS) == 0) {
        ntfs_msg_t smsg;
        memset(&smsg, 0, sizeof(smsg));
        smsg.type = NTFS_MSG_SET_GEOMETRY;
        smsg.window_id = g_ctx_client.window_id;
        smsg.x = px;
        smsg.y = py;
        smsg.width = CTX_WIDTH;
        smsg.height = CTX_HEIGHT;
        smsg.flags = 2100;
        syscall(SYS_SEND, g_ctx_client.sock, (uint64_t)(uintptr_t)&smsg, sizeof(smsg), 0, 0);

        ntfs_msg_t fmsg;
        memset(&fmsg, 0, sizeof(fmsg));
        fmsg.type = NTFS_MSG_FOCUS_WINDOW;
        fmsg.window_id = g_ctx_client.window_id;
        syscall(SYS_SEND, g_ctx_client.sock, (uint64_t)(uintptr_t)&fmsg, sizeof(fmsg), 0, 0);

        g_ctx_open = 1;
        g_ctx_hover = -1;
        draw_context_menu(-1);
    }
}

static void toggle_start_menu(void) {
    if (g_ctx_open) close_context_menu();

    if (g_menu_open) {
        ntfs_client_close(&g_menu_client);
        g_menu_open = 0;
    } else {
        if (ntfs_client_connect(&g_menu_client, "StartMenu", MENU_WIDTH, MENU_HEIGHT, NTFS_WIN_FRAMELESS) == 0) {
            /* Position popup window above taskbar */
            ntfs_msg_t smsg;
            memset(&smsg, 0, sizeof(smsg));
            smsg.type = NTFS_MSG_SET_GEOMETRY;
            smsg.window_id = g_menu_client.window_id;
            smsg.x = 0;
            smsg.y = (int32_t)g_screen_height - (int32_t)TASKBAR_HEIGHT - (int32_t)MENU_HEIGHT;
            smsg.width = MENU_WIDTH;
            smsg.height = MENU_HEIGHT;
            smsg.flags = 2000; /* topmost z-order */
            syscall(SYS_SEND, g_menu_client.sock, (uint64_t)(uintptr_t)&smsg, sizeof(smsg), 0, 0);

            /* Focus start menu popup */
            ntfs_msg_t fmsg;
            memset(&fmsg, 0, sizeof(fmsg));
            fmsg.type = NTFS_MSG_FOCUS_WINDOW;
            fmsg.window_id = g_menu_client.window_id;
            syscall(SYS_SEND, g_menu_client.sock, (uint64_t)(uintptr_t)&fmsg, sizeof(fmsg), 0, 0);

            draw_start_menu();
            g_menu_open = 1;
        }
    }
    draw_taskbar();
}

void gshss_main(void) {
    puts("[gshss] Graphical Shell Subsystem starting up...\n");

    /* 0. Discover actual display dimensions from display server */
    int q_sock = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (q_sock >= 0) {
        sockaddr_un_t addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        for (size_t i = 0; NTFS_SOCKET_PATH[i] && i < sizeof(addr.sun_path) - 1; i++) {
            addr.sun_path[i] = NTFS_SOCKET_PATH[i];
        }
        for (int retry = 0; retry < 20; retry++) {
            if (syscall(SYS_CONNECT, q_sock, (uint64_t)(uintptr_t)&addr, sizeof(addr), 0, 0) == 0) {
                ntfs_msg_t qmsg;
                memset(&qmsg, 0, sizeof(qmsg));
                qmsg.type = NTFS_MSG_GET_SCREEN_INFO;
                syscall(SYS_SEND, q_sock, (uint64_t)(uintptr_t)&qmsg, sizeof(qmsg), 0, 0);
                ntfs_msg_t rmsg;
                if (syscall(SYS_RECV, q_sock, (uint64_t)(uintptr_t)&rmsg, sizeof(rmsg), 0, 0) == (int64_t)sizeof(rmsg)) {
                    if (rmsg.width > 0 && rmsg.height > 0) {
                        g_screen_width = rmsg.width;
                        g_screen_height = rmsg.height;
                    }
                }
                break;
            }
            syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
        }
        syscall(SYS_CLOSE, q_sock, 0, 0, 0, 0);
    }

    /* 1. Connect background surface */
    if (ntfs_client_connect(&g_bg_client, "Desktop", g_screen_width, g_screen_height, NTFS_WIN_BACKGROUND) < 0) {
        puts("[gshss] ERROR: failed to create desktop background surface\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    /* 2. Connect taskbar surface */
    if (ntfs_client_connect(&g_tb_client, "Taskbar", g_screen_width, TASKBAR_HEIGHT, NTFS_WIN_TASKBAR) < 0) {
        puts("[gshss] ERROR: failed to create taskbar surface\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    ntfs_msg_t tb_geom;
    memset(&tb_geom, 0, sizeof(tb_geom));
    tb_geom.type = NTFS_MSG_SET_GEOMETRY;
    tb_geom.window_id = g_tb_client.window_id;
    tb_geom.x = 0;
    tb_geom.y = (int32_t)g_screen_height - (int32_t)TASKBAR_HEIGHT;
    tb_geom.width = g_screen_width;
    tb_geom.height = TASKBAR_HEIGHT;
    tb_geom.flags = 1000;
    syscall(SYS_SEND, g_tb_client.sock, (uint64_t)(uintptr_t)&tb_geom, sizeof(tb_geom), 0, 0);

    draw_background();
    draw_taskbar();

    puts("[gshss] desktop and taskbar initialized\n");

    /* Signal readiness to splash screen */
    syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)"gshss_ready", 4096, 0, 0, 0);

    uint64_t last_clock_tick = 0;

    /* 3. Event and clock loop */
    while (1) {
        if (g_bg_client.sock < 0 || g_tb_client.sock < 0) {
            syscall(SYS_EXIT, 0, 0, 0, 0, 0);
            return;
        }

        /* Check background events (dismiss menus or open context menu on right click) */
        ntfs_msg_t bg_msg;
        while (ntfs_client_poll_event(&g_bg_client, &bg_msg)) {
            if (bg_msg.type == NTFS_MSG_MOUSE_EVENT) {
                uint8_t left_down = (bg_msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                uint8_t right_down = (bg_msg.buttons_or_key & NTFS_MOUSE_BTN_RIGHT);
                uint8_t left_click = (left_down && !g_prev_bg_left);
                uint8_t right_click = (right_down && !g_prev_bg_right);

                if (left_click) {
                    if (g_err_open) close_desktop_error();
                    if (g_wall_dlg_open) close_wallpaper_dialog();
                    if (g_menu_open) toggle_start_menu();
                    if (g_ctx_open) close_context_menu();
                } else if (right_click) {
                    if (g_err_open) close_desktop_error();
                    if (g_wall_dlg_open) close_wallpaper_dialog();
                    open_context_menu(bg_msg.x, bg_msg.y);
                }

                g_prev_bg_left = left_down;
                g_prev_bg_right = right_down;
            }
        }

        /* Check taskbar events */
        ntfs_msg_t tb_msg;
        while (ntfs_client_poll_event(&g_tb_client, &tb_msg)) {
            if (tb_msg.type == NTFS_MSG_MOUSE_EVENT) {
                uint8_t left_down = (tb_msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                uint8_t left_click = (left_down && !g_prev_tb_left);
                if (left_click) {
                    if (g_err_open) close_desktop_error();
                    if (g_wall_dlg_open) close_wallpaper_dialog();
                    if (g_ctx_open) close_context_menu();
                    /* Check Start Button click at (3, 3, 62, 22) */
                    if (tb_msg.x >= 3 && tb_msg.x <= 65 && tb_msg.y >= 3 && tb_msg.y <= 25) {
                        toggle_start_menu();
                    }
                }
                g_prev_tb_left = left_down;
            } else if (tb_msg.type == NTFS_MSG_KEY_EVENT) {
                if (tb_msg.buttons_or_key == NTFS_KEY_WIN) {
                    if (g_err_open) close_desktop_error();
                    toggle_start_menu();
                }
            }
        }

        /* Check start menu events if open */
        if (g_menu_open && g_menu_client.sock >= 0) {
            ntfs_msg_t menu_msg;
            while (ntfs_client_poll_event(&g_menu_client, &menu_msg)) {
                if (menu_msg.type == NTFS_MSG_MOUSE_EVENT) {
                    uint8_t left_down = (menu_msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                    uint8_t left_click = (left_down && !g_prev_menu_left);
                    if (left_click) {
                        if (menu_msg.x >= 24 && menu_msg.x < MENU_WIDTH) {
                            int item = (menu_msg.y - 6) / 21;
                            if (item >= 0 && item < MENU_ITEM_COUNT) {
                                toggle_start_menu();
                                launch_app(item);
                            }
                        }
                    }
                    g_prev_menu_left = left_down;
                } else if (menu_msg.type == NTFS_MSG_KEY_EVENT) {
                    if (menu_msg.buttons_or_key == NTFS_KEY_WIN || menu_msg.buttons_or_key == 27) {
                        toggle_start_menu();
                    }
                } else if (menu_msg.type == NTFS_MSG_CLOSE_WINDOW) {
                    toggle_start_menu();
                }
            }
        }

        /* Check desktop context menu events if open */
        if (g_ctx_open && g_ctx_client.sock >= 0) {
            ntfs_msg_t ctx_msg;
            while (ntfs_client_poll_event(&g_ctx_client, &ctx_msg)) {
                if (ctx_msg.type == NTFS_MSG_MOUSE_EVENT) {
                    uint8_t left_down = (ctx_msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                    uint8_t left_click = (left_down && !g_prev_ctx_left);

                    int hover = (ctx_msg.y - 4) / 21;
                    if (hover < 0 || hover >= CTX_ITEM_COUNT) hover = -1;
                    if (hover != g_ctx_hover) {
                        g_ctx_hover = hover;
                        draw_context_menu(hover);
                    }

                    if (left_click) {
                        if (hover >= 0 && hover < CTX_ITEM_COUNT) {
                            close_context_menu();
                            launch_ctx_action(hover);
                        } else {
                            close_context_menu();
                        }
                    }
                    g_prev_ctx_left = left_down;
                } else if (ctx_msg.type == NTFS_MSG_KEY_EVENT) {
                    if (ctx_msg.buttons_or_key == 27 || ctx_msg.buttons_or_key == NTFS_KEY_WIN) {
                        close_context_menu();
                    }
                } else if (ctx_msg.type == NTFS_MSG_CLOSE_WINDOW) {
                    close_context_menu();
                }
            }
        }

        /* Check error dialog events if open */
        if (g_err_open && g_err_client.sock >= 0) {
            ntfs_msg_t err_msg;
            while (ntfs_client_poll_event(&g_err_client, &err_msg)) {
                if (err_msg.type == NTFS_MSG_MOUSE_EVENT) {
                    uint8_t left_down = (err_msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                    uint8_t left_click = (left_down && !g_prev_err_left);
                    if (left_click) {
                        close_desktop_error();
                    }
                    g_prev_err_left = left_down;
                } else if (err_msg.type == NTFS_MSG_KEY_EVENT) {
                    if (err_msg.buttons_or_key == 13 || err_msg.buttons_or_key == 27 ||
                        err_msg.buttons_or_key == 32 || err_msg.buttons_or_key == '\n') {
                        close_desktop_error();
                    }
                } else if (err_msg.type == NTFS_MSG_CLOSE_WINDOW) {
                    close_desktop_error();
                }
            }
        }

        /* Check wallpaper dialog events if open */
        if (g_wall_dlg_open && g_wall_dlg_client.sock >= 0) {
            ntfs_msg_t dlg_msg;
            while (ntfs_client_poll_event(&g_wall_dlg_client, &dlg_msg)) {
                if (dlg_msg.type == NTFS_MSG_MOUSE_EVENT) {
                    uint8_t left_down = (dlg_msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                    uint8_t left_click = (left_down && !g_prev_dlg_left);
                    if (left_click) {
                        int mx = dlg_msg.x;
                        int my = dlg_msg.y;

                        /* 1. Close button [X] at (w - 18, 3, 15, 14) */
                        if (mx >= WALL_DLG_W - 18 && mx <= WALL_DLG_W - 3 && my >= 3 && my <= 17) {
                            close_wallpaper_dialog();
                        }
                        /* 2. Listbox items at x in [12, w - 12], y in [44, 44 + 180] */
                        else if (mx >= 12 && mx <= WALL_DLG_W - 12 && my >= 46 && my < 46 + 10 * 17) {
                            int idx = g_dlg_scroll + (my - 46) / 17;
                            if (idx >= 0 && idx < g_dlg_count) {
                                g_dlg_selected = idx;
                                draw_wallpaper_dialog();
                            }
                        }
                        /* 3. [ Apply ] button at (w - 165, btn_y, 70, 24) */
                        else if (mx >= WALL_DLG_W - 165 && mx <= WALL_DLG_W - 95 && my >= 238 && my <= 262) {
                            apply_selected_wallpaper();
                        }
                        /* 4. [ Cancel ] button at (w - 85, btn_y, 70, 24) */
                        else if (mx >= WALL_DLG_W - 85 && mx <= WALL_DLG_W - 15 && my >= 238 && my <= 262) {
                            close_wallpaper_dialog();
                        }
                    }
                    g_prev_dlg_left = left_down;
                } else if (dlg_msg.type == NTFS_MSG_KEY_EVENT) {
                    if (dlg_msg.buttons_or_key == 27) {
                        close_wallpaper_dialog();
                    } else if (dlg_msg.buttons_or_key == 13 || dlg_msg.buttons_or_key == '\n') {
                        apply_selected_wallpaper();
                    }
                } else if (dlg_msg.type == NTFS_MSG_CLOSE_WINDOW) {
                    close_wallpaper_dialog();
                }
            }
        }

        /* Clock update check every second */
        uint64_t cur_time = (uint64_t)syscall(SYS_TIME, 0, 0, 0, 0, 0);
        if (cur_time != last_clock_tick) {
            last_clock_tick = cur_time;
            draw_taskbar();
        }

        syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
    }
}
