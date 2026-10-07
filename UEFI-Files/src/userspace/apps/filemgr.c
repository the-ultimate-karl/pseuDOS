#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

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

#define DEFAULT_WIN_W 640
#define DEFAULT_WIN_H 440
#define MIN_WIN_W     440
#define MIN_WIN_H     280

#define ROW_HEIGHT    20
#define TOOLBAR_H     28
#define ADDR_BAR_H    26
#define STATUS_BAR_H  22
#define SCROLLBAR_W   16
#define SIDEBAR_W     110
#define MAX_ITEMS     256
#define PATH_MAX_LEN  256

/* View Modes */
enum {
    VIEW_DETAILS = 0,
    VIEW_ICONS = 1
};

/* Colors */
#define COLOR_SEL_BG     0x00000080  /* Win95 Selection Navy */
#define COLOR_SEL_FG     0x00FFFFFF  /* Selection White Text */
#define COLOR_ROW_ALT    0x00F8F8F8  /* Alternating row color */
#define COLOR_FOLDER_TAB 0x00D4A017  /* Dark Ochre */
#define COLOR_FOLDER_BOD 0x00FFD700  /* Golden Yellow */
#define COLOR_DOC_PAPER  0x00FFFFFF  /* Document White */
#define COLOR_APP_HDR    0x00000080  /* App Titlebar */
#define COLOR_SKY_BLUE   0x0087CEEB  /* Image icon sky */
#define COLOR_BREADCRUMB 0x00D0D0D0  /* Pill bg */

typedef struct {
    char name[64];
    uint32_t size;
    uint32_t type; /* 0 = File, 1 = Directory */
    int is_protected;
    char date[24];
    char type_str[20];
} file_item_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
    char path[PATH_MAX_LEN];
} breadcrumb_t;

static ntfs_client_t g_client;
static char g_cwd[PATH_MAX_LEN] = "/";
static file_item_t g_items[MAX_ITEMS];
static int g_item_count = 0;
static int g_selected_idx = -1;
static int g_scroll_offset = 0;
static uint64_t g_total_bytes = 0;

/* View Mode */
static int g_view_mode = VIEW_DETAILS;

/* Quick Search / Filter */
static char g_filter[32] = "";
static int g_filter_active = 0;

/* Breadcrumbs */
static breadcrumb_t g_breadcrumbs[16];
static int g_breadcrumb_count = 0;

/* Clipboard (Option 1: Copy, Cut, Paste) */
static char s_clip_src[PATH_MAX_LEN] = "";
static char s_clip_name[64] = "";
static int s_clip_is_cut = 0;
static int s_clip_has_data = 0;

/* In-place Renaming */
static int g_renaming = 0;
static int g_rename_target_idx = -1;
static char g_rename_buf[64] = "";

static uint64_t g_last_click_tick = 0;
static int g_last_click_idx = -1;
static char g_status_msg[64] = "Ready";

static int g_ctx_open = 0;
static int g_ctx_x = 0;
static int g_ctx_y = 0;
static int g_ctx_target_idx = -1;
static int g_ctx_hover = -1;
static uint8_t g_prev_left_down = 0;
static uint8_t g_prev_right_down = 0;

/* Sidebar entries (Option 2) */
typedef struct {
    const char *label;
    const char *path;
    int icon;
} sidebar_item_t;

enum {
    ICON_UP_DIR,
    ICON_FOLDER,
    ICON_APP,
    ICON_DOC,
    ICON_FILE,
    ICON_IMAGE,
    ICON_DRIVE
};

static const sidebar_item_t g_sidebar[] = {
    { "Root",   "/",               ICON_DRIVE },
    { "Home",   "/home/user",      ICON_FOLDER },
    { "Apps",   "/protected/apps", ICON_APP },
    { "Temp",   "/tmp",            ICON_FOLDER },
    { "Mounts", "/mounts",         ICON_DRIVE }
};
#define NUM_SIDEBAR_ITEMS 5

static void render_filemgr(void);

/* Modal Dialog for Error / Warning (Classic Windows 95 Message Box) */
static int g_modal_err_open = 0;
static char g_modal_err_l1[64] = "";
static char g_modal_err_l2[64] = "";

/* Toolbar Button Coordinates */
typedef struct {
    const char *label;
    int x;
    int y;
    int w;
    int h;
    int id;
} btn_t;

enum {
    BTN_UP = 1,
    BTN_ROOT,
    BTN_APPS,
    BTN_NEW_FOLDER,
    BTN_VIEW,
    BTN_REFRESH,
    BTN_DELETE
};

static void str_copy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void show_error_modal(const char *l1, const char *l2) {
    g_modal_err_open = 1;
    str_copy(g_modal_err_l1, l1 ? l1 : "", sizeof(g_modal_err_l1));
    str_copy(g_modal_err_l2, l2 ? l2 : "", sizeof(g_modal_err_l2));
    render_filemgr();
}

static size_t str_len(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static int str_eq(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    while (*s1 && *s2) {
        if (*s1 != *s2) return 0;
        s1++;
        s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

static int str_ends_with(const char *s, const char *suffix) {
    size_t ls = str_len(s);
    size_t lsub = str_len(suffix);
    if (lsub > ls) return 0;
    const char *p = s + ls - lsub;
    while (*p && *suffix) {
        char c1 = (*p >= 'A' && *p <= 'Z') ? (*p + 32) : *p;
        char c2 = (*suffix >= 'A' && *suffix <= 'Z') ? (*suffix + 32) : *suffix;
        if (c1 != c2) return 0;
        p++;
        suffix++;
    }
    return 1;
}

static int str_contains_case_insensitive(const char *haystack, const char *needle) {
    if (!needle || needle[0] == '\0') return 1;
    if (!haystack) return 0;
    size_t lh = str_len(haystack);
    size_t ln = str_len(needle);
    if (ln > lh) return 0;
    for (size_t i = 0; i <= lh - ln; i++) {
        size_t j = 0;
        while (j < ln) {
            char ch = haystack[i + j];
            char cn = needle[j];
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (cn >= 'A' && cn <= 'Z') cn += 32;
            if (ch != cn) break;
            j++;
        }
        if (j == ln) return 1;
    }
    return 0;
}

static void str_cat(char *dst, const char *src, size_t max_len) {
    size_t l = str_len(dst);
    size_t i = 0;
    while (src && src[i] && l + 1 < max_len) {
        dst[l++] = src[i++];
    }
    dst[l] = '\0';
}

static void build_full_path(const char *name, char *out, size_t max_len) {
    if (!out || max_len == 0) return;
    str_copy(out, g_cwd, max_len);
    if (!str_eq(g_cwd, "/")) {
        size_t l = str_len(out);
        if (l + 1 < max_len) {
            out[l++] = '/';
            out[l] = '\0';
        }
    }
    size_t l = str_len(out);
    for (int ci = 0; name && name[ci] && l + 1 < max_len; ci++) {
        out[l++] = name[ci];
    }
    out[l] = '\0';
}

static void draw_string_clipped(uint32_t *buf, int win_w, int win_h, int x, int y, const char *str, uint32_t fg, int max_x) {
    if (!str || !buf) return;
    int cur_x = x;
    while (*str) {
        if (cur_x + 8 > max_x) break;
        gfx_draw_char(buf, win_w, win_h, cur_x, y, *str, fg, 0, 1);
        cur_x += 8;
        str++;
    }
}

static void u64_to_str(uint64_t val, char *buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[32];
    int ti = 0;
    while (val > 0) {
        tmp[ti++] = '0' + (val % 10);
        val /= 10;
    }
    int i = 0;
    while (ti > 0) {
        buf[i++] = tmp[--ti];
    }
    buf[i] = '\0';
}

static void format_size(uint32_t bytes, char *buf, size_t max_len) {
    if (bytes < 1024) {
        char nbuf[16];
        u64_to_str(bytes, nbuf);
        buf[0] = '\0';
        str_copy(buf, nbuf, max_len);
        size_t l = str_len(buf);
        if (l + 3 < max_len) {
            buf[l] = ' '; buf[l+1] = 'B'; buf[l+2] = '\0';
        }
    } else if (bytes < 1024 * 1024) {
        uint32_t kb = bytes / 1024;
        uint32_t rem = ((bytes % 1024) * 10) / 1024;
        char nbuf[16], rbuf[8];
        u64_to_str(kb, nbuf);
        u64_to_str(rem, rbuf);
        buf[0] = '\0';
        str_copy(buf, nbuf, max_len);
        size_t l = str_len(buf);
        if (l + 5 < max_len) {
            buf[l++] = '.';
            buf[l++] = rbuf[0];
            buf[l++] = ' ';
            buf[l++] = 'K';
            buf[l++] = 'B';
            buf[l] = '\0';
        }
    } else {
        uint32_t mb = bytes / (1024 * 1024);
        uint32_t rem = ((bytes % (1024 * 1024)) * 10) / (1024 * 1024);
        char nbuf[16], rbuf[8];
        u64_to_str(mb, nbuf);
        u64_to_str(rem, rbuf);
        buf[0] = '\0';
        str_copy(buf, nbuf, max_len);
        size_t l = str_len(buf);
        if (l + 5 < max_len) {
            buf[l++] = '.';
            buf[l++] = rbuf[0];
            buf[l++] = ' ';
            buf[l++] = 'M';
            buf[l++] = 'B';
            buf[l] = '\0';
        }
    }
}

static void determine_type_str(const char *name, uint32_t type, char *out, size_t out_len) {
    if (type == 1) {
        if (str_eq(name, "..")) {
            str_copy(out, "Parent Folder", out_len);
        } else {
            str_copy(out, "Folder", out_len);
        }
        return;
    }
    if (str_ends_with(name, ".exe") || str_ends_with(name, ".bin")) {
        str_copy(out, "Application", out_len);
    } else if (str_ends_with(name, ".txt") || str_ends_with(name, ".log")) {
        str_copy(out, "Text Document", out_len);
    } else if (str_ends_with(name, ".cfg") || str_ends_with(name, ".ini")) {
        str_copy(out, "Configuration", out_len);
    } else if (str_ends_with(name, ".c") || str_ends_with(name, ".h") || str_ends_with(name, ".s")) {
        str_copy(out, "Source Code", out_len);
    } else if (str_ends_with(name, ".png") || str_ends_with(name, ".ppm") ||
               str_ends_with(name, ".bmp") || str_ends_with(name, ".jpg") ||
               str_ends_with(name, ".jpeg")) {
        str_copy(out, "Image File", out_len);
    } else if (str_ends_with(name, ".efi")) {
        str_copy(out, "UEFI Binary", out_len);
    } else {
        str_copy(out, "File", out_len);
    }
}

static int get_item_icon_type(const file_item_t *item) {
    if (str_eq(g_cwd, "/mounts") && !str_eq(item->name, "..")) {
        return ICON_DRIVE;
    }
    if (item->type == 1) {
        return str_eq(item->name, "..") ? ICON_UP_DIR : ICON_FOLDER;
    }
    if (str_ends_with(item->name, ".exe") || str_ends_with(item->name, ".bin")) {
        return ICON_APP;
    }
    if (str_ends_with(item->name, ".bmp") || str_ends_with(item->name, ".png") ||
        str_ends_with(item->name, ".jpg") || str_ends_with(item->name, ".jpeg")) {
        return ICON_IMAGE;
    }
    if (str_ends_with(item->name, ".txt") || str_ends_with(item->name, ".log") ||
        str_ends_with(item->name, ".cfg") || str_ends_with(item->name, ".c") ||
        str_ends_with(item->name, ".h")) {
        return ICON_DOC;
    }
    return ICON_FILE;
}

/* 16x16 Icon Rendering */
static void draw_icon(uint32_t *pixels, int win_w, int win_h, int x, int y, int icon_type) {
    if (icon_type == ICON_UP_DIR) {
        gfx_fill_rect(pixels, win_w, win_h, x + 1, y + 2, 5, 2, COLOR_FOLDER_TAB);
        gfx_fill_rect(pixels, win_w, win_h, x, y + 4, 13, 9, COLOR_FOLDER_BOD);
        gfx_draw_bevel(pixels, win_w, win_h, x, y + 4, 13, 9, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 6, y + 6, 2, 5, 0x00008000);
        gfx_fill_rect(pixels, win_w, win_h, x + 5, y + 7, 4, 1, 0x00008000);
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 8, 6, 1, 0x00008000);
    } else if (icon_type == ICON_FOLDER) {
        gfx_fill_rect(pixels, win_w, win_h, x + 1, y + 2, 5, 2, COLOR_FOLDER_TAB);
        gfx_fill_rect(pixels, win_w, win_h, x, y + 4, 13, 9, COLOR_FOLDER_BOD);
        gfx_draw_bevel(pixels, win_w, win_h, x, y + 4, 13, 9, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 2, y + 6, 9, 1, COLOR_FOLDER_TAB);
    } else if (icon_type == ICON_APP) {
        gfx_fill_rect(pixels, win_w, win_h, x + 1, y + 1, 12, 12, COLOR_WHITE);
        gfx_draw_bevel(pixels, win_w, win_h, x + 1, y + 1, 12, 12, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 2, y + 2, 10, 3, COLOR_APP_HDR);
        gfx_fill_rect(pixels, win_w, win_h, x + 3, y + 6, 4, 3, COLOR_RED);
        gfx_fill_rect(pixels, win_w, win_h, x + 8, y + 6, 2, 2, COLOR_BTN_SHADOW);
        gfx_fill_rect(pixels, win_w, win_h, x + 3, y + 10, 6, 1, COLOR_BTN_SHADOW);
    } else if (icon_type == ICON_IMAGE) {
        gfx_fill_rect(pixels, win_w, win_h, x + 1, y + 1, 13, 12, COLOR_SKY_BLUE);
        gfx_draw_bevel(pixels, win_w, win_h, x + 1, y + 1, 13, 12, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 9, y + 3, 3, 3, COLOR_YELLOW);
        gfx_fill_rect(pixels, win_w, win_h, x + 3, y + 8, 4, 4, 0x00804000);
        gfx_fill_rect(pixels, win_w, win_h, x + 7, y + 7, 5, 5, 0x00008000);
    } else if (icon_type == ICON_DOC) {
        gfx_fill_rect(pixels, win_w, win_h, x + 2, y + 1, 8, 12, COLOR_DOC_PAPER);
        gfx_fill_rect(pixels, win_w, win_h, x + 10, y + 3, 1, 10, COLOR_DOC_PAPER);
        gfx_fill_rect(pixels, win_w, win_h, x + 8, y + 1, 2, 2, COLOR_BTN_SHADOW);
        gfx_draw_bevel(pixels, win_w, win_h, x + 2, y + 1, 9, 12, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 4, 4, 1, COLOR_BTN_SHADOW);
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 6, 5, 1, COLOR_BTN_SHADOW);
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 8, 5, 1, COLOR_BTN_SHADOW);
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 10, 3, 1, COLOR_BTN_SHADOW);
    } else if (icon_type == ICON_DRIVE) {
        gfx_fill_rect(pixels, win_w, win_h, x + 1, y + 3, 13, 9, COLOR_BTN_FACE);
        gfx_draw_bevel(pixels, win_w, win_h, x + 1, y + 3, 13, 9, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 3, y + 6, 6, 2, COLOR_BLACK);
        gfx_fill_rect(pixels, win_w, win_h, x + 11, y + 7, 2, 2, 0x0000FF00);
    } else {
        gfx_fill_rect(pixels, win_w, win_h, x + 2, y + 1, 9, 12, COLOR_WHITE);
        gfx_draw_bevel(pixels, win_w, win_h, x + 2, y + 1, 9, 12, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 5, 5, 4, COLOR_BTN_LIGHT);
    }
}

/* 32x32 Large Icon Rendering (Option 3) */
static void draw_icon_32(uint32_t *pixels, int win_w, int win_h, int x, int y, int icon_type) {
    if (icon_type == ICON_UP_DIR) {
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 4, 10, 4, COLOR_FOLDER_TAB);
        gfx_fill_rect(pixels, win_w, win_h, x + 2, y + 8, 28, 20, COLOR_FOLDER_BOD);
        gfx_draw_bevel(pixels, win_w, win_h, x + 2, y + 8, 28, 20, 0);
        /* Green Up Arrow */
        gfx_fill_rect(pixels, win_w, win_h, x + 14, y + 12, 4, 10, 0x00008000);
        gfx_fill_rect(pixels, win_w, win_h, x + 12, y + 14, 8, 2, 0x00008000);
        gfx_fill_rect(pixels, win_w, win_h, x + 10, y + 16, 12, 2, 0x00008000);
    } else if (icon_type == ICON_FOLDER) {
        gfx_fill_rect(pixels, win_w, win_h, x + 4, y + 4, 12, 4, COLOR_FOLDER_TAB);
        gfx_fill_rect(pixels, win_w, win_h, x + 2, y + 8, 28, 20, COLOR_FOLDER_BOD);
        gfx_draw_bevel(pixels, win_w, win_h, x + 2, y + 8, 28, 20, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 5, y + 12, 20, 2, COLOR_FOLDER_TAB);
    } else if (icon_type == ICON_APP) {
        gfx_fill_rect(pixels, win_w, win_h, x + 3, y + 3, 26, 26, COLOR_WHITE);
        gfx_draw_bevel(pixels, win_w, win_h, x + 3, y + 3, 26, 26, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 5, y + 5, 22, 6, COLOR_APP_HDR);
        gfx_fill_rect(pixels, win_w, win_h, x + 7, y + 14, 8, 6, COLOR_RED);
        gfx_fill_rect(pixels, win_w, win_h, x + 17, y + 14, 6, 6, COLOR_BLUE);
        gfx_fill_rect(pixels, win_w, win_h, x + 7, y + 22, 16, 3, COLOR_BTN_SHADOW);
    } else if (icon_type == ICON_IMAGE) {
        gfx_fill_rect(pixels, win_w, win_h, x + 3, y + 3, 26, 26, COLOR_SKY_BLUE);
        gfx_draw_bevel(pixels, win_w, win_h, x + 3, y + 3, 26, 26, 0);
        /* Golden Sun */
        gfx_fill_rect(pixels, win_w, win_h, x + 19, y + 6, 6, 6, COLOR_YELLOW);
        /* Mountain peaks */
        gfx_fill_rect(pixels, win_w, win_h, x + 6, y + 17, 8, 9, 0x00804000);
        gfx_fill_rect(pixels, win_w, win_h, x + 14, y + 14, 12, 12, 0x00008000);
    } else if (icon_type == ICON_DOC) {
        gfx_fill_rect(pixels, win_w, win_h, x + 5, y + 3, 18, 26, COLOR_DOC_PAPER);
        gfx_fill_rect(pixels, win_w, win_h, x + 17, y + 3, 6, 6, COLOR_BTN_SHADOW);
        gfx_draw_bevel(pixels, win_w, win_h, x + 5, y + 3, 19, 26, 0);
        for (int l = 0; l < 4; l++) {
            gfx_fill_rect(pixels, win_w, win_h, x + 9, y + 12 + l * 3, 11, 1, COLOR_BTN_SHADOW);
        }
    } else {
        gfx_fill_rect(pixels, win_w, win_h, x + 5, y + 3, 22, 26, COLOR_WHITE);
        gfx_draw_bevel(pixels, win_w, win_h, x + 5, y + 3, 22, 26, 0);
        gfx_fill_rect(pixels, win_w, win_h, x + 8, y + 9, 14, 10, COLOR_BTN_LIGHT);
    }
}

/* File Copy Utility (Option 1) */
static int copy_file(const char *src_path, const char *dst_path) {
    vfs_stat_t st;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0) {
        return -1;
    }
    if (st.size == 0) {
        return (int)syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)"", 0, 0, 0);
    }
    char buf[4096];
    size_t offset = 0;
    int first = 1;
    while (offset < st.size) {
        size_t to_read = (st.size - offset < sizeof(buf)) ? (st.size - offset) : sizeof(buf);
        int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)buf, to_read, offset, 0);
        if (rd <= 0) break;
        int wr = (int)syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)buf, (size_t)rd, first ? 0 : 1, 0);
        if (wr != 0) return -1;
        offset += rd;
        first = 0;
    }
    return 0;
}

/* Directory Enumeration and Sorting */
static void sort_items(void) {
    for (int i = 0; i < g_item_count - 1; i++) {
        for (int j = 0; j < g_item_count - i - 1; j++) {
            int swap = 0;
            if (str_eq(g_items[j].name, "..")) continue;
            if (str_eq(g_items[j + 1].name, "..")) {
                swap = 1;
            } else if (g_items[j].type < g_items[j + 1].type) {
                swap = 1;
            } else if (g_items[j].type == g_items[j + 1].type) {
                const char *p1 = g_items[j].name;
                const char *p2 = g_items[j + 1].name;
                while (*p1 && *p2) {
                    char c1 = (*p1 >= 'A' && *p1 <= 'Z') ? (*p1 + 32) : *p1;
                    char c2 = (*p2 >= 'A' && *p2 <= 'Z') ? (*p2 + 32) : *p2;
                    if (c1 > c2) { swap = 1; break; }
                    if (c1 < c2) { swap = 0; break; }
                    p1++;
                    p2++;
                }
                if (!swap && *p1 && !*p2) swap = 1;
            }
            if (swap) {
                file_item_t tmp = g_items[j];
                g_items[j] = g_items[j + 1];
                g_items[j + 1] = tmp;
            }
        }
    }
}

static void sort_items_by_size(void) {
    for (int i = 0; i < g_item_count - 1; i++) {
        for (int j = 0; j < g_item_count - i - 1; j++) {
            int swap = 0;
            if (str_eq(g_items[j].name, "..")) continue;
            if (str_eq(g_items[j + 1].name, "..")) {
                swap = 1;
            } else if (g_items[j].type < g_items[j + 1].type) {
                swap = 1;
            } else if (g_items[j].type == g_items[j + 1].type) {
                if (g_items[j].size < g_items[j + 1].size) {
                    swap = 1;
                }
            }
            if (swap) {
                file_item_t tmp = g_items[j];
                g_items[j] = g_items[j + 1];
                g_items[j + 1] = tmp;
            }
        }
    }
}

static void read_directory(const char *path) {
    g_item_count = 0;
    g_selected_idx = -1;
    g_scroll_offset = 0;
    g_total_bytes = 0;
    g_ctx_open = 0;
    g_renaming = 0;

    str_copy(g_cwd, path, sizeof(g_cwd));

    if (!str_eq(g_cwd, "/")) {
        str_copy(g_items[0].name, "..", sizeof(g_items[0].name));
        g_items[0].type = 1;
        g_items[0].size = 0;
        g_items[0].is_protected = 0;
        str_copy(g_items[0].type_str, "Parent Folder", sizeof(g_items[0].type_str));
        g_items[0].date[0] = '\0';
        g_item_count = 1;
    }

    static char names_buf[4096];
    int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)g_cwd, (uint64_t)(uintptr_t)names_buf, sizeof(names_buf), 0, 0);
    if (nbytes > 0) {
        size_t pos = 0;
        while (pos < (size_t)nbytes && g_item_count < MAX_ITEMS) {
            const char *name = &names_buf[pos];
            size_t nlen = str_len(name);
            if (nlen == 0) break;
            pos += nlen + 1;

            if (str_eq(name, ".") || str_eq(name, "..")) continue;

            char child_path[PATH_MAX_LEN];
            build_full_path(name, child_path, sizeof(child_path));

            vfs_stat_t st;
            if (syscall(SYS_STAT, (uint64_t)(uintptr_t)child_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) == 0) {
                file_item_t *item = &g_items[g_item_count++];
                str_copy(item->name, name, sizeof(item->name));
                item->size = st.size;
                item->type = st.type;
                item->is_protected = st.is_protected;
                const char *d = st.date_modified[0] ? st.date_modified : (st.date_created[0] ? st.date_created : "");
                str_copy(item->date, d, sizeof(item->date));
                determine_type_str(item->name, item->type, item->type_str, sizeof(item->type_str));

                if (str_eq(g_cwd, "/mounts")) {
                    char raw_marker[PATH_MAX_LEN];
                    str_copy(raw_marker, child_path, sizeof(raw_marker));
                    str_cat(raw_marker, "/.raw", sizeof(raw_marker));
                    vfs_stat_t rst;
                    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)raw_marker, (uint64_t)(uintptr_t)&rst, 0, 0, 0) == 0) {
                        str_copy(item->type_str, "RAW Storage Device", sizeof(item->type_str));
                    } else {
                        str_copy(item->type_str, "Local Disk (FAT32)", sizeof(item->type_str));
                    }
                }

                if (item->type == 0) {
                    g_total_bytes += item->size;
                }
            }
        }
    }

    sort_items();
    str_copy(g_status_msg, "Ready", sizeof(g_status_msg));
}

static void navigate_to(const char *name) {
    if (str_eq(name, "..")) {
        size_t l = str_len(g_cwd);
        while (l > 1 && g_cwd[l - 1] != '/') l--;
        if (l > 1) {
            g_cwd[l - 1] = '\0';
        } else {
            g_cwd[1] = '\0';
        }
        read_directory(g_cwd);
        return;
    }

    char next_path[PATH_MAX_LEN];
    build_full_path(name, next_path, sizeof(next_path));

    char raw_check[PATH_MAX_LEN];
    str_copy(raw_check, next_path, sizeof(raw_check));
    str_cat(raw_check, "/.raw", sizeof(raw_check));
    vfs_stat_t rst;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)raw_check, (uint64_t)(uintptr_t)&rst, 0, 0, 0) == 0) {
        show_error_modal("You need to format this", "filesystem first to use it!");
        return;
    }

    read_directory(next_path);
}

static void go_up(void) {
    if (!str_eq(g_cwd, "/")) {
        navigate_to("..");
    }
}

static void execute_or_open(file_item_t *item) {
    if (!item) return;
    if (item->type == 1) {
        char full_target[PATH_MAX_LEN];
        build_full_path(item->name, full_target, sizeof(full_target));
        char raw_check[PATH_MAX_LEN];
        str_copy(raw_check, full_target, sizeof(raw_check));
        str_cat(raw_check, "/.raw", sizeof(raw_check));
        vfs_stat_t rst;
        if (syscall(SYS_STAT, (uint64_t)(uintptr_t)raw_check, (uint64_t)(uintptr_t)&rst, 0, 0, 0) == 0) {
            show_error_modal("You need to format this", "filesystem first to use it!");
            return;
        }
        navigate_to(item->name);
        return;
    }

    char full_path[PATH_MAX_LEN];
    build_full_path(item->name, full_path, sizeof(full_path));

    if (str_ends_with(item->name, ".exe") || str_ends_with(item->name, ".bin")) {
        if (str_eq(item->name, "heap_error.exe")) {
            show_error_modal("Out of kernel", "heap!");
            return;
        }
        int64_t ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)full_path, 0, 0, 0, 0);
        if (ret == -ENOMEM) {
            show_error_modal("Out of kernel", "heap!");
        } else if (ret == -ENOENT) {
            show_error_modal("Application binary", "not found!");
        } else if (ret < 0) {
            show_error_modal("Failed to execute", "application!");
        }
    } else if (str_ends_with(item->name, ".txt") || str_ends_with(item->name, ".log") ||
               str_ends_with(item->name, ".cfg") || str_ends_with(item->name, ".ini")) {
        /* Open in Notepad with real path argument! */
        int64_t ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/notepad.exe", (uint64_t)(uintptr_t)full_path, 0, 0, 0);
        if (ret == -ENOMEM) {
            show_error_modal("Out of kernel", "heap!");
        } else if (ret < 0) {
            show_error_modal("Failed to open", "Notepad!");
        }
    } else if (str_ends_with(item->name, ".bmp") || str_ends_with(item->name, ".png") ||
               str_ends_with(item->name, ".jpg") || str_ends_with(item->name, ".jpeg")) {
        /* Open in Paint with real path argument! */
        int64_t ret = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/paint.exe", (uint64_t)(uintptr_t)full_path, 0, 0, 0);
        if (ret == -ENOMEM) {
            show_error_modal("Out of kernel", "heap!");
        } else if (ret < 0) {
            show_error_modal("Failed to open", "Paint!");
        }
    }
}

static void action_new_folder(void) {
    char new_path[PATH_MAX_LEN];
    for (int num = 1; num < 100; num++) {
        char base_name[32] = "NewFolder";
        if (num > 1) {
            char nbuf[8];
            u64_to_str(num, nbuf);
            str_cat(base_name, nbuf, sizeof(base_name));
        }
        build_full_path(base_name, new_path, sizeof(new_path));

        vfs_stat_t st;
        if (syscall(SYS_STAT, (uint64_t)(uintptr_t)new_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0) {
            syscall(SYS_MKDIR, (uint64_t)(uintptr_t)new_path, 0, 0, 0, 0);
            read_directory(g_cwd);
            return;
        }
    }
}

static void action_new_text_file(void) {
    char new_path[PATH_MAX_LEN];
    for (int num = 1; num < 100; num++) {
        char base_name[32] = "NewDoc";
        if (num > 1) {
            char nbuf[8];
            u64_to_str(num, nbuf);
            str_cat(base_name, nbuf, sizeof(base_name));
        }
        str_cat(base_name, ".txt", sizeof(base_name));
        build_full_path(base_name, new_path, sizeof(new_path));

        vfs_stat_t st;
        if (syscall(SYS_STAT, (uint64_t)(uintptr_t)new_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0) {
            syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)new_path, (uint64_t)(uintptr_t)"", 0, 0, 0);
            read_directory(g_cwd);
            return;
        }
    }
}

static void action_delete_selected(void) {
    if (g_selected_idx < 0 || g_selected_idx >= g_item_count) return;
    file_item_t *item = &g_items[g_selected_idx];
    if (str_eq(item->name, "..")) return;

    if (item->is_protected) {
        str_copy(g_status_msg, "Cannot delete protected system item!", sizeof(g_status_msg));
        return;
    }

    char full_path[PATH_MAX_LEN];
    build_full_path(item->name, full_path, sizeof(full_path));
    syscall(SYS_UNLINK, (uint64_t)(uintptr_t)full_path, 1, 0, 0, 0);
    read_directory(g_cwd);
}

static void action_copy_selected(void) {
    if (g_selected_idx < 0 || g_selected_idx >= g_item_count) return;
    file_item_t *item = &g_items[g_selected_idx];
    if (str_eq(item->name, "..")) return;
    build_full_path(item->name, s_clip_src, sizeof(s_clip_src));
    str_copy(s_clip_name, item->name, sizeof(s_clip_name));
    s_clip_is_cut = 0;
    s_clip_has_data = 1;
    str_copy(g_status_msg, "Copied to clipboard", sizeof(g_status_msg));
}

static void action_cut_selected(void) {
    if (g_selected_idx < 0 || g_selected_idx >= g_item_count) return;
    file_item_t *item = &g_items[g_selected_idx];
    if (str_eq(item->name, "..")) return;
    if (item->is_protected) {
        str_copy(g_status_msg, "Cannot cut protected system item!", sizeof(g_status_msg));
        return;
    }
    build_full_path(item->name, s_clip_src, sizeof(s_clip_src));
    str_copy(s_clip_name, item->name, sizeof(s_clip_name));
    s_clip_is_cut = 1;
    s_clip_has_data = 1;
    str_copy(g_status_msg, "Cut to clipboard", sizeof(g_status_msg));
}

static void action_paste(void) {
    if (!s_clip_has_data || s_clip_src[0] == '\0') {
        str_copy(g_status_msg, "Clipboard is empty", sizeof(g_status_msg));
        return;
    }
    char target_path[PATH_MAX_LEN];
    build_full_path(s_clip_name, target_path, sizeof(target_path));
    if (str_eq(s_clip_src, target_path)) {
        str_copy(g_status_msg, "Source and destination are identical", sizeof(g_status_msg));
        return;
    }
    if (copy_file(s_clip_src, target_path) != 0) {
        str_copy(g_status_msg, "Paste failed (permission or disk error)", sizeof(g_status_msg));
        return;
    }
    if (s_clip_is_cut) {
        syscall(SYS_UNLINK, (uint64_t)(uintptr_t)s_clip_src, 1, 0, 0, 0);
        s_clip_has_data = 0;
        s_clip_src[0] = '\0';
        str_copy(g_status_msg, "Moved item", sizeof(g_status_msg));
    } else {
        str_copy(g_status_msg, "Pasted item", sizeof(g_status_msg));
    }
    read_directory(g_cwd);
}

static void action_start_rename(void) {
    if (g_selected_idx < 0 || g_selected_idx >= g_item_count) return;
    file_item_t *item = &g_items[g_selected_idx];
    if (str_eq(item->name, "..") || item->is_protected) {
        str_copy(g_status_msg, "Cannot rename this item", sizeof(g_status_msg));
        return;
    }
    g_renaming = 1;
    g_rename_target_idx = g_selected_idx;
    str_copy(g_rename_buf, item->name, sizeof(g_rename_buf));
    str_copy(g_status_msg, "Type new name and press Enter", sizeof(g_status_msg));
}

static void action_commit_rename(void) {
    if (!g_renaming || g_rename_target_idx < 0 || g_rename_target_idx >= g_item_count) {
        g_renaming = 0;
        return;
    }
    file_item_t *item = &g_items[g_rename_target_idx];
    if (g_rename_buf[0] != '\0' && !str_eq(g_rename_buf, item->name)) {
        char old_path[PATH_MAX_LEN], new_path[PATH_MAX_LEN];
        build_full_path(item->name, old_path, sizeof(old_path));
        build_full_path(g_rename_buf, new_path, sizeof(new_path));
        if (copy_file(old_path, new_path) == 0) {
            syscall(SYS_UNLINK, (uint64_t)(uintptr_t)old_path, 1, 0, 0, 0);
            str_copy(g_status_msg, "Renamed item", sizeof(g_status_msg));
        } else {
            str_copy(g_status_msg, "Rename failed", sizeof(g_status_msg));
        }
        read_directory(g_cwd);
    }
    g_renaming = 0;
}

/* Context Menus (Option 1) */
static const char *g_ctx_item_actions[] = {
    "Open",
    "Open in Notepad",
    "Copy",
    "Cut",
    "Rename",
    "Delete",
    "Properties"
};
#define CTX_ITEM_ACTION_COUNT 7

static const char *g_ctx_space_actions[] = {
    "Paste",
    "New Folder",
    "New Text File",
    "Refresh",
    "Sort by Name",
    "Sort by Size"
};
#define CTX_SPACE_ACTION_COUNT 6

static void draw_ctx_menu(uint32_t *pixels, int win_w, int win_h) {
    if (!g_ctx_open) return;

    int num_actions = (g_ctx_target_idx >= 0) ? CTX_ITEM_ACTION_COUNT : CTX_SPACE_ACTION_COUNT;
    int ctx_w = 150;
    int ctx_h = 6 + num_actions * 20;

    int cx = g_ctx_x;
    int cy = g_ctx_y;
    if (cx + ctx_w > win_w - 6) cx = win_w - 6 - ctx_w;
    if (cy + ctx_h > win_h - STATUS_BAR_H - 4) cy = win_h - STATUS_BAR_H - 4 - ctx_h;
    if (cx < 6) cx = 6;
    if (cy < 30) cy = 30;

    gfx_fill_rect(pixels, win_w, win_h, cx, cy, ctx_w, ctx_h, COLOR_WIN_BG);
    gfx_draw_bevel(pixels, win_w, win_h, cx, cy, ctx_w, ctx_h, 0);

    for (int i = 0; i < num_actions; i++) {
        int iy = cy + 3 + i * 20;
        int is_hover = (i == g_ctx_hover);
        const char *label = (g_ctx_target_idx >= 0) ? g_ctx_item_actions[i] : g_ctx_space_actions[i];

        if (is_hover) {
            gfx_fill_rect(pixels, win_w, win_h, cx + 2, iy, ctx_w - 4, 19, COLOR_SEL_BG);
            gfx_draw_string(pixels, win_w, win_h, cx + 16, iy + 2, label, COLOR_SEL_FG, 0, 1);
        } else {
            uint32_t txt_col = COLOR_TEXT;
            if (g_ctx_target_idx < 0 && i == 0 && !s_clip_has_data) {
                txt_col = COLOR_BTN_SHADOW; /* Disabled Paste */
            }
            gfx_draw_string(pixels, win_w, win_h, cx + 16, iy + 2, label, txt_col, 0, 1);
        }
    }
}

static void execute_ctx_action(int idx) {
    if (g_ctx_target_idx >= 0 && g_ctx_target_idx < g_item_count) {
        file_item_t *item = &g_items[g_ctx_target_idx];
        switch (idx) {
            case 0: /* Open */
                execute_or_open(item);
                break;
            case 1: /* Open in Notepad */
                if (item->type == 0) {
                    char full_path[PATH_MAX_LEN];
                    build_full_path(item->name, full_path, sizeof(full_path));
                    syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/notepad.exe", (uint64_t)(uintptr_t)full_path, 0, 0, 0);
                    str_copy(g_status_msg, "Opened in Notepad", sizeof(g_status_msg));
                } else {
                    str_copy(g_status_msg, "Cannot open directory in Notepad", sizeof(g_status_msg));
                }
                break;
            case 2: /* Copy */
                g_selected_idx = g_ctx_target_idx;
                action_copy_selected();
                break;
            case 3: /* Cut */
                g_selected_idx = g_ctx_target_idx;
                action_cut_selected();
                break;
            case 4: /* Rename */
                g_selected_idx = g_ctx_target_idx;
                action_start_rename();
                break;
            case 5: /* Delete */
                g_selected_idx = g_ctx_target_idx;
                action_delete_selected();
                break;
            case 6: /* Properties */ {
                char pbuf[64];
                char sbuf[16];
                format_size(item->size, sbuf, sizeof(sbuf));
                str_copy(pbuf, item->name, sizeof(pbuf));
                str_cat(pbuf, " (", sizeof(pbuf));
                str_cat(pbuf, sbuf, sizeof(pbuf));
                str_cat(pbuf, ", ", sizeof(pbuf));
                str_cat(pbuf, item->type_str, sizeof(pbuf));
                str_cat(pbuf, ")", sizeof(pbuf));
                str_copy(g_status_msg, pbuf, sizeof(g_status_msg));
                break;
            }
        }
    } else {
        switch (idx) {
            case 0: /* Paste */
                action_paste();
                break;
            case 1: /* New Folder */
                action_new_folder();
                break;
            case 2: /* New Text File */
                action_new_text_file();
                break;
            case 3: /* Refresh */
                read_directory(g_cwd);
                break;
            case 4: /* Sort by Name */
                sort_items();
                break;
            case 5: /* Sort by Size */
                sort_items_by_size();
                break;
        }
    }
}

static void draw_error_modal(uint32_t *pixels, int win_w, int win_h) {
    int mw = 340, mh = 140;
    int mx = (win_w - mw) / 2;
    int my = (win_h - mh) / 2;
    if (mx < 4) mx = 4;
    if (my < 4) my = 4;

    gfx_draw_error_dialog(pixels, win_w, win_h, mx, my, mw, mh, "pseuDOS", g_modal_err_l1, g_modal_err_l2);
}

/* UI Rendering */
static void render_filemgr(void) {
    if (!g_client.pixels) return;
    int win_w = (int)g_client.width;
    int win_h = (int)g_client.height;

    /* 1. Base Window Background */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, 0, win_w, win_h, COLOR_WIN_BG);

    /* 2. Top Toolbar (y = 2 to 28) */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 2, 2, win_w - 4, TOOLBAR_H, COLOR_BTN_FACE);

    btn_t buttons[] = {
        { "Up",         4,   4, 46, 22, BTN_UP },
        { "Root",       54,  4, 50, 22, BTN_ROOT },
        { "Apps",       108, 4, 50, 22, BTN_APPS },
        { "New Folder", 162, 4, 88, 22, BTN_NEW_FOLDER },
        { (g_view_mode == VIEW_DETAILS) ? "Icons" : "Details", 254, 4, 56, 22, BTN_VIEW },
        { "Refresh",    314, 4, 62, 22, BTN_REFRESH },
        { "Delete",     380, 4, 58, 22, BTN_DELETE }
    };
    int num_buttons = 7;

    for (int b = 0; b < num_buttons; b++) {
        gfx_draw_bevel(g_client.pixels, win_w, win_h, buttons[b].x, buttons[b].y, buttons[b].w, buttons[b].h, 0);
        gfx_draw_string(g_client.pixels, win_w, win_h, buttons[b].x + 6, buttons[b].y + 4, buttons[b].label, COLOR_TEXT, 0, 1);
    }

    /* Filter Box on Right Side of Toolbar (Option 5) */
    int filter_x = win_w - 146;
    int filter_y = 4;
    int filter_w = 140;
    int filter_h = 22;
    if (filter_x > 444) {
        gfx_draw_string(g_client.pixels, win_w, win_h, filter_x - 38, filter_y + 4, "Find:", COLOR_TEXT, 0, 1);
        gfx_draw_bevel(g_client.pixels, win_w, win_h, filter_x, filter_y, filter_w, filter_h, 1);
        gfx_fill_rect(g_client.pixels, win_w, win_h, filter_x + 1, filter_y + 1, filter_w - 2, filter_h - 2, COLOR_WHITE);
        if (g_filter[0] != '\0') {
            gfx_draw_string(g_client.pixels, win_w, win_h, filter_x + 4, filter_y + 3, g_filter, COLOR_TEXT, 0, 1);
        } else if (g_filter_active) {
            gfx_fill_rect(g_client.pixels, win_w, win_h, filter_x + 4, filter_y + 3, 2, 14, COLOR_BLACK);
        } else {
            gfx_draw_string(g_client.pixels, win_w, win_h, filter_x + 4, filter_y + 3, "Filter...", COLOR_BTN_SHADOW, 0, 1);
        }
    }

    /* 3. Address Bar with Clickable Breadcrumbs (Option 5) */
    gfx_draw_string(g_client.pixels, win_w, win_h, 8, 38, "Address:", COLOR_TEXT, 0, 1);
    int addr_x = 76;
    int addr_y = 34;
    int addr_w = win_w - addr_x - 6;
    int addr_h = 20;
    gfx_draw_bevel(g_client.pixels, win_w, win_h, addr_x, addr_y, addr_w, addr_h, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, addr_x + 1, addr_y + 1, addr_w - 2, addr_h - 2, COLOR_WHITE);

    /* Render Clickable Breadcrumb Pills */
    g_breadcrumb_count = 0;
    int bx = addr_x + 4;
    int by = addr_y + 2;

    /* Always start with Root */
    int root_w = 40;
    g_breadcrumbs[0].x = bx;
    g_breadcrumbs[0].y = by;
    g_breadcrumbs[0].w = root_w;
    g_breadcrumbs[0].h = 16;
    str_copy(g_breadcrumbs[0].path, "/", sizeof(g_breadcrumbs[0].path));
    gfx_fill_rect(g_client.pixels, win_w, win_h, bx, by, root_w, 16, COLOR_BREADCRUMB);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, bx, by, root_w, 16, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, bx + 4, by + 1, "Root", COLOR_TEXT, 0, 1);
    bx += root_w + 2;
    g_breadcrumb_count = 1;

    /* Parse subfolders */
    if (!str_eq(g_cwd, "/")) {
        char accum[PATH_MAX_LEN] = "";
        const char *p = g_cwd;
        while (*p) {
            if (*p == '/') { p++; continue; }
            char token[64];
            int ti = 0;
            while (*p && *p != '/' && ti < 63) token[ti++] = *p++;
            token[ti] = '\0';

            str_cat(accum, "/", sizeof(accum));
            str_cat(accum, token, sizeof(accum));

            /* Draw separator '>' */
            gfx_draw_string(g_client.pixels, win_w, win_h, bx, by + 1, ">", COLOR_BTN_SHADOW, 0, 1);
            bx += 10;

            int seg_w = (int)str_len(token) * 8 + 10;
            if (bx + seg_w > addr_x + addr_w - 4) break;

            if (g_breadcrumb_count < 16) {
                g_breadcrumbs[g_breadcrumb_count].x = bx;
                g_breadcrumbs[g_breadcrumb_count].y = by;
                g_breadcrumbs[g_breadcrumb_count].w = seg_w;
                g_breadcrumbs[g_breadcrumb_count].h = 16;
                str_copy(g_breadcrumbs[g_breadcrumb_count].path, accum, sizeof(g_breadcrumbs[g_breadcrumb_count].path));
                g_breadcrumb_count++;
            }

            gfx_fill_rect(g_client.pixels, win_w, win_h, bx, by, seg_w, 16, COLOR_BREADCRUMB);
            gfx_draw_bevel(g_client.pixels, win_w, win_h, bx, by, seg_w, 16, 0);
            gfx_draw_string(g_client.pixels, win_w, win_h, bx + 5, by + 1, token, COLOR_TEXT, 0, 1);
            bx += seg_w + 2;
        }
    }

    /* 4. Left Sidebar: Quick Access (Option 2) */
    int sb_x = 6;
    int sb_y = 58;
    int sb_w = SIDEBAR_W;
    int list_h = win_h - sb_y - STATUS_BAR_H - 6;
    if (list_h < 60) list_h = 60;

    gfx_draw_bevel(g_client.pixels, win_w, win_h, sb_x, sb_y, sb_w, list_h, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, sb_x + 1, sb_y + 1, sb_w - 2, list_h - 2, COLOR_WHITE);

    /* Sidebar Header */
    gfx_fill_rect(g_client.pixels, win_w, win_h, sb_x + 1, sb_y + 1, sb_w - 2, 20, COLOR_BTN_FACE);
    gfx_fill_rect(g_client.pixels, win_w, win_h, sb_x + 1, sb_y + 20, sb_w - 2, 1, COLOR_BTN_SHADOW);
    gfx_draw_string(g_client.pixels, win_w, win_h, sb_x + 6, sb_y + 3, "Quick Access", COLOR_TEXT, 0, 1);

    /* Sidebar Items */
    for (int s = 0; s < NUM_SIDEBAR_ITEMS; s++) {
        int sy = sb_y + 24 + s * 24;
        int is_curr = str_eq(g_cwd, g_sidebar[s].path);
        if (is_curr) {
            gfx_fill_rect(g_client.pixels, win_w, win_h, sb_x + 2, sy - 2, sb_w - 4, 22, COLOR_SEL_BG);
        }
        draw_icon(g_client.pixels, win_w, win_h, sb_x + 6, sy + 1, g_sidebar[s].icon);
        uint32_t tcol = is_curr ? COLOR_SEL_FG : COLOR_TEXT;
        gfx_draw_string(g_client.pixels, win_w, win_h, sb_x + 24, sy + 1, g_sidebar[s].label, tcol, 0, 1);
    }

    /* 5. Main Content Area (Right Pane) */
    int list_x = sb_x + sb_w + 4;
    int list_w = win_w - list_x - 6;

    gfx_draw_bevel(g_client.pixels, win_w, win_h, list_x, sb_y, list_w, list_h, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, sb_y + 1, list_w - 2, list_h - 2, COLOR_WHITE);

    if (g_view_mode == VIEW_DETAILS) {
        /* Table Column Headers */
        int hdr_y = sb_y + 1;
        int hdr_h = 20;
        gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, hdr_y, list_w - 2, hdr_h, COLOR_BTN_FACE);
        gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, hdr_y + hdr_h - 1, list_w - 2, 1, COLOR_BTN_SHADOW);

        int col_name_x = list_x + 4;
        int col_name_w = 150;
        int col_type_x = col_name_x + col_name_w;
        int col_type_w = 110;
        int col_size_x = col_type_x + col_type_w;
        int col_size_w = 75;
        int col_date_x = col_size_x + col_size_w;
        int col_date_max_x = list_x + list_w - SCROLLBAR_W - 4;

        draw_string_clipped(g_client.pixels, win_w, win_h, col_name_x + 6, hdr_y + 3, "Name", COLOR_TEXT, col_type_x - 4);
        gfx_fill_rect(g_client.pixels, win_w, win_h, col_type_x - 2, hdr_y + 2, 1, hdr_h - 4, COLOR_BTN_SHADOW);
        draw_string_clipped(g_client.pixels, win_w, win_h, col_type_x + 6, hdr_y + 3, "Type", COLOR_TEXT, col_size_x - 4);
        gfx_fill_rect(g_client.pixels, win_w, win_h, col_size_x - 2, hdr_y + 2, 1, hdr_h - 4, COLOR_BTN_SHADOW);
        draw_string_clipped(g_client.pixels, win_w, win_h, col_size_x + 6, hdr_y + 3, "Size", COLOR_TEXT, col_date_x - 4);
        gfx_fill_rect(g_client.pixels, win_w, win_h, col_date_x - 2, hdr_y + 2, 1, hdr_h - 4, COLOR_BTN_SHADOW);
        draw_string_clipped(g_client.pixels, win_w, win_h, col_date_x + 6, hdr_y + 3, "Modified", COLOR_TEXT, col_date_max_x);

        /* Rows */
        int content_y = hdr_y + hdr_h;
        int content_h = list_h - hdr_h - 2;
        int visible_rows = content_h / ROW_HEIGHT;
        int max_scroll = (g_item_count > visible_rows) ? (g_item_count - visible_rows) : 0;
        if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;
        if (g_scroll_offset < 0) g_scroll_offset = 0;

        int row_w = list_w - SCROLLBAR_W - 4;
        int drawn = 0;

        for (int i = 0; i < g_item_count; i++) {
            file_item_t *item = &g_items[i];
            /* Apply Search Filter (Option 5) */
            if (g_filter[0] != '\0' && !str_eq(item->name, "..")) {
                if (!str_contains_case_insensitive(item->name, g_filter)) continue;
            }

            if (drawn < g_scroll_offset) {
                drawn++;
                continue;
            }

            int r = drawn - g_scroll_offset;
            if (r >= visible_rows) break;
            drawn++;

            int ry = content_y + r * ROW_HEIGHT;
            int is_selected = (i == g_selected_idx);

            uint32_t bg_color = is_selected ? COLOR_SEL_BG : ((i % 2 == 1) ? COLOR_ROW_ALT : COLOR_WHITE);
            uint32_t text_color = is_selected ? COLOR_SEL_FG : COLOR_TEXT;

            gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, ry, row_w, ROW_HEIGHT, bg_color);

            /* Icon */
            draw_icon(g_client.pixels, win_w, win_h, col_name_x + 4, ry + 2, get_item_icon_type(item));

            /* Name or In-Place Rename Box */
            if (g_renaming && i == g_rename_target_idx) {
                int rbox_x = col_name_x + 22;
                int rbox_w = col_name_w - 26;
                gfx_fill_rect(g_client.pixels, win_w, win_h, rbox_x, ry + 1, rbox_w, 18, COLOR_WHITE);
                gfx_draw_bevel(g_client.pixels, win_w, win_h, rbox_x, ry + 1, rbox_w, 18, 1);
                draw_string_clipped(g_client.pixels, win_w, win_h, rbox_x + 2, ry + 2, g_rename_buf, COLOR_TEXT, rbox_x + rbox_w - 4);
                int clen = (int)str_len(g_rename_buf);
                if (rbox_x + 2 + clen * 8 + 2 <= rbox_x + rbox_w - 2) {
                    gfx_fill_rect(g_client.pixels, win_w, win_h, rbox_x + 2 + clen * 8, ry + 2, 2, 14, COLOR_BLACK);
                }
            } else {
                draw_string_clipped(g_client.pixels, win_w, win_h, col_name_x + 24, ry + 2, item->name, text_color, col_type_x - 6);
            }

            /* Type */
            draw_string_clipped(g_client.pixels, win_w, win_h, col_type_x + 6, ry + 2, item->type_str, text_color, col_size_x - 6);

            /* Size */
            if (item->type == 0) {
                char sbuf[16];
                format_size(item->size, sbuf, sizeof(sbuf));
                draw_string_clipped(g_client.pixels, win_w, win_h, col_size_x + 6, ry + 2, sbuf, text_color, col_date_x - 6);
            }

            /* Modified */
            char date_clean[24];
            str_copy(date_clean, item->date, sizeof(date_clean));
            if (str_len(date_clean) > 16) date_clean[16] = '\0';
            draw_string_clipped(g_client.pixels, win_w, win_h, col_date_x + 6, ry + 2, date_clean, text_color, col_date_max_x);
        }
    } else {
        /* Large Icons View (Option 3) */
        int content_y = sb_y + 4;
        int cell_w = 76;
        int cell_h = 64;
        int grid_cols = (list_w - SCROLLBAR_W - 8) / cell_w;
        if (grid_cols < 1) grid_cols = 1;

        int drawn = 0;
        for (int i = 0; i < g_item_count; i++) {
            file_item_t *item = &g_items[i];
            if (g_filter[0] != '\0' && !str_eq(item->name, "..")) {
                if (!str_contains_case_insensitive(item->name, g_filter)) continue;
            }

            int item_drawn_idx = drawn++;
            int col = item_drawn_idx % grid_cols;
            int row = item_drawn_idx / grid_cols - g_scroll_offset;

            int cell_x = list_x + 6 + col * cell_w;
            int cell_y = content_y + row * cell_h;

            if (cell_y < content_y - cell_h || cell_y > sb_y + list_h - 10) continue;

            int is_selected = (i == g_selected_idx);
            if (is_selected) {
                gfx_fill_rect(g_client.pixels, win_w, win_h, cell_x + 2, cell_y + 2, cell_w - 4, cell_h - 4, COLOR_SEL_BG);
            }

            /* 32x32 Large Icon */
            draw_icon_32(g_client.pixels, win_w, win_h, cell_x + (cell_w - 32) / 2, cell_y + 4, get_item_icon_type(item));

            /* Truncated Centered Name */
            char label[10];
            str_copy(label, item->name, sizeof(label));
            if (str_len(item->name) >= 9) {
                label[6] = '.';
                label[7] = '.';
                label[8] = '\0';
            }
            int tw = (int)str_len(label) * 8;
            int lx = cell_x + (cell_w - tw) / 2;
            uint32_t tcol = is_selected ? COLOR_SEL_FG : COLOR_TEXT;
            gfx_draw_string(g_client.pixels, win_w, win_h, lx, cell_y + 42, label, tcol, 0, 1);
        }
    }

    /* Scrollbar */
    int sb_bar_x = win_w - 6 - SCROLLBAR_W - 1;
    gfx_fill_rect(g_client.pixels, win_w, win_h, sb_bar_x, sb_y + 1, SCROLLBAR_W, list_h - 2, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, sb_bar_x, sb_y + 1, SCROLLBAR_W, SCROLLBAR_W, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, sb_bar_x + 4, sb_y + 3, "^", COLOR_TEXT, 0, 1);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, sb_bar_x, sb_y + list_h - SCROLLBAR_W - 1, SCROLLBAR_W, SCROLLBAR_W, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, sb_bar_x + 4, sb_y + list_h - SCROLLBAR_W + 1, "v", COLOR_TEXT, 0, 1);

    /* 6. Bottom Status Bar */
    int status_y = win_h - STATUS_BAR_H - 2;
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 6, status_y, win_w - 12, STATUS_BAR_H, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, 7, status_y + 1, win_w - 14, STATUS_BAR_H - 2, COLOR_BTN_FACE);

    char stat_txt[96];
    char cbuf[16];
    u64_to_str(g_item_count, cbuf);
    str_copy(stat_txt, cbuf, sizeof(stat_txt));
    str_cat(stat_txt, " items | ", sizeof(stat_txt));
    if (g_selected_idx >= 0 && g_selected_idx < g_item_count) {
        file_item_t *sel = &g_items[g_selected_idx];
        str_cat(stat_txt, sel->name, sizeof(stat_txt));
        if (sel->type == 0) {
            str_cat(stat_txt, " (", sizeof(stat_txt));
            char sbuf[16];
            format_size(sel->size, sbuf, sizeof(sbuf));
            str_cat(stat_txt, sbuf, sizeof(stat_txt));
            str_cat(stat_txt, ")", sizeof(stat_txt));
        }
    } else {
        char tbuf[16];
        format_size((uint32_t)g_total_bytes, tbuf, sizeof(tbuf));
        str_cat(stat_txt, tbuf, sizeof(stat_txt));
    }
    str_cat(stat_txt, " | ", sizeof(stat_txt));
    str_cat(stat_txt, g_status_msg, sizeof(stat_txt));

    gfx_draw_string(g_client.pixels, win_w, win_h, 12, status_y + 3, stat_txt, COLOR_TEXT, 0, 1);

    /* 7. Context Menu */
    if (g_ctx_open) {
        draw_ctx_menu(g_client.pixels, win_w, win_h);
    }

    /* 8. Modal Dialog for Errors / RAW Filesystem */
    if (g_modal_err_open) {
        draw_error_modal(g_client.pixels, win_w, win_h);
    }

    ntfs_client_damage(&g_client, 0, 0, win_w, win_h);
}

void filemgr_main(void) {
    if (ntfs_client_connect(&g_client, "File Manager", DEFAULT_WIN_W, DEFAULT_WIN_H, NTFS_WIN_NORMAL) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    read_directory("/");
    render_filemgr();

    ntfs_msg_t msg;
    while (1) {
        while (ntfs_client_poll_event(&g_client, &msg)) {
            int win_w = (int)g_client.width;
            int win_h = (int)g_client.height;

            if (msg.type == NTFS_MSG_WINDOW_RESIZED) {
                render_filemgr();
            } else if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                ntfs_client_close(&g_client);
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                return;
            } else if (msg.type == NTFS_MSG_MOUSE_EVENT) {
                int mx = msg.x;
                int my = msg.y;
                uint8_t left_down = (msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                uint8_t right_down = (msg.buttons_or_key & NTFS_MOUSE_BTN_RIGHT);

                uint8_t left_click = (left_down && !g_prev_left_down);
                uint8_t right_click = (right_down && !g_prev_right_down);

                g_prev_left_down = left_down;
                g_prev_right_down = right_down;

                /* Handle Error / Warning Modal Dialog Click */
                if (g_modal_err_open) {
                    if (left_click) {
                        int mw = 340, mh = 140;
                        int mx_box = (win_w - mw) / 2;
                        int my_box = (win_h - mh) / 2;
                        if (mx_box < 4) mx_box = 4;
                        if (my_box < 4) my_box = 4;
                        int btn_w = 64, btn_h = 22;
                        int btn_x = mx_box + (mw - btn_w) / 2;
                        int btn_y = my_box + mh - 30;

                        if ((mx >= btn_x && mx <= btn_x + btn_w && my >= btn_y && my <= btn_y + btn_h) ||
                            (mx >= mx_box && mx <= mx_box + mw && my >= my_box && my <= my_box + mh)) {
                            g_modal_err_open = 0;
                            render_filemgr();
                        }
                    }
                    continue;
                }

                /* Handle Context Menu Hover and Click */
                if (g_ctx_open) {
                    int num_actions = (g_ctx_target_idx >= 0) ? CTX_ITEM_ACTION_COUNT : CTX_SPACE_ACTION_COUNT;
                    int ctx_w = 150;
                    int ctx_h = 6 + num_actions * 20;
                    int cx = g_ctx_x;
                    int cy = g_ctx_y;
                    if (cx + ctx_w > win_w - 6) cx = win_w - 6 - ctx_w;
                    if (cy + ctx_h > win_h - STATUS_BAR_H - 4) cy = win_h - STATUS_BAR_H - 4 - ctx_h;
                    if (cx < 6) cx = 6;
                    if (cy < 30) cy = 30;

                    if (mx >= cx && mx < cx + ctx_w && my >= cy && my < cy + ctx_h) {
                        int hover = (my - cy - 3) / 20;
                        if (hover >= 0 && hover < num_actions) {
                            g_ctx_hover = hover;
                        } else {
                            g_ctx_hover = -1;
                        }
                    } else {
                        g_ctx_hover = -1;
                    }

                    if (left_click) {
                        if (g_ctx_hover >= 0) {
                            execute_ctx_action(g_ctx_hover);
                        }
                        g_ctx_open = 0;
                        render_filemgr();
                        continue;
                    }
                }

                /* Handle Context Menu Right Click */
                if (right_click) {
                    int sb_y = 58;
                    int list_x = 6 + SIDEBAR_W + 4;
                    int list_w = win_w - list_x - 6;
                    int list_h = win_h - sb_y - STATUS_BAR_H - 6;

                    if (mx >= list_x && mx <= list_x + list_w - SCROLLBAR_W && my >= sb_y && my < sb_y + list_h) {
                        if (g_view_mode == VIEW_DETAILS) {
                            int content_y = sb_y + 21;
                            int row = (my - content_y) / ROW_HEIGHT;
                            int clicked_idx = g_scroll_offset + row;
                            if (clicked_idx >= 0 && clicked_idx < g_item_count) {
                                g_selected_idx = clicked_idx;
                                g_ctx_target_idx = clicked_idx;
                            } else {
                                g_ctx_target_idx = -1;
                            }
                        } else {
                            /* Icons View */
                            int cell_w = 76;
                            int cell_h = 64;
                            int grid_cols = (list_w - SCROLLBAR_W - 8) / cell_w;
                            if (grid_cols < 1) grid_cols = 1;
                            int col = (mx - list_x - 6) / cell_w;
                            int row = (my - sb_y - 4) / cell_h + g_scroll_offset;
                            int clicked_idx = row * grid_cols + col;
                            if (col >= 0 && col < grid_cols && clicked_idx >= 0 && clicked_idx < g_item_count) {
                                g_selected_idx = clicked_idx;
                                g_ctx_target_idx = clicked_idx;
                            } else {
                                g_ctx_target_idx = -1;
                            }
                        }
                    } else {
                        g_ctx_target_idx = -1;
                    }
                    g_ctx_x = mx;
                    g_ctx_y = my;
                    g_ctx_open = 1;
                    g_ctx_hover = -1;
                    render_filemgr();
                    continue;
                }

                /* Handle Mouse Wheel Scrolling */
                if (msg.buttons_or_key & NTFS_MOUSE_WHEEL_UP) {
                    if (g_scroll_offset > 0) {
                        g_scroll_offset -= 2;
                        if (g_scroll_offset < 0) g_scroll_offset = 0;
                        render_filemgr();
                    }
                } else if (msg.buttons_or_key & NTFS_MOUSE_WHEEL_DOWN) {
                    g_scroll_offset += 2;
                    render_filemgr();
                }

                if (left_click) {
                    /* Check Toolbar Buttons */
                    if (my >= 4 && my <= 26) {
                        if (mx >= 4 && mx <= 50) {
                            go_up();
                            render_filemgr();
                        } else if (mx >= 54 && mx <= 104) {
                            read_directory("/");
                            render_filemgr();
                        } else if (mx >= 108 && mx <= 158) {
                            read_directory("/protected/apps");
                            render_filemgr();
                        } else if (mx >= 162 && mx <= 250) {
                            action_new_folder();
                            render_filemgr();
                        } else if (mx >= 254 && mx <= 310) {
                            /* Toggle View Mode (Option 3) */
                            g_view_mode = (g_view_mode == VIEW_DETAILS) ? VIEW_ICONS : VIEW_DETAILS;
                            g_scroll_offset = 0;
                            render_filemgr();
                        } else if (mx >= 314 && mx <= 376) {
                            read_directory(g_cwd);
                            render_filemgr();
                        } else if (mx >= 380 && mx <= 438) {
                            action_delete_selected();
                            render_filemgr();
                        } else if (mx >= win_w - 146 && mx <= win_w - 6) {
                            /* Focus Search Filter */
                            g_filter_active = 1;
                            render_filemgr();
                        }
                    }

                    /* Check Clickable Breadcrumbs in Address Bar (Option 5) */
                    if (my >= 34 && my <= 54) {
                        for (int bc = 0; bc < g_breadcrumb_count; bc++) {
                            if (mx >= g_breadcrumbs[bc].x && mx < g_breadcrumbs[bc].x + g_breadcrumbs[bc].w) {
                                read_directory(g_breadcrumbs[bc].path);
                                render_filemgr();
                                break;
                            }
                        }
                    }

                    /* Check Sidebar Clicks (Option 2) */
                    int sb_x = 6;
                    int sb_y = 58;
                    int sb_w = SIDEBAR_W;
                    if (mx >= sb_x && mx < sb_x + sb_w && my >= sb_y + 22 && my < sb_y + 22 + NUM_SIDEBAR_ITEMS * 24) {
                        int s_idx = (my - (sb_y + 22)) / 24;
                        if (s_idx >= 0 && s_idx < NUM_SIDEBAR_ITEMS) {
                            read_directory(g_sidebar[s_idx].path);
                            render_filemgr();
                        }
                    }

                    /* Check Main List / Grid View Clicks */
                    int list_x = sb_x + sb_w + 4;
                    int list_w = win_w - list_x - 6;
                    int list_h = win_h - sb_y - STATUS_BAR_H - 6;

                    if (mx >= list_x && mx <= list_x + list_w - SCROLLBAR_W && my >= sb_y && my < sb_y + list_h) {
                        g_filter_active = 0; /* Defocus filter on list click */
                        int clicked_idx = -1;

                        if (g_view_mode == VIEW_DETAILS) {
                            int content_y = sb_y + 21;
                            int row = (my - content_y) / ROW_HEIGHT;
                            clicked_idx = g_scroll_offset + row;
                        } else {
                            /* Icons View */
                            int cell_w = 76;
                            int cell_h = 64;
                            int grid_cols = (list_w - SCROLLBAR_W - 8) / cell_w;
                            if (grid_cols < 1) grid_cols = 1;
                            int col = (mx - list_x - 6) / cell_w;
                            int row = (my - sb_y - 4) / cell_h + g_scroll_offset;
                            if (col >= 0 && col < grid_cols) {
                                clicked_idx = row * grid_cols + col;
                            }
                        }

                        if (clicked_idx >= 0 && clicked_idx < g_item_count) {
                            uint64_t cur_tick = (uint64_t)syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
                            if (clicked_idx == g_last_click_idx && (cur_tick - g_last_click_tick) < 45) {
                                execute_or_open(&g_items[clicked_idx]);
                                g_last_click_idx = -1;
                                g_last_click_tick = 0;
                            } else {
                                g_selected_idx = clicked_idx;
                                g_last_click_idx = clicked_idx;
                                g_last_click_tick = cur_tick;
                            }
                            render_filemgr();
                        }
                    }

                    /* Check Scrollbar Clicks */
                    int sb_bar_x = win_w - 6 - SCROLLBAR_W - 1;
                    if (mx >= sb_bar_x && mx <= sb_bar_x + SCROLLBAR_W && my >= sb_y + 1 && my < sb_y + list_h) {
                        if (my <= sb_y + SCROLLBAR_W) {
                            if (g_scroll_offset > 0) {
                                g_scroll_offset--;
                                render_filemgr();
                            }
                        } else if (my >= sb_y + list_h - SCROLLBAR_W) {
                            g_scroll_offset++;
                            render_filemgr();
                        }
                    }
                }
            } else if (msg.type == NTFS_MSG_KEY_EVENT) {
                uint32_t key = msg.buttons_or_key & 0xFF;

                /* Handle Error / Warning Modal Dialog Dismissal */
                if (g_modal_err_open) {
                    if (key == '\r' || key == '\n' || key == 27 || key == ' ') {
                        g_modal_err_open = 0;
                        render_filemgr();
                    }
                    continue;
                }

                /* Handle In-Place Rename Mode (Option 1) */
                if (g_renaming) {
                    if (key == '\r' || key == '\n') {
                        action_commit_rename();
                        render_filemgr();
                    } else if (key == 27) { /* Escape */
                        g_renaming = 0;
                        render_filemgr();
                    } else if (key == '\b') {
                        size_t l = str_len(g_rename_buf);
                        if (l > 0) {
                            g_rename_buf[l - 1] = '\0';
                            render_filemgr();
                        }
                    } else if (key >= 32 && key <= 126) {
                        size_t l = str_len(g_rename_buf);
                        if (l + 1 < sizeof(g_rename_buf)) {
                            g_rename_buf[l] = (char)key;
                            g_rename_buf[l + 1] = '\0';
                            render_filemgr();
                        }
                    }
                    continue;
                }

                /* Handle Quick Filter Input (Option 5) */
                if (g_filter_active) {
                    if (key == 27) { /* Escape clears filter */
                        g_filter[0] = '\0';
                        g_filter_active = 0;
                        render_filemgr();
                    } else if (key == '\r' || key == '\n') {
                        g_filter_active = 0;
                        render_filemgr();
                    } else if (key == '\b') {
                        size_t l = str_len(g_filter);
                        if (l > 0) {
                            g_filter[l - 1] = '\0';
                            render_filemgr();
                        }
                    } else if (key >= 32 && key <= 126) {
                        size_t l = str_len(g_filter);
                        if (l + 1 < sizeof(g_filter)) {
                            g_filter[l] = (char)key;
                            g_filter[l + 1] = '\0';
                            render_filemgr();
                        }
                    }
                    continue;
                }

                /* Normal Navigation Keys */
                if (key == '\r' || key == '\n') {
                    if (g_selected_idx >= 0 && g_selected_idx < g_item_count) {
                        execute_or_open(&g_items[g_selected_idx]);
                        render_filemgr();
                    }
                } else if (key == '\b') {
                    go_up();
                    render_filemgr();
                } else if (key == 27) {
                    if (g_ctx_open) {
                        g_ctx_open = 0;
                        render_filemgr();
                    } else {
                        ntfs_client_close(&g_client);
                        syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                        return;
                    }
                }
            }
        }
        syscall(SYS_SLEEP, 30, 0, 0, 0, 0);
    }
}
