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

static void draw_scaled_char(uint32_t *buf, int width, int height, int x, int y, char c, uint32_t fg, int scale) {
    if ((unsigned char)c < 32 || (unsigned char)c > 127) c = '?';
    const uint8_t *glyph = g_vga_font_8x16[(unsigned char)c - 32];
    for (int r = 0; r < 16; r++) {
        uint8_t bits = glyph[r];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                gfx_fill_rect(buf, width, height, x + col * scale, y + r * scale, scale, scale, fg);
            }
        }
    }
}

static void draw_scaled_string(uint32_t *buf, int width, int height, int x, int y, const char *str, uint32_t fg, int scale) {
    int cx = x;
    while (*str) {
        draw_scaled_char(buf, width, height, cx, y, *str, fg, scale);
        cx += 8 * scale;
        str++;
    }
}

void splash_main(void) {
    puts("[splash] system loading screen starting up...\n");

    uint32_t width = 1280;
    uint32_t height = 720;

    int q_sock = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (q_sock >= 0) {
        sockaddr_un_t addr;
        ntfs_memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        for (size_t i = 0; NTFS_SOCKET_PATH[i] && i < sizeof(addr.sun_path) - 1; i++) {
            addr.sun_path[i] = NTFS_SOCKET_PATH[i];
        }
        for (int retry = 0; retry < 20; retry++) {
            if (syscall(SYS_CONNECT, q_sock, (uint64_t)(uintptr_t)&addr, sizeof(addr), 0, 0) == 0) {
                ntfs_msg_t qmsg;
                ntfs_memset(&qmsg, 0, sizeof(qmsg));
                qmsg.type = NTFS_MSG_GET_SCREEN_INFO;
                syscall(SYS_SEND, q_sock, (uint64_t)(uintptr_t)&qmsg, sizeof(qmsg), 0, 0);
                ntfs_msg_t rmsg;
                if (syscall(SYS_RECV, q_sock, (uint64_t)(uintptr_t)&rmsg, sizeof(rmsg), 0, 0) == (int64_t)sizeof(rmsg)) {
                    if (rmsg.width > 0 && rmsg.height > 0) {
                        width = rmsg.width;
                        height = rmsg.height;
                    }
                }
                break;
            }
            syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
        }
        syscall(SYS_CLOSE, q_sock, 0, 0, 0, 0);
    }

    ntfs_client_t client;
    if (ntfs_client_connect(&client, "Splash", width, height, NTFS_WIN_SPLASH) < 0) {
        puts("[splash] ERROR: failed to connect splash client\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    if (!client.pixels) {
        puts("[splash] ERROR: no client pixels\n");
        ntfs_client_close(&client);
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    /* Fill retro teal background */
    for (uint32_t i = 0; i < width * height; i++) {
        client.pixels[i] = COLOR_DESKTOP;
    }

    /* Draw centered text: "loading system..." scaled 2x */
    const char *text = "loading system...";
    int text_len = (int)strlen(text);
    int text_w = text_len * 16;
    int text_h = 32;
    int tx = ((int)width - text_w) / 2;
    int ty = ((int)height - text_h) / 2;
    draw_scaled_string(client.pixels, width, height, tx, ty, text, COLOR_WHITE, 2);

    /* Present splash frame */
    ntfs_client_damage(&client, 0, 0, width, height);

    puts("[splash] loading screen active, waiting for gshss...\n");

    /* Wait until gshss signals readiness (or max timeout) */
    for (int t = 0; t < 200; t++) {
        int ready_id = (int)syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)"gshss_ready", 0, 0, 0, 0);
        if (ready_id > 0) {
            puts("[splash] gshss ready signal detected; dismissing loading screen...\n");
            break;
        }

        ntfs_msg_t msg;
        if (ntfs_client_poll_event(&client, &msg)) {
            if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                break;
            }
        }

        syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
    }

    /* Clean exit: destroy splash window and return */
    ntfs_client_close(&client);
    puts("[splash] loading screen dismissed\n");
    syscall(SYS_EXIT, 0, 0, 0, 0, 0);
}
