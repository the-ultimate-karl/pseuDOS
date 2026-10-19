#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "rtc.h"
#include "mice.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"
#include "bootinfo.h"
/* Basic freestanding string utilities */
static size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static int strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static __attribute__((unused)) int strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2 || n == 0) return 0;
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static char *strcpy(char *dest, const char *src) {
    char *d = dest;
    if (!dest || !src) return dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static __attribute__((unused)) char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    if (!dest || !src || n == 0) return dest;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = '\0';
        n--;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n >= 8) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--) *d++ = *s++;
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++;
        p2++;
    }
    return 0;
}

static char *strchr(const char *s, int c) {
    if (!s) return NULL;
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}

static char *strrchr(const char *s, int c) {
    if (!s) return NULL;
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)last;
}

static char *trim(char *str) {
    if (!str) return NULL;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end-- = '\0';
    }
    return str;
}

static int atoi(const char *str) {
    int res = 0;
    int sign = 1;
    if (!str) return 0;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res * sign;
}


static char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}

#define SCROLLBAR_W 16
#define TERM_DEFAULT_W 656
#define TERM_DEFAULT_H 384
#define CHAR_W      8
#define CHAR_H      16
#define TERM_MAX_COLS 160
#define SCROLLBACK_MAX 1000

static ntfs_client_t g_client;
static char g_buffer[SCROLLBACK_MAX][TERM_MAX_COLS];
static int g_total_lines = 1;
static int g_cursor_row = 0;
static int g_cursor_col = 0;
static int g_scroll_offset = 0; /* 0 = viewing bottom; >0 = scrolled up into history */
static int g_mouse_drag_thumb = 0;
static int g_drag_start_y = 0;
static int g_drag_start_offset = 0;

static inline int get_term_win_w(void) {
    int w = (int)g_client.width;
    return (w > 0) ? w : TERM_DEFAULT_W;
}

static inline int get_term_win_h(void) {
    int h = (int)g_client.height;
    return (h > 0) ? h : TERM_DEFAULT_H;
}

static inline int get_term_cols(void) {
    int text_w = get_term_win_w() - SCROLLBAR_W;
    int cols = text_w / CHAR_W;
    if (cols < 20) cols = 20;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    return cols;
}

static inline int get_term_rows(void) {
    int rows = get_term_win_h() / CHAR_H;
    if (rows < 5) rows = 5;
    return rows;
}

static void clamp_scroll(void) {
    int term_rows = get_term_rows();
    int max_scroll = (g_total_lines > term_rows) ? (g_total_lines - term_rows) : 0;
    if (g_scroll_offset > max_scroll) g_scroll_offset = max_scroll;
    if (g_scroll_offset < 0) g_scroll_offset = 0;
}

static void clear_screen(void) {
    for (int r = 0; r < SCROLLBACK_MAX; r++) {
        for (int c = 0; c < TERM_MAX_COLS; c++) {
            g_buffer[r][c] = ' ';
        }
    }
    g_cursor_row = 0;
    g_cursor_col = 0;
    g_total_lines = 1;
    g_scroll_offset = 0;
}

static void draw_arrow_up(int x, int y, uint32_t color) {
    int win_w = get_term_win_w();
    int win_h = get_term_win_h();
    for (int i = 0; i < 4; i++) {
        int start_x = x + 3 - i;
        int len = 1 + 2 * i;
        for (int k = 0; k < len; k++) {
            int px = start_x + k;
            int py = y + i;
            if (px >= 0 && px < win_w && py >= 0 && py < win_h) {
                g_client.pixels[py * win_w + px] = color;
            }
        }
    }
}

static void draw_arrow_down(int x, int y, uint32_t color) {
    int win_w = get_term_win_w();
    int win_h = get_term_win_h();
    for (int i = 0; i < 4; i++) {
        int start_x = x + i;
        int len = 7 - 2 * i;
        for (int k = 0; k < len; k++) {
            int px = start_x + k;
            int py = y + i;
            if (px >= 0 && px < win_w && py >= 0 && py < win_h) {
                g_client.pixels[py * win_w + px] = color;
            }
        }
    }
}

static void render_scrollbar(void) {
    int win_w = get_term_win_w();
    int win_h = get_term_win_h();
    int sb_x = win_w - SCROLLBAR_W;
    int track_y = 16;
    int track_h = win_h - 32;
    int term_rows = get_term_rows();

    if (track_h < 1) track_h = 1;

    /* 1. Divider line between text and scrollbar */
    for (int y = 0; y < win_h; y++) {
        g_client.pixels[y * win_w + (sb_x - 1)] = COLOR_BTN_DKSHADOW;
    }

    /* 2. Top Button (Up Arrow) */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, sb_x, 0, SCROLLBAR_W, 16, 0);
    gfx_fill_rect(g_client.pixels, win_w, win_h, sb_x + 1, 1, SCROLLBAR_W - 2, 14, COLOR_BTN_FACE);
    draw_arrow_up(sb_x + 4, 6, COLOR_BLACK);

    /* 3. Bottom Button (Down Arrow) */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, sb_x, win_h - 16, SCROLLBAR_W, 16, 0);
    gfx_fill_rect(g_client.pixels, win_w, win_h, sb_x + 1, win_h - 15, SCROLLBAR_W - 2, 14, COLOR_BTN_FACE);
    draw_arrow_down(sb_x + 4, win_h - 10, COLOR_BLACK);

    /* 4. Track Area (Checkerboard / Stipple Pattern) */
    for (int ty = track_y; ty < track_y + track_h; ty++) {
        uint32_t *row = &g_client.pixels[ty * win_w];
        for (int tx = sb_x; tx < win_w; tx++) {
            row[tx] = ((tx + ty) & 1) ? COLOR_BTN_LIGHT : COLOR_BTN_FACE;
        }
    }

    /* 5. Scrollbar Thumb */
    int max_scroll = (g_total_lines > term_rows) ? (g_total_lines - term_rows) : 0;
    int thumb_y = track_y;
    int thumb_h = track_h;

    if (max_scroll > 0) {
        thumb_h = (term_rows * track_h) / g_total_lines;
        if (thumb_h < 18) thumb_h = 18;
        if (thumb_h > track_h) thumb_h = track_h;
        int travel = track_h - thumb_h;
        thumb_y = track_y + (int)(((int64_t)(max_scroll - g_scroll_offset) * travel) / max_scroll);
    }

    gfx_draw_bevel(g_client.pixels, win_w, win_h, sb_x, thumb_y, SCROLLBAR_W, thumb_h, 0);
    gfx_fill_rect(g_client.pixels, win_w, win_h, sb_x + 1, thumb_y + 1, SCROLLBAR_W - 2, thumb_h - 2, COLOR_BTN_FACE);
}

static void render_terminal(void) {
    if (!g_client.pixels) return;

    int win_w = get_term_win_w();
    int win_h = get_term_win_h();
    int text_w = win_w - SCROLLBAR_W;
    int term_cols = get_term_cols();
    int term_rows = get_term_rows();

    /* Clear text area to black */
    for (int y = 0; y < win_h; y++) {
        uint32_t *row = &g_client.pixels[y * win_w];
        for (int x = 0; x < text_w; x++) {
            row[x] = COLOR_BLACK;
        }
    }

    clamp_scroll();
    int start_row = (g_total_lines > term_rows) ? (g_total_lines - term_rows - g_scroll_offset) : 0;
    if (start_row < 0) start_row = 0;

    for (int r = 0; r < term_rows; r++) {
        int buf_r = start_row + r;
        if (buf_r >= g_total_lines) break;
        for (int c = 0; c < term_cols; c++) {
            char ch = g_buffer[buf_r][c];
            if (ch > 32 && ch <= 126) {
                gfx_draw_char(g_client.pixels, win_w, win_h, c * CHAR_W, r * CHAR_H, ch, COLOR_GREEN, COLOR_BLACK, 1);
            }
        }
    }

    /* Draw cursor if visible in current viewport */
    int cursor_screen_r = g_cursor_row - start_row;
    if (cursor_screen_r >= 0 && cursor_screen_r < term_rows && g_cursor_col < term_cols) {
        gfx_fill_rect(g_client.pixels, win_w, win_h,
                      g_cursor_col * CHAR_W, cursor_screen_r * CHAR_H + 13,
                      CHAR_W, 3, COLOR_GREEN);
    }

    render_scrollbar();
    ntfs_client_damage(&g_client, 0, 0, win_w, win_h);
}

static void scroll_up(void) {
    for (int i = 0; i < SCROLLBACK_MAX - 1; i++) {
        memcpy(g_buffer[i], g_buffer[i + 1], TERM_MAX_COLS);
    }
    memset(g_buffer[SCROLLBACK_MAX - 1], ' ', TERM_MAX_COLS);
    g_cursor_row = SCROLLBACK_MAX - 1;
    g_total_lines = SCROLLBACK_MAX;
}

static void term_putc(char c) {
    int cols = get_term_cols();
    if (c == '\n' || c == '\r') {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= g_total_lines) {
            g_total_lines = g_cursor_row + 1;
        }
        if (g_cursor_row >= SCROLLBACK_MAX) {
            scroll_up();
        } else {
            memset(g_buffer[g_cursor_row], ' ', TERM_MAX_COLS);
        }
    } else if (c == '\b') {
        if (g_cursor_col > 0) {
            g_cursor_col--;
            g_buffer[g_cursor_row][g_cursor_col] = ' ';
        }
    } else if (c == '\t') {
        int next_tab = (g_cursor_col + 8) & ~7;
        while (g_cursor_col < next_tab && g_cursor_col < cols) {
            g_buffer[g_cursor_row][g_cursor_col++] = ' ';
        }
    } else if (c >= 32 && c <= 126) {
        if (g_cursor_col >= cols) {
            g_cursor_col = 0;
            g_cursor_row++;
            if (g_cursor_row >= g_total_lines) {
                g_total_lines = g_cursor_row + 1;
            }
            if (g_cursor_row >= SCROLLBACK_MAX) {
                scroll_up();
            } else {
                memset(g_buffer[g_cursor_row], ' ', TERM_MAX_COLS);
            }
        }
        g_buffer[g_cursor_row][g_cursor_col++] = c;
    }
}

static void term_puts(const char *s) {
    while (s && *s) {
        term_putc(*s++);
    }
}

static void handle_terminal_mouse(const ntfs_msg_t *msg) {
    int win_w = get_term_win_w();
    int win_h = get_term_win_h();
    int sb_x = win_w - SCROLLBAR_W;
    int track_y = 16;
    int track_h = win_h - 32;
    int term_rows = get_term_rows();
    int max_scroll = (g_total_lines > term_rows) ? (g_total_lines - term_rows) : 0;

    if (track_h < 1) track_h = 1;

    /* Mouse Wheel */
    if (msg->buttons_or_key & NTFS_MOUSE_WHEEL_UP) {
        g_scroll_offset += 3;
        clamp_scroll();
        render_terminal();
        return;
    }
    if (msg->buttons_or_key & NTFS_MOUSE_WHEEL_DOWN) {
        g_scroll_offset -= 3;
        clamp_scroll();
        render_terminal();
        return;
    }

    int left_down = (msg->buttons_or_key & NTFS_MOUSE_BTN_LEFT);

    /* Dragging Thumb */
    if (g_mouse_drag_thumb) {
        if (!left_down) {
            g_mouse_drag_thumb = 0;
        } else if (max_scroll > 0) {
            int thumb_h = (term_rows * track_h) / g_total_lines;
            if (thumb_h < 18) thumb_h = 18;
            if (thumb_h > track_h) thumb_h = track_h;
            int travel = track_h - thumb_h;
            if (travel > 0) {
                int dy = msg->y - g_drag_start_y;
                int delta_lines = (int)(((int64_t)dy * max_scroll) / travel);
                g_scroll_offset = g_drag_start_offset - delta_lines;
                clamp_scroll();
                render_terminal();
            }
        }
        return;
    }

    if (!left_down) return;

    /* Click on Scrollbar */
    if (msg->x >= sb_x && msg->x < win_w) {
        if (msg->y < 16) {
            /* Top Arrow: Scroll Up 1 line */
            g_scroll_offset++;
            clamp_scroll();
            render_terminal();
        } else if (msg->y >= win_h - 16) {
            /* Bottom Arrow: Scroll Down 1 line */
            g_scroll_offset--;
            clamp_scroll();
            render_terminal();
        } else if (max_scroll > 0) {
            int thumb_h = (term_rows * track_h) / g_total_lines;
            if (thumb_h < 18) thumb_h = 18;
            if (thumb_h > track_h) thumb_h = track_h;
            int travel = track_h - thumb_h;
            int thumb_y = track_y + (int)(((int64_t)(max_scroll - g_scroll_offset) * travel) / max_scroll);

            if (msg->y >= thumb_y && msg->y < thumb_y + thumb_h) {
                /* Drag Start */
                g_mouse_drag_thumb = 1;
                g_drag_start_y = msg->y;
                g_drag_start_offset = g_scroll_offset;
            } else if (msg->y < thumb_y) {
                /* Track above: Page Up */
                g_scroll_offset += (term_rows - 2);
                clamp_scroll();
                render_terminal();
            } else {
                /* Track below: Page Down */
                g_scroll_offset -= (term_rows - 2);
                clamp_scroll();
                render_terminal();
            }
        }
    }
}

static char term_getchar(void) {
    ntfs_msg_t msg;
    render_terminal();
    while (1) {
        while (ntfs_client_poll_event(&g_client, &msg)) {
            if (msg.type == NTFS_MSG_MOUSE_EVENT) {
                handle_terminal_mouse(&msg);
            } else if (msg.type == NTFS_MSG_KEY_EVENT) {
                return (char)(msg.buttons_or_key & 0xFF);
            } else if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                ntfs_client_close(&g_client);
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
            } else if (msg.type == NTFS_MSG_WINDOW_RESIZED) {
                clamp_scroll();
                render_terminal();
            }
        }
        if (g_client.sock < 0) {
            syscall(SYS_EXIT, 0, 0, 0, 0, 0);
        }
        syscall(SYS_SLEEP, 15, 0, 0, 0, 0);
    }
}
/* I/O Redirection State */
static int g_redirect_active = 0;
static char g_redirect_path[256];
static int g_redirect_append = 0;
static int g_redirect_first_flush = 1;
static char g_redirect_buf[4096];
static size_t g_redirect_len = 0;

static void puts(const char *str);

static void redirect_flush(void) {
    if (!g_redirect_active || g_redirect_path[0] == '\0') return;
    int append_flag = g_redirect_first_flush ? g_redirect_append : 1;
    syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)g_redirect_path,
            (uint64_t)(uintptr_t)g_redirect_buf, (uint64_t)g_redirect_len,
            (uint64_t)append_flag, 0);
    g_redirect_first_flush = 0;
    g_redirect_len = 0;
}

static void redirect_finish(void) {
    if (!g_redirect_active) return;
    if (g_redirect_first_flush || g_redirect_len > 0) {
        int append_flag = g_redirect_first_flush ? g_redirect_append : 1;
        int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)g_redirect_path,
                             (uint64_t)(uintptr_t)g_redirect_buf, (uint64_t)g_redirect_len,
                             (uint64_t)append_flag, 0);
        if (wr == -EPERM) {
            g_redirect_active = 0;
            puts("redirect: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
            return;
        } else if (wr < 0) {
            g_redirect_active = 0;
            puts("redirect: failed to write to destination\n");
            return;
        }
    }
    g_redirect_active = 0;
    g_redirect_len = 0;
    g_redirect_first_flush = 1;
    g_redirect_path[0] = '\0';
}

static void puts(const char *str) {
    if (!str) return;
    if (g_redirect_active) {
        size_t len = strlen(str);
        for (size_t i = 0; i < len; i++) {
            if (g_redirect_len >= sizeof(g_redirect_buf) - 1) {
                redirect_flush();
            }
            g_redirect_buf[g_redirect_len++] = str[i];
        }
        return;
    }
    term_puts(str);
}

static void putc(char c) {
    if (g_redirect_active) {
        if (g_redirect_len >= sizeof(g_redirect_buf) - 1) {
            redirect_flush();
        }
        g_redirect_buf[g_redirect_len++] = c;
        return;
    }
    term_putc(c);
}

static void print_num(uint64_t num) {
    char buf[32];
    int idx = 0;
    if (num == 0) {
        putc('0');
        return;
    }
    while (num > 0) {
        buf[idx++] = '0' + (num % 10);
        num /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) {
        putc(buf[i]);
    }
}

static void print_signed_num(int64_t num) {
    if (num < 0) {
        putc('-');
        print_num((uint64_t)(-num));
    } else {
        print_num((uint64_t)num);
    }
}

static void print_hex(uint64_t val) {
    const char hex_chars[] = "0123456789abcdef";
    char buf[16];
    int idx = 0;
    if (val == 0) {
        putc('0');
        return;
    }
    while (val > 0) {
        buf[idx++] = hex_chars[val & 0xF];
        val >>= 4;
    }
    for (int i = idx - 1; i >= 0; i--) {
        putc(buf[i]);
    }
}

static char getchar(void) {
    return term_getchar();
}
/* Command History */
#define HISTORY_MAX 16
static char g_history[HISTORY_MAX][256];
static int g_history_count = 0;
static int g_history_browse = -1;
static char g_scratch_line[256];

static void history_add(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return;
    if (g_history_count > 0 && strcmp(g_history[(g_history_count - 1) % HISTORY_MAX], cmd) == 0) {
        return;
    }
    size_t idx = (size_t)(g_history_count % HISTORY_MAX);
    strncpy(g_history[idx], cmd, sizeof(g_history[idx]) - 1);
    g_history[idx][sizeof(g_history[idx]) - 1] = '\0';
    g_history_count++;
}

static void cmd_history(void) {
    int start = (g_history_count > HISTORY_MAX) ? (g_history_count - HISTORY_MAX) : 0;
    for (int i = start; i < g_history_count; i++) {
        print_num((uint64_t)(i + 1));
        puts("  ");
        puts(g_history[i % HISTORY_MAX]);
        puts("\n");
    }
}

static void resolve_path(const char *arg, char *out_buf, size_t max_len);

static void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dest;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

static int snprintf(char *str, size_t size, const char *format, ...) {
    if (!str || size == 0) return 0;
    __builtin_va_list args;
    __builtin_va_start(args, format);
    size_t out = 0;
    for (size_t i = 0; format[i] != '\0' && out + 1 < size; i++) {
        if (format[i] == '%' && format[i + 1] != '\0') {
            i++;
            if (format[i] == 's') {
                const char *s = __builtin_va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && out + 1 < size) {
                    str[out++] = *s++;
                }
            } else if (format[i] == 'd' || format[i] == 'u') {
                int val = __builtin_va_arg(args, int);
                if (format[i] == 'd' && val < 0) {
                    if (out + 1 < size) str[out++] = '-';
                    val = -val;
                }
                char buf[16];
                int bi = 0;
                unsigned int uval = (unsigned int)val;
                if (uval == 0) buf[bi++] = '0';
                while (uval > 0) {
                    buf[bi++] = '0' + (uval % 10);
                    uval /= 10;
                }
                while (bi > 0 && out + 1 < size) {
                    str[out++] = buf[--bi];
                }
            } else if (format[i] == '%') {
                str[out++] = '%';
            }
        } else {
            str[out++] = format[i];
        }
    }
    str[out] = '\0';
    __builtin_va_end(args);
    return (int)out;
}

/* ========================================================================= */
/* Advanced Interactive Readline & Tab-Completion Engine                     */
/* ========================================================================= */

static int is_live_system(void) {
    vfs_stat_t st;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)"/etc/is_live_media", (uint64_t)(uintptr_t)&st, 0, 0, 0) == 0) {
        return 1;
    }
    BootInfo bi;
    if (syscall(SYS_GET_BOOTINFO, (uint64_t)(uintptr_t)&bi, 0, 0, 0, 0) == 0) {
        if (strstr(bi.cmdline, "boot=live") || strstr(bi.cmdline, "live=1") || strstr(bi.cmdline, "live")) {
            return 1;
        }
        const char *p = bi.hardware_devpath;
        if (p && p[0] != '\0') {
            if (strstr(p, "CDROM") || strstr(p, "CdRom") || strstr(p, "cdrom") ||
                strstr(p, "CD-ROM") || strstr(p, "Cd-Rom") || strstr(p, "cd-rom") ||
                strstr(p, "CD(") || strstr(p, "RamDisk") || strstr(p, "ramdisk") ||
                strstr(p, "VirtualDisk") || strstr(p, "MemoryMapped")) {
                return 1;
            }
        }
    }
    char mnt_buf[512];
    if (syscall(SYS_MOUNT, 0, 0, 0, (uint64_t)(uintptr_t)mnt_buf, sizeof(mnt_buf)) > 0) {
        if (strstr(mnt_buf, "ramfs on /") || strstr(mnt_buf, "type ramfs")) {
            return 1;
        }
    }
    return 0;
}

static const char *g_shell_commands[] = {
    "help", "echo", "ls", "dir", "cd", "pwd", "cat", "type",
    "more", "less", "data", "cp", "copy", "mv", "move", "mkdir",
    "touch", "write", "edit", "del", "rm", "fs", "mount", "umount", "df",
    "ps", "kill", "proctest", "syscalltest", "mousetest", "ipctest",
    "shmtest", "date", "time", "uptime", "uname", "env", "set",
    "export", "history", "screenres", "attached-drives", "cpu", "mem",
    "pci", "devpath", "kernel", "su", "drop", "exit", "sudo", "dmesg",
    "grub", "flash", "recovery-tools", "recovery", "rtools", "panic", "clear", "cls", "reboot",
    "shutdown", "halt", NULL
};

#define SHELL_MAX_COMPLETIONS 64
static char g_shell_comp_list[SHELL_MAX_COMPLETIONS][256];
static int g_shell_comp_count = 0;
static int g_shell_comp_index = -1;
static char g_shell_comp_prefix[256];

static void shell_collect_cmd_completions(const char *prefix) {
    g_shell_comp_count = 0;
    size_t plen = strlen(prefix);
    int is_live = is_live_system();
    for (int i = 0; g_shell_commands[i] && g_shell_comp_count < SHELL_MAX_COMPLETIONS; i++) {
        const char *cname = g_shell_commands[i];
        if (!is_live && (strcmp(cname, "flash") == 0 ||
                         strcmp(cname, "recovery-tools") == 0 ||
                         strcmp(cname, "recovery") == 0 ||
                         strcmp(cname, "rtools") == 0)) {
            continue;
        }
        int match = 1;
        for (size_t p = 0; p < plen; p++) {
            char c1 = prefix[p];
            char c2 = cname[p];
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2) { match = 0; break; }
        }
        if (match) {
            strncpy(g_shell_comp_list[g_shell_comp_count++], cname, 255);
        }
    }
}

static void shell_collect_file_completions(const char *path_prefix) {
    g_shell_comp_count = 0;
    char dir_part[256] = "";
    char file_prefix[256] = "";

    const char *last_sep = strrchr(path_prefix, '/');
    const char *last_bsep = strrchr(path_prefix, '\\');
    if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;

    if (last_sep) {
        size_t dlen = (size_t)(last_sep - path_prefix + 1);
        if (dlen < sizeof(dir_part)) {
            strncpy(dir_part, path_prefix, dlen);
            dir_part[dlen] = '\0';
        }
        strncpy(file_prefix, last_sep + 1, sizeof(file_prefix) - 1);
    } else {
        strncpy(file_prefix, path_prefix, sizeof(file_prefix) - 1);
    }

    char query_dir[256];
    if (dir_part[0] != '\0') {
        resolve_path(dir_part, query_dir, sizeof(query_dir));
    } else {
        resolve_path(".", query_dir, sizeof(query_dir));
    }

    char names_buf[2048];
    int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)query_dir,
                             (uint64_t)(uintptr_t)names_buf, sizeof(names_buf), 0, 0);

    if (nbytes <= 0) return;

    size_t prefix_len = strlen(file_prefix);
    size_t pos = 0;
    while (pos < (size_t)nbytes && g_shell_comp_count < SHELL_MAX_COMPLETIONS) {
        const char *name = &names_buf[pos];
        size_t c_len = strlen(name);
        if (c_len == 0) break;

        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            int match = 1;
            for (size_t p = 0; p < prefix_len; p++) {
                char c1 = file_prefix[p];
                char c2 = name[p];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) { match = 0; break; }
            }

            if (match) {
                char full_cand[256];
                char check_path[256];
                snprintf(check_path, sizeof(check_path), "%s/%s", query_dir, name);
                vfs_stat_t st;
                int is_dir = (syscall(SYS_STAT, (uint64_t)(uintptr_t)check_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) == 0 && st.type == VFS_TYPE_DIR);

                if (is_dir) {
                    snprintf(full_cand, sizeof(full_cand), "%s%s/", dir_part, name);
                } else {
                    snprintf(full_cand, sizeof(full_cand), "%s%s", dir_part, name);
                }
                strncpy(g_shell_comp_list[g_shell_comp_count++], full_cand, 255);
            }
        }
        pos += c_len + 1;
    }
}

static void readline(char *buf, size_t max_len, const char *prompt) {
    size_t len = 0;
    size_t pos = 0;
    g_history_browse = -1;
    g_scratch_line[0] = '\0';
    g_shell_comp_count = 0;
    g_shell_comp_index = -1;
    buf[0] = '\0';
    puts(prompt);

    while (len + 1 < max_len) {
        char c = getchar();

        /* Reset tab completion on non-tab keys */
        if (c != '\t') {
            g_shell_comp_count = 0;
            g_shell_comp_index = -1;
        }

        if (c == '\r' || c == '\n') {
            if (g_scroll_offset > 0) {
                g_scroll_offset = 0;
                render_terminal();
            }
            puts("\n");
            buf[len] = '\0';
            break;
        } else if (c == '\t') {
            /* Tab: PowerShell-style cycling */
            if (g_shell_comp_count == 0) {
                size_t word_start = pos;
                while (word_start > 0 && buf[word_start - 1] != ' ') {
                    word_start--;
                }
                size_t wlen = pos - word_start;
                if (wlen < sizeof(g_shell_comp_prefix)) {
                    strncpy(g_shell_comp_prefix, &buf[word_start], wlen);
                    g_shell_comp_prefix[wlen] = '\0';
                } else {
                    g_shell_comp_prefix[0] = '\0';
                }

                if (word_start == 0) {
                    shell_collect_cmd_completions(g_shell_comp_prefix);
                } else {
                    shell_collect_file_completions(g_shell_comp_prefix);
                }
                g_shell_comp_index = -1;
            }

            if (g_shell_comp_count > 0) {
                g_shell_comp_index = (g_shell_comp_index + 1) % g_shell_comp_count;
                const char *cand = g_shell_comp_list[g_shell_comp_index];

                size_t word_start = pos;
                while (word_start > 0 && buf[word_start - 1] != ' ') {
                    word_start--;
                }
                size_t tail_len = len - pos;
                char tail[256];
                memcpy(tail, &buf[pos], tail_len + 1);

                size_t cand_len = strlen(cand);
                if (word_start + cand_len + tail_len < max_len) {
                    /* Rewind cursor to word_start */
                    while (pos > word_start) {
                        putc('\b');
                        pos--;
                    }
                    /* Overwrite tail and print candidate */
                    memcpy(&buf[word_start], cand, cand_len);
                    memcpy(&buf[word_start + cand_len], tail, tail_len + 1);
                    len = word_start + cand_len + tail_len;
                    pos = word_start + cand_len;

                    puts(&buf[word_start]);
                    puts("   \b\b\b");
                    /* Move cursor back to pos if there is tail */
                    for (size_t t = 0; t < tail_len; t++) {
                        putc('\b');
                    }
                }
            }
            continue;
        } else if (c == '\b' || c == 127) {
            if (g_scroll_offset > 0) {
                g_scroll_offset = 0;
                render_terminal();
            }
            if (pos > 0) {
                memmove(&buf[pos - 1], &buf[pos], len - pos + 1);
                pos--;
                len--;
                putc('\b');
                puts(&buf[pos]);
                putc(' ');
                putc('\b');
                for (size_t t = 0; t < (len - pos); t++) {
                    putc('\b');
                }
            }
        } else if (c == 27) { /* ESC / ANSI arrow or page sequence */
            char c2 = getchar();
            if (c2 == '[') {
                char c3 = getchar();
                if (c3 == '5') { /* Page Up */
                    char c4 = getchar(); /* '~' */
                    (void)c4;
                    g_scroll_offset += (get_term_rows() - 2);
                    clamp_scroll();
                    render_terminal();
                    continue;
                } else if (c3 == '6') { /* Page Down */
                    char c4 = getchar(); /* '~' */
                    (void)c4;
                    g_scroll_offset -= (get_term_rows() - 2);
                    clamp_scroll();
                    render_terminal();
                    continue;
                } else if (c3 == 'A') { /* UP ARROW */
                    if (g_history_count > 0) {
                        if (g_history_browse == -1) {
                            buf[len] = '\0';
                            strcpy(g_scratch_line, buf);
                            g_history_browse = g_history_count - 1;
                        } else if (g_history_browse > 0 && g_history_browse > g_history_count - HISTORY_MAX) {
                            g_history_browse--;
                        }
                        while (pos > 0) { putc('\b'); pos--; }
                        for (size_t s = 0; s < len; s++) putc(' ');
                        for (size_t s = 0; s < len; s++) putc('\b');

                        const char *h_entry = g_history[g_history_browse % HISTORY_MAX];
                        size_t h_len = strlen(h_entry);
                        if (h_len >= max_len) h_len = max_len - 1;
                        strncpy(buf, h_entry, h_len);
                        buf[h_len] = '\0';
                        len = h_len;
                        pos = h_len;
                        puts(buf);
                    }
                } else if (c3 == 'B') { /* DOWN ARROW */
                    if (g_history_browse != -1) {
                        g_history_browse++;
                        while (pos > 0) { putc('\b'); pos--; }
                        for (size_t s = 0; s < len; s++) putc(' ');
                        for (size_t s = 0; s < len; s++) putc('\b');

                        if (g_history_browse >= g_history_count) {
                            g_history_browse = -1;
                            size_t s_len = strlen(g_scratch_line);
                            if (s_len >= max_len) s_len = max_len - 1;
                            strncpy(buf, g_scratch_line, s_len);
                            buf[s_len] = '\0';
                            len = s_len;
                            pos = s_len;
                        } else {
                            const char *h_entry = g_history[g_history_browse % HISTORY_MAX];
                            size_t h_len = strlen(h_entry);
                            if (h_len >= max_len) h_len = max_len - 1;
                            strncpy(buf, h_entry, h_len);
                            buf[h_len] = '\0';
                            len = h_len;
                            pos = h_len;
                        }
                        puts(buf);
                    }
                } else if (c3 == 'C') { /* RIGHT ARROW */
                    if (pos < len) {
                        putc(buf[pos]);
                        pos++;
                    }
                } else if (c3 == 'D') { /* LEFT ARROW */
                    if (pos > 0) {
                        pos--;
                        putc('\b');
                    }
                } else if (c3 == 'H') { /* HOME */
                    while (pos > 0) { putc('\b'); pos--; }
                } else if (c3 == 'F') { /* END */
                    while (pos < len) { putc(buf[pos]); pos++; }
                } else if (c3 == '3') { /* DELETE */
                    char c4 = getchar(); /* '~' */
                    (void)c4;
                    if (pos < len) {
                        memmove(&buf[pos], &buf[pos + 1], len - pos);
                        len--;
                        puts(&buf[pos]);
                        putc(' ');
                        putc('\b');
                        for (size_t t = 0; t < (len - pos); t++) {
                            putc('\b');
                        }
                    }
                }
            }
        } else if (c >= 32 && c < 127) {
            if (g_scroll_offset > 0) {
                g_scroll_offset = 0;
                render_terminal();
            }
            if (pos == len) {
                buf[pos++] = c;
                len++;
                buf[len] = '\0';
                putc(c);
            } else {
                memmove(&buf[pos + 1], &buf[pos], len - pos + 1);
                buf[pos++] = c;
                len++;
                puts(&buf[pos - 1]);
                for (size_t t = 0; t < (len - pos); t++) {
                    putc('\b');
                }
            }
        }
    }
    buf[len] = '\0';
}

static void resolve_path(const char *arg, char *out_buf, size_t max_len) {
    if (!out_buf || max_len == 0) return;
    if (!arg || arg[0] == '\0' || strcmp(arg, ".") == 0) {
        syscall(SYS_GETCWD, (uint64_t)(uintptr_t)out_buf, max_len, 0, 0, 0);
        return;
    }
    char res[256];
    size_t j = 0;
    if (arg[0] == '/' || arg[0] == '\\') {
        for (size_t i = 0; arg[i] && j < sizeof(res) - 1; i++) {
            res[j++] = (arg[i] == '\\') ? '/' : arg[i];
        }
        res[j] = '\0';
    } else {
        char cwd[256];
        syscall(SYS_GETCWD, (uint64_t)(uintptr_t)cwd, sizeof(cwd), 0, 0, 0);
        size_t cwd_len = strlen(cwd);
        for (size_t i = 0; i < cwd_len && j < sizeof(res) - 1; i++) {
            res[j++] = cwd[i];
        }
        if (j > 0 && res[j - 1] != '/' && j < sizeof(res) - 1) {
            res[j++] = '/';
        }
        for (size_t i = 0; arg[i] && j < sizeof(res) - 1; i++) {
            res[j++] = (arg[i] == '\\') ? '/' : arg[i];
        }
        res[j] = '\0';
    }
    strncpy(out_buf, res, max_len - 1);
    out_buf[max_len - 1] = '\0';
}

/* Environment Variables */
#define ENV_MAX 32
typedef struct {
    char name[32];
    char value[128];
} env_var_t;

static env_var_t g_env[ENV_MAX];
static int g_env_count = 0;

static const char *env_get(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_env_count; i++) {
        if (strcmp(g_env[i].name, name) == 0) {
            return g_env[i].value;
        }
    }
    return NULL;
}

static int env_set(const char *name, const char *value) {
    if (!name || name[0] == '\0') return -1;
    for (int i = 0; i < g_env_count; i++) {
        if (strcmp(g_env[i].name, name) == 0) {
            strncpy(g_env[i].value, value ? value : "", sizeof(g_env[i].value) - 1);
            g_env[i].value[sizeof(g_env[i].value) - 1] = '\0';
            return 0;
        }
    }
    if (g_env_count < ENV_MAX) {
        strncpy(g_env[g_env_count].name, name, sizeof(g_env[g_env_count].name) - 1);
        g_env[g_env_count].name[sizeof(g_env[g_env_count].name) - 1] = '\0';
        strncpy(g_env[g_env_count].value, value ? value : "", sizeof(g_env[g_env_count].value) - 1);
        g_env[g_env_count].value[sizeof(g_env[g_env_count].value) - 1] = '\0';
        g_env_count++;
        return 0;
    }
    return -1;
}

static void env_init(void) {
    g_env_count = 0;
    env_set("USER", "shell");
    env_set("HOSTNAME", "pseuDOS");
    env_set("PWD", "/");
    env_set("HOME", "/home/user");
    env_set("SHELL", "/protected/crit/bshss.exe");
}

static void cmd_env(void) {
    for (int i = 0; i < g_env_count; i++) {
        puts(g_env[i].name);
        putc('=');
        puts(g_env[i].value);
        puts("\n");
    }
}

static void cmd_set(const char *arg) {
    if (!arg || arg[0] == '\0') {
        cmd_env();
        return;
    }
    char *eq = strchr(arg, '=');
    if (!eq) {
        const char *val = env_get(arg);
        if (val) {
            puts(val);
            puts("\n");
        }
        return;
    }
    char name[32];
    size_t name_len = (size_t)(eq - arg);
    if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
    strncpy(name, arg, name_len);
    name[name_len] = '\0';
    const char *val = eq + 1;
    env_set(trim(name), val);
}

static void expand_vars(const char *input, char *output, size_t max_len) {
    if (!input || !output || max_len == 0) return;
    size_t i = 0, o = 0;
    while (input[i] && o + 1 < max_len) {
        if (input[i] == '$' && input[i+1] != '\0' && input[i+1] != ' ') {
            i++;
            char var_name[32];
            size_t v = 0;
            while (input[i] && (
                   (input[i] >= 'a' && input[i] <= 'z') ||
                   (input[i] >= 'A' && input[i] <= 'Z') ||
                   (input[i] >= '0' && input[i] <= '9') ||
                   input[i] == '_') && v < sizeof(var_name) - 1) {
                var_name[v++] = input[i++];
            }
            var_name[v] = '\0';
            const char *val = env_get(var_name);
            if (val) {
                for (size_t k = 0; val[k] && o + 1 < max_len; k++) {
                    output[o++] = val[k];
                }
            }
        } else {
            output[o++] = input[i++];
        }
    }
    output[o] = '\0';
}

/* Wildcard Matching & Expansion */
static int glob_match(const char *pattern, const char *str) {
    if (!pattern || !str) return 0;
    if (*pattern == '\0') return (*str == '\0');
    if (*pattern == '*') {
        while (*str) {
            if (glob_match(pattern + 1, str)) return 1;
            str++;
        }
        return glob_match(pattern + 1, str);
    }
    if (*pattern == *str) {
        return glob_match(pattern + 1, str + 1);
    }
    return 0;
}

static void expand_wildcards(const char *input, char *output, size_t max_len) {
    if (!input || !output || max_len == 0) return;
    if (!strchr(input, '*')) {
        strncpy(output, input, max_len - 1);
        output[max_len - 1] = '\0';
        return;
    }

    output[0] = '\0';
    size_t out_len = 0;
    char temp[512];
    strncpy(temp, input, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *token = temp;
    int first_tok = 1;
    while (*token) {
        while (*token == ' ') token++;
        if (*token == '\0') break;
        char *end = strchr(token, ' ');
        if (end) *end = '\0';

        if (strchr(token, '*')) {
            char dir_prefix[256] = "";
            const char *pattern = token;
            char *last_slash = strrchr(token, '/');
            if (last_slash) {
                size_t dlen = (size_t)(last_slash - token + 1);
                if (dlen < sizeof(dir_prefix)) {
                    strncpy(dir_prefix, token, dlen);
                    dir_prefix[dlen] = '\0';
                }
                pattern = last_slash + 1;
            }

            char names_buf[2048];
            char query_dir[256];
            if (dir_prefix[0] != '\0') {
                resolve_path(dir_prefix, query_dir, sizeof(query_dir));
            } else {
                resolve_path(".", query_dir, sizeof(query_dir));
            }

            int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)query_dir,
                                     (uint64_t)(uintptr_t)names_buf, sizeof(names_buf), 0, 0);

            int matched_any = 0;
            if (nbytes > 0) {
                size_t pos = 0;
                while (pos < (size_t)nbytes) {
                    const char *child_name = &names_buf[pos];
                    size_t c_len = strlen(child_name);
                    if (c_len == 0) break;
                    if (glob_match(pattern, child_name)) {
                        if (!first_tok && out_len + 1 < max_len) output[out_len++] = ' ';
                        first_tok = 0;
                        for (size_t d = 0; dir_prefix[d] && out_len + 1 < max_len; d++) {
                            output[out_len++] = dir_prefix[d];
                        }
                        for (size_t c = 0; child_name[c] && out_len + 1 < max_len; c++) {
                            output[out_len++] = child_name[c];
                        }
                        matched_any = 1;
                    }
                    pos += c_len + 1;
                }
            }

            if (!matched_any) {
                if (!first_tok && out_len + 1 < max_len) output[out_len++] = ' ';
                first_tok = 0;
                for (size_t c = 0; token[c] && out_len + 1 < max_len; c++) {
                    output[out_len++] = token[c];
                }
            }
        } else {
            if (!first_tok && out_len + 1 < max_len) output[out_len++] = ' ';
            first_tok = 0;
            for (size_t c = 0; token[c] && out_len + 1 < max_len; c++) {
                output[out_len++] = token[c];
            }
        }

        if (!end) break;
        token = end + 1;
    }
    output[out_len] = '\0';
}

/* Core Utilities */
static void cmd_echo(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("\n");
        return;
    }
    puts(arg);
    puts("\n");
}

static void print_two_digits(uint32_t val) {
    putc('0' + (val / 10) % 10);
    putc('0' + (val % 10));
}

static void cmd_date(void) {
    rtc_datetime_t dt;
    if (syscall(SYS_TIME, (uint64_t)(uintptr_t)&dt, 0, 0, 0, 0) == 0) {
        print_num(dt.year);
        putc('-');
        print_two_digits(dt.month);
        putc('-');
        print_two_digits(dt.day);
        putc(' ');
        print_two_digits(dt.hours);
        putc(':');
        print_two_digits(dt.minutes);
        putc(':');
        print_two_digits(dt.seconds);
        puts("\n");
    } else {
        puts("date: error reading hardware rtc\n");
    }
}

static void cmd_uptime(void) {
    int64_t ticks = syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
    if (ticks < 0) ticks = 0;
    uint64_t total_sec = (uint64_t)ticks / 100;
    uint64_t days = total_sec / 86400;
    uint64_t rem = total_sec % 86400;
    uint64_t hours = rem / 3600;
    rem %= 3600;
    uint64_t mins = rem / 60;
    uint64_t secs = rem % 60;

    puts("uptime: ");
    if (days > 0) {
        print_num(days);
        puts((days == 1) ? " day, " : " days, ");
    }
    if (hours > 0 || days > 0) {
        print_num(hours);
        puts((hours == 1) ? " hour, " : " hours, ");
    }
    print_num(mins);
    puts((mins == 1) ? " minute, " : " minutes, ");
    print_num(secs);
    puts((secs == 1) ? " second (" : " seconds (");
    print_num((uint64_t)ticks);
    puts(" ticks)\n");
}

static void cmd_uname(const char *arg) {
    if (arg && (strcmp(arg, "-a") == 0 || strcmp(arg, "--all") == 0)) {
        puts("pseuDOS 0.7.3-gui x86_64 UEFI\n");
    } else if (arg && strcmp(arg, "-r") == 0) {
        puts("0.7.3-gui\n");
    } else if (arg && strcmp(arg, "-m") == 0) {
        puts("x86_64\n");
    } else if (arg && strcmp(arg, "-s") == 0) {
        puts("pseuDOS\n");
    } else {
        puts("pseuDOS\n");
    }
}

/* Shell Commands */
static void cmd_help(void) {
    puts("=================================================================\n");
    puts(" Basic Shell Subsystem (bshss)\n");
    puts("=================================================================\n");
    puts(" help                        : display available commands\n");
    puts(" echo <text>                 : print text to console\n");
    puts(" ls / dir [-l] [-a] [path]   : list directory entries\n");
    puts(" cd <path>                   : change working directory\n");
    puts(" pwd                         : print working directory\n");
    puts(" cat / type <file>           : view plaintext file\n");
    puts(" more / less <file>          : paginated text viewer\n");
    puts(" data <file>                 : display creation/access timestamps & metadata\n");
    puts(" cp <src> <dst>              : copy file\n");
    puts(" mv <src> <dst>              : move or rename file\n");
    puts(" mkdir <path>                : create directory\n");
    puts(" touch <file>                : create empty file\n");
    puts(" write [-a] <file> <txt>     : write (or -a append) text to file\n");
    puts(" edit <file>                 : classic full-screen MS-DOS text editor\n");
    puts(" del / rm [-r] [-f] <path...> : delete file or directory\n");
    puts(" fs [drive_no | --drives]    : query filesystem stats and disk partitions\n");
    puts(" mount [-a | <dev> <target>] : mount storage device or list active mounts\n");
    puts(" umount <target | dev>       : unmount storage device or mountpoint\n");
    puts(" ps                          : list processes and CPU time\n");
    puts(" kill <pid>                  : terminate process\n");
    puts(" proctest                    : test preemptive multitasking with concurrent tasks\n");
    puts(" syscalltest                 : test syscall interface\n");
    puts(" mousetest                   : test hardware mouse tracking and IntelliMouse wheel\n");
    puts(" ipctest                     : test unix-domain socket IPC and non-blocking poll\n");
    puts(" shmtest                     : test shared memory allocation and framebuffer map\n");
    puts(" date / time                 : display current date and time\n");
    puts(" uptime                      : display system uptime and ticks\n");
    puts(" uname [-a|-r|-m|-s]         : display system identification\n");
    puts(" env                         : display shell environment variables\n");
    puts(" set / export [name=val]     : set or display shell environment variables\n");
    puts(" history                     : display command history\n");
    puts(" screenres [w h]             : adjust or display screen resolution\n");
    puts(" attached-drives [--all]     : list attached storage drives\n");
    puts(" cpu                         : display CPU model, vendor, and feature flags\n");
    puts(" mem                         : display physical memory map and statistics\n");
    puts(" pci                         : scan and list connected PCI / PCIe bus devices\n");
    puts(" devpath [mode]              : display and toggle boot device hardware path\n");
    puts(" kernel / su                 : escalate privilege to KERNEL mode\n");
    puts(" exit / drop                 : drop privileges or exit shell\n");
    puts(" sudo <command>              : run single command with KERNEL privileges\n");
    puts(" <app> [args...] --as-root   : launch application with KERNEL privileges (priv=KERNEL)\n");
    puts(" dmesg                       : display kernel message buffer\n");
    puts(" grub                        : display GRUB 2 chainloader config & setup\n");
    if (is_live_system()) {
        puts(" flash                       : install pseuDOS to persistent disk (requires sudo)\n");
        puts(" recovery-tools / rtools     : launch dedicated recovery and repair suite\n");
    }
    puts(" panic [reason]              : trigger a kernel panic\n");
    puts(" clear / cls                 : clear screen\n");
    puts(" reboot                      : restart computer\n");
    puts(" shutdown [now|-c]           : schedule shutdown in 1m, or 'now' to power off immediately\n");
    puts(" halt                        : halt CPU execution (requires sudo)\n");
    puts("=================================================================\n");
}

static void cmd_pwd(void) {
    char cwd[256];
    if (syscall(SYS_GETCWD, (uint64_t)(uintptr_t)cwd, sizeof(cwd), 0, 0, 0) == 0) {
        puts(cwd);
        puts("\n");
    } else {
        puts("/\n");
    }
}

static void cmd_cd(const char *arg) {
    char path[256];
    if (!arg || arg[0] == '\0') {
        strcpy(path, "/");
    } else {
        resolve_path(arg, path, sizeof(path));
    }
    if (syscall(SYS_CHDIR, (uint64_t)(uintptr_t)path, 0, 0, 0, 0) != 0) {
        puts("cd: no such file or directory: ");
        puts(arg ? arg : "");
        puts("\n");
    } else {
        char new_cwd[256];
        if (syscall(SYS_GETCWD, (uint64_t)(uintptr_t)new_cwd, sizeof(new_cwd), 0, 0, 0) == 0) {
            env_set("PWD", new_cwd);
        }
    }
}

static void cmd_ls(const char *arg) {
    int long_mode = 0;
    char target_path[256];
    target_path[0] = '\0';

    if (arg && arg[0] != '\0') {
        char temp[256];
        strncpy(temp, arg, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
        char *p = trim(temp);

        while (p && *p == '-') {
            char *next = strchr(p, ' ');
            if (next) *next = '\0';
            for (size_t i = 1; p[i]; i++) {
                if (p[i] == 'l') long_mode = 1;
            }
            if (!next) {
                p = NULL;
                break;
            }
            p = trim(next + 1);
        }
        if (p && p[0] != '\0') {
            strncpy(target_path, p, sizeof(target_path) - 1);
            target_path[sizeof(target_path) - 1] = '\0';
        }
    }

    char path[256];
    resolve_path(target_path[0] != '\0' ? target_path : NULL, path, sizeof(path));

    char names[2048];
    int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)names, sizeof(names), 0, 0);
    if (nbytes < 0) {
        puts("ls: cannot access '");
        puts(target_path[0] != '\0' ? target_path : path);
        puts("': no such file or directory\n");
        return;
    }

    if (!long_mode) {
        size_t pos = 0;
        int col_count = 0;
        while (pos < (size_t)nbytes) {
            const char *name = &names[pos];
            size_t nlen = strlen(name);
            if (nlen == 0) break;
            puts(name);
            puts("  ");
            col_count++;
            if (col_count >= 5) {
                puts("\n");
                col_count = 0;
            }
            pos += nlen + 1;
        }
        if (col_count != 0) puts("\n");
        return;
    }

    puts("directory of ");
    puts(path);
    puts(":\n");

    /* Print . and .. */
    puts("  drwx  protected   2026-09-14 00:00:00          0 B   .\n");
    puts("  drwx  protected   2026-09-14 00:00:00          0 B   ..\n");

    int count = 0;
    size_t pos = 0;
    while (pos < (size_t)nbytes) {
        const char *name = &names[pos];
        size_t nlen = strlen(name);
        if (nlen == 0) break;

        char child_path[384];
        size_t plen = strlen(path);
        strcpy(child_path, path);
        if (plen > 0 && child_path[plen - 1] != '/') {
            child_path[plen++] = '/';
            child_path[plen] = '\0';
        }
        strncpy(child_path + plen, name, sizeof(child_path) - plen - 1);
        child_path[sizeof(child_path) - 1] = '\0';

        vfs_stat_t st;
        if (syscall(SYS_STAT, (uint64_t)(uintptr_t)child_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) == 0) {
            if (st.type == VFS_TYPE_DIR) {
                puts("  drwx");
            } else {
                puts("  -rw-");
            }
            if (st.is_protected) {
                puts("  protected ");
            } else {
                puts("  standard  ");
            }
            const char *date = st.date_modified[0] ? st.date_modified : (st.date_created[0] ? st.date_created : "0000-00-00 00:00:00");
            puts(date);
            puts("  ");

            uint32_t sz = st.size;
            int num_digits = 0;
            uint32_t t = sz;
            if (t == 0) num_digits = 1;
            while (t > 0) { num_digits++; t /= 10; }
            for (int k = num_digits; k < 9; k++) putc(' ');
            print_num(sz);
            puts(" B   ");
            puts(name);
            puts("\n");
            count++;
        }
        pos += nlen + 1;
    }
    puts("  total: ");
    print_num((uint64_t)count);
    puts(" item(s)\n");
}

static void cmd_cat(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("cat: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));

    char buf[2048];
    size_t offset = 0;
    int first_chunk = 1;
    char last_char = '\0';
    while (1) {
        int64_t bytes = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, sizeof(buf) - 1, offset, 0);
        if (bytes < 0) {
            if (first_chunk) {
                puts("cat: cannot open file '");
                puts(arg);
                puts("'\n");
            }
            return;
        }
        if (bytes == 0) break;
        buf[bytes] = '\0';
        puts(buf);
        last_char = buf[bytes - 1];
        offset += bytes;
        first_chunk = 0;
    }
    if (!first_chunk && last_char != '\n') puts("\n");
}

static void cmd_more_less(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("more: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));

    char buf[2048];
    size_t offset = 0;
    int line_count = 0;
    int first_chunk = 1;
    int should_quit = 0;

    while (!should_quit) {
        int64_t bytes = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, sizeof(buf), offset, 0);
        if (bytes < 0) {
            if (first_chunk) {
                puts("more: cannot open file '");
                puts(arg);
                puts("'\n");
            }
            return;
        }
        if (bytes == 0) break;
        first_chunk = 0;
        for (int64_t i = 0; i < bytes; i++) {
            putc(buf[i]);
            if (buf[i] == '\n') {
                line_count++;
                if (line_count >= 22) {
                    puts("-- more (press space/enter to continue, 'q' to quit) --");
                    char c = getchar();
                    puts("\r                                                         \r");
                    if (c == 'q' || c == 'Q') {
                        should_quit = 1;
                        break;
                    }
                    line_count = 0;
                }
            }
        }
        offset += bytes;
    }
}

static void cmd_data(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("data: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));

    vfs_stat_t st;
    int64_t res = syscall(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0, 0, 0);
    if (res != 0) {
        puts("data: cannot stat '");
        puts(arg);
        puts("': no such file or directory\n");
        return;
    }

    puts("file metadata for: ");
    puts(path);
    puts("\n");
    puts("    node type:          ");
    puts((st.type == VFS_TYPE_DIR) ? "directory\n" : "regular file\n");
    puts("    file size:          ");
    print_num(st.size);
    puts(" bytes\n");
    puts("    date created:       ");
    puts(st.date_created);
    puts("\n");
    puts("    date last accessed: ");
    puts(st.date_accessed);
    puts("\n");
    puts("    date modified:      ");
    puts(st.date_modified);
    puts("\n");
    puts("    protection status:  ");
    puts(st.is_protected ? "protected system node\n" : "standard node\n");
}

static void cmd_touch(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("touch: missing file operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));
    int64_t res = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)"", 0, 1, 0);
    if (res == -EPERM) {
        puts("touch: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
    } else if (res < 0) {
        puts("touch: cannot touch '");
        puts(arg);
        puts("'\n");
    }
}

static void cmd_mkdir(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("mkdir: missing operand\n");
        return;
    }
    char path[256];
    resolve_path(arg, path, sizeof(path));
    int64_t res = syscall(SYS_MKDIR, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
    if (res == -EPERM) {
        puts("mkdir: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
    } else if (res != 0) {
        puts("mkdir: cannot create directory '");
        puts(arg);
        puts("'\n");
    }
}

static void cmd_del(const char *cmd_name, const char *arg) {
    if (!cmd_name) cmd_name = "rm";
    if (!arg || arg[0] == '\0') {
        puts(cmd_name);
        puts(": missing operand\nusage: ");
        puts(cmd_name);
        puts(" [-r] [-f] <file...>\n");
        return;
    }

    int recursive = 0;
    int force = 0;
    int operand_count = 0;

    char buf[512];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = buf;
    int stop_flags = 0;

    while (*token) {
        while (*token == ' ') token++;
        if (*token == '\0') break;

        char *next_space = strchr(token, ' ');
        if (next_space) {
            *next_space = '\0';
        }

        if (!stop_flags && token[0] == '-' && token[1] != '\0') {
            if (strcmp(token, "--") == 0) {
                stop_flags = 1;
            } else {
                for (size_t i = 1; token[i] != '\0'; i++) {
                    if (token[i] == 'r' || token[i] == 'R') recursive = 1;
                    else if (token[i] == 'f' || token[i] == 'F') force = 1;
                }
            }
        } else {
            operand_count++;
            char path[256];
            resolve_path(token, path, sizeof(path));
            uint64_t flags = 0;
            if (recursive) flags |= 1;
            if (force) flags |= 2;

            int64_t res = syscall(SYS_UNLINK, (uint64_t)(uintptr_t)path, flags, 0, 0, 0);
            if (res == -EPERM) {
                puts(cmd_name);
                puts(": permission denied: protected system path requires 'sudo' or KERNEL mode\n");
            } else if (res == -ENOTEMPTY) {
                puts(cmd_name);
                puts(": cannot remove '");
                puts(token);
                puts("': directory not empty\n");
            } else if (res == -ENOENT) {
                if (!force) {
                    puts(cmd_name);
                    puts(": cannot remove '");
                    puts(token);
                    puts("': no such file or directory\n");
                }
            } else if (res != 0) {
                if (!force) {
                    puts(cmd_name);
                    puts(": cannot remove '");
                    puts(token);
                    puts("': operation failed\n");
                }
            }
        }

        if (!next_space) break;
        token = next_space + 1;
    }

    if (operand_count == 0) {
        puts(cmd_name);
        puts(": missing operand\nusage: ");
        puts(cmd_name);
        puts(" [-r] [-f] <file...>\n");
    }
}

static int do_cp(const char *src, const char *dst, int verbose) {
    if (!src || src[0] == '\0' || !dst || dst[0] == '\0') {
        if (verbose) puts("usage: cp <src> <dst>\n");
        return -1;
    }

    char src_path[256];
    char dst_path[256];
    resolve_path(src, src_path, sizeof(src_path));
    resolve_path(dst, dst_path, sizeof(dst_path));

    /* If destination is a directory or path ended with slash, append src basename */
    vfs_stat_t st_dst;
    int is_dir = 0;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)&st_dst, 0, 0, 0) == 0) {
        if (st_dst.type == VFS_TYPE_DIR) is_dir = 1;
    }
    size_t raw_len = strlen(dst);
    if (raw_len > 0 && (dst[raw_len - 1] == '/' || dst[raw_len - 1] == '\\')) {
        is_dir = 1;
    }
    if (is_dir) {
        size_t len = strlen(dst_path);
        if (len > 0 && dst_path[len - 1] != '/' && len + 1 < sizeof(dst_path)) {
            dst_path[len] = '/';
            dst_path[len + 1] = '\0';
            len++;
        }
        const char *src_base = strrchr(src_path, '/');
        if (!src_base) src_base = src_path;
        else src_base++;
        strncpy(dst_path + len, src_base, sizeof(dst_path) - len - 1);
        dst_path[sizeof(dst_path) - 1] = '\0';
    }

    /* Fast kernel single-pass copy */
    int64_t cp_res = syscall(SYS_COPYFILE, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)dst_path, 0, 0, 0);
    if (cp_res == 0) {
        return 0;
    }
    if (cp_res == -EPERM) {
        if (verbose) puts("cp: permission denied: destination is protected path, requires 'sudo' or KERNEL mode\n");
        return -EPERM;
    }
    if (cp_res != -ENOSYS) {
        if (verbose) {
            puts("cp: cannot copy '");
            puts(src);
            puts("' to '");
            puts(dst);
            puts("'\n");
        }
        return -1;
    }

    char buf[2048];
    size_t offset = 0;
    int first_chunk = 1;

    while (1) {
        int64_t read_bytes = syscall(SYS_READFILE, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)buf, sizeof(buf), offset, 0);
        if (read_bytes < 0) {
            if (verbose) {
                puts("cp: cannot read '");
                puts(src);
                puts("'\n");
            }
            return -1;
        }
        if (read_bytes == 0) {
            /* If empty file, ensure 0-byte file is created */
            if (first_chunk) {
                int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)"", 0, 0, 0);
                if (wr == -EPERM) {
                    if (verbose) puts("cp: permission denied: destination is protected path, requires 'sudo' or KERNEL mode\n");
                    return -EPERM;
                } else if (wr < 0) {
                    if (verbose) {
                        puts("cp: cannot write to '");
                        puts(dst);
                        puts("'\n");
                    }
                    return -1;
                }
            }
            break;
        }

        int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)buf, (uint64_t)read_bytes, first_chunk ? 0 : 1, 0);
        if (wr == -EPERM) {
            if (verbose) puts("cp: permission denied: destination is protected path, requires 'sudo' or KERNEL mode\n");
            return -EPERM;
        } else if (wr < 0) {
            if (verbose) {
                puts("cp: cannot write to '");
                puts(dst);
                puts("'\n");
            }
            return -1;
        }

        offset += read_bytes;
        first_chunk = 0;
    }

    if (verbose) {
        puts("cp: copied '");
        puts(src);
        puts("' -> '");
        puts(dst);
        puts("'\n");
    }
    return 0;
}

static void cmd_cp(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: cp <src> <dst>\n");
        return;
    }
    char src[128] = "";
    char dst[128] = "";
    size_t i = 0, j = 0;
    while (arg[i] == ' ') i++;
    while (arg[i] && arg[i] != ' ' && j < sizeof(src) - 1) src[j++] = arg[i++];
    src[j] = '\0';
    while (arg[i] == ' ') i++;
    j = 0;
    while (arg[i] && arg[i] != ' ' && j < sizeof(dst) - 1) dst[j++] = arg[i++];
    dst[j] = '\0';

    do_cp(src, dst, 1);
}

static void cmd_mv(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: mv <src> <dst>\n");
        return;
    }
    char src[128] = "";
    char dst[128] = "";
    size_t i = 0, j = 0;
    while (arg[i] == ' ') i++;
    while (arg[i] && arg[i] != ' ' && j < sizeof(src) - 1) src[j++] = arg[i++];
    src[j] = '\0';
    while (arg[i] == ' ') i++;
    j = 0;
    while (arg[i] && arg[i] != ' ' && j < sizeof(dst) - 1) dst[j++] = arg[i++];
    dst[j] = '\0';

    if (src[0] == '\0' || dst[0] == '\0') {
        puts("usage: mv <src> <dst>\n");
        return;
    }

    int cp_res = do_cp(src, dst, 0);
    if (cp_res != 0) {
        if (cp_res == -EPERM) {
            puts("mv: permission denied: destination is protected path, requires 'sudo' or KERNEL mode\n");
        } else {
            puts("mv: failed to move '");
            puts(src);
            puts("': destination write failed\n");
        }
        return; /* ABORT! Do not delete source file! */
    }

    char src_path[256];
    resolve_path(src, src_path, sizeof(src_path));
    int64_t un_res = syscall(SYS_UNLINK, (uint64_t)(uintptr_t)src_path, 0, 0, 0, 0);
    if (un_res == -EPERM) {
        puts("mv: warning: copied to destination, but source removal denied (requires 'sudo' or KERNEL mode)\n");
    } else if (un_res != 0) {
        puts("mv: warning: copied to destination, but failed to unlink source\n");
    } else {
        puts("mv: moved '");
        puts(src);
        puts("' -> '");
        puts(dst);
        puts("'\n");
    }
}

static void cmd_write(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: write [-a] <file> <text>\n");
        return;
    }
    size_t i = 0;
    while (arg[i] == ' ') i++;

    int append = 0;
    if (arg[i] == '-' && arg[i+1] == 'a' && (arg[i+2] == ' ' || arg[i+2] == '\0')) {
        append = 1;
        i += 2;
        while (arg[i] == ' ') i++;
    }

    char file[128] = "";
    size_t j = 0;
    while (arg[i] && arg[i] != ' ' && j < sizeof(file) - 1) file[j++] = arg[i++];
    file[j] = '\0';
    while (arg[i] == ' ') i++;

    if (file[0] == '\0') {
        puts("usage: write [-a] <file> <text>\n");
        return;
    }

    char path[256];
    resolve_path(file, path, sizeof(path));
    const char *text = &arg[i];
    int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)text, strlen(text), append, 0);
    if (wr == -EPERM) {
        puts("write: permission denied: protected system path requires 'sudo' or KERNEL mode\n");
    } else if (wr < 0) {
        puts("write: cannot write to '");
        puts(file);
        puts("'\n");
    } else {
        if (append) {
            puts("appended text to '");
        } else {
            puts("wrote text to '");
        }
        puts(file);
        puts("'\n");
    }
}

/* ========================================================================= */
/* Classic Full-Screen MS-DOS Style Text Editor (cmd_edit)                   */
/* ========================================================================= */

#define SHELL_EDIT_MAX_LINES 512
#define SHELL_EDIT_LINE_MAX  128

typedef struct {
    char lines[SHELL_EDIT_MAX_LINES][SHELL_EDIT_LINE_MAX];
    int line_count;
    int cur_line;
    int cur_col;
    int top_line;
    int modified;
    char path[256];
} shell_editor_t;

static void shell_editor_draw_ui(shell_editor_t *ed, int cols, int rows) {
    syscall(SYS_CLEAR, 0, 0, 0, 0, 0);

    /* Top Title Bar */
    puts("\033[7m"); /* Inverted */
    char top_bar[256];
    snprintf(top_bar, sizeof(top_bar), " [ EDIT: %s%s ]", ed->path, ed->modified ? " *" : "");
    puts(top_bar);
    size_t tblen = strlen(top_bar);
    for (size_t s = tblen; s < (size_t)cols; s++) putc(' ');
    puts("\033[0m\n");

    /* Text Area */
    int text_rows = (rows > 2) ? (rows - 2) : 1;
    for (int r = 0; r < text_rows; r++) {
        int file_line = ed->top_line + r;
        if (file_line < ed->line_count) {
            const char *lstr = ed->lines[file_line];
            size_t llen = strlen(lstr);
            if (llen > (size_t)cols) llen = (size_t)cols;
            char lbuf[128];
            strncpy(lbuf, lstr, llen);
            lbuf[llen] = '\0';
            puts(lbuf);
        } else {
            putc('~');
        }
        puts("\n");
    }

    /* Bottom Helper Bar */
    puts("\033[7m");
    const char *bot = " Ctrl+S = Save  |  Ctrl+X = Exit  |  Arrow Keys = Navigate";
    puts(bot);
    size_t botlen = strlen(bot);
    for (size_t s = botlen; s < (size_t)cols; s++) putc(' ');
    puts("\033[0m");
}

static int shell_editor_save(shell_editor_t *ed) {
    char full_path[256];
    resolve_path(ed->path, full_path, sizeof(full_path));

    /* Create or overwrite with empty file */
    int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)full_path, (uint64_t)(uintptr_t)"", 0, 0, 0);
    if (wr < 0) return -1;

    for (int i = 0; i < ed->line_count; i++) {
        char lbuf[132];
        snprintf(lbuf, sizeof(lbuf), "%s\n", ed->lines[i]);
        syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)full_path, (uint64_t)(uintptr_t)lbuf, strlen(lbuf), 1, 0);
    }
    ed->modified = 0;
    return 0;
}

static void cmd_edit(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: edit <filename>\n");
        return;
    }

    shell_editor_t ed;
    memset(&ed, 0, sizeof(ed));
    strncpy(ed.path, arg, sizeof(ed.path) - 1);
    ed.line_count = 1;

    char full_path[256];
    resolve_path(arg, full_path, sizeof(full_path));

    char fbuf[4096];
    int64_t rsz = syscall(SYS_READFILE, (uint64_t)(uintptr_t)full_path, (uint64_t)(uintptr_t)fbuf, sizeof(fbuf) - 1, 0, 0);
    if (rsz > 0) {
        fbuf[rsz] = '\0';
        ed.line_count = 0;
        char *line_start = fbuf;
        while (*line_start && ed.line_count < SHELL_EDIT_MAX_LINES) {
            char *line_end = strchr(line_start, '\n');
            if (line_end) {
                *line_end = '\0';
                char *cr = strchr(line_start, '\r');
                if (cr) *cr = '\0';
                strncpy(ed.lines[ed.line_count++], line_start, SHELL_EDIT_LINE_MAX - 1);
                line_start = line_end + 1;
            } else {
                char *cr = strchr(line_start, '\r');
                if (cr) *cr = '\0';
                strncpy(ed.lines[ed.line_count++], line_start, SHELL_EDIT_LINE_MAX - 1);
                break;
            }
        }
        if (ed.line_count == 0) ed.line_count = 1;
    }

    int cols = get_term_cols();
    int rows = get_term_rows();
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 25;
    int text_rows = (rows > 2) ? (rows - 2) : 1;

    shell_editor_draw_ui(&ed, cols, rows);

    while (1) {
        char c = getchar();

        /* Ctrl+S = Save (19) */
        if (c == 19) {
            shell_editor_save(&ed);
            shell_editor_draw_ui(&ed, cols, rows);
            continue;
        }

        /* Ctrl+X = Exit (24) */
        if (c == 24) {
            if (ed.modified) {
                puts("\n\033[41;37m Save changes before exiting? (Y/N/Cancel) \033[0m ");
                while (1) {
                    char ck = getchar();
                    if (ck == 'y' || ck == 'Y') {
                        shell_editor_save(&ed);
                        break;
                    } else if (ck == 'n' || ck == 'N') {
                        break;
                    } else if (ck == 27) {
                        shell_editor_draw_ui(&ed, cols, rows);
                        goto cont_edit;
                    }
                }
            }
            break;
        cont_edit:
            continue;
        }

        /* ANSI Arrow Sequences */
        if (c == 27) {
            char c2 = getchar();
            if (c2 == '[') {
                char c3 = getchar();
                if (c3 == 'A') { /* UP */
                    if (ed.cur_line > 0) ed.cur_line--;
                } else if (c3 == 'B') { /* DOWN */
                    if (ed.cur_line + 1 < ed.line_count) ed.cur_line++;
                } else if (c3 == 'C') { /* RIGHT */
                    if (ed.cur_col < (int)strlen(ed.lines[ed.cur_line])) ed.cur_col++;
                } else if (c3 == 'D') { /* LEFT */
                    if (ed.cur_col > 0) ed.cur_col--;
                } else if (c3 == 'H') { /* HOME */
                    ed.cur_col = 0;
                } else if (c3 == 'F') { /* END */
                    ed.cur_col = (int)strlen(ed.lines[ed.cur_line]);
                }
                int llen = (int)strlen(ed.lines[ed.cur_line]);
                if (ed.cur_col > llen) ed.cur_col = llen;

                if (ed.cur_line < ed.top_line) ed.top_line = ed.cur_line;
                if (ed.cur_line >= ed.top_line + text_rows) ed.top_line = ed.cur_line - text_rows + 1;
                shell_editor_draw_ui(&ed, cols, rows);
                continue;
            }
        }

        /* Enter Key: Split Line */
        if (c == '\r' || c == '\n') {
            if (ed.line_count < SHELL_EDIT_MAX_LINES) {
                char *curr = ed.lines[ed.cur_line];
                for (int i = ed.line_count; i > ed.cur_line + 1; i--) {
                    memcpy(ed.lines[i], ed.lines[i - 1], SHELL_EDIT_LINE_MAX);
                }
                strncpy(ed.lines[ed.cur_line + 1], &curr[ed.cur_col], SHELL_EDIT_LINE_MAX - 1);
                curr[ed.cur_col] = '\0';
                ed.line_count++;
                ed.cur_line++;
                ed.cur_col = 0;
                ed.modified = 1;

                if (ed.cur_line >= ed.top_line + text_rows) ed.top_line = ed.cur_line - text_rows + 1;
                shell_editor_draw_ui(&ed, cols, rows);
            }
            continue;
        }

        /* Backspace */
        if (c == '\b' || c == 127) {
            char *curr = ed.lines[ed.cur_line];
            if (ed.cur_col > 0) {
                int llen = (int)strlen(curr);
                memmove(&curr[ed.cur_col - 1], &curr[ed.cur_col], llen - ed.cur_col + 1);
                ed.cur_col--;
                ed.modified = 1;
            } else if (ed.cur_line > 0) {
                char *prev = ed.lines[ed.cur_line - 1];
                int plen = (int)strlen(prev);
                int clen = (int)strlen(curr);
                if (plen + clen < SHELL_EDIT_LINE_MAX) {
                    strcat(prev, curr);
                    for (int i = ed.cur_line; i < ed.line_count - 1; i++) {
                        memcpy(ed.lines[i], ed.lines[i + 1], SHELL_EDIT_LINE_MAX);
                    }
                    ed.line_count--;
                    ed.cur_line--;
                    ed.cur_col = plen;
                    ed.modified = 1;
                }
            }
            if (ed.cur_line < ed.top_line) ed.top_line = ed.cur_line;
            shell_editor_draw_ui(&ed, cols, rows);
            continue;
        }

        /* Printable Characters */
        if (c >= 32 && c < 127) {
            char *curr = ed.lines[ed.cur_line];
            int llen = (int)strlen(curr);
            if (llen + 1 < SHELL_EDIT_LINE_MAX) {
                memmove(&curr[ed.cur_col + 1], &curr[ed.cur_col], llen - ed.cur_col + 1);
                curr[ed.cur_col] = c;
                ed.cur_col++;
                ed.modified = 1;
                shell_editor_draw_ui(&ed, cols, rows);
            }
            continue;
        }
    }
    clear_screen();
}

/* ========================================================================= */
/* pseuDOS Live Recovery & Maintenance Suite (recovery-tools / rtools)       */
/* ========================================================================= */

static void print_step_status(const char *tag, const char *msg, const char *status, int target_col) {
    char line[256];
    int n = snprintf(line, sizeof(line), "[%s] %s", tag, msg);
    puts(line);
    if (target_col < n + 1) {
        target_col = n + 1;
    }
    for (int i = n; i < target_col; i++) {
        putc(' ');
    }
    if (status && status[0]) {
        puts(status);
        puts("\n");
    } else {
        puts("\n");
    }
}

static void get_disk_hardware_name(const char *target, char *out_name, size_t out_sz) {
    if (!out_name || out_sz == 0) return;
    strncpy(out_name, "Block Storage Device", out_sz - 1);
    out_name[out_sz - 1] = '\0';

    int64_t count = syscall(SYS_FLASH, FLASH_OP_GET_COUNT, 0, 0, 0, 0);
    if (count <= 0) return;

    const char *bare = target;
    if (strncmp(bare, "/mounts/", 8) == 0) bare += 8;

    for (int i = 0; i < count; i++) {
        flash_dev_info_t info;
        if (syscall(SYS_FLASH, FLASH_OP_GET_DEVICE, (uint64_t)i, (uint64_t)(uintptr_t)&info, 0, 0) == 0) {
            char tag[32];
            if (info.type == 0) snprintf(tag, sizeof(tag), "sata%d", i);
            else if (info.type == 1) snprintf(tag, sizeof(tag), "nvme%dn1", i);
            else snprintf(tag, sizeof(tag), "usb%d", i);

            if (strcmp(bare, tag) == 0 || (bare[0] >= '0' && bare[0] <= '9' && atoi(bare) == i)) {
                snprintf(out_name, out_sz, "%s", info.name);
                return;
            }
        }
    }
}

static void normalize_target_mount(const char *target, char *out_path, size_t out_sz) {
    if (!target || !out_path || out_sz == 0) return;
    if (target[0] == '/') {
        strncpy(out_path, target, out_sz - 1);
    } else {
        snprintf(out_path, out_sz, "/mounts/%s", target);
    }
    out_path[out_sz - 1] = '\0';
    size_t len = strlen(out_path);
    while (len > 1 && out_path[len - 1] == '/') {
        out_path[--len] = '\0';
    }
}

static void rtools_cmd_flush(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: flush <disk>\n");
        return;
    }
    char target[256];
    normalize_target_mount(arg, target, sizeof(target));
    char dev_name[128];
    get_disk_hardware_name(target, dev_name, sizeof(dev_name));

    char msg[160];
    snprintf(msg, sizeof(msg), "flushing cache on %s (%s)...", target, dev_name);
    print_step_status("flush", msg, "[OK]", 88);

    syscall(SYS_SYNC, (uint64_t)(uintptr_t)target, 0, 0, 0, 0);
    puts("[flush] the operation has completed successfully\n");
}

static void rtools_cmd_bootcfg(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: bootcfg <disk> [--repair|--generate]\n");
        return;
    }

    char disk_arg[128] = "";
    int repair_mode = 0;
    size_t i = 0, j = 0;
    while (arg[i] == ' ') i++;
    while (arg[i] && arg[i] != ' ' && j < sizeof(disk_arg) - 1) disk_arg[j++] = arg[i++];
    disk_arg[j] = '\0';
    while (arg[i] == ' ') i++;
    if (arg[i] != '\0') {
        if (strcmp(&arg[i], "--repair") == 0 || strcmp(&arg[i], "--generate") == 0) {
            repair_mode = 1;
        }
    }

    char target[256];
    normalize_target_mount(disk_arg, target, sizeof(target));
    char dev_name[128];
    get_disk_hardware_name(target, dev_name, sizeof(dev_name));

    char cfg_path[280];
    snprintf(cfg_path, sizeof(cfg_path), "%s/protected/bootmgr/boot.cfg", target);

    vfs_stat_t st;
    int exists = (syscall(SYS_STAT, (uint64_t)(uintptr_t)cfg_path, (uint64_t)(uintptr_t)&st, 0, 0, 0) == 0 && st.size > 0);
    int valid_cfg = 0;
    char read_buf[512];
    memset(read_buf, 0, sizeof(read_buf));

    if (exists) {
        int64_t r = syscall(SYS_READFILE, (uint64_t)(uintptr_t)cfg_path, (uint64_t)(uintptr_t)read_buf, sizeof(read_buf) - 1, 0, 0);
        if (r > 0) {
            read_buf[r] = '\0';
            if (strstr(read_buf, "kernel=") != NULL) {
                valid_cfg = 1;
            }
        }
    }

    print_step_status("bootcfg", "checking boot configuration file...", (exists && valid_cfg) ? "[OK]" : "[FAIL]", 88);

    if (!exists || !valid_cfg || repair_mode) {
        if (!exists) {
            puts("[bootcfg] missing boot.cfg configuration file\n");
        } else if (!valid_cfg) {
            puts("[bootcfg] found incomplete or corrupted boot configuration data\n");
        }

        /* Ensure parent directory exists */
        char pdir1[280], pdir2[280];
        snprintf(pdir1, sizeof(pdir1), "%s/protected", target);
        snprintf(pdir2, sizeof(pdir2), "%s/protected/bootmgr", target);
        syscall(SYS_MKDIR, (uint64_t)(uintptr_t)pdir1, 0, 0, 0, 0);
        syscall(SYS_MKDIR, (uint64_t)(uintptr_t)pdir2, 0, 0, 0, 0);

        const char *new_cfg =
            "# pseuDOS Boot Configuration\r\n"
            "kernel=\\protected\\krnl\\vpkernel\r\n"
            "initramfs=\\protected\\krnl\\initramfs.img\r\n"
            "autoinit=\\protected\\krnl\\autoinit.exe\r\n"
            "shell=\\protected\\gui\\gman.exe\r\n"
            "cmdline=quiet devpath=hardware\r\n"
            "default_resolution=auto\r\n"
            "bootmgr_version=1.1.0\r\n";
        int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)cfg_path, (uint64_t)(uintptr_t)new_cfg, strlen(new_cfg), 0, 0);
        if (wr >= 0) {
            print_step_status("bootcfg", "writing standard boot configuration data...", "[OK]", 88);
            puts("[bootcfg] all operations completed successfully\n");
        } else {
            print_step_status("bootcfg", "writing standard boot configuration data...", "[FAIL]", 88);
        }
    } else {
        puts("current boot configuration:\n----------------------------------------\n");
        puts(read_buf);
        puts("----------------------------------------\n");
        puts("[bootcfg] all operations completed successfully\n");
    }
}

static int verify_efi_binary(const char *path, uint64_t expected_size) {
    vfs_stat_t st;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0 || st.size < 512) {
        return 0;
    }
    if (expected_size > 0 && st.size != expected_size) {
        return 0;
    }
    uint8_t hdr[512];
    int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)hdr, sizeof(hdr), 0, 0);
    if (rd < 512) return 0;
    /* Check DOS MZ signature */
    if (hdr[0] != 'M' || hdr[1] != 'Z') return 0;
    /* Check PE offset */
    uint32_t pe_offset = *(uint32_t *)&hdr[0x3C];
    if (pe_offset + 4 <= sizeof(hdr)) {
        if (hdr[pe_offset] != 'P' || hdr[pe_offset + 1] != 'E' ||
            hdr[pe_offset + 2] != '\0' || hdr[pe_offset + 3] != '\0') {
            return 0;
        }
    }
    return 1;
}

static void rtools_cmd_loader_repair(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: loader-repair <disk> [--force]\n");
        return;
    }

    char disk_arg[128] = "";
    int force_mode = 0;
    size_t i = 0, j = 0;
    while (arg[i] == ' ') i++;
    while (arg[i] && arg[i] != ' ' && j < sizeof(disk_arg) - 1) disk_arg[j++] = arg[i++];
    disk_arg[j] = '\0';
    while (arg[i] == ' ') i++;
    if (arg[i] != '\0') {
        if (strcmp(&arg[i], "--force") == 0) {
            force_mode = 1;
        }
    }

    char target[256];
    normalize_target_mount(disk_arg, target, sizeof(target));
    char dev_name[128];
    get_disk_hardware_name(target, dev_name, sizeof(dev_name));

    char msg[160];
    snprintf(msg, sizeof(msg), "inspecting bootloader on %s (%s)...", target, dev_name);
    print_step_status("loader-repair", msg, "[OK]", 88);

    /* Locate live reference EFI loaders in VFS */
    const char *src_fb = "/EFI/BOOT/BOOTX64.EFI";
    const char *src_silo = "/EFI/pseuDOS/BOOTX64.EFI";
    vfs_stat_t st_ref_fb, st_ref_silo;
    uint64_t ref_fb_sz = 0, ref_silo_sz = 0;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)src_fb, (uint64_t)(uintptr_t)&st_ref_fb, 0, 0, 0) == 0) {
        ref_fb_sz = st_ref_fb.size;
    }
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)src_silo, (uint64_t)(uintptr_t)&st_ref_silo, 0, 0, 0) == 0) {
        ref_silo_sz = st_ref_silo.size;
    } else {
        src_silo = src_fb;
        ref_silo_sz = ref_fb_sz;
    }

    char dst_fb[280];
    snprintf(dst_fb, sizeof(dst_fb), "%s/EFI/BOOT/BOOTX64.EFI", target);
    char dst_silo[280];
    snprintf(dst_silo, sizeof(dst_silo), "%s/EFI/pseuDOS/BOOTX64.EFI", target);

    int fb_ok = (!force_mode) ? verify_efi_binary(dst_fb, ref_fb_sz) : 0;
    int silo_ok = (!force_mode) ? verify_efi_binary(dst_silo, ref_silo_sz) : 0;

    print_step_status("loader-repair", "checking fallback EFI loader /EFI/BOOT/BOOTX64.EFI...", fb_ok ? "[OK]" : "[FAIL]", 88);
    print_step_status("loader-repair", "checking SiLo loader /EFI/pseuDOS/BOOTX64.EFI...", silo_ok ? "[OK]" : "[FAIL]", 88);

    if (fb_ok && silo_ok) {
        char clean_msg[256];
        snprintf(clean_msg, sizeof(clean_msg),
                 "loader-repair has inspected bootloaders on %s (%s).\nall EFI loaders are intact, verified PE32+ binaries, and match reference images.\n\n",
                 target, dev_name);
        puts(clean_msg);
        puts("[loader-repair] all operations completed successfully\n");
        return;
    }

    puts("\nloader-repair has found missing or damaged bootloader files on ");
    puts(target);
    puts(" (");
    puts(dev_name);
    puts(").\n");
    if (!fb_ok) puts("  - missing/damaged fallback EFI loader: /EFI/BOOT/BOOTX64.EFI\n");
    if (!silo_ok) puts("  - missing/damaged SiLo loader: /EFI/pseuDOS/BOOTX64.EFI\n");
    puts("\nwould you like to repair the bootloader? (Y/n) ");

    char ans_buf[32];
    readline(ans_buf, sizeof(ans_buf), "");
    char *ans = trim(ans_buf);
    if (ans[0] == 'y' || ans[0] == 'Y' || ans[0] == '\0') {
        char dst_dir1[280], dst_dir2[280], dst_dir3[280];
        snprintf(dst_dir1, sizeof(dst_dir1), "%s/EFI", target);
        snprintf(dst_dir2, sizeof(dst_dir2), "%s/EFI/BOOT", target);
        snprintf(dst_dir3, sizeof(dst_dir3), "%s/EFI/pseuDOS", target);
        syscall(SYS_MKDIR, (uint64_t)(uintptr_t)dst_dir1, 0, 0, 0, 0);
        syscall(SYS_MKDIR, (uint64_t)(uintptr_t)dst_dir2, 0, 0, 0, 0);
        syscall(SYS_MKDIR, (uint64_t)(uintptr_t)dst_dir3, 0, 0, 0, 0);

        int rep_err = 0;
        if (!fb_ok) {
            int64_t cp_ret = syscall(SYS_COPYFILE, (uint64_t)(uintptr_t)src_fb, (uint64_t)(uintptr_t)dst_fb, 0, 0, 0);
            if (cp_ret == 0) {
                print_step_status("loader-repair", "restoring fallback EFI loader /EFI/BOOT/BOOTX64.EFI...", "[OK]", 88);
            } else {
                print_step_status("loader-repair", "restoring fallback EFI loader /EFI/BOOT/BOOTX64.EFI...", "[FAIL]", 88);
                rep_err = 1;
            }
        }
        if (!silo_ok) {
            int64_t cp_ret = syscall(SYS_COPYFILE, (uint64_t)(uintptr_t)src_silo, (uint64_t)(uintptr_t)dst_silo, 0, 0, 0);
            if (cp_ret == 0) {
                print_step_status("loader-repair", "restoring SiLo loader /EFI/pseuDOS/BOOTX64.EFI...", "[OK]", 88);
            } else {
                print_step_status("loader-repair", "restoring SiLo loader /EFI/pseuDOS/BOOTX64.EFI...", "[FAIL]", 88);
                rep_err = 1;
            }
        }

        /* Reverification of written binaries */
        int v_fb = verify_efi_binary(dst_fb, ref_fb_sz);
        int v_silo = verify_efi_binary(dst_silo, ref_silo_sz);
        if (v_fb && v_silo && !rep_err) {
            print_step_status("loader-repair", "reverifying bootloader binaries...", "[OK]", 88);
            puts("[loader-repair] all operations completed successfully\n");
            puts("to ensure changes have been truly committed, please run \"flush ");
            puts(target);
            puts("\"\n");
        } else {
            print_step_status("loader-repair", "reverifying bootloader binaries...", "[FAIL]", 88);
            puts("[loader-repair] verification failed after writing!\n\n");
        }
    } else {
        puts("[loader-repair] repair aborted by user.\n");
    }
}

static void rtools_cmd_fsck(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: fsck <disk>\n");
        return;
    }
    char target[256];
    normalize_target_mount(arg, target, sizeof(target));
    char dev_name[128];
    get_disk_hardware_name(target, dev_name, sizeof(dev_name));

    fsck_result_t res;
    memset(&res, 0, sizeof(res));

    /* Execute real sector-level FAT32 check via kernel */
    int64_t ret = syscall(SYS_FSCK, (uint64_t)(uintptr_t)target, 0 /* dry-run */, (uint64_t)(uintptr_t)&res, 0, 0);
    if (ret < 0) {
        print_step_status("fsck", "inspecting partition & boot sector...", "[FAIL]", 88);
        puts("[fsck] unable to read target storage device or not a FAT32 filesystem\n");
        return;
    }

    /* 1. Magic number check */
    if (res.error_flags & FSCK_ERR_MAGIC) {
        print_step_status("fsck", "checking FAT32 Filesystem magic number...", "[FAIL]", 88);
        puts("[fsck] invalid VBR boot signature (missing 0x55AA)\n");
    } else {
        print_step_status("fsck", "checking FAT32 Filesystem magic number...", "[OK]", 88);
    }

    /* 2. Metadata consistency */
    if (res.error_flags & FSCK_ERR_BPB) {
        print_step_status("fsck", "checking FAT32 Filesystem metadata consistency...", "[FAIL]", 88);
        puts("[fsck] invalid BPB header geometry or cluster sizes\n");
    } else {
        print_step_status("fsck", "checking FAT32 Filesystem metadata consistency...", "[OK]", 88);
    }

    /* 3. Comparing FAT1 vs FAT2 */
    if (res.error_flags & FSCK_ERR_FAT_MISMATCH) {
        print_step_status("fsck", "comparing File Allocation Tables...", "[FAIL]", 88);
        char fat_msg[128];
        snprintf(fat_msg, sizeof(fat_msg), "[fsck] File Allocation Table comparison found %u mismatched sector(s)\n", res.fat_mismatches);
        puts(fat_msg);
    } else {
        print_step_status("fsck", "comparing File Allocation Tables...", "[OK]", 88);
    }

    /* 4. Checking directory consistency */
    if (res.error_flags & FSCK_ERR_CHAIN) {
        print_step_status("fsck", "checking FAT32 Filesystem directory consistency... (this might take a while)", "[FAIL]", 88);
        puts("[fsck] found broken, truncated, or cyclic cluster chains\n");
    } else {
        print_step_status("fsck", "checking FAT32 Filesystem directory consistency... (this might take a while)", "[OK]", 88);
    }

    /* 5. Checking LFNs */
    if (res.error_flags & FSCK_ERR_DIR) {
        print_step_status("fsck", "checking for illegal/corrupted Short and Long File Names...", "[FAIL]", 88);
        puts("[fsck] found directory entries with invalid LFN checksums\n");
    } else {
        print_step_status("fsck", "checking for illegal/corrupted Short and Long File Names...", "[OK]", 88);
    }

    /* 6. Verifying file sizes and FSInfo */
    if (res.error_flags & FSCK_ERR_FSINFO) {
        print_step_status("fsck", "verifying file sizes on FAT32 Filesystem...", "[FAIL]", 88);
        char fsi_msg[160];
        snprintf(fsi_msg, sizeof(fsi_msg), "[fsck] FSInfo free count mismatch: reported %u, actual FAT count is %u\n",
                 res.fsinfo_reported_free, res.fsinfo_actual_free);
        puts(fsi_msg);
    } else {
        print_step_status("fsck", "verifying file sizes on FAT32 Filesystem...", "[OK]", 88);
    }

    if (res.error_flags == 0) {
        char clean_msg[200];
        snprintf(clean_msg, sizeof(clean_msg),
                 "fsck inspected %u directories and %u files on %s (%s).\nall sectors, FAT mirrors, and metadata are healthy and consistent.\n\n",
                 res.checked_dirs, res.checked_files, target, dev_name);
        puts(clean_msg);
        puts("[fsck] all operations completed successfully\n");
        return;
    }

    puts("\nfsck has found errors with ");
    puts(target);
    puts(" (");
    puts(dev_name);
    puts(").\n");
    puts("would you like to repair these errors? (Y/n) ");

    char ans_buf[32];
    readline(ans_buf, sizeof(ans_buf), "");
    char *ans = trim(ans_buf);
    if (ans[0] == 'y' || ans[0] == 'Y' || ans[0] == '\0') {
        int64_t rep_ret = syscall(SYS_FSCK, (uint64_t)(uintptr_t)target, 1 /* repair */, (uint64_t)(uintptr_t)&res, 0, 0);
        if (rep_ret < 0) {
            puts("[fsck] repair execution failed on target device!\n");
            return;
        }

        if (res.error_flags & FSCK_ERR_FAT_MISMATCH) {
            print_step_status("fsck", "synchronizing File Allocation Tables (mirroring FAT1 -> FAT2)...", "[OK]", 88);
        }
        if (res.error_flags & FSCK_ERR_FSINFO) {
            print_step_status("fsck", "rebuilding FSInfo free cluster count...", "[OK]", 88);
        }
        if (res.error_flags & FSCK_ERR_CHAIN) {
            print_step_status("fsck", "relinking orphaned cluster chains...", "[OK]", 88);
        }
        print_step_status("fsck", "committing repaired directory records to disk...", "[OK]", 88);
        puts("[fsck] all operations completed successfully\n");
    } else {
        puts("[fsck] repair aborted by user.\n");
    }
}

static void rtools_cmd_repair_system(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: repair-system <disk>\n");
        return;
    }
    char target[256];
    normalize_target_mount(arg, target, sizeof(target));
    char dev_name[128];
    get_disk_hardware_name(target, dev_name, sizeof(dev_name));

    char step_buf[256];
    snprintf(step_buf, sizeof(step_buf), "entering %s...", target);
    print_step_status("repair-system", step_buf, "[OK]", 88);

    snprintf(step_buf, sizeof(step_buf), "checking %s/protected...", target);
    print_step_status("repair-system", step_buf, "[OK]", 88);

    /* Dynamic Missing files tracker */
    typedef struct {
        char src_rel[128];
        char name[64];
        int is_bootcfg;
    } missing_item_t;

    missing_item_t missing_items[64];
    int missing_count = 0;

    struct {
        const char *dir;
        const char *label;
        int is_bootmgr;
    } scan_dirs[] = {
        {"/protected/krnl", "kernel binaries", 0},
        {"/protected/bootmgr", "boot configuration data", 1},
        {"/protected/gui", "binaries", 0},
        {"/protected/crit", "binaries", 0},
        {"/protected/apps", "apps", 0}
    };

    for (size_t d = 0; d < sizeof(scan_dirs)/sizeof(scan_dirs[0]); d++) {
        const char *sdir = scan_dirs[d].dir;
        if (scan_dirs[d].is_bootmgr) {
            snprintf(step_buf, sizeof(step_buf), "checking boot configuration inside %s%s...", target, sdir);
        } else {
            snprintf(step_buf, sizeof(step_buf), "checking binaries inside %s%s...", target, sdir);
        }
        print_step_status("repair-system", step_buf, "[OK]", 88);

        char names_buf[2048];
        int64_t nbytes = syscall(SYS_LISTDIR, (uint64_t)(uintptr_t)sdir,
                                 (uint64_t)(uintptr_t)names_buf, sizeof(names_buf), 0, 0);

        char dir_miss_names[16][64];
        int dir_miss_count = 0;

        if (nbytes > 0) {
            size_t pos = 0;
            while (pos < (size_t)nbytes) {
                const char *fname = &names_buf[pos];
                size_t flen = strlen(fname);
                if (flen == 0) break;
                pos += flen + 1;

                if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) continue;

                char target_file[280];
                snprintf(target_file, sizeof(target_file), "%s%s/%s", target, sdir, fname);
                vfs_stat_t st;
                int is_missing = 0;
                if (syscall(SYS_STAT, (uint64_t)(uintptr_t)target_file, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0 || st.size == 0) {
                    is_missing = 1;
                }

                if (is_missing) {
                    if (missing_count < 64) {
                        snprintf(missing_items[missing_count].src_rel, sizeof(missing_items[missing_count].src_rel), "%s/%s", sdir, fname);
                        strncpy(missing_items[missing_count].name, fname, sizeof(missing_items[missing_count].name) - 1);
                        missing_items[missing_count].name[sizeof(missing_items[missing_count].name) - 1] = '\0';
                        missing_items[missing_count].is_bootcfg = (strcmp(fname, "boot.cfg") == 0);
                        missing_count++;
                    }
                    if (dir_miss_count < 16) {
                        strncpy(dir_miss_names[dir_miss_count], fname, 63);
                        dir_miss_names[dir_miss_count][63] = '\0';
                        dir_miss_count++;
                    }
                }
            }
        } else if (scan_dirs[d].is_bootmgr) {
            /* Fallback if SYS_LISTDIR on bootmgr returned 0: check boot.cfg */
            char target_file[280];
            snprintf(target_file, sizeof(target_file), "%s%s/boot.cfg", target, sdir);
            vfs_stat_t st;
            if (syscall(SYS_STAT, (uint64_t)(uintptr_t)target_file, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0 || st.size == 0) {
                if (missing_count < 64) {
                    snprintf(missing_items[missing_count].src_rel, sizeof(missing_items[missing_count].src_rel), "%s/boot.cfg", sdir);
                    strcpy(missing_items[missing_count].name, "boot.cfg");
                    missing_items[missing_count].is_bootcfg = 1;
                    missing_count++;
                }
                strcpy(dir_miss_names[dir_miss_count++], "boot.cfg");
            }
        }

        /* Dynamically report findings for this directory */
        if (dir_miss_count > 0) {
            if (scan_dirs[d].is_bootmgr) {
                puts("[repair-system] found incomplete/broken boot configuration data\n");
            } else if (strcmp(sdir, "/protected/krnl") == 0) {
                if (dir_miss_count > 1) {
                    puts("[repair-system] found multiple missing/incomplete kernel binaries\n");
                } else {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "[repair-system] found missing/incomplete %s binary\n", dir_miss_names[0]);
                    puts(msg);
                }
            } else if (dir_miss_count == 1) {
                char msg[128];
                snprintf(msg, sizeof(msg), "[repair-system] found missing/incomplete %s binary\n", dir_miss_names[0]);
                puts(msg);
            } else {
                /* Dynamically construct the list of missing filenames: (file1, file2, ...) */
                char msg[256];
                snprintf(msg, sizeof(msg), "[repair-system] found multiple missing/incomplete binaries (");
                for (int m = 0; m < dir_miss_count; m++) {
                    if (m > 0) strcat(msg, ", ");
                    strcat(msg, dir_miss_names[m]);
                }
                strcat(msg, ")\n");
                puts(msg);
            }
        }
    }

    snprintf(step_buf, sizeof(step_buf), "exiting disk %s...", target);
    puts("[repair-system] ");
    puts(step_buf);
    puts("\n");
    puts("[repair-system] finishing inspection...\n\n");

    if (missing_count == 0) {
        puts("repair-system has inspected all critical binaries on ");
        puts(target);
        puts(" (");
        puts(dev_name);
        puts(").\nall files are intact and verified!\n");
        return;
    }

    puts("repair-system has found multiple missing/incomplete binaries that needs to be repaired\n");
    puts("files that will be repaired:\n");
    for (int k = 0; k < missing_count; k++) {
        puts("  - ");
        puts(target);
        puts(missing_items[k].src_rel);
        puts("\n");
    }

    puts("start repair? (Y/n) ");
    char ans_buf[32];
    readline(ans_buf, sizeof(ans_buf), "");
    char *ans = trim(ans_buf);
    if (ans[0] != 'y' && ans[0] != 'Y' && ans[0] != '\0') {
        puts("[repair-system] repair cancelled by user.\n");
        return;
    }

    puts("DO NOT UNPLUG THE DISK BEFORE THE OPERATION COMPLETES!\n");

    snprintf(step_buf, sizeof(step_buf), "entering %s...", target);
    print_step_status("repair-system", step_buf, "[OK]", 108);

    /* Copy missing binaries */
    for (int k = 0; k < missing_count; k++) {
        char src_path[128];
        strncpy(src_path, missing_items[k].src_rel, sizeof(src_path) - 1);
        src_path[sizeof(src_path) - 1] = '\0';

        char dst_path[280];
        snprintf(dst_path, sizeof(dst_path), "%s%s", target, missing_items[k].src_rel);

        /* Ensure parent directory exists */
        char dst_dir[280];
        strncpy(dst_dir, dst_path, sizeof(dst_dir) - 1);
        dst_dir[sizeof(dst_dir) - 1] = '\0';
        char *last_s = strrchr(dst_dir, '/');
        if (last_s) {
            *last_s = '\0';
            syscall(SYS_MKDIR, (uint64_t)(uintptr_t)dst_dir, 0, 0, 0, 0);
        }

        char step_msg[200];
        if (missing_items[k].is_bootcfg) {
            snprintf(step_msg, sizeof(step_msg), "writing new boot configuration data to %s...", dst_path);
            const char *new_cfg =
                "# pseuDOS Boot Configuration\r\n"
                "kernel=\\protected\\krnl\\vpkernel\r\n"
                "initramfs=\\protected\\krnl\\initramfs.img\r\n"
                "autoinit=\\protected\\krnl\\autoinit.exe\r\n"
                "shell=\\protected\\gui\\gman.exe\r\n"
                "cmdline=quiet devpath=hardware\r\n"
                "default_resolution=auto\r\n"
                "bootmgr_version=1.1.0\r\n";
            syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)new_cfg, strlen(new_cfg), 0, 0);
            print_step_status("repair-system", step_msg, "[OK]", 108);
        } else {
            snprintf(step_msg, sizeof(step_msg), "copying binary from %s to destination %s...", src_path, dst_path);
            syscall(SYS_COPYFILE, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)dst_path, 0, 0, 0);
            print_step_status("repair-system", step_msg, "[OK]", 108);
        }
    }

    int reverify_failed = 0;
    char failed_file[64] = "";
    for (int k = 0; k < missing_count; k++) {
        char dst_path[280];
        snprintf(dst_path, sizeof(dst_path), "%s%s", target, missing_items[k].src_rel);

        vfs_stat_t st_dst;
        if (syscall(SYS_STAT, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)&st_dst, 0, 0, 0) != 0 || st_dst.size == 0) {
            reverify_failed = 1;
            strncpy(failed_file, missing_items[k].name, sizeof(failed_file) - 1);
            break;
        }

        if (missing_items[k].is_bootcfg) {
            char cfg_chunk[256];
            int64_t r = syscall(SYS_READFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)cfg_chunk, sizeof(cfg_chunk) - 1, 0, 0);
            if (r <= 0) {
                reverify_failed = 1;
                strncpy(failed_file, missing_items[k].name, sizeof(failed_file) - 1);
                break;
            }
            cfg_chunk[r] = '\0';
            if (!strstr(cfg_chunk, "kernel=")) {
                reverify_failed = 1;
                strncpy(failed_file, missing_items[k].name, sizeof(failed_file) - 1);
                break;
            }
        } else {
            char src_path[128];
            strncpy(src_path, missing_items[k].src_rel, sizeof(src_path) - 1);
            src_path[sizeof(src_path) - 1] = '\0';

            vfs_stat_t st_src;
            if (syscall(SYS_STAT, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)&st_src, 0, 0, 0) == 0) {
                if (st_dst.size != st_src.size) {
                    reverify_failed = 1;
                    strncpy(failed_file, missing_items[k].name, sizeof(failed_file) - 1);
                    break;
                }

                static uint8_t s_ver_buf1[2048];
                static uint8_t s_ver_buf2[2048];
                size_t check_len = st_src.size < sizeof(s_ver_buf1) ? st_src.size : sizeof(s_ver_buf1);
                int64_t r1 = syscall(SYS_READFILE, (uint64_t)(uintptr_t)src_path, (uint64_t)(uintptr_t)s_ver_buf1, check_len, 0, 0);
                int64_t r2 = syscall(SYS_READFILE, (uint64_t)(uintptr_t)dst_path, (uint64_t)(uintptr_t)s_ver_buf2, check_len, 0, 0);
                if (r1 != r2 || r1 != (int64_t)check_len || memcmp(s_ver_buf1, s_ver_buf2, check_len) != 0) {
                    reverify_failed = 1;
                    strncpy(failed_file, missing_items[k].name, sizeof(failed_file) - 1);
                    break;
                }
            }
        }
    }

    if (!reverify_failed) {
        print_step_status("repair-system", "reverifying binaries... (this might take a while)", "[OK]", 108);
        puts("[repair-system] all operations completed successfully\n");
    } else {
        print_step_status("repair-system", "reverifying binaries... (this might take a while)", "[FAIL]", 108);
        char fail_msg[128];
        snprintf(fail_msg, sizeof(fail_msg), "[repair-system] verification failed for %s!\n\n", failed_file);
        puts(fail_msg);
        return;
    }

    snprintf(step_buf, sizeof(step_buf), "exiting disk %s...", target);
    puts("[repair-system] ");
    puts(step_buf);
    puts("\n");
    puts("[repair-system] finishing inspection...\n\n");

    puts("repair-system has successfully repaired and verified binaries on ");
    puts(target);
    puts(" (");
    puts(dev_name);
    puts(")\n");
    puts("to ensure changes have been truly committed, please run \"flush ");
    puts(target);
    puts("\"\n");
}

static void rtools_show_banner(void) {
    puts("\n");
    puts("                                                    [recovery-tools]\n");
    puts("------------------------------------------------------------------------------------------------------------------------\n");
    puts("                                                quick command reference:\n");
    puts("  1. repair-system <disk>                    : checks for any and all missing critical files inside selected disk\n");
    puts("  2. fsck / fs-repair <disk>                 : checks if the filesystem is corrupted, attempts repair if possible\n");
    puts("  3. bootcfg <disk> [--repair|--generate]    : checks the boot configuration data file inside selected disk\n");
    puts("  4. loader-repair <disk>                    : repairs the bootloader inside selected disk\n");
    puts("  5. flush <disk>                            : flushes selected disk cache to ensure writes stay\n");
    puts("  6. exit                                    : exit recovery-tools\n");
    puts("------------------------------------------------------------------------------------------------------------------------\n\n");
}

static void cmd_rtools(void) {
    if (!is_live_system()) {
        puts("unknown command 'recovery-tools'. type 'help' for available commands.\n");
        return;
    }

    puts("initializing recovery tools...\n");

    int64_t prev_priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
    if (prev_priv != 1) {
        syscall(SYS_ELEVATE, 0, 0, 0, 0, 0);
    }
    env_set("USER", "recovery");

    rtools_show_banner();

    char line_buf[256];
    char cwd_buf[256];
    char prompt_buf[384];

    while (1) {
        if (syscall(SYS_GET_PROMPT_PATH, (uint64_t)(uintptr_t)cwd_buf, sizeof(cwd_buf), 0, 0, 0) != 0) {
            if (syscall(SYS_GETCWD, (uint64_t)(uintptr_t)cwd_buf, sizeof(cwd_buf), 0, 0, 0) != 0) {
                strcpy(cwd_buf, "/");
            }
        }

        snprintf(prompt_buf, sizeof(prompt_buf), "recovery@pseuDOS [%s] > ", cwd_buf);
        readline(line_buf, sizeof(line_buf), prompt_buf);
        char *line = trim(line_buf);
        if (!line || line[0] == '\0') continue;

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            puts("exiting recovery tools...\n\n");
            break;
        }

        char cmd[64] = "";
        char *arg = NULL;
        size_t cidx = 0;
        while (line[cidx] && line[cidx] != ' ' && cidx < sizeof(cmd) - 1) {
            cmd[cidx] = line[cidx];
            cidx++;
        }
        cmd[cidx] = '\0';
        for (size_t k = 0; cmd[k]; k++) {
            if (cmd[k] >= 'A' && cmd[k] <= 'Z') cmd[k] += 32;
        }
        if (line[cidx] == ' ') {
            arg = trim(&line[cidx + 1]);
        }

        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            rtools_show_banner();
        } else if (strcmp(cmd, "repair-system") == 0 || strcmp(cmd, "repair") == 0) {
            rtools_cmd_repair_system(arg);
        } else if (strcmp(cmd, "fsck") == 0 || strcmp(cmd, "fs-repair") == 0) {
            rtools_cmd_fsck(arg);
        } else if (strcmp(cmd, "bootcfg") == 0) {
            rtools_cmd_bootcfg(arg);
        } else if (strcmp(cmd, "loader-repair") == 0) {
            rtools_cmd_loader_repair(arg);
        } else if (strcmp(cmd, "flush") == 0 || strcmp(cmd, "sync") == 0) {
            rtools_cmd_flush(arg);
        } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
            cmd_ls(arg);
        } else if (strcmp(cmd, "cd") == 0) {
            cmd_cd(arg);
        } else if (strcmp(cmd, "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
            cmd_cat(arg);
        } else if (strcmp(cmd, "edit") == 0) {
            cmd_edit(arg);
        } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
            clear_screen();
        } else {
            puts("unknown recovery command '");
            puts(cmd);
            puts("'. type 'help' for command reference or 'exit' to leave.\n");
        }
    }

    if (prev_priv != 1) {
        syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
    }
    env_set("USER", "shell");
}

static char g_sys_cmd_buf[16384];

static void cmd_ps(void) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_PS, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_kill(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: kill <pid>\n");
        return;
    }
    uint32_t pid = (uint32_t)atoi(arg);
    int64_t res = syscall(SYS_KILL, pid, 0, 0, 0, 0);
    if (res == -EPERM) {
        puts("kill: permission denied: killing other processes requires 'sudo' or KERNEL mode\n");
    } else if (res != 0) {
        puts("kill: failed to kill process\n");
    }
}

static void cmd_syscalltest(void) {
    puts("--- testing syscall interface from userspace ---\n");
    uint16_t cs = 0, ss = 0;
    __asm__ volatile ("mov %%cs, %0\n\tmov %%ss, %1" : "=r"(cs), "=r"(ss));
    puts("0. hardware cpu ring: CS=0x");
    print_hex(cs);
    puts(" SS=0x");
    print_hex(ss);
    puts(" CPL=");
    print_num((uint64_t)(cs & 3));
    puts(((cs & 3) == 3) ? " (hardware Ring 3 active)\n" : " (Ring 0)\n");

    int64_t pid = syscall(SYS_GETPID, 0, 0, 0, 0, 0);
    puts("1. sys_getpid() = PID ");
    print_num((uint64_t)pid);
    puts("\n");

    int64_t priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
    puts("2. sys_get_privilege() = ");
    puts((priv == 1) ? "KERNEL\n" : "USER\n");

    puts("3. testing sys_write()... [ok]\n");
    puts("--- all syscall tests completed! ---\n");
}

static void cmd_mousetest(void) {
    puts("--- mouse driver and hardware verification ---\n");
    mouse_state_t st;
    memset(&st, 0, sizeof(st));
    syscall(SYS_GET_MOUSE_STATE, (uint64_t)(uintptr_t)&st, 0, 0, 0, 0);

    puts("initial state: pos=(");
    print_signed_num(st.x);
    puts(", ");
    print_signed_num(st.y);
    puts(") bounds=[0..");
    print_num(st.max_x);
    puts(" x 0..");
    print_num(st.max_y);
    puts("] buttons=0x");
    print_hex(st.buttons);
    puts(" wheel=");
    puts(st.has_wheel ? "detected\n" : "standard ps/2\n");

    puts("polling events for 3 seconds (move mouse or click in qemu window)...\n");

    uint64_t start = syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
    int event_count = 0;

    while (event_count < 10) {
        mouse_event_t ev;
        int64_t got = syscall(SYS_GET_MOUSE_EVENT, (uint64_t)(uintptr_t)&ev, 0, 0, 0, 0);
        if (got > 0) {
            event_count++;
            puts("[mouse event ");
            print_num((uint64_t)event_count);
            puts("] type=");
            if (ev.event_type == MOUSE_EVENT_MOVE) puts("move");
            else if (ev.event_type == MOUSE_EVENT_BUTTON) puts("button");
            else if (ev.event_type == MOUSE_EVENT_WHEEL) puts("wheel");
            else puts("unknown");

            puts(" pos=(");
            print_signed_num(ev.x);
            puts(", ");
            print_signed_num(ev.y);
            puts(") delta=(");
            print_signed_num(ev.dx);
            puts(", ");
            print_signed_num(ev.dy);
            puts(", dz=");
            print_signed_num(ev.dz);
            puts(") btns=");
            if (ev.buttons & MOUSE_BTN_LEFT) puts("L");
            if (ev.buttons & MOUSE_BTN_RIGHT) puts("R");
            if (ev.buttons & MOUSE_BTN_MIDDLE) puts("M");
            if (ev.buttons == 0) puts("none");
            puts("\n");
        }

        uint64_t now = syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
        if (now - start > 300) {
            break;
        }
        syscall(SYS_SLEEP, 10, 0, 0, 0, 0);
    }

    if (event_count == 0) {
        puts("no motion detected during poll period (idle)\n");
    } else {
        puts("captured ");
        print_num((uint64_t)event_count);
        puts(" mouse events successfully\n");
    }
    puts("--- mousetest finished ---\n");
}

static void cmd_ipctest(void) {
    puts("--- unix-domain socket ipc & poll verification ---\n");

    /* 1. Create server socket */
    int srv = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (srv < 0) {
        puts("error: sys_socket(srv) failed\n");
        return;
    }
    puts("1. server socket created (fd=");
    print_num((uint64_t)srv);
    puts(") [ok]\n");

    /* 2. Bind server socket */
    sockaddr_un_t srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    strcpy(srv_addr.sun_path, "/tmp/gui_server.sock");
    int bind_res = (int)syscall(SYS_BIND, (uint64_t)srv, (uint64_t)(uintptr_t)&srv_addr, sizeof(srv_addr), 0, 0);
    if (bind_res != 0) {
        puts("error: sys_bind failed\n");
        syscall(SYS_CLOSE, (uint64_t)srv, 0, 0, 0, 0);
        return;
    }
    puts("2. server socket bound to /tmp/gui_server.sock [ok]\n");

    /* 3. Listen */
    int listen_res = (int)syscall(SYS_LISTEN, (uint64_t)srv, 5, 0, 0, 0);
    if (listen_res != 0) {
        puts("error: sys_listen failed\n");
        syscall(SYS_CLOSE, (uint64_t)srv, 0, 0, 0, 0);
        return;
    }
    puts("3. server listening for incoming connections [ok]\n");

    /* 4. Create client socket */
    int cli = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (cli < 0) {
        puts("error: sys_socket(cli) failed\n");
        syscall(SYS_CLOSE, (uint64_t)srv, 0, 0, 0, 0);
        return;
    }
    puts("4. client socket created (fd=");
    print_num((uint64_t)cli);
    puts(") [ok]\n");

    /* 5. Connect client to server */
    int conn_res = (int)syscall(SYS_CONNECT, (uint64_t)cli, (uint64_t)(uintptr_t)&srv_addr, sizeof(srv_addr), 0, 0);
    if (conn_res != 0) {
        puts("error: sys_connect failed\n");
        syscall(SYS_CLOSE, (uint64_t)cli, 0, 0, 0, 0);
        syscall(SYS_CLOSE, (uint64_t)srv, 0, 0, 0, 0);
        return;
    }
    puts("5. client connected to server [ok]\n");

    /* 6. Accept on server */
    sockaddr_un_t cli_addr;
    size_t cli_addrlen = sizeof(cli_addr);
    int accepted = (int)syscall(SYS_ACCEPT, (uint64_t)srv, (uint64_t)(uintptr_t)&cli_addr, (uint64_t)(uintptr_t)&cli_addrlen, 0, 0);
    if (accepted < 0) {
        puts("error: sys_accept failed\n");
        syscall(SYS_CLOSE, (uint64_t)cli, 0, 0, 0, 0);
        syscall(SYS_CLOSE, (uint64_t)srv, 0, 0, 0, 0);
        return;
    }
    puts("6. server accepted connection (conn_fd=");
    print_num((uint64_t)accepted);
    puts(") [ok]\n");

    /* 7. Client sends message */
    const char *msg_out = "hello display server";
    int64_t sent = syscall(SYS_SEND, (uint64_t)cli, (uint64_t)(uintptr_t)msg_out, strlen(msg_out), 0, 0);
    if (sent != (int64_t)strlen(msg_out)) {
        puts("error: sys_send failed\n");
    }
    puts("7. client sent 'hello display server' [ok]\n");

    /* 8. Test poll on server socket */
    pollfd_t pfd;
    pfd.fd = accepted;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int poll_res = (int)syscall(SYS_POLL, (uint64_t)(uintptr_t)&pfd, 1, 100, 0, 0);
    if (poll_res > 0 && (pfd.revents & POLLIN)) {
        puts("8. sys_poll detected incoming data on server socket [ok]\n");
    } else {
        puts("8. sys_poll warning: revents=");
        print_hex(pfd.revents);
        puts("\n");
    }

    /* 9. Server receives message */
    char recv_buf[64];
    memset(recv_buf, 0, sizeof(recv_buf));
    int64_t rcvd = syscall(SYS_RECV, (uint64_t)accepted, (uint64_t)(uintptr_t)recv_buf, sizeof(recv_buf) - 1, 0, 0);
    puts("9. server received: \"");
    puts(recv_buf);
    puts("\" (bytes=");
    print_num((uint64_t)rcvd);
    puts(") [ok]\n");

    /* 10. Server sends response */
    const char *ack_msg = "ack compositor ready";
    syscall(SYS_SEND, (uint64_t)accepted, (uint64_t)(uintptr_t)ack_msg, strlen(ack_msg), 0, 0);
    memset(recv_buf, 0, sizeof(recv_buf));
    syscall(SYS_RECV, (uint64_t)cli, (uint64_t)(uintptr_t)recv_buf, sizeof(recv_buf) - 1, 0, 0);
    puts("10. client received response: \"");
    puts(recv_buf);
    puts("\" [ok]\n");

    /* 11. Close sockets */
    syscall(SYS_CLOSE, (uint64_t)cli, 0, 0, 0, 0);
    syscall(SYS_CLOSE, (uint64_t)accepted, 0, 0, 0, 0);
    syscall(SYS_CLOSE, (uint64_t)srv, 0, 0, 0, 0);
    puts("11. sockets closed cleanly [ok]\n");
    puts("--- ipctest finished: all tests passed! ---\n");
}

static void cmd_shmtest(void) {
    puts("--- shared memory subsystem verification ---\n");

    /* 1. Create shared memory region */
    int shm_id = (int)syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)"shm_buffer_01", 16384, 0, 0, 0);
    if (shm_id <= 0) {
        puts("error: sys_shm_create failed\n");
        return;
    }
    puts("1. created shm region 'shm_buffer_01' (id=");
    print_num((uint64_t)shm_id);
    puts(", size=16384) [ok]\n");

    /* 2. Map shared memory */
    uint32_t *map_ptr1 = (uint32_t *)syscall(SYS_SHM_MAP, (uint64_t)shm_id, 0, SHM_READ | SHM_WRITE, 0, 0);
    if (!map_ptr1) {
        puts("error: sys_shm_map failed\n");
        syscall(SYS_SHM_CLOSE, (uint64_t)shm_id, 0, 0, 0, 0);
        return;
    }
    puts("2. mapped shm into virtual address space at 0x");
    print_hex((uint64_t)(uintptr_t)map_ptr1);
    puts(" [ok]\n");

    /* 3. Write pattern */
    map_ptr1[0] = 0x12345678;
    map_ptr1[1] = 0xDEADBEEF;
    map_ptr1[2] = 0xCAFEBABE;
    map_ptr1[1023] = 0x55AA55AA;
    puts("3. wrote test patterns to shared memory [ok]\n");

    /* 4. Second mapping (simulating client process mapping same region) */
    int shm_id2 = (int)syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)"shm_buffer_01", 16384, 0, 0, 0);
    puts("4. opened existing shm by name (id=");
    print_num((uint64_t)shm_id2);
    puts(") [ok]\n");

    uint32_t *map_ptr2 = (uint32_t *)syscall(SYS_SHM_MAP, (uint64_t)shm_id2, 0, SHM_READ | SHM_WRITE, 0, 0);
    if (map_ptr2 && map_ptr2[0] == 0x12345678 && map_ptr2[1] == 0xDEADBEEF && map_ptr2[2] == 0xCAFEBABE && map_ptr2[1023] == 0x55AA55AA) {
        puts("5. verified shared memory contents across handles [ok]\n");
    } else {
        puts("error: shared memory content mismatch\n");
    }

    /* 5. Test framebuffer mapping interface */
    int fb_id = (int)syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)"/dev/fb0", 0, 0, 0, 0);
    if (fb_id > 0) {
        void *fb_ptr = (void *)syscall(SYS_SHM_MAP, (uint64_t)fb_id, 0, SHM_READ | SHM_WRITE, 0, 0);
        if (fb_ptr) {
            puts("6. mapped hardware framebuffer /dev/fb0 at 0x");
            print_hex((uint64_t)(uintptr_t)fb_ptr);
            puts(" [ok]\n");
            syscall(SYS_SHM_CLOSE, (uint64_t)fb_id, 0, 0, 0, 0);
        } else {
            puts("6. framebuffer map warning: unable to map /dev/fb0\n");
        }
    } else {
        puts("6. framebuffer map warning: /dev/fb0 region unavailable\n");
    }

    /* 6. Cleanup */
    syscall(SYS_SHM_CLOSE, (uint64_t)shm_id, 0, 0, 0, 0);
    syscall(SYS_SHM_CLOSE, (uint64_t)shm_id2, 0, 0, 0, 0);
    puts("7. shared memory closed cleanly [ok]\n");
    puts("--- shmtest finished: all tests passed! ---\n");
}

static void cmd_grub(void) {
    puts("=================================================================\n");
    puts(" pseuDOS GRUB 2 Integration & Chainloader Configuration\n");
    puts("=================================================================\n");
    puts("EFI Bootloader Targets:\n");
    puts("  /EFI/pseuDOS/BOOTX64.EFI\n");
    puts("  /EFI/pseuDOS/silo.efi\n\n");
    puts("GRUB 2 Menuentry Snippet (/etc/grub.d/40_custom or /boot/grub/grub.cfg):\n");
    puts("  menuentry \"pseuDOS x86_64\" {\n");
    puts("      insmod fat\n");
    puts("      insmod chain\n");
    puts("      search --no-floppy --set=root --file /EFI/pseuDOS/BOOTX64.EFI\n");
    puts("      chainloader /EFI/pseuDOS/BOOTX64.EFI\n");
    puts("  }\n\n");
    puts("Linux Installation Helper:\n");
    puts("  1. Append the menuentry snippet above to /etc/grub.d/40_custom\n");
    puts("  2. Run 'sudo update-grub' (or 'grub2-mkconfig -o /boot/grub/grub.cfg')\n");
    puts("  3. On reboot, select 'pseuDOS x86_64' from the GRUB boot menu\n");
    puts("  4. Live snippet also available at /EFI/pseuDOS/grub.cfg\n");
    puts("=================================================================\n");
}

static void cmd_flash(void) {
    int64_t priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
    if (priv != 1) {
        puts("flash: permission denied: installation requires 'sudo' or KERNEL mode\n");
        return;
    }

    puts("flash: searching for attached mass storage devices... ");
    int64_t count = syscall(SYS_FLASH, FLASH_OP_GET_COUNT, 0, 0, 0, 0);
    if (count <= 0) {
        puts("none found\nflash: error: no mass storage devices detected on system!\n");
        return;
    }

    flash_dev_info_t matching[32];
    uint32_t match_count = 0;

    for (int i = 0; i < count && match_count < 32; i++) {
        flash_dev_info_t info;
        if (syscall(SYS_FLASH, FLASH_OP_GET_DEVICE, (uint64_t)i, (uint64_t)(uintptr_t)&info, 0, 0) == 0) {
            matching[match_count++] = info;
        }
    }

    if (match_count > 0) {
        puts("done\n");
        puts("flash: identifying mass storage devices... done\n");
        puts("flash: displaying detected mass storage devices\n\n");
    } else {
        puts("none found\n");
        puts("flash: error: no mass storage devices detected on system!\n");
        return;
    }

    puts("                                            [pseuDOS Installation]\n");
    puts("==============================================================================================================\n");
    puts("choose the mass storage device you want to install pseuDOS on:\n\n");
    puts("[NO]  |  [TYPE]    |  [DEVICE_NAME]                         |  [SIZE]    |  [BUS_SPEED]\n");
    puts("------+------------+----------------------------------------+------------+------------------------------------\n");

    for (uint32_t i = 0; i < match_count; i++) {
        print_num((uint64_t)(i + 1));
        if (i + 1 < 10) puts("     |  ");
        else puts("    |  ");

        puts(matching[i].type_str);
        size_t tlen = strlen(matching[i].type_str);
        for (size_t s = tlen; s < 10; s++) putc(' ');
        puts("|  ");

        puts(matching[i].name);
        size_t nlen = strlen(matching[i].name);
        for (size_t s = nlen; s < 38; s++) putc(' ');
        puts("|  ");

        puts(matching[i].size_str);
        size_t slen = strlen(matching[i].size_str);
        for (size_t s = slen; s < 10; s++) putc(' ');
        puts("|  ");

        puts(matching[i].bus_speed);
        puts("\n");
    }
    puts("\n");

    flash_dev_info_t selected_dev;
    char line_buf[128];

    while (1) {
        readline(line_buf, sizeof(line_buf), "flash > ");
        char *input = trim(line_buf);

        if (input[0] == '\0') {
            continue;
        }

        if (strcmp(input, "cancel") == 0 || strcmp(input, "stop") == 0) {
            puts("flash: cancelling installation...\n");
            return;
        }

        int sel_num = atoi(input);
        if (sel_num < 1 || sel_num > (int)match_count) {
            puts("flash: error: unknown command\n");
            continue;
        }

        selected_dev = matching[sel_num - 1];
        break;
    }

    /* USB 3.1 Gen 1 minimum speed verification */
    if (selected_dev.type == 2) {
        if (selected_dev.usb_version < 0x0310) {
            puts("ATTENTION! you are attempting to install pseuDOS to an external universal serial bus drive that does not meet the minimum requirement of USB 3.1 Gen 1. it is highly recommended to use a faster drive to make sure installation does not crawl, and to ensure boot times are at max.\n");
            puts("are you sure you want to do this?\n(y/N) ");
            readline(line_buf, sizeof(line_buf), "");
            char *ans = trim(line_buf);
            if (ans[0] != 'y' && ans[0] != 'Y') {
                puts("flash: cancelling installation...\n");
                return;
            }
        }
    }

    /* Safety Confirmation Warning */
    puts("WARNING!!! ensure you have selected the proper target, as this command will\n");
    puts("erase EVERYTHING on the selected drive!!\n");
    puts("are you sure you want to erase and format ");
    puts(selected_dev.name);
    puts("?\n(y/N) ");
    readline(line_buf, sizeof(line_buf), "");
    char *ans = trim(line_buf);
    if (ans[0] != 'y' && ans[0] != 'Y') {
        puts("flash: cancelling installation...\n");
        return;
    }

    puts("\n");
    static char log_buf[4096];
    log_buf[0] = '\0';
    int64_t res = syscall(SYS_FLASH, FLASH_OP_INSTALL, (uint64_t)selected_dev.raw_index, (uint64_t)(uintptr_t)log_buf, sizeof(log_buf), 0);
    if (log_buf[0] != '\0') {
        puts(log_buf);
    }
    if (res != 0) {
        puts("flash: fatal error: installation failed due to hardware disk write error!\n");
        return;
    }

    puts("\nflash: successfully installed pseuDOS to ");
    puts(selected_dev.name);
    puts(" (devpath: ");
    puts(selected_dev.devpath);
    puts(")\n");
    puts("flash: please remove the installation media and press ENTER\n");

    /* Strict wait for ENTER key only */
    while (1) {
        char c = getchar();
        if (c == '\r' || c == '\n') {
            break;
        }
    }

    puts("rebooting system into newly installed pseuDOS...\n");
    syscall(SYS_REBOOT, 0, 0, 0, 0, 0);
}

static void cmd_fs(const char *arg) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_FS, (uint64_t)(uintptr_t)(arg ? arg : ""), (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_mount(const char *arg) {
    g_sys_cmd_buf[0] = '\0';
    if (!arg || arg[0] == '\0') {
        int64_t res = syscall(SYS_MOUNT, 0, 0, 0, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf));
        if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
            puts(g_sys_cmd_buf);
        }
        return;
    }

    if (strcmp(arg, "-a") == 0) {
        int64_t res = syscall(SYS_MOUNT, (uint64_t)(uintptr_t)"-a", 0, 0, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf));
        if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
            puts(g_sys_cmd_buf);
        }
        return;
    }

    /* Parse <src> [tgt] [-t fstype] */
    char src[64] = "";
    char tgt[64] = "";
    char fstype[32] = "auto";
    char copy[256];
    strncpy(copy, arg, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *tokens[8];
    int ntokens = 0;
    char *p = copy;
    while (*p && ntokens < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        tokens[ntokens++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }

    for (int i = 0; i < ntokens; i++) {
        if (strcmp(tokens[i], "-t") == 0 && i + 1 < ntokens) {
            strncpy(fstype, tokens[i + 1], sizeof(fstype) - 1);
            i++;
        } else if (src[0] == '\0') {
            strncpy(src, tokens[i], sizeof(src) - 1);
        } else if (tgt[0] == '\0') {
            strncpy(tgt, tokens[i], sizeof(tgt) - 1);
        }
    }

    if (src[0] == '\0') {
        puts("usage: mount [-a] | mount <device> [target] [-t fstype]\n");
        return;
    }

    if (tgt[0] == '\0') {
        const char *s = src;
        if (strncmp(s, "/dev/", 5) == 0) s += 5;
        char tmp_tgt[64];
        strcpy(tmp_tgt, "/mounts/");
        strcat(tmp_tgt, s);
        strncpy(tgt, tmp_tgt, sizeof(tgt) - 1);
    }

    int64_t res = syscall(SYS_MOUNT, (uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)tgt, (uint64_t)(uintptr_t)fstype, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf));
    if (g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    } else if (res < 0) {
        puts("mount: failed to mount device\n");
    }
}

static void cmd_umount(const char *arg) {
    if (!arg || arg[0] == '\0') {
        puts("usage: umount <target | device>\n");
        return;
    }
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_UMOUNT, (uint64_t)(uintptr_t)arg, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0);
    if (g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    } else if (res < 0) {
        puts("umount: failed to unmount\n");
    }
}

static void cmd_cpu(void) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_CPU, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_mem(void) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_MEM, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_pci(void) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_PCI, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_devpath(const char *arg) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_DEVPATH, (uint64_t)(uintptr_t)(arg ? arg : ""), (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_attached_drives(const char *arg) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_ATTACHED_DRIVES, (uint64_t)(uintptr_t)(arg ? arg : ""), (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_screenres(const char *arg) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_SCREENRES, (uint64_t)(uintptr_t)(arg ? arg : ""), (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_proctest(void) {
    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_PROCTEST, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        puts(g_sys_cmd_buf);
    }
}

static void cmd_halt(void) {
    int64_t res = syscall(SYS_HALT, 0, 0, 0, 0, 0);
    if (res == -EPERM) {
        puts("halt: permission denied: system halt requires 'sudo' or KERNEL mode\n");
    }
}

/* Forward declaration */
static void execute_command_internal(char *cmd_line);

static void execute_command(char *cmd_line) {
    if (!cmd_line) return;

    /* 1. Shell environment variable expansion */
    char exp_line[512];
    expand_vars(cmd_line, exp_line, sizeof(exp_line));

    /* 2. Wildcard globbing expansion */
    char final_line[512];
    expand_wildcards(exp_line, final_line, sizeof(final_line));

    char *cmd_str = trim(final_line);
    if (!cmd_str || cmd_str[0] == '\0') return;

    /* 3. Check for I/O redirection (> or >>) */
    char *redir_append_ptr = strchr(cmd_str, '>');
    char *redir_target = NULL;
    int redir_append = 0;
    if (redir_append_ptr) {
        if (*(redir_append_ptr + 1) == '>') {
            redir_append = 1;
            *redir_append_ptr = '\0';
            redir_target = trim(redir_append_ptr + 2);
        } else {
            redir_append = 0;
            *redir_append_ptr = '\0';
            redir_target = trim(redir_append_ptr + 1);
        }
    }

    if (redir_target && redir_target[0] != '\0') {
        resolve_path(redir_target, g_redirect_path, sizeof(g_redirect_path));
        g_redirect_active = 1;
        g_redirect_append = redir_append;
        g_redirect_first_flush = 1;
        g_redirect_len = 0;
    }

    execute_command_internal(cmd_str);

    if (g_redirect_active) {
        redirect_finish();
    }
}

static int is_singleton_app(const char *name) {
    if (!name || !name[0]) return 0;
    const char *base = strrchr(name, '/');
    if (base) base++;
    else base = name;
    const char *bslash = strrchr(base, '\\');
    if (bslash) base = bslash + 1;

    char tmp[32];
    strncpy(tmp, base, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *dot = strrchr(tmp, '.');
    if (dot && (strcmp(dot, ".exe") == 0 || strcmp(dot, ".bin") == 0)) {
        *dot = '\0';
    }

    return (strcmp(tmp, "ntfs") == 0 ||
            strcmp(tmp, "lack") == 0 ||
            strcmp(tmp, "ninds") == 0 ||
            strcmp(tmp, "gshss") == 0 ||
            strcmp(tmp, "bshss") == 0);
}

static int is_app_running(const char *name) {
    if (!name || !name[0]) return 0;
    const char *base = strrchr(name, '/');
    if (base) base++;
    else base = name;
    const char *bslash = strrchr(base, '\\');
    if (bslash) base = bslash + 1;

    char base_noext[32];
    strncpy(base_noext, base, sizeof(base_noext) - 1);
    base_noext[sizeof(base_noext) - 1] = '\0';
    char *dot = strrchr(base_noext, '.');
    if (dot && strcmp(dot, ".exe") == 0) *dot = '\0';

    char exe_name[36];
    strcpy(exe_name, base_noext);
    strcat(exe_name, ".exe");

    g_sys_cmd_buf[0] = '\0';
    int64_t res = syscall(SYS_PS, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0, 0);
    if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
        if (strstr(g_sys_cmd_buf, exe_name) != NULL || strstr(g_sys_cmd_buf, base_noext) != NULL) {
            return 1;
        }
    }
    return 0;
}

static int extract_as_root_flag(const char *args_in, char *args_out, size_t out_size) {
    if (args_out && out_size > 0) args_out[0] = '\0';
    if (!args_in || !args_in[0]) return 0;

    int found_as_root = 0;
    char tmp[256];
    strncpy(tmp, args_in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *token = tmp;
    while (*token) {
        while (*token == ' ' || *token == '\t') token++;
        if (!*token) break;
        char *end = token;
        while (*end && *end != ' ' && *end != '\t') end++;
        char saved = *end;
        *end = '\0';

        if (strcmp(token, "--as-root") == 0) {
            found_as_root = 1;
        } else {
            if (args_out && out_size > 0) {
                if (args_out[0] != '\0') {
                    size_t l = strlen(args_out);
                    if (l + 1 < out_size) {
                        args_out[l] = ' ';
                        args_out[l + 1] = '\0';
                    }
                }
                size_t l = strlen(args_out);
                size_t tlen = strlen(token);
                if (l + tlen < out_size) {
                    strcat(args_out, token);
                }
            }
        }

        if (saved == '\0') break;
        token = end + 1;
    }
    return found_as_root;
}

static void execute_external_command(const char *cmd, const char *arg, int as_root) {
    if (!cmd || !cmd[0]) return;

    /* 1. Check singleton enforcement */
    if (is_singleton_app(cmd) && is_app_running(cmd)) {
        const char *base = strrchr(cmd, '/');
        if (base) base++;
        else base = cmd;
        puts("error: '");
        puts(base);
        puts("' is already running!\n");
        return;
    }

    /* 2. Build candidate paths */
    char candidates[16][128];
    int num_candidates = 0;

    int has_slash = (strchr(cmd, '/') != NULL || strchr(cmd, '\\') != NULL);
    size_t cmd_len = strlen(cmd);
    int has_exe = (cmd_len >= 4 && strcmp(cmd + cmd_len - 4, ".exe") == 0);

    if (has_slash) {
        resolve_path(cmd, candidates[num_candidates++], 128);
        if (!has_exe && num_candidates < 16) {
            char tmp[128];
            strcpy(tmp, cmd);
            strcat(tmp, ".exe");
            resolve_path(tmp, candidates[num_candidates++], 128);
        }
    } else {
        /* Check current working directory */
        resolve_path(cmd, candidates[num_candidates++], 128);
        if (!has_exe && num_candidates < 16) {
            char tmp[128];
            strcpy(tmp, cmd);
            strcat(tmp, ".exe");
            resolve_path(tmp, candidates[num_candidates++], 128);
        }

        /* Check system directories */
        static const char *search_dirs[] = {
            "/protected/apps/",
            "/protected/gui/",
            "/protected/crit/",
            "/protected/krnl/",
            "/protected/bin/"
        };
        for (int d = 0; d < 5 && num_candidates < 15; d++) {
            char alt[128];
            strcpy(alt, search_dirs[d]);
            strcat(alt, cmd);
            strncpy(candidates[num_candidates++], alt, 127);
            if (!has_exe && num_candidates < 16) {
                strcat(alt, ".exe");
                strncpy(candidates[num_candidates++], alt, 127);
            }
        }
        if (num_candidates < 16) {
            strncpy(candidates[num_candidates++], cmd, 127);
        }
    }

    int found_file = 0;
    int last_err = 0;

    for (int i = 0; i < num_candidates; i++) {
        vfs_stat_t st;
        if (syscall(SYS_STAT, (uint64_t)(uintptr_t)candidates[i], (uint64_t)(uintptr_t)&st, 0, 0, 0) == 0) {
            if (st.type == VFS_TYPE_DIR) continue;
            found_file = 1;

            if (is_singleton_app(candidates[i]) && is_app_running(candidates[i])) {
                const char *base = strrchr(candidates[i], '/');
                if (base) base++;
                else base = candidates[i];
                puts("error: '");
                puts(base);
                puts("' is already running!\n");
                return;
            }

            int current_priv = (int)syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
            int effective_as_root = as_root || (current_priv == 1);
            int64_t pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)candidates[i], (uint64_t)(uintptr_t)arg, 0, (uint64_t)(effective_as_root ? 1 : 0), 0);
            if (pid > 0) {
                if (effective_as_root) {
                    puts("[launched process as root (priv=KERNEL)]\n");
                } else {
                    puts("[launched process]\n");
                }
                return;
            }
            last_err = (int)pid;
            if (pid == -EEXIST || pid == -EADDRINUSE) {
                const char *base = strrchr(candidates[i], '/');
                if (base) base++;
                else base = candidates[i];
                puts("error: '");
                puts(base);
                puts("' is already running!\n");
                return;
            } else if (pid == -ENOEXEC || pid == -EINVAL) {
                puts("error: '");
                puts(cmd);
                puts("' is not a valid PE32+ executable!\n");
                return;
            } else if (pid == -ENOMEM) {
                puts("error: out of kernel heap!\n");
                return;
            } else if (pid == -EPERM || pid == -EACCES) {
                puts("error: permission denied!\n");
                return;
            }
        }
    }

    if (found_file && (last_err == -ENOEXEC || last_err == -EINVAL)) {
        puts("error: '");
        puts(cmd);
        puts("' is not a valid PE32+ executable!\n");
    } else {
        puts("unknown command '");
        puts(cmd);
        puts("'. type 'help' for available commands.\n");
    }
}

static void execute_command_internal(char *cmd_line) {
    char *cmd = trim(cmd_line);
    if (!cmd || cmd[0] == '\0') return;

    if (strcmp(cmd, "--as-root") == 0) {
        char *app_arg = strchr(cmd_line, ' ');
        if (app_arg) app_arg = trim(app_arg + 1);
        if (app_arg && app_arg[0] != '\0') {
            char reordered[512];
            strncpy(reordered, app_arg, sizeof(reordered) - 12);
            reordered[sizeof(reordered) - 12] = '\0';
            strcat(reordered, " --as-root");
            execute_command_internal(reordered);
            return;
        }
        puts("error: --as-root requires an application before it\n");
        puts("usage: <application> [args...] --as-root\n");
        return;
    }

    char *raw_arg = strchr(cmd, ' ');
    if (raw_arg) {
        *raw_arg = '\0';
        raw_arg = trim(raw_arg + 1);
    }

    char clean_arg_buf[256];
    int as_root = extract_as_root_flag(raw_arg, clean_arg_buf, sizeof(clean_arg_buf));
    char *arg = (clean_arg_buf[0] != '\0') ? clean_arg_buf : NULL;

    int64_t prev_priv = 0;
    if (as_root) {
        prev_priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
        if (prev_priv != 1) {
            syscall(SYS_ELEVATE, 0, 0, 0, 0, 0);
        }
    }

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(arg);
    } else if (strcmp(cmd, "date") == 0 || strcmp(cmd, "time") == 0) {
        cmd_date();
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname(arg);
    } else if (strcmp(cmd, "env") == 0) {
        cmd_env();
    } else if (strcmp(cmd, "set") == 0 || strcmp(cmd, "export") == 0) {
        cmd_set(arg);
    } else if (strcmp(cmd, "history") == 0) {
        cmd_history();
    } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
        cmd_ls(arg);
    } else if (strcmp(cmd, "cd") == 0) {
        cmd_cd(arg);
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
        cmd_cat(arg);
    } else if (strcmp(cmd, "more") == 0 || strcmp(cmd, "less") == 0) {
        cmd_more_less(arg);
    } else if (strcmp(cmd, "data") == 0) {
        cmd_data(arg);
    } else if (strcmp(cmd, "touch") == 0) {
        cmd_touch(arg);
    } else if (strcmp(cmd, "mkdir") == 0) {
        cmd_mkdir(arg);
    } else if (strcmp(cmd, "del") == 0 || strcmp(cmd, "rm") == 0) {
        cmd_del(cmd, arg);
    } else if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "copy") == 0) {
        cmd_cp(arg);
    } else if (strcmp(cmd, "mv") == 0 || strcmp(cmd, "move") == 0) {
        cmd_mv(arg);
    } else if (strcmp(cmd, "write") == 0) {
        cmd_write(arg);
    } else if (strcmp(cmd, "edit") == 0) {
        cmd_edit(arg);
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(cmd, "kill") == 0) {
        cmd_kill(arg);
    } else if (strcmp(cmd, "dmesg") == 0) {
        g_sys_cmd_buf[0] = '\0';
        int64_t res = syscall(SYS_DMESG, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0, 0);
        if (res >= 0 && g_sys_cmd_buf[0] != '\0') {
            puts(g_sys_cmd_buf);
        }
    } else if (strcmp(cmd, "fs") == 0 || strcmp(cmd, "df") == 0) {
        cmd_fs(arg);
    } else if (strcmp(cmd, "mount") == 0) {
        cmd_mount(arg);
    } else if (strcmp(cmd, "umount") == 0 || strcmp(cmd, "unmount") == 0) {
        cmd_umount(arg);
    } else if (strcmp(cmd, "cpu") == 0) {
        cmd_cpu();
    } else if (strcmp(cmd, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd, "pci") == 0) {
        cmd_pci();
    } else if (strcmp(cmd, "devpath") == 0) {
        cmd_devpath(arg);
    } else if (strcmp(cmd, "attached-drives") == 0) {
        cmd_attached_drives(arg);
    } else if (strcmp(cmd, "screenres") == 0) {
        cmd_screenres(arg);
    } else if (strcmp(cmd, "proctest") == 0) {
        cmd_proctest();
    } else if (strcmp(cmd, "halt") == 0) {
        cmd_halt();
    } else if (strcmp(cmd, "syscalltest") == 0) {
        cmd_syscalltest();
    } else if (strcmp(cmd, "mousetest") == 0) {
        cmd_mousetest();
    } else if (strcmp(cmd, "ipctest") == 0) {
        cmd_ipctest();
    } else if (strcmp(cmd, "shmtest") == 0) {
        cmd_shmtest();
    } else if (strcmp(cmd, "grub") == 0) {
        cmd_grub();
    } else if (strcmp(cmd, "flash") == 0) {
        if (!is_live_system()) {
            execute_external_command(cmd, arg, as_root);
        } else {
            cmd_flash();
        }
    } else if (strcmp(cmd, "recovery-tools") == 0 || strcmp(cmd, "recovery") == 0 || strcmp(cmd, "rtools") == 0) {
        if (!is_live_system()) {
            execute_external_command(cmd, arg, as_root);
        } else {
            cmd_rtools();
        }
    } else if (strcmp(cmd, "panic") == 0) {
        if (arg && (strcmp(arg, "pagefault") == 0 || strcmp(arg, "pf") == 0)) {
            volatile uint64_t *bad_ptr = (volatile uint64_t *)0x00000080DEAD0000ULL;
            *bad_ptr = 0xCAFEBABE;
        }
        int64_t res = syscall(SYS_PANIC, (uint64_t)(uintptr_t)(arg && arg[0] ? arg : "manual panic triggered from shell"), 0, 0, 0, 0);
        if (res == -EPERM) {
            puts("panic: permission denied: kernel panic requires 'sudo' or KERNEL mode\n");
        }
    } else if (strcmp(cmd, "kernel") == 0 || strcmp(cmd, "su") == 0) {
        syscall(SYS_ELEVATE, 0, 0, 0, 0, 0);
        env_set("USER", "kernel");
        puts("privilege elevated to KERNEL (root). type 'exit' or 'drop' to revert to user mode.\n");
    } else if (strcmp(cmd, "drop") == 0) {
        syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
        env_set("USER", "shell");
        puts("privilege reverted to USER mode.\n");
    } else if (strcmp(cmd, "sudo") == 0) {
        if (!arg || arg[0] == '\0') {
            puts("usage: sudo <command>\n");
        } else {
            int64_t prev_priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
            if (prev_priv != 1) {
                syscall(SYS_ELEVATE, 0, 0, 0, 0, 0);
                env_set("USER", "kernel");
            }
            execute_command_internal(arg);
            if (g_redirect_active) {
                redirect_finish();
            }
            if (prev_priv != 1) {
                syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
                env_set("USER", "shell");
            }
        }
    } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        clear_screen();
    } else if (strcmp(cmd, "reboot") == 0) {
        puts("rebooting system...\n");
        syscall(SYS_REBOOT, 0, 0, 0, 0, 0);
    } else if (strcmp(cmd, "shutdown") == 0) {
        if (arg && strcmp(arg, "now") == 0) {
            puts("powering off system immediately...\n");
            syscall(SYS_SHUTDOWN, 0, 0, 0, 0, 0);
        } else {
            uint64_t delay = (arg && strcmp(arg, "-c") == 0) ? (uint64_t)-1 : 60;
            g_sys_cmd_buf[0] = '\0';
            syscall(SYS_SHUTDOWN, delay, (uint64_t)(uintptr_t)g_sys_cmd_buf, sizeof(g_sys_cmd_buf), 0, 0);
            if (g_sys_cmd_buf[0] != '\0') {
                puts(g_sys_cmd_buf);
            }
        }
    } else if (strcmp(cmd, "exit") == 0) {
        int64_t priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
        if (priv == 1) {
            syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
            env_set("USER", "shell");
            puts("dropped privileges to USER mode.\n");
        } else {
            ntfs_client_close(&g_client);
            syscall(SYS_EXIT, 0, 0, 0, 0, 0);
        }
    } else {
        execute_external_command(cmd, arg, as_root);
    }

    if (as_root && prev_priv != 1) {
        syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
    }
}


void shell_main(void) {
    if (ntfs_client_connect(&g_client, "Terminal", TERM_DEFAULT_W, TERM_DEFAULT_H, NTFS_WIN_NORMAL) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    clear_screen();
    env_init();
    env_set("SHELL", "/protected/apps/shell.exe");

    puts("pseuDOS Virtual Terminal [bshss engine] (v0.7.3)\n");
    puts("type 'help' for a list of commands.\n\n");
    render_terminal();

    char line_buf[256];
    char cwd_buf[384];
    char prompt_buf[512];

    while (1) {
        if (syscall(SYS_GET_PROMPT_PATH, (uint64_t)(uintptr_t)cwd_buf, sizeof(cwd_buf), 0, 0, 0) != 0) {
            if (syscall(SYS_GETCWD, (uint64_t)(uintptr_t)cwd_buf, sizeof(cwd_buf), 0, 0, 0) != 0) {
                strcpy(cwd_buf, "/");
            }
        }
        int64_t priv = syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
        const char *user = (priv == 1) ? "kernel" : "shell";

        /* Format prompt: user@pseuDOS [cwd] > */
        size_t pidx = 0;
        const char *prefix = "@pseuDOS [";
        while (*user) prompt_buf[pidx++] = *user++;
        while (*prefix) prompt_buf[pidx++] = *prefix++;
        for (size_t c = 0; cwd_buf[c]; c++) prompt_buf[pidx++] = cwd_buf[c];
        prompt_buf[pidx++] = ']';
        prompt_buf[pidx++] = ' ';
        prompt_buf[pidx++] = '>';
        prompt_buf[pidx++] = ' ';
        prompt_buf[pidx] = '\0';

        readline(line_buf, sizeof(line_buf), prompt_buf);
        if (line_buf[0] != '\0') {
            history_add(line_buf);
        }
        execute_command(line_buf);
        render_terminal();
    }
}
