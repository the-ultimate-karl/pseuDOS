#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

#define NOTE_DEFAULT_W 520
#define NOTE_DEFAULT_H 360
#define MAX_LINES 100
#define LINE_CAP 128

static ntfs_client_t g_client;
static char g_text[MAX_LINES][LINE_CAP];
static int g_line_lens[MAX_LINES];
static int g_num_lines = 1;
static int g_cursor_row = 0;
static int g_cursor_col = 0;
static char g_file_path[128] = "";
static int g_menu_open = 0;
static char g_status_msg[64] = "";
static uint8_t g_prev_left = 0;

static int g_save_dlg_open = 0;
static char g_save_input[64] = "notes.txt";
static int g_save_input_len = 9;

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
    while (ti > 0) {
        buf[i++] = tmp[--ti];
    }
    buf[i] = '\0';
}

static int save_notepad_file(const char *path) {
    char save_buf[MAX_LINES * (LINE_CAP + 1)];
    size_t pos = 0;
    for (int r = 0; r < g_num_lines; r++) {
        for (int c = 0; c < g_line_lens[r]; c++) {
            save_buf[pos++] = g_text[r][c];
        }
        if (r < g_num_lines - 1) {
            save_buf[pos++] = '\n';
        }
    }
    int64_t wr = syscall(SYS_WRITEFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)save_buf, pos, 0, 0);
    if (wr >= 0) {
        const char *fn = path;
        for (const char *p = path; *p; p++) {
            if (*p == '/' || *p == '\\') fn = p + 1;
        }
        char msg[64] = "Saved to ";
        int mp = 9;
        for (int i = 0; fn[i] && mp < 55; i++) msg[mp++] = fn[i];
        msg[mp++] = '!';
        msg[mp] = '\0';
        ntfs_strncpy(g_status_msg, msg, sizeof(g_status_msg) - 1);
        return 0;
    } else {
        ntfs_strncpy(g_status_msg, "Save failed!", sizeof(g_status_msg) - 1);
        return -1;
    }
}

static void do_save_as(void) {
    if (g_save_input_len == 0) {
        ntfs_strncpy(g_save_input, "notes.txt", sizeof(g_save_input) - 1);
        g_save_input_len = 9;
    }
    char full[128];
    if (g_save_input[0] != '/') {
        ntfs_strncpy(full, "/home/user/", sizeof(full) - 1);
        int p = 11;
        for (int i = 0; g_save_input[i] && p < 120; i++) full[p++] = g_save_input[i];
        full[p] = '\0';
    } else {
        ntfs_strncpy(full, g_save_input, sizeof(full) - 1);
    }
    int has_dot = 0;
    for (int i = 0; full[i]; i++) {
        if (full[i] == '.') has_dot = 1;
        if (full[i] == '/') has_dot = 0;
    }
    if (!has_dot) {
        int flen = 0;
        while (full[flen]) flen++;
        if (flen < 120) {
            full[flen++] = '.';
            full[flen++] = 't';
            full[flen++] = 'x';
            full[flen++] = 't';
            full[flen] = '\0';
        }
    }
    ntfs_strncpy(g_file_path, full, sizeof(g_file_path) - 1);
    save_notepad_file(g_file_path);
    g_save_dlg_open = 0;
}

static int load_notepad_file(const char *path) {
    vfs_stat_t st;
    if (syscall(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&st, 0, 0, 0) != 0 || st.size == 0) {
        ntfs_strncpy(g_status_msg, "File not found", sizeof(g_status_msg) - 1);
        return -1;
    }
    char read_buf[MAX_LINES * LINE_CAP];
    size_t to_read = (st.size < sizeof(read_buf) - 1) ? st.size : (sizeof(read_buf) - 1);
    int64_t rd = syscall(SYS_READFILE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)read_buf, to_read, 0, 0);
    if (rd <= 0) return -1;

    read_buf[rd] = '\0';
    ntfs_memset(g_text, 0, sizeof(g_text));
    ntfs_memset(g_line_lens, 0, sizeof(g_line_lens));
    int cur_r = 0, cur_c = 0;
    for (int64_t i = 0; i < rd; i++) {
        char ch = read_buf[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            g_line_lens[cur_r] = cur_c;
            if (cur_r + 1 < MAX_LINES) {
                cur_r++;
                cur_c = 0;
            }
        } else if (cur_c < LINE_CAP - 1) {
            g_text[cur_r][cur_c++] = ch;
        }
    }
    g_line_lens[cur_r] = cur_c;
    g_num_lines = cur_r + 1;
    g_cursor_row = 0;
    g_cursor_col = 0;
    ntfs_strncpy(g_status_msg, "Loaded file", sizeof(g_status_msg) - 1);
    return 0;
}

static void render_notepad(void) {
    if (!g_client.pixels) return;

    int win_w = (int)g_client.width;
    int win_h = (int)g_client.height;
    if (win_w < 200) win_w = 200;
    if (win_h < 150) win_h = 150;

    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, 0, win_w, win_h, COLOR_WIN_BG);

    /* Menu bar / Ribbon */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, 0, win_w, 22, COLOR_WIN_BG);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 0, 0, win_w, 22, 0);

    /* "File" menu button */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 4, 2, 40, 18, g_menu_open ? 1 : 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 10, 4, "File", COLOR_TEXT, 0, 1);

    /* "Save" quick ribbon button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 48, 2, 44, 18, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 48, 2, 44, 18, 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 54, 4, "Save", COLOR_TEXT, 0, 1);

    /* "Save As" quick ribbon button */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 96, 2, 64, 18, COLOR_BTN_FACE);
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 96, 2, 64, 18, g_save_dlg_open ? 1 : 0);
    gfx_draw_string(g_client.pixels, win_w, win_h, 101, 4, "Save As", COLOR_TEXT, 0, 1);

    /* Static menu labels */
    gfx_draw_string(g_client.pixels, win_w, win_h, 172, 4, "Edit   Help", COLOR_TEXT, 0, 1);

    /* Main text box */
    int tx = 4;
    int ty = 24;
    int tw = win_w - 8;
    int th = win_h - 48;

    gfx_draw_bevel(g_client.pixels, win_w, win_h, tx, ty, tw, th, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, tx + 1, ty + 1, tw - 2, th - 2, COLOR_WHITE);

    /* Text rendering */
    int visible_rows = (th - 6) / 16;
    int max_visible_cols = (tw - 8) / 8;
    for (int r = 0; r < visible_rows && r < g_num_lines; r++) {
        int py = ty + 3 + r * 16;
        for (int c = 0; c < g_line_lens[r] && c < max_visible_cols; c++) {
            gfx_draw_char(g_client.pixels, win_w, win_h, tx + 4 + c * 8, py, g_text[r][c], COLOR_BLACK, COLOR_WHITE, 1);
        }
    }

    /* Cursor */
    int cx = tx + 4 + g_cursor_col * 8;
    int cy = ty + 3 + g_cursor_row * 16;
    if (cx < tx + tw - 4 && cy < ty + th - 16) {
        gfx_fill_rect(g_client.pixels, win_w, win_h, cx, cy, 2, 16, COLOR_BLACK);
    }

    /* Status Bar at bottom */
    int sb_y = win_h - 22;
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 2, sb_y, win_w - 4, 20, 1);

    char stat[64] = "Ln ";
    char lbuf[16], cbuf[16];
    u32_to_str(g_cursor_row + 1, lbuf);
    u32_to_str(g_cursor_col + 1, cbuf);
    int p = 3;
    for (int k = 0; lbuf[k]; k++) stat[p++] = lbuf[k];
    stat[p++] = ','; stat[p++] = ' '; stat[p++] = 'C'; stat[p++] = 'o'; stat[p++] = 'l'; stat[p++] = ' ';
    for (int k = 0; cbuf[k]; k++) stat[p++] = cbuf[k];
    stat[p] = '\0';

    gfx_draw_string(g_client.pixels, win_w, win_h, 10, sb_y + 2, stat, COLOR_TEXT, 0, 1);

    /* Status message on right of status bar */
    if (g_status_msg[0] != '\0') {
        gfx_draw_string(g_client.pixels, win_w, win_h, win_w - 200, sb_y + 2, g_status_msg, 0x00008000, 0, 1);
    }

    /* File Dropdown Menu */
    if (g_menu_open) {
        int mx = 4;
        int my = 21;
        int mw = 100;
        int mh = 86;
        gfx_fill_rect(g_client.pixels, win_w, win_h, mx, my, mw, mh, COLOR_WIN_BG);
        gfx_draw_bevel(g_client.pixels, win_w, win_h, mx, my, mw, mh, 0);

        gfx_draw_string(g_client.pixels, win_w, win_h, mx + 10, my + 4, "New", COLOR_TEXT, 0, 1);
        gfx_draw_string(g_client.pixels, win_w, win_h, mx + 10, my + 20, "Open", COLOR_TEXT, 0, 1);
        gfx_draw_string(g_client.pixels, win_w, win_h, mx + 10, my + 36, "Save", COLOR_TEXT, 0, 1);
        gfx_draw_string(g_client.pixels, win_w, win_h, mx + 10, my + 52, "Save As...", COLOR_TEXT, 0, 1);
        gfx_draw_string(g_client.pixels, win_w, win_h, mx + 10, my + 68, "Exit", COLOR_TEXT, 0, 1);
    }

    /* Save As Modal Dialog */
    if (g_save_dlg_open) {
        int dw = 320;
        int dh = 120;
        int dx = (win_w - dw) / 2;
        int dy = (win_h - dh) / 2;

        /* Window frame */
        gfx_fill_rect(g_client.pixels, win_w, win_h, dx, dy, dw, dh, COLOR_WIN_BG);
        gfx_draw_bevel(g_client.pixels, win_w, win_h, dx, dy, dw, dh, 0);

        /* Title bar */
        gfx_fill_rect(g_client.pixels, win_w, win_h, dx + 2, dy + 2, dw - 4, 20, COLOR_WIN_TITLE);
        gfx_draw_string(g_client.pixels, win_w, win_h, dx + 6, dy + 4, "Save File As (/home/user/)", COLOR_TITLE_TEXT, 0, 1);
        gfx_draw_string(g_client.pixels, win_w, win_h, dx + dw - 16, dy + 4, "X", COLOR_TITLE_TEXT, 0, 1);

        /* Label */
        gfx_draw_string(g_client.pixels, win_w, win_h, dx + 12, dy + 26, "File name (.txt):", COLOR_TEXT, 0, 1);

        /* Text input field */
        gfx_draw_bevel(g_client.pixels, win_w, win_h, dx + 10, dy + 44, dw - 20, 24, 1);
        gfx_fill_rect(g_client.pixels, win_w, win_h, dx + 11, dy + 45, dw - 22, 22, COLOR_WHITE);
        gfx_draw_string(g_client.pixels, win_w, win_h, dx + 16, dy + 48, g_save_input, COLOR_TEXT, 0, 1);
        gfx_fill_rect(g_client.pixels, win_w, win_h, dx + 16 + g_save_input_len * 8, dy + 47, 2, 16, COLOR_BLACK);

        /* [ Save ] button */
        gfx_fill_rect(g_client.pixels, win_w, win_h, dx + dw - 145, dy + dh - 32, 60, 22, COLOR_BTN_FACE);
        gfx_draw_bevel(g_client.pixels, win_w, win_h, dx + dw - 145, dy + dh - 32, 60, 22, 0);
        gfx_draw_string(g_client.pixels, win_w, win_h, dx + dw - 133, dy + dh - 28, "Save", COLOR_TEXT, 0, 1);

        /* [ Cancel ] button */
        gfx_fill_rect(g_client.pixels, win_w, win_h, dx + dw - 75, dy + dh - 32, 60, 22, COLOR_BTN_FACE);
        gfx_draw_bevel(g_client.pixels, win_w, win_h, dx + dw - 75, dy + dh - 32, 60, 22, 0);
        gfx_draw_string(g_client.pixels, win_w, win_h, dx + dw - 67, dy + dh - 28, "Cancel", COLOR_TEXT, 0, 1);
    }

    ntfs_client_damage(&g_client, 0, 0, win_w, win_h);
}

void notepad_main(void) {
    char arg_buf[256];
    arg_buf[0] = '\0';
    syscall(SYS_GET_ARGS, (uint64_t)(uintptr_t)arg_buf, sizeof(arg_buf), 0, 0, 0);

    /* Trim leading whitespace */
    char *file_path = arg_buf;
    while (*file_path == ' ' || *file_path == '\t') file_path++;

    char win_title[64];
    if (file_path[0] != '\0') {
        ntfs_strncpy(g_file_path, file_path, sizeof(g_file_path) - 1);
        const char *fname = file_path;
        for (const char *p = file_path; *p; p++) {
            if (*p == '/' || *p == '\\') fname = p + 1;
        }
        ntfs_strncpy(g_save_input, fname, sizeof(g_save_input) - 1);
        int l = 0; while (g_save_input[l]) l++; g_save_input_len = l;
        char *t = win_title;
        const char *pfx = "Notepad - ";
        while (*pfx) *t++ = *pfx++;
        for (int i = 0; fname[i] && i < 32; i++) *t++ = fname[i];
        *t = '\0';
    } else {
        ntfs_strncpy(win_title, "Notepad - Untitled", sizeof(win_title) - 1);
    }

    if (ntfs_client_connect(&g_client, win_title, NOTE_DEFAULT_W, NOTE_DEFAULT_H, NTFS_WIN_NORMAL) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    ntfs_memset(g_text, 0, sizeof(g_text));
    ntfs_memset(g_line_lens, 0, sizeof(g_line_lens));
    g_num_lines = 1;
    g_cursor_row = 0;
    g_cursor_col = 0;

    /* If a file path was passed, read file and populate lines */
    if (file_path[0] != '\0') {
        load_notepad_file(file_path);
    }

    render_notepad();

    ntfs_msg_t msg;
    while (1) {
        int updated = 0;
        while (ntfs_client_poll_event(&g_client, &msg)) {
            if (msg.type == NTFS_MSG_MOUSE_EVENT) {
                int mx = msg.x;
                int my = msg.y;
                uint8_t left_down = (msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                uint8_t left_click = (left_down && !g_prev_left);
                g_prev_left = left_down;

                if (left_click) {
                    if (g_save_dlg_open) {
                        int win_w = (int)g_client.width;
                        int win_h = (int)g_client.height;
                        int dw = 320, dh = 120;
                        int dx = (win_w - dw) / 2, dy = (win_h - dh) / 2;
                        /* [ Save ] button */
                        if (mx >= dx + dw - 145 && mx <= dx + dw - 85 && my >= dy + dh - 32 && my <= dy + dh - 10) {
                            do_save_as();
                            updated = 1;
                        }
                        /* [ Cancel ] button */
                        else if (mx >= dx + dw - 75 && mx <= dx + dw - 15 && my >= dy + dh - 32 && my <= dy + dh - 10) {
                            g_save_dlg_open = 0;
                            updated = 1;
                        }
                        /* Titlebar X */
                        else if (mx >= dx + dw - 20 && mx <= dx + dw - 2 && my >= dy + 2 && my <= dy + 20) {
                            g_save_dlg_open = 0;
                            updated = 1;
                        }
                    } else if (g_menu_open) {
                        if (mx >= 4 && mx <= 104 && my >= 21 && my <= 107) {
                            if (my < 37) {
                                /* New */
                                ntfs_memset(g_text, 0, sizeof(g_text));
                                ntfs_memset(g_line_lens, 0, sizeof(g_line_lens));
                                g_num_lines = 1;
                                g_cursor_row = 0;
                                g_cursor_col = 0;
                                g_file_path[0] = '\0';
                                ntfs_strncpy(g_save_input, "notes.txt", sizeof(g_save_input) - 1);
                                g_save_input_len = 9;
                                ntfs_strncpy(g_status_msg, "New document", sizeof(g_status_msg) - 1);
                            } else if (my < 53) {
                                /* Open */
                                if (g_file_path[0] != '\0') load_notepad_file(g_file_path);
                                else load_notepad_file("/home/user/notes.txt");
                            } else if (my < 69) {
                                /* Save */
                                if (g_file_path[0] == '\0') {
                                    g_save_dlg_open = 1;
                                } else {
                                    save_notepad_file(g_file_path);
                                }
                            } else if (my < 85) {
                                /* Save As... */
                                g_save_dlg_open = 1;
                            } else {
                                /* Exit */
                                ntfs_client_close(&g_client);
                                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                                return;
                            }
                        }
                        g_menu_open = 0;
                        updated = 1;
                    } else {
                        /* Check "File" menu click */
                        if (mx >= 4 && mx <= 44 && my >= 2 && my <= 20) {
                            g_menu_open = 1;
                            updated = 1;
                        } else if (mx >= 48 && mx <= 92 && my >= 2 && my <= 20) {
                            /* Quick "Save" button click */
                            if (g_file_path[0] == '\0') {
                                g_save_dlg_open = 1;
                            } else {
                                save_notepad_file(g_file_path);
                            }
                            updated = 1;
                        } else if (mx >= 96 && mx <= 162 && my >= 2 && my <= 20) {
                            /* Quick "Save As" button click */
                            g_save_dlg_open = 1;
                            updated = 1;
                        } else {
                            /* Click inside text editor box to reposition cursor */
                            int tx = 4, ty = 24, tw = (int)g_client.width - 8, th = (int)g_client.height - 48;
                            if (mx >= tx && mx < tx + tw && my >= ty && my < ty + th) {
                                int r = (my - (ty + 3)) / 16;
                                int c = (mx - (tx + 4)) / 8;
                                if (r >= 0 && r < g_num_lines) {
                                    g_cursor_row = r;
                                    if (c < 0) c = 0;
                                    if (c > g_line_lens[g_cursor_row]) c = g_line_lens[g_cursor_row];
                                    g_cursor_col = c;
                                    updated = 1;
                                }
                            }
                        }
                    }
                }
            } else if (msg.type == NTFS_MSG_KEY_EVENT) {
                char ch = (char)(msg.buttons_or_key & 0xFF);
                if (g_save_dlg_open) {
                    if (ch == 27) { /* ESC */
                        g_save_dlg_open = 0;
                        updated = 1;
                    } else if (ch == '\r' || ch == '\n') {
                        do_save_as();
                        updated = 1;
                    } else if (ch == '\b') {
                        if (g_save_input_len > 0) {
                            g_save_input[--g_save_input_len] = '\0';
                            updated = 1;
                        }
                    } else if (ch >= 32 && ch <= 126 && ch != '/' && ch != '\\') {
                        if (g_save_input_len < (int)sizeof(g_save_input) - 5) {
                            g_save_input[g_save_input_len++] = ch;
                            g_save_input[g_save_input_len] = '\0';
                            updated = 1;
                        }
                    }
                } else {
                    if (ch == 27) { /* ESC */
                        if (g_menu_open) {
                            g_menu_open = 0;
                            updated = 1;
                        } else {
                            ntfs_client_close(&g_client);
                            syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                            return;
                        }
                    } else if (ch == 19) { /* Ctrl+S */
                        if (g_file_path[0] == '\0') {
                            g_save_dlg_open = 1;
                        } else {
                            save_notepad_file(g_file_path);
                        }
                        updated = 1;
                    } else if (ch == '\r' || ch == '\n') {
                    if (g_num_lines < MAX_LINES - 1) {
                        g_cursor_row++;
                        g_cursor_col = 0;
                        if (g_cursor_row >= g_num_lines) {
                            g_num_lines = g_cursor_row + 1;
                        }
                        updated = 1;
                    }
                } else if (ch == '\b') {
                    if (g_cursor_col > 0) {
                        g_cursor_col--;
                        g_text[g_cursor_row][g_cursor_col] = ' ';
                        g_line_lens[g_cursor_row] = g_cursor_col;
                        updated = 1;
                    } else if (g_cursor_row > 0) {
                        g_cursor_row--;
                        g_cursor_col = g_line_lens[g_cursor_row];
                        updated = 1;
                    }
                } else if (ch >= 32 && ch <= 126) {
                    if (g_cursor_col < LINE_CAP - 1) {
                        g_text[g_cursor_row][g_cursor_col++] = ch;
                        if (g_cursor_col > g_line_lens[g_cursor_row]) {
                            g_line_lens[g_cursor_row] = g_cursor_col;
                        }
                        updated = 1;
                    }
                }
            }
        } else if (msg.type == NTFS_MSG_WINDOW_RESIZED) {
                updated = 1;
            } else if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                ntfs_client_close(&g_client);
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                return;
            }
        }

        if (updated) {
            render_notepad();
        }

        syscall(SYS_SLEEP, 30, 0, 0, 0, 0);
    }
}
