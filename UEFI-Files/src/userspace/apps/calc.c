#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

#define CALC_W 240
#define CALC_H 300

static ntfs_client_t g_client;

static int64_t g_acc = 0;
static int64_t g_operand = 0;
static char g_op = 0;
static int g_new_entry = 1;
static char g_display[32] = "0";

typedef struct {
    char label[4];
    int x;
    int y;
    int w;
    int h;
    char key;
} calc_btn_t;

static const calc_btn_t g_buttons[] = {
    {"7", 12, 70, 48, 42, '7'},
    {"8", 68, 70, 48, 42, '8'},
    {"9", 124, 70, 48, 42, '9'},
    {"/", 180, 70, 48, 42, '/'},

    {"4", 12, 122, 48, 42, '4'},
    {"5", 68, 122, 48, 42, '5'},
    {"6", 124, 122, 48, 42, '6'},
    {"*", 180, 122, 48, 42, '*'},

    {"1", 12, 174, 48, 42, '1'},
    {"2", 68, 174, 48, 42, '2'},
    {"3", 124, 174, 48, 42, '3'},
    {"-", 180, 174, 48, 42, '-'},

    {"0", 12, 226, 48, 42, '0'},
    {"C", 68, 226, 48, 42, 'C'},
    {"=", 124, 226, 48, 42, '='},
    {"+", 180, 226, 48, 42, '+'}
};
#define NUM_BUTTONS 16

static void format_display(int64_t val) {
    if (val == 0) {
        g_display[0] = '0';
        g_display[1] = '\0';
        return;
    }
    int neg = (val < 0);
    uint64_t uval = neg ? (uint64_t)(-val) : (uint64_t)val;
    char tmp[32];
    int ti = 0;
    while (uval > 0) {
        tmp[ti++] = '0' + (uval % 10);
        uval /= 10;
    }
    int i = 0;
    if (neg) g_display[i++] = '-';
    while (ti > 0) {
        g_display[i++] = tmp[--ti];
    }
    g_display[i] = '\0';
}

static void render_calculator(void) {
    if (!g_client.pixels) return;

    gfx_fill_rect(g_client.pixels, CALC_W, CALC_H, 0, 0, CALC_W, CALC_H, COLOR_WIN_BG);

    /* Sunken display box */
    gfx_draw_bevel(g_client.pixels, CALC_W, CALC_H, 12, 14, CALC_W - 24, 40, 1);
    gfx_fill_rect(g_client.pixels, CALC_W, CALC_H, 14, 16, CALC_W - 28, 36, COLOR_WHITE);

    /* Text in display box aligned right */
    int len = 0;
    while (g_display[len]) len++;
    int tx = CALC_W - 24 - 10 - len * 8;
    if (tx < 18) tx = 18;
    gfx_draw_string(g_client.pixels, CALC_W, CALC_H, tx, 26, g_display, COLOR_TEXT, 0, 1);

    /* Buttons */
    for (int i = 0; i < NUM_BUTTONS; i++) {
        const calc_btn_t *btn = &g_buttons[i];
        gfx_fill_rect(g_client.pixels, CALC_W, CALC_H, btn->x, btn->y, btn->w, btn->h, COLOR_BTN_FACE);
        gfx_draw_bevel(g_client.pixels, CALC_W, CALC_H, btn->x, btn->y, btn->w, btn->h, 0);

        uint32_t col = (btn->key >= '0' && btn->key <= '9') ? COLOR_BLUE : (btn->key == 'C' ? COLOR_RED : COLOR_BLACK);
        gfx_draw_string(g_client.pixels, CALC_W, CALC_H, btn->x + (btn->w / 2) - 4, btn->y + (btn->h / 2) - 8, btn->label, col, 0, 1);
    }

    ntfs_client_damage(&g_client, 0, 0, CALC_W, CALC_H);
}

static void handle_calc_key(char k) {
    if (k >= '0' && k <= '9') {
        int d = k - '0';
        if (g_new_entry) {
            g_operand = d;
            g_new_entry = 0;
        } else {
            g_operand = g_operand * 10 + d;
        }
        format_display(g_operand);
    } else if (k == 'C' || k == 'c') {
        g_acc = 0;
        g_operand = 0;
        g_op = 0;
        g_new_entry = 1;
        format_display(0);
    } else if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (g_op != 0 && !g_new_entry) {
            if (g_op == '+') g_acc += g_operand;
            else if (g_op == '-') g_acc -= g_operand;
            else if (g_op == '*') g_acc *= g_operand;
            else if (g_op == '/' && g_operand != 0) g_acc /= g_operand;
        } else {
            g_acc = g_operand;
        }
        g_op = k;
        g_new_entry = 1;
        format_display(g_acc);
    } else if (k == '=' || k == '\r' || k == '\n') {
        if (g_op == '+') g_acc += g_operand;
        else if (g_op == '-') g_acc -= g_operand;
        else if (g_op == '*') g_acc *= g_operand;
        else if (g_op == '/' && g_operand != 0) g_acc /= g_operand;
        else g_acc = g_operand;

        g_operand = g_acc;
        g_op = 0;
        g_new_entry = 1;
        format_display(g_acc);
    }
}

void calc_main(void) {
    if (ntfs_client_connect(&g_client, "Calculator", CALC_W, CALC_H, NTFS_WIN_NORMAL | NTFS_WIN_FIXED_SIZE) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    render_calculator();

    ntfs_msg_t msg;
    uint8_t prev_left = 0;

    while (1) {
        int updated = 0;
        while (ntfs_client_poll_event(&g_client, &msg)) {
            if (msg.type == NTFS_MSG_KEY_EVENT) {
                char k = (char)(msg.buttons_or_key & 0xFF);
                handle_calc_key(k);
                updated = 1;
            } else if (msg.type == NTFS_MSG_MOUSE_EVENT) {
                uint8_t left_down = (msg.buttons_or_key & NTFS_MOUSE_BTN_LEFT);
                uint8_t left_click = (left_down && !prev_left);
                if (left_click) {
                    int mx = msg.x;
                    int my = msg.y;
                    for (int i = 0; i < NUM_BUTTONS; i++) {
                        const calc_btn_t *btn = &g_buttons[i];
                        if (mx >= btn->x && mx < btn->x + btn->w &&
                            my >= btn->y && my < btn->y + btn->h) {
                            handle_calc_key(btn->key);
                            updated = 1;
                            break;
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
            render_calculator();
        }

        syscall(SYS_SLEEP, 30, 0, 0, 0, 0);
    }
}
