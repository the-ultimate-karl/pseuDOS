#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

#define VIEW_DEFAULT_W 640
#define VIEW_DEFAULT_H 480
#define TOOLBAR_H      32

/* 36 MB Memory arena for image decoding */
#define ARENA_SIZE (36 * 1024 * 1024)
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

int abs(int x) {
    return (x < 0) ? -x : x;
}

static void *arena_alloc(size_t sz) {
    size_t total = sizeof(alloc_hdr_t) + ((sz + 15) & ~15);
    if (s_arena_offset + total > ARENA_SIZE) return NULL;
    alloc_hdr_t *hdr = (alloc_hdr_t *)(s_arena + s_arena_offset);
    hdr->size = sz;
    s_arena_offset += total;
    return (void *)(hdr + 1);
}

static void *arena_realloc(void *ptr, size_t sz) {
    if (!ptr) return arena_alloc(sz);
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    void *new_ptr = arena_alloc(sz);
    if (!new_ptr) return NULL;
    size_t copy_sz = (hdr->size < sz) ? hdr->size : sz;
    memcpy(new_ptr, ptr, copy_sz);
    return new_ptr;
}

static void arena_free(void *ptr) {
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
#define STBI_MALLOC(sz)           arena_alloc(sz)
#define STBI_REALLOC(p,newsz)     arena_realloc(p,newsz)
#define STBI_FREE(p)              arena_free(p)
#define STBI_ASSERT(x)            ((void)0)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static ntfs_client_t g_client;
static char g_current_path[256];
static char g_folder_path[256];
static char g_file_list[64][64];
static int g_file_count = 0;
static int g_current_idx = -1;

/* Decoded image state */
static int g_img_loaded = 0;
static int g_img_w = 0;
static int g_img_h = 0;
static uint32_t *g_img_pixels = NULL; /* Persisted in arena or reloaded */
static int g_zoom_1to1 = 0;

static size_t view_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static int view_strcasecmp(const char *s1, const char *s2) {
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

static int is_image_extension(const char *name) {
    size_t len = view_strlen(name);
    if (len < 4) return 0;
    const char *ext = name + len - 4;
    if (view_strcasecmp(ext, ".bmp") == 0 ||
        view_strcasecmp(ext, ".png") == 0 ||
        view_strcasecmp(ext, ".jpg") == 0) {
        return 1;
    }
    if (len >= 5 && view_strcasecmp(name + len - 5, ".jpeg") == 0) {
        return 1;
    }
    return 0;
}

static void scan_directory(const char *dir_path, const char *initial_file) {
    g_file_count = 0;
    g_current_idx = -1;

    static char names_buf[4096];
    int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)dir_path, (uint64_t)(uintptr_t)names_buf, sizeof(names_buf), 0, 0);
    if (nbytes > 0) {
        size_t pos = 0;
        while (pos < (size_t)nbytes && g_file_count < 64) {
            const char *name = &names_buf[pos];
            size_t nlen = view_strlen(name);
            if (nlen == 0) break;
            pos += nlen + 1;

            if (is_image_extension(name)) {
                ntfs_strncpy(g_file_list[g_file_count], name, 63);
                if (initial_file && view_strcasecmp(name, initial_file) == 0) {
                    g_current_idx = g_file_count;
                }
                g_file_count++;
            }
        }
    }
    if (g_current_idx < 0 && g_file_count > 0) {
        g_current_idx = 0;
    }
}

static void u32_to_str(uint32_t val, char *buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[16];
    int ti = 0;
    while (val > 0) {
        tmp[ti++] = '0' + (val % 10);
        val /= 10;
    }
    int i = 0;
    while (ti > 0) buf[i++] = tmp[--ti];
    buf[i] = '\0';
}

static int load_image_file(const char *path) {
    g_img_loaded = 0;
    g_img_pixels = NULL;
    arena_reset();

    vfs_stat_t st;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0) {
        return -1;
    }
    if (st.size == 0) return -1;

    uint8_t *file_data = (uint8_t *)arena_alloc(st.size + 16);
    if (!file_data) return -1;

    int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)file_data, st.size, 0, 0);
    if (rd <= 0) return -1;

    int w = 0, h = 0, channels = 0;
    unsigned char *decoded = stbi_load_from_memory(file_data, (int)rd, &w, &h, &channels, 4);
    if (!decoded || w <= 0 || h <= 0) {
        return -1;
    }

    g_img_w = w;
    g_img_h = h;
    /* Convert decoded RGBA buffer to 32-bit ARGB/XRGB in arena */
    size_t pixel_bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
    g_img_pixels = (uint32_t *)arena_alloc(pixel_bytes);
    if (!g_img_pixels) {
        /* Fallback: use decoded pointer directly if layout matches */
        g_img_pixels = (uint32_t *)decoded;
    } else {
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
            const uint8_t *px = decoded + i * 4;
            g_img_pixels[i] = COLOR_RGB(px[0], px[1], px[2]);
        }
    }

    g_img_loaded = 1;
    return 0;
}

static void render_view_ui(void) {
    if (!g_client.pixels) return;

    int win_w = (int)g_client.width;
    int win_h = (int)g_client.height;
    if (win_w <= 0 || win_h <= 0) return;

    int view_h = win_h - TOOLBAR_H;
    if (view_h < 10) view_h = 10;

    /* 1. Clear viewport area to dark retro backdrop (dark gray 0x202020) */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, 0, win_w, view_h, 0x00202020);

    /* 2. Render Image */
    if (g_img_loaded && g_img_pixels && g_img_w > 0 && g_img_h > 0) {
        int dst_w, dst_h;
        if (g_zoom_1to1) {
            dst_w = g_img_w;
            dst_h = g_img_h;
        } else {
            /* Scale to fit within view area */
            int max_w = win_w - 8;
            int max_h = view_h - 8;
            if (max_w < 1) max_w = 1;
            if (max_h < 1) max_h = 1;

            if (g_img_w <= max_w && g_img_h <= max_h) {
                dst_w = g_img_w;
                dst_h = g_img_h;
            } else {
                if ((int64_t)g_img_w * max_h > (int64_t)g_img_h * max_w) {
                    dst_w = max_w;
                    dst_h = (int)((int64_t)g_img_h * max_w / g_img_w);
                    if (dst_h < 1) dst_h = 1;
                } else {
                    dst_h = max_h;
                    dst_w = (int)((int64_t)g_img_w * max_h / g_img_h);
                    if (dst_w < 1) dst_w = 1;
                }
            }
        }

        int start_x = (win_w - dst_w) / 2;
        int start_y = (view_h - dst_h) / 2;

        /* Nearest-neighbor blit with clipping */
        for (int dy = 0; dy < dst_h; dy++) {
            int py = start_y + dy;
            if (py < 0 || py >= view_h) continue;

            int sy = (int)((int64_t)dy * g_img_h / dst_h);
            if (sy >= g_img_h) sy = g_img_h - 1;

            const uint32_t *src_row = &g_img_pixels[sy * g_img_w];
            uint32_t *dst_row = &g_client.pixels[py * win_w];

            for (int dx = 0; dx < dst_w; dx++) {
                int px = start_x + dx;
                if (px < 0 || px >= win_w) continue;

                int sx = (int)((int64_t)dx * g_img_w / dst_w);
                if (sx >= g_img_w) sx = g_img_w - 1;

                dst_row[px] = src_row[sx];
            }
        }
    } else {
        const char *err = "No image loaded or unable to decode.";
        gfx_draw_string(g_client.pixels, win_w, win_h, (win_w - 280) / 2, view_h / 2, err, COLOR_WHITE, 0, 1);
    }

    /* 3. Bottom Toolbar (Classic Windows 3D Panel) */
    int tb_y = view_h;
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, tb_y, win_w, TOOLBAR_H, COLOR_WIN_BG);
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, tb_y, win_w, 1, COLOR_WHITE);
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, tb_y + 1, win_w, 1, COLOR_BTN_LIGHT);

    /* [ < Prev ] Button: (6, tb_y + 4, 60, 22) */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 6, tb_y + 4, 60, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 6, tb_y + 4, 60, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 15, tb_y + 7, "< Prev", COLOR_BLACK, 0, 1);

    /* [ Next > ] Button: (72, tb_y + 4, 60, 22) */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 72, tb_y + 4, 60, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 72, tb_y + 4, 60, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 81, tb_y + 7, "Next >", COLOR_BLACK, 0, 1);

    /* [ Zoom Mode ] Button: (138, tb_y + 4, 76, 22) */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 138, tb_y + 4, 76, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 138, tb_y + 4, 76, 22, g_zoom_1to1 ? 1 : 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 146, tb_y + 7, g_zoom_1to1 ? "1:1 Zoom" : "Fit View", COLOR_BLACK, 0, 1);

    /* [ Set Wall ] Button: (220, tb_y + 4, 86, 22) */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 220, tb_y + 4, 86, 22, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 220, tb_y + 4, 86, 22, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 228, tb_y + 7, "Set Wall", COLOR_BLACK, 0, 1);

    /* Image Info Status Label (Right aligned) */
    if (g_img_loaded) {
        char info[64];
        char wbuf[12], hbuf[12];
        u32_to_str(g_img_w, wbuf);
        u32_to_str(g_img_h, hbuf);

        char *ip = info;
        char *wp = wbuf;
        while (*wp) *ip++ = *wp++;
        *ip++ = ' '; *ip++ = 'x'; *ip++ = ' ';
        char *hp = hbuf;
        while (*hp) *ip++ = *hp++;
        *ip++ = ' '; *ip++ = 'p'; *ip++ = 'x';
        *ip = '\0';

        int info_len = (int)view_strlen(info);
        int info_x = win_w - (info_len * 8) - 14;
        if (info_x > 320) {
            gfx_draw_string(g_client.pixels, win_w, win_h, info_x, tb_y + 8, info, COLOR_BTN_DKSHADOW, 0, 1);
        }
    }

    ntfs_client_damage(&g_client, 0, 0, win_w, win_h);
}

static void update_title(void) {
    /* Title update is handled at window creation.
     * Sending SET_GEOMETRY without current coords/z-order displaces the window. */
}

static void show_current_file(void) {
    if (g_current_idx < 0 || g_current_idx >= g_file_count) return;

    char full[256];
    ntfs_strncpy(full, g_folder_path, sizeof(full) - 1);
    size_t flen = view_strlen(full);
    if (flen > 0 && full[flen - 1] != '/') {
        full[flen++] = '/';
        full[flen] = '\0';
    }
    const char *fn = g_file_list[g_current_idx];
    size_t nlen = view_strlen(fn);
    for (size_t i = 0; i < nlen && flen < sizeof(full) - 1; i++) {
        full[flen++] = fn[i];
    }
    full[flen] = '\0';

    ntfs_strncpy(g_current_path, full, sizeof(g_current_path) - 1);
    load_image_file(g_current_path);
    update_title();
    render_view_ui();
}

static void next_image(void) {
    if (g_file_count <= 0) return;
    g_current_idx = (g_current_idx + 1) % g_file_count;
    show_current_file();
}

static void prev_image(void) {
    if (g_file_count <= 0) return;
    g_current_idx = (g_current_idx - 1 + g_file_count) % g_file_count;
    show_current_file();
}

static void set_as_wallpaper(void) {
    if (g_current_path[0] != '\0') {
        syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)"/home/user/wallpaper.cfg",
                (uint64_t)(uintptr_t)g_current_path, view_strlen(g_current_path), 0, 0);
    }
}

void view_main(void) {
    char arg_buf[256];
    arg_buf[0] = '\0';
    syscall(SYS_GET_ARGS, (uint64_t)(uintptr_t)arg_buf, sizeof(arg_buf), 0, 0, 0);

    char *image_path = arg_buf;
    while (*image_path == ' ' || *image_path == '\t') image_path++;

    /* Extract directory and filename */
    const char *fname = image_path;
    const char *last_slash = NULL;
    for (const char *p = image_path; *p; p++) {
        if (*p == '/' || *p == '\\') last_slash = p;
    }

    if (last_slash) {
        size_t dlen = last_slash - image_path;
        if (dlen == 0) dlen = 1; /* Root / */
        for (size_t i = 0; i < dlen && i < sizeof(g_folder_path) - 1; i++) {
            g_folder_path[i] = image_path[i];
        }
        g_folder_path[dlen] = '\0';
        fname = last_slash + 1;
    } else if (image_path[0] != '\0') {
        ntfs_strncpy(g_folder_path, "/home/user", sizeof(g_folder_path) - 1);
        fname = image_path;
    } else {
        ntfs_strncpy(g_folder_path, "/home/user/Wallpapers", sizeof(g_folder_path) - 1);
        fname = "Under Construction.png";
    }

    scan_directory(g_folder_path, fname);

    /* Calculate nice initial window size */
    uint32_t init_w = VIEW_DEFAULT_W;
    uint32_t init_h = VIEW_DEFAULT_H;

    /* Build full initial path */
    if (image_path[0] != '\0') {
        ntfs_strncpy(g_current_path, image_path, sizeof(g_current_path) - 1);
    } else if (g_file_count > 0 && g_current_idx >= 0) {
        ntfs_strncpy(g_current_path, g_folder_path, sizeof(g_current_path) - 1);
        size_t flen = view_strlen(g_current_path);
        if (flen > 0 && g_current_path[flen - 1] != '/') {
            g_current_path[flen++] = '/';
            g_current_path[flen] = '\0';
        }
        const char *cf = g_file_list[g_current_idx];
        for (size_t i = 0; cf[i] && flen < sizeof(g_current_path) - 1; i++) {
            g_current_path[flen++] = cf[i];
        }
        g_current_path[flen] = '\0';
    }

    if (g_current_path[0] != '\0') {
        load_image_file(g_current_path);
        if (g_img_loaded && g_img_w > 0 && g_img_h > 0) {
            init_w = g_img_w + 16;
            init_h = g_img_h + TOOLBAR_H + 8;
            if (init_w > 960) init_w = 960;
            if (init_h > 640) init_h = 640;
            if (init_w < VIEW_DEFAULT_W) init_w = VIEW_DEFAULT_W;
            if (init_h < VIEW_DEFAULT_H) init_h = VIEW_DEFAULT_H;
        }
    }

    char title[64];
    ntfs_strncpy(title, "ImageView - pseuDOS", sizeof(title) - 1);

    if (ntfs_client_connect(&g_client, title, init_w, init_h, NTFS_WIN_NORMAL) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    update_title();
    render_view_ui();

    ntfs_msg_t msg;
    uint8_t prev_left = 0;

    while (1) {
        int updated = 0;
        while (ntfs_client_poll_event(&g_client, &msg)) {
            if (msg.type == NTFS_MSG_WINDOW_RESIZED) {
                updated = 1;
            } else if (msg.type == NTFS_MSG_KEY_EVENT) {
                uint32_t k = msg.buttons_or_key;
                if (k == 0x11 || k == '<' || k == 'a' || k == 'A' || k == 0x25 /* Left */) {
                    prev_image();
                } else if (k == 0x12 || k == '>' || k == 'd' || k == 'D' || k == 0x27 /* Right */) {
                    next_image();
                } else if (k == 'z' || k == 'Z' || k == ' ') {
                    g_zoom_1to1 = !g_zoom_1to1;
                    updated = 1;
                } else if (k == 'w' || k == 'W') {
                    set_as_wallpaper();
                } else if (k == 27 /* ESC */ || k == 'q' || k == 'Q') {
                    ntfs_client_close(&g_client);
                    syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                    return;
                }
            } else if (msg.type == NTFS_MSG_MOUSE_EVENT) {
                uint8_t left_down = (msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                uint8_t left_click = (left_down && !prev_left);
                if (left_click) {
                    int mx = msg.x;
                    int my = msg.y;
                    int win_h = (int)g_client.height;
                    int tb_y = win_h - TOOLBAR_H;

                    /* Check toolbar buttons */
                    if (my >= tb_y + 4 && my <= tb_y + 26) {
                        if (mx >= 6 && mx <= 66) {
                            prev_image();
                        } else if (mx >= 72 && mx <= 132) {
                            next_image();
                        } else if (mx >= 138 && mx <= 214) {
                            g_zoom_1to1 = !g_zoom_1to1;
                            updated = 1;
                        } else if (mx >= 220 && mx <= 306) {
                            set_as_wallpaper();
                        }
                    }
                }
                prev_left = left_down;
            } else if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                ntfs_client_close(&g_client);
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                return;
            }
        }

        if (updated) {
            render_view_ui();
        }

        syscall(SYS_SLEEP, 30, 0, 0, 0, 0);
    }
}
