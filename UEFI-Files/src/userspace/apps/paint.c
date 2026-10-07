#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

#define PAINT_DEFAULT_W 520
#define PAINT_DEFAULT_H 380

#define CANVAS_X 56
#define CANVAS_Y 8

/* Memory arena for image decoding */
#define ARENA_SIZE (4 * 1024 * 1024)
static uint8_t s_arena[ARENA_SIZE];
static size_t s_arena_offset = 0;

typedef struct {
    size_t size;
} alloc_hdr_t;

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
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

int abs(int x) {
    return (x < 0) ? -x : x;
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

static ntfs_client_t g_client;
static char s_current_image[256] = {0};

static const uint32_t g_palette[] = {
    COLOR_BLACK,
    COLOR_WHITE,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_YELLOW,
    0x0000FFFF, /* Cyan */
    0x00FF00FF, /* Magenta */
    0x00808080, /* Gray */
    0x00800000, /* Maroon */
    0x00008000, /* Dark Green */
    0x00000080  /* Navy */
};
#define NUM_COLORS 12

static uint32_t g_selected_color = COLOR_BLACK;
static uint8_t g_prev_left = 0;

static inline int get_paint_win_w(void) {
    int w = (int)g_client.width;
    return (w > 0) ? w : PAINT_DEFAULT_W;
}

static inline int get_paint_win_h(void) {
    int h = (int)g_client.height;
    return (h > 0) ? h : PAINT_DEFAULT_H;
}

static inline int get_canvas_w(void) {
    int w = get_paint_win_w() - CANVAS_X - 8;
    return (w > 10) ? w : 10;
}

static inline int get_canvas_h(void) {
    int h = get_paint_win_h() - 16;
    return (h > 10) ? h : 10;
}

static void clear_canvas(void) {
    if (!g_client.pixels) return;
    int win_w = get_paint_win_w();
    int win_h = get_paint_win_h();
    int canvas_w = get_canvas_w();
    int canvas_h = get_canvas_h();
    gfx_fill_rect(g_client.pixels, win_w, win_h, CANVAS_X + 1, CANVAS_Y + 1, canvas_w - 2, canvas_h - 2, COLOR_WHITE);
    ntfs_client_damage(&g_client, CANVAS_X, CANVAS_Y, canvas_w, canvas_h);
}

static void draw_brush(int cx, int cy, uint32_t color) {
    int win_w = get_paint_win_w();
    int win_h = get_paint_win_h();
    int canvas_w = get_canvas_w();
    int canvas_h = get_canvas_h();

    if (cx < CANVAS_X + 2 || cx >= CANVAS_X + canvas_w - 3 ||
        cy < CANVAS_Y + 2 || cy >= CANVAS_Y + canvas_h - 3) {
        return;
    }
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            gfx_draw_pixel(g_client.pixels, win_w, win_h, cx + dx, cy + dy, color);
        }
    }
    ntfs_client_damage(&g_client, cx - 2, cy - 2, 5, 5);
}

static char s_save_status[32] = "";
static int g_picker_open = 0;
static char g_picker_files[16][64];
static int g_picker_count = 0;
static int g_picker_selected = 0;
#define MODAL_MAX_W 320
#define MODAL_MAX_H 220
static uint32_t s_modal_bg[MODAL_MAX_W * MODAL_MAX_H];
static int s_picker_px = 0, s_picker_py = 0, s_picker_pw = 0, s_picker_ph = 0;

static int g_save_dlg_open = 0;
static char g_save_name[64] = "drawing.bmp";
static int g_save_name_len = 11;
static int s_save_dlg_px = 0, s_save_dlg_py = 0, s_save_dlg_pw = 0, s_save_dlg_ph = 0;

static size_t paint_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static char *paint_strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while (*src) *d++ = *src++;
    *d = '\0';
    return dest;
}

static int paint_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return (int)((unsigned char)*s1 - (unsigned char)*s2);
}

static void picker_scan_files(void) {
    g_picker_count = 0;
    g_picker_selected = 0;
    char list_buf[2048];
    int64_t n = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)"/home/user", (uint64_t)(uintptr_t)list_buf, sizeof(list_buf), 0, 0);
    if (n > 0) {
        char *tok = list_buf;
        for (int64_t i = 0; i <= n; i++) {
            if (list_buf[i] == '\n' || list_buf[i] == '\0') {
                list_buf[i] = '\0';
                if (tok[0] != '\0' && g_picker_count < 16) {
                    size_t tlen = paint_strlen(tok);
                    if (tlen > 4) {
                        const char *ext = tok + tlen - 4;
                        if (paint_strcasecmp(ext, ".bmp") == 0 ||
                            paint_strcasecmp(ext, ".png") == 0 ||
                            paint_strcasecmp(ext, ".jpg") == 0) {
                            ntfs_strncpy(g_picker_files[g_picker_count++], tok, 63);
                        }
                    }
                }
                tok = &list_buf[i + 1];
            }
        }
    }
    if (g_picker_count == 0) {
        ntfs_strncpy(g_picker_files[0], "sample.bmp", 63);
        ntfs_strncpy(g_picker_files[1], "sample.png", 63);
        ntfs_strncpy(g_picker_files[2], "image1.jpg", 63);
        ntfs_strncpy(g_picker_files[3], "image2.png", 63);
        g_picker_count = 4;
    }
}

static void render_toolbar(void);

static int paint_save_bmp(const char *path) {
    if (!path || !g_client.pixels) return -1;
    int canvas_w = get_canvas_w();
    int canvas_h = get_canvas_h();
    int win_w = get_paint_win_w();

    int cw = canvas_w - 2;
    int ch = canvas_h - 2;
    if (cw <= 0 || ch <= 0) return -1;

    size_t row_bytes = (size_t)cw * 4;
    size_t image_bytes = row_bytes * (size_t)ch;
    size_t file_size = 54 + image_bytes;

    arena_reset();
    uint8_t *bmp_buf = (uint8_t *)my_malloc(file_size);
    if (!bmp_buf) {
        ntfs_strncpy(s_save_status, "Err Mem", sizeof(s_save_status) - 1);
        render_toolbar();
        return -1;
    }

    /* 1. BITMAPFILEHEADER (14 bytes) */
    bmp_buf[0] = 'B';
    bmp_buf[1] = 'M';
    *(uint32_t *)(bmp_buf + 2) = (uint32_t)file_size;
    *(uint16_t *)(bmp_buf + 6) = 0;
    *(uint16_t *)(bmp_buf + 8) = 0;
    *(uint32_t *)(bmp_buf + 10) = 54; /* offset to pixel data */

    /* 2. BITMAPINFOHEADER (40 bytes) */
    *(uint32_t *)(bmp_buf + 14) = 40; /* header size */
    *(int32_t *)(bmp_buf + 18) = cw;   /* width */
    *(int32_t *)(bmp_buf + 22) = ch;   /* height (bottom-up) */
    *(uint16_t *)(bmp_buf + 26) = 1;   /* planes */
    *(uint16_t *)(bmp_buf + 28) = 32;  /* 32 bpp */
    *(uint32_t *)(bmp_buf + 30) = 0;   /* BI_RGB */
    *(uint32_t *)(bmp_buf + 34) = (uint32_t)image_bytes;
    *(int32_t *)(bmp_buf + 38) = 2835;
    *(int32_t *)(bmp_buf + 42) = 2835;
    *(uint32_t *)(bmp_buf + 46) = 0;
    *(uint32_t *)(bmp_buf + 50) = 0;

    /* 3. Pixel data copied bottom-to-top */
    uint8_t *dst = bmp_buf + 54;
    for (int y = ch - 1; y >= 0; y--) {
        int src_y = CANVAS_Y + 1 + y;
        uint32_t *src_row = &g_client.pixels[src_y * win_w + (CANVAS_X + 1)];
        memcpy(dst, src_row, row_bytes);
        dst += row_bytes;
    }

    int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)bmp_buf, file_size, 0, 0);
    arena_reset();

    if (wr > 0) {
        ntfs_strncpy(s_save_status, "Saved!", sizeof(s_save_status) - 1);
        render_toolbar();
        return 0;
    } else {
        ntfs_strncpy(s_save_status, "Failed!", sizeof(s_save_status) - 1);
        render_toolbar();
        return -1;
    }
}

static void render_picker_dialog(int win_w, int win_h) {
    int pw = s_picker_pw;
    int ph = s_picker_ph;
    int px = s_picker_px;
    int py = s_picker_py;

    /* Window frame */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px, py, pw, ph, COLOR_WIN_BG);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, px, py, pw, ph, 0);

    /* Title bar */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + 2, py + 2, pw - 4, 20, COLOR_WIN_TITLE);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + 6, py + 4, "Open Image (/home/user)", COLOR_TITLE_TEXT, 0, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + pw - 16, py + 4, "X", COLOR_TITLE_TEXT, 0, 1);

    /* Listbox */
    int lx = px + 10;
    int ly = py + 26;
    int lw = pw - 20;
    int lh = 132;
    gfx_draw_bevel(g_client.pixels, win_w, win_h, lx, ly, lw, lh, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, lx + 1, ly + 1, lw - 2, lh - 2, COLOR_WHITE);

    for (int i = 0; i < g_picker_count && i < 8; i++) {
        int iy = ly + 2 + i * 16;
        if (i == g_picker_selected) {
            gfx_fill_rect(g_client.pixels, win_w, win_h, lx + 2, iy, lw - 4, 16, 0x00000080);
            gfx_draw_string(g_client.pixels, win_w, win_h, lx + 6, iy + 2, g_picker_files[i], COLOR_WHITE, 0, 1);
        } else {
            gfx_draw_string(g_client.pixels, win_w, win_h, lx + 6, iy + 2, g_picker_files[i], COLOR_BLACK, 0, 1);
        }
    }

    /* [ Open ] button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + pw - 145, py + ph - 32, 60, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, px + pw - 145, py + ph - 32, 60, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + pw - 133, py + ph - 28, "Open", COLOR_TEXT, 0, 1);

    /* [ Cancel ] button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + pw - 75, py + ph - 32, 60, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, px + pw - 75, py + ph - 32, 60, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + pw - 67, py + ph - 28, "Cancel", COLOR_TEXT, 0, 1);
}

static void close_picker(void) {
    if (!g_picker_open) return;
    int win_w = get_paint_win_w();
    for (int y = 0; y < s_picker_ph; y++) {
        for (int x = 0; x < s_picker_pw; x++) {
            g_client.pixels[(s_picker_py + y) * win_w + (s_picker_px + x)] = s_modal_bg[y * s_picker_pw + x];
        }
    }
    g_picker_open = 0;
    render_toolbar();
    ntfs_client_damage(&g_client, s_picker_px, s_picker_py, s_picker_pw, s_picker_ph);
}

static void open_picker(void) {
    if (g_picker_open || g_save_dlg_open) return;
    picker_scan_files();
    int win_w = get_paint_win_w();
    int win_h = get_paint_win_h();
    s_picker_pw = 300;
    s_picker_ph = 200;
    s_picker_px = (win_w - s_picker_pw) / 2;
    s_picker_py = (win_h - s_picker_ph) / 2;
    if (s_picker_px < CANVAS_X) s_picker_px = CANVAS_X;

    for (int y = 0; y < s_picker_ph; y++) {
        for (int x = 0; x < s_picker_pw; x++) {
            s_modal_bg[y * s_picker_pw + x] = g_client.pixels[(s_picker_py + y) * win_w + (s_picker_px + x)];
        }
    }
    g_picker_open = 1;
    render_picker_dialog(win_w, win_h);
    render_toolbar();
    ntfs_client_damage(&g_client, s_picker_px, s_picker_py, s_picker_pw, s_picker_ph);
}

static void render_save_dialog(int win_w, int win_h) {
    int pw = s_save_dlg_pw;
    int ph = s_save_dlg_ph;
    int px = s_save_dlg_px;
    int py = s_save_dlg_py;

    /* Window frame */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px, py, pw, ph, COLOR_WIN_BG);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, px, py, pw, ph, 0);

    /* Title bar */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + 2, py + 2, pw - 4, 20, COLOR_WIN_TITLE);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + 6, py + 4, "Save Image (/home/user)", COLOR_TITLE_TEXT, 0, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + pw - 16, py + 4, "X", COLOR_TITLE_TEXT, 0, 1);

    /* Label */
    gfx_draw_string(g_client.pixels, win_w, win_h, px + 12, py + 26, "File name (.bmp):", COLOR_TEXT, 0, 1);

    /* Text input field */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, px + 10, py + 44, pw - 20, 24, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + 11, py + 45, pw - 22, 22, COLOR_WHITE);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + 16, py + 48, g_save_name, COLOR_TEXT, 0, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + 16 + g_save_name_len * 8, py + 47, 2, 16, COLOR_BLACK);

    /* [ Save ] button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + pw - 145, py + ph - 32, 60, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, px + pw - 145, py + ph - 32, 60, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + pw - 133, py + ph - 28, "Save", COLOR_TEXT, 0, 1);

    /* [ Cancel ] button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, px + pw - 75, py + ph - 32, 60, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, px + pw - 75, py + ph - 32, 60, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, px + pw - 67, py + ph - 28, "Cancel", COLOR_TEXT, 0, 1);
}

static void close_save_dialog(void) {
    if (!g_save_dlg_open) return;
    int win_w = get_paint_win_w();
    for (int y = 0; y < s_save_dlg_ph; y++) {
        for (int x = 0; x < s_save_dlg_pw; x++) {
            g_client.pixels[(s_save_dlg_py + y) * win_w + (s_save_dlg_px + x)] = s_modal_bg[y * s_save_dlg_pw + x];
        }
    }
    g_save_dlg_open = 0;
    render_toolbar();
    ntfs_client_damage(&g_client, s_save_dlg_px, s_save_dlg_py, s_save_dlg_pw, s_save_dlg_ph);
}

static void open_save_dialog(void) {
    if (g_save_dlg_open || g_picker_open) return;
    int win_w = get_paint_win_w();
    int win_h = get_paint_win_h();
    s_save_dlg_pw = 300;
    s_save_dlg_ph = 120;
    s_save_dlg_px = (win_w - s_save_dlg_pw) / 2;
    s_save_dlg_py = (win_h - s_save_dlg_ph) / 2;
    if (s_save_dlg_px < CANVAS_X) s_save_dlg_px = CANVAS_X;

    for (int y = 0; y < s_save_dlg_ph; y++) {
        for (int x = 0; x < s_save_dlg_pw; x++) {
            s_modal_bg[y * s_save_dlg_pw + x] = g_client.pixels[(s_save_dlg_py + y) * win_w + (s_save_dlg_px + x)];
        }
    }
    g_save_dlg_open = 1;
    render_save_dialog(win_w, win_h);
    render_toolbar();
    ntfs_client_damage(&g_client, s_save_dlg_px, s_save_dlg_py, s_save_dlg_pw, s_save_dlg_ph);
}

static void do_paint_save(void) {
    if (g_save_name_len == 0) {
        ntfs_strncpy(g_save_name, "drawing.bmp", sizeof(g_save_name) - 1);
        g_save_name_len = 11;
    }
    char full[128] = "/home/user/";
    paint_strcat(full, g_save_name);

    size_t len = paint_strlen(full);
    int has_bmp = 0;
    if (len >= 4 && paint_strcasecmp(full + len - 4, ".bmp") == 0) {
        has_bmp = 1;
    }
    if (!has_bmp && len < 120) {
        paint_strcat(full, ".bmp");
    }

    close_save_dialog();
    paint_save_bmp(full);
}

static void render_toolbar(void) {
    if (!g_client.pixels) return;
    int win_w = get_paint_win_w();
    int win_h = get_paint_win_h();

    /* Left toolbar border */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 4, 4, 48, win_h - 8, 0);

    /* Color Palette */
    for (int i = 0; i < NUM_COLORS; i++) {
        int cx = 8 + (i % 2) * 20;
        int cy = 10 + (i / 2) * 20;
        gfx_fill_rect(g_client.pixels, win_w, win_h, cx, cy, 18, 18, g_palette[i]);
        gfx_draw_bevel(g_client.pixels, win_w, win_h, cx, cy, 18, 18, (g_palette[i] == g_selected_color) ? 1 : 0);
    }

    /* Selected Color Swatch */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 8, 136, 38, 38, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, 10, 138, 34, 34, g_selected_color);

    /* Clear Button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 6, 182, 42, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 6, 182, 42, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 11, 186, "Clr", COLOR_TEXT, 0, 1);

    /* Open Button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 6, 208, 42, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 6, 208, 42, 22, g_picker_open ? 1 : 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 9, 212, "Open", COLOR_TEXT, 0, 1);

    /* Save Button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 6, 234, 42, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 6, 234, 42, 22, g_save_dlg_open ? 1 : 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 9, 238, "Save", COLOR_TEXT, 0, 1);

    /* Status Text */
    if (s_save_status[0] != '\0') {
        gfx_draw_string(g_client.pixels, win_w, win_h, 6, 260, s_save_status, 0x00008000, 0, 1);
    }

    ntfs_client_damage(&g_client, 0, 0, CANVAS_X, win_h);
}

static void render_paint_ui(void) {
    if (!g_client.pixels) return;
    int win_w = get_paint_win_w();
    int win_h = get_paint_win_h();
    int canvas_w = get_canvas_w();
    int canvas_h = get_canvas_h();

    /* Window background */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, 0, win_w, win_h, COLOR_WIN_BG);

    render_toolbar();

    /* Canvas Frame */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, CANVAS_X, CANVAS_Y, canvas_w, canvas_h, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, CANVAS_X + 1, CANVAS_Y + 1, canvas_w - 2, canvas_h - 2, COLOR_WHITE);

    if (g_picker_open) {
        render_picker_dialog(win_w, win_h);
    } else if (g_save_dlg_open) {
        render_save_dialog(win_w, win_h);
    }

    ntfs_client_damage(&g_client, 0, 0, win_w, win_h);
}

/* Open, decode, crush (downsample), and plot image onto canvas */
static int paint_open_image(const char *path) {
    if (!path || path[0] == '\0' || !g_client.pixels) return -1;

    vfs_stat_t st;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0) {
        return -1;
    }
    if (st.size == 0) return -1;

    arena_reset();
    uint8_t *file_data = (uint8_t *)my_malloc(st.size + 16);
    if (!file_data) {
        arena_reset();
        return -1;
    }

    int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)file_data, st.size, 0, 0);
    if (rd <= 0) {
        arena_reset();
        return -1;
    }

    int img_w = 0, img_h = 0, channels = 0;
    unsigned char *decoded = stbi_load_from_memory(file_data, (int)rd, &img_w, &img_h, &channels, 4);
    if (!decoded || img_w <= 0 || img_h <= 0) {
        arena_reset();
        return -1;
    }

    int win_w = get_paint_win_w();
    int win_h = get_paint_win_h();
    int canvas_w = get_canvas_w();
    int canvas_h = get_canvas_h();

    /* Clear canvas */
    clear_canvas();

    /* Aspect-ratio preserving crush (downscale) to fit within canvas */
    int max_w = canvas_w - 4;
    int max_h = canvas_h - 4;
    int dst_w = img_w;
    int dst_h = img_h;

    if (dst_w > max_w || dst_h > max_h) {
        if ((int64_t)dst_w * max_h > (int64_t)dst_h * max_w) {
            dst_w = max_w;
            dst_h = (int)((int64_t)img_h * max_w / img_w);
            if (dst_h < 1) dst_h = 1;
        } else {
            dst_h = max_h;
            dst_w = (int)((int64_t)img_w * max_h / img_h);
            if (dst_w < 1) dst_w = 1;
        }
    }

    int start_x = CANVAS_X + 1 + (canvas_w - 2 - dst_w) / 2;
    int start_y = CANVAS_Y + 1 + (canvas_h - 2 - dst_h) / 2;

    /* Plot pixels with nearest-neighbor downsampling */
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = (int)((int64_t)dy * img_h / dst_h);
        if (sy >= img_h) sy = img_h - 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (int)((int64_t)dx * img_w / dst_w);
            if (sx >= img_w) sx = img_w - 1;

            const uint8_t *px = decoded + (sy * img_w + sx) * 4;
            /* If transparent (alpha < 128), keep white background */
            if (px[3] < 128) continue;

            uint32_t col = COLOR_RGB(px[0], px[1], px[2]);
            gfx_draw_pixel(g_client.pixels, win_w, win_h, start_x + dx, start_y + dy, col);
        }
    }

    /* Redraw damaged canvas region */
    ntfs_client_damage(&g_client, CANVAS_X, CANVAS_Y, canvas_w, canvas_h);

    /* Record currently opened image and free arena */
    ntfs_strncpy(s_current_image, path, sizeof(s_current_image) - 1);
    arena_reset();
    return 0;
}

void paint_main(void) {
    char arg_buf[256];
    arg_buf[0] = '\0';
    syscall(SYS_GET_ARGS, (uint64_t)(uintptr_t)arg_buf, sizeof(arg_buf), 0, 0, 0);

    /* Trim leading whitespace */
    char *image_path = arg_buf;
    while (*image_path == ' ' || *image_path == '\t') image_path++;

    char win_title[64];
    if (image_path[0] != '\0') {
        const char *fname = image_path;
        for (const char *p = image_path; *p; p++) {
            if (*p == '/' || *p == '\\') fname = p + 1;
        }
        char *t = win_title;
        const char *pfx = "Paint - ";
        while (*pfx) *t++ = *pfx++;
        for (int i = 0; fname[i] && i < 32; i++) *t++ = fname[i];
        *t = '\0';
    } else {
        ntfs_strncpy(win_title, "Paint - Untitled", sizeof(win_title) - 1);
    }

    if (ntfs_client_connect(&g_client, win_title, PAINT_DEFAULT_W, PAINT_DEFAULT_H, NTFS_WIN_NORMAL) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    render_paint_ui();

    /* If image path argument was provided, load and crush it onto canvas */
    if (image_path[0] != '\0') {
        paint_open_image(image_path);
    }

    ntfs_msg_t msg;
    while (1) {
        while (ntfs_client_poll_event(&g_client, &msg)) {
            if (msg.type == NTFS_MSG_MOUSE_EVENT) {
                int mx = msg.x;
                int my = msg.y;
                int canvas_w = get_canvas_w();
                int canvas_h = get_canvas_h();
                uint8_t left_down = (msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                uint8_t left_click = (left_down && !g_prev_left);

                if (g_save_dlg_open) {
                    int pw = s_save_dlg_pw;
                    int ph = s_save_dlg_ph;
                    int px = s_save_dlg_px;
                    int py = s_save_dlg_py;
                    if (left_click) {
                        /* Check [ Save ] button at (px + pw - 145, py + ph - 32, 60, 22) */
                        if (mx >= px + pw - 145 && mx <= px + pw - 85 && my >= py + ph - 32 && my <= py + ph - 10) {
                            do_paint_save();
                        }
                        /* Check [ Cancel ] button at (px + pw - 75, py + ph - 32, 60, 22) */
                        else if (mx >= px + pw - 75 && mx <= px + pw - 15 && my >= py + ph - 32 && my <= py + ph - 10) {
                            close_save_dialog();
                        }
                        /* Check Titlebar X button at (px + pw - 20, py + 2, 18, 18) */
                        else if (mx >= px + pw - 20 && mx <= px + pw - 2 && my >= py + 2 && my <= py + 20) {
                            close_save_dialog();
                        }
                    }
                } else if (g_picker_open) {
                    int pw = s_picker_pw;
                    int ph = s_picker_ph;
                    int px = s_picker_px;
                    int py = s_picker_py;
                    if (left_click) {
                        /* Check listbox row clicks */
                        int lx = px + 10;
                        int ly = py + 26;
                        int lw = pw - 20;
                        int lh = 132;
                        if (mx >= lx && mx < lx + lw && my >= ly && my < ly + lh) {
                            int sel = (my - (ly + 2)) / 16;
                            if (sel >= 0 && sel < g_picker_count && sel < 8) {
                                g_picker_selected = sel;
                                render_picker_dialog(get_paint_win_w(), get_paint_win_h());
                                ntfs_client_damage(&g_client, px, py, pw, ph);
                            }
                        }
                        /* Check [ Open ] button at (px + pw - 145, py + ph - 32, 60, 22) */
                        else if (mx >= px + pw - 145 && mx <= px + pw - 85 && my >= py + ph - 32 && my <= py + ph - 10) {
                            if (g_picker_count > 0 && g_picker_selected < g_picker_count) {
                                char full_path[128] = "/home/user/";
                                paint_strcat(full_path, g_picker_files[g_picker_selected]);
                                close_picker();
                                paint_open_image(full_path);
                            }
                        }
                        /* Check [ Cancel ] button at (px + pw - 75, py + ph - 32, 60, 22) */
                        else if (mx >= px + pw - 75 && mx <= px + pw - 15 && my >= py + ph - 32 && my <= py + ph - 10) {
                            close_picker();
                        }
                        /* Check Titlebar X button at (px + pw - 20, py + 2, 18, 18) */
                        else if (mx >= px + pw - 20 && mx <= px + pw - 2 && my >= py + 2 && my <= py + 20) {
                            close_picker();
                        }
                    }
                } else {
                    if (left_click) {
                        /* Check color swatch click */
                        for (int i = 0; i < NUM_COLORS; i++) {
                            int cx = 8 + (i % 2) * 20;
                            int cy = 10 + (i / 2) * 20;
                            if (mx >= cx && mx < cx + 18 && my >= cy && my < cy + 18) {
                                if (g_selected_color != g_palette[i]) {
                                    g_selected_color = g_palette[i];
                                    render_toolbar();
                                }
                                break;
                            }
                        }

                        /* Check clear button at (6, 182, 42, 22) */
                        if (mx >= 4 && mx <= 50 && my >= 180 && my <= 206) {
                            s_current_image[0] = '\0';
                            clear_canvas();
                        }

                        /* Check open button at (6, 208, 42, 22) */
                        else if (mx >= 4 && mx <= 50 && my >= 206 && my <= 232) {
                            open_picker();
                        }

                        /* Check save button at (6, 234, 42, 22) */
                        else if (mx >= 4 && mx <= 50 && my >= 232 && my <= 258) {
                            open_save_dialog();
                        }
                    }

                    /* Drawing on canvas */
                    if (left_down) {
                        if (mx >= CANVAS_X + 1 && mx < CANVAS_X + canvas_w - 1 &&
                            my >= CANVAS_Y + 1 && my < CANVAS_Y + canvas_h - 1) {
                            draw_brush(mx, my, g_selected_color);
                        }
                    }
                }

                g_prev_left = left_down;
            } else if (msg.type == NTFS_MSG_KEY_EVENT) {
                char ch = (char)(msg.buttons_or_key & 0xFF);
                if (g_save_dlg_open) {
                    if (ch == 27) { /* ESC */
                        close_save_dialog();
                    } else if (ch == '\r' || ch == '\n') {
                        do_paint_save();
                    } else if (ch == '\b') {
                        if (g_save_name_len > 0) {
                            g_save_name[--g_save_name_len] = '\0';
                            render_save_dialog(get_paint_win_w(), get_paint_win_h());
                            ntfs_client_damage(&g_client, s_save_dlg_px, s_save_dlg_py, s_save_dlg_pw, s_save_dlg_ph);
                        }
                    } else if (ch >= 32 && ch <= 126 && ch != '/' && ch != '\\') {
                        if (g_save_name_len < (int)sizeof(g_save_name) - 5) {
                            g_save_name[g_save_name_len++] = ch;
                            g_save_name[g_save_name_len] = '\0';
                            render_save_dialog(get_paint_win_w(), get_paint_win_h());
                            ntfs_client_damage(&g_client, s_save_dlg_px, s_save_dlg_py, s_save_dlg_pw, s_save_dlg_ph);
                        }
                    }
                } else if (g_picker_open) {
                    if (ch == 27) {
                        close_picker();
                    } else if (ch == '\r' || ch == '\n') {
                        if (g_picker_count > 0 && g_picker_selected < g_picker_count) {
                            char full_path[128] = "/home/user/";
                            paint_strcat(full_path, g_picker_files[g_picker_selected]);
                            close_picker();
                            paint_open_image(full_path);
                        }
                    }
                }
            } else if (msg.type == NTFS_MSG_WINDOW_RESIZED) {
                render_paint_ui();
                if (s_current_image[0] != '\0') {
                    paint_open_image(s_current_image);
                }
            } else if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                ntfs_client_close(&g_client);
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                return;
            }
        }

        syscall(SYS_SLEEP, 20, 0, 0, 0, 0);
    }
}
