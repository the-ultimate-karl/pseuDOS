#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "bootinfo.h"
#include "mice.h"
#include "ntfs_protocol.h"

static size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void puts(const char *str) {
    if (!str) return;
    syscall(SYS_WRITE, 1, (uint64_t)(uintptr_t)str, strlen(str), 0, 0);
}

static void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
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

static char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

/* Internal Server State */
static uint32_t *g_fb_ptr = NULL;
static uint32_t g_screen_width = 1280;
static uint32_t g_screen_height = 720;
static uint32_t g_screen_pitch = 1280;

static int g_compositor_fd = -1;
static int g_wm_fd = -1;
static int g_compositor_shm_id = -1;
static uint32_t *g_compositor_canvas = NULL;
static int g_last_known_tty = 1;
static int g_last_buf_idx = 0;

typedef struct {
    int active;
    uint32_t id;
    int client_fd;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    int shm_id;
    char title[32];
    uint32_t flags;
    int z_order;
} ntfs_window_t;

static ntfs_window_t g_windows[MAX_WINDOWS];
static uint32_t g_next_window_id = 1;
static uint32_t g_focused_window_id = 0;

#define CURSOR_W 12
#define CURSOR_H 19

static const char *g_cursor_sprite[CURSOR_H] = {
    "B...........",
    "BB..........",
    "BWB.........",
    "BWBWB.......",
    "BWWWB.......",
    "BWWWWWB.....",
    "BWWWWWB.....",
    "BWWWWWWB....",
    "BWWWWWWWB...",
    "BWWWWWWWWB..",
    "BWWWWWWWWWB.",
    "BWWWWWBBBBBB",
    "BWWWBWWB....",
    "BWWB..BWWB..",
    "BWB....BWWB.",
    "BB......BWWB",
    "B........BWB",
    "..........BB",
    "............"
};

static int32_t g_cur_x = 640;
static int32_t g_cur_y = 360;
static uint32_t g_cursor_bg[CURSOR_H][CURSOR_W];
static int g_cursor_visible = 0;
static int g_cursor_saved = 0;

static void cursor_hide(void) {
    if (!g_cursor_visible || !g_fb_ptr || !g_cursor_saved) return;
    for (int r = 0; r < CURSOR_H; r++) {
        int py = g_cur_y + r;
        if (py < 0 || py >= (int)g_screen_height) continue;
        for (int c = 0; c < CURSOR_W; c++) {
            int px = g_cur_x + c;
            if (px < 0 || px >= (int)g_screen_width) continue;
            g_fb_ptr[py * g_screen_pitch + px] = g_cursor_bg[r][c];
        }
    }
    g_cursor_visible = 0;
}

static void cursor_show(void) {
    if (!g_fb_ptr) return;
    for (int r = 0; r < CURSOR_H; r++) {
        int py = g_cur_y + r;
        if (py < 0 || py >= (int)g_screen_height) continue;
        for (int c = 0; c < CURSOR_W; c++) {
            int px = g_cur_x + c;
            if (px < 0 || px >= (int)g_screen_width) continue;
            g_cursor_bg[r][c] = g_fb_ptr[py * g_screen_pitch + px];
        }
    }
    g_cursor_saved = 1;

    for (int r = 0; r < CURSOR_H; r++) {
        int py = g_cur_y + r;
        if (py < 0 || py >= (int)g_screen_height) continue;
        const char *line = g_cursor_sprite[r];
        for (int c = 0; c < CURSOR_W; c++) {
            int px = g_cur_x + c;
            if (px < 0 || px >= (int)g_screen_width) continue;
            char ch = line[c];
            if (ch == 'B') {
                g_fb_ptr[py * g_screen_pitch + px] = COLOR_BLACK;
            } else if (ch == 'W') {
                g_fb_ptr[py * g_screen_pitch + px] = COLOR_WHITE;
            }
        }
    }
    g_cursor_visible = 1;
}

static void cursor_move(int32_t nx, int32_t ny) {
    if (nx == g_cur_x && ny == g_cur_y && g_cursor_visible) return;
    cursor_hide();
    g_cur_x = nx;
    g_cur_y = ny;
    cursor_show();
}

#define MAX_CLIENT_FDS 64
static int g_client_fds[MAX_CLIENT_FDS];
static int g_client_roles[MAX_CLIENT_FDS];

static void init_server_state(void) {
    memset(g_windows, 0, sizeof(g_windows));
    for (int i = 0; i < MAX_CLIENT_FDS; i++) {
        g_client_fds[i] = -1;
        g_client_roles[i] = NTFS_ROLE_CLIENT;
    }
}

static void add_client(int fd) {
    for (int i = 0; i < MAX_CLIENT_FDS; i++) {
        if (g_client_fds[i] == -1) {
            g_client_fds[i] = fd;
            g_client_roles[i] = NTFS_ROLE_CLIENT;
            return;
        }
    }
}

static void remove_client(int fd) {
    if (fd == g_compositor_fd) {
        g_compositor_fd = -1;
        g_compositor_canvas = NULL;
    }
    if (fd == g_wm_fd) {
        g_wm_fd = -1;
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].client_fd == fd) {
            ntfs_msg_t notify;
            memset(&notify, 0, sizeof(notify));
            notify.type = NTFS_MSG_WINDOW_DESTROYED;
            notify.window_id = g_windows[i].id;
            if (g_wm_fd >= 0) syscall(SYS_SEND, g_wm_fd, (uint64_t)(uintptr_t)&notify, sizeof(notify), 0, 0);
            if (g_compositor_fd >= 0) syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&notify, sizeof(notify), 0, 0);
            if (g_windows[i].shm_id > 0) syscall(SYS_SHM_CLOSE, g_windows[i].shm_id, 0, 0, 0, 0);
            g_windows[i].active = 0;
        }
    }

    for (int i = 0; i < MAX_CLIENT_FDS; i++) {
        if (g_client_fds[i] == fd) {
            g_client_fds[i] = -1;
            g_client_roles[i] = NTFS_ROLE_CLIENT;
            break;
        }
    }
    syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
}

void ntfs_main(void) {
    puts("[ntfs] Not The Fanciest Server starting up...\n");

    init_server_state();

    /* 1. Discover physical display dimensions via BootInfo */
    BootInfo bi;
    if (syscall(SYS_GET_BOOTINFO, (uint64_t)(uintptr_t)&bi, 0, 0, 0, 0) == 0) {
        if (bi.fb.width > 0 && bi.fb.height > 0) {
            g_screen_width = (uint32_t)bi.fb.width;
            g_screen_height = (uint32_t)bi.fb.height;
            g_screen_pitch = (uint32_t)(bi.fb.pixels_per_scanline > 0 ? bi.fb.pixels_per_scanline : bi.fb.width);
        }
    }

    /* 2. Map physical hardware framebuffer via /dev/fb0 */
    int fb_shm = (int)syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)"/dev/fb0", 0, 0, 0, 0);
    if (fb_shm < 0) {
        puts("[ntfs] ERROR: unable to acquire /dev/fb0 shared memory\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    g_fb_ptr = (uint32_t *)(uintptr_t)syscall(SYS_SHM_MAP, fb_shm, 0, SHM_READ | SHM_WRITE, 0, 0);
    if (!g_fb_ptr) {
        puts("[ntfs] ERROR: failed to map physical framebuffer into address space\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    /* Clear screen to desktop background color */
    for (uint32_t y = 0; y < g_screen_height; y++) {
        uint32_t *row = &g_fb_ptr[y * g_screen_pitch];
        for (uint32_t x = 0; x < g_screen_width; x++) {
            row[x] = COLOR_DESKTOP;
        }
    }
    cursor_show();

    /* 3. Bind UNIX domain server socket */
    int srv_sock = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (srv_sock < 0) {
        puts("[ntfs] ERROR: socket creation failed\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    sockaddr_un_t srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sun_family = AF_UNIX;
    strncpy(srv_addr.sun_path, NTFS_SOCKET_PATH, sizeof(srv_addr.sun_path) - 1);

    int bind_res = (int)syscall(SYS_BIND, srv_sock, (uint64_t)(uintptr_t)&srv_addr, sizeof(srv_addr), 0, 0);
    if (bind_res < 0) {
        puts("[ntfs] ERROR: unable to bind /tmp/ntfs.sock\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    syscall(SYS_LISTEN, srv_sock, 64, 0, 0, 0);
    puts("[ntfs] display server listening on /tmp/ntfs.sock\n");

    /* 4. Event loop */
    pollfd_t poll_fds[MAX_CLIENT_FDS + 8];
    mouse_event_t mouse_ev;
    uint32_t prev_mouse_x = 0;
    uint32_t prev_mouse_y = 0;
    uint8_t  prev_mouse_btns = 0;

    while (1) {
        /* Setup poll array */
        int num_poll = 0;
        poll_fds[num_poll].fd = srv_sock;
        poll_fds[num_poll].events = POLLIN;
        poll_fds[num_poll].revents = 0;
        num_poll++;

        for (int i = 0; i < MAX_CLIENT_FDS; i++) {
            if (g_client_fds[i] >= 0) {
                poll_fds[num_poll].fd = g_client_fds[i];
                poll_fds[num_poll].events = POLLIN;
                poll_fds[num_poll].revents = 0;
                num_poll++;
            }
        }

        /* Add fd 0 (stdin/keyboard) */
        poll_fds[num_poll].fd = 0;
        poll_fds[num_poll].events = POLLIN;
        poll_fds[num_poll].revents = 0;
        int kbd_idx = num_poll;
        num_poll++;

        int ready = (int)syscall(SYS_POLL, (uint64_t)(uintptr_t)poll_fds, num_poll, 1, 0, 0);

        int active_tty = (int)syscall(SYS_TTY_GET, 0, 0, 0, 0, 0);
        if (active_tty == 1 && g_last_known_tty == 3) {
            /* Switched back to GUI TTY1 -> Repaint full framebuffer from compositor canvas */
            if (g_compositor_canvas && g_fb_ptr) {
                cursor_hide();
                uint32_t *src_canvas = &g_compositor_canvas[g_last_buf_idx * (g_screen_width * g_screen_height)];
                for (uint32_t r = 0; r < g_screen_height; r++) {
                    memcpy(&g_fb_ptr[r * g_screen_pitch], &src_canvas[r * g_screen_width], g_screen_width * sizeof(uint32_t));
                }
                cursor_show();
            }
        }
        g_last_known_tty = active_tty;

        /* Check if server listening socket /tmp/ntfs.sock was destroyed */
        if (ready > 0 && (poll_fds[0].revents & (POLLNVAL | POLLHUP | POLLERR))) {
            puts("[ntfs] FATAL: /tmp/ntfs.sock destroyed, display server exiting\n");
            break;
        }

        /* Handle new incoming connection */
        if (ready > 0 && (poll_fds[0].revents & POLLIN)) {
            sockaddr_un_t client_addr;
            size_t client_addrlen = sizeof(client_addr);
            int new_fd = (int)syscall(SYS_ACCEPT, srv_sock, (uint64_t)(uintptr_t)&client_addr, (uint64_t)(uintptr_t)&client_addrlen, 0, 0);
            if (new_fd >= 0) {
                add_client(new_fd);
            }
        }

        /* Handle keyboard input from stdin */
        if (ready > 0 && (poll_fds[kbd_idx].revents & POLLIN)) {
            if (active_tty == 1) {
                char kc = 0;
                while (syscall(SYS_READ, 0, (uint64_t)(uintptr_t)&kc, 1, MSG_DONTWAIT, 0) > 0 && kc != 0) {
                    uint8_t ukey = (uint8_t)kc;
                    if (ukey == NTFS_KEY_WIN) {
                        /* Windows Key pressed -> Notify taskbar (gshss) to toggle start menu */
                        for (int w = 0; w < MAX_WINDOWS; w++) {
                            if (g_windows[w].active && (g_windows[w].flags & NTFS_WIN_TASKBAR)) {
                                ntfs_msg_t kmsg;
                                memset(&kmsg, 0, sizeof(kmsg));
                                kmsg.type = NTFS_MSG_KEY_EVENT;
                                kmsg.window_id = g_windows[w].id;
                                kmsg.buttons_or_key = NTFS_KEY_WIN;
                                syscall(SYS_SEND, g_windows[w].client_fd, (uint64_t)(uintptr_t)&kmsg, sizeof(kmsg), MSG_DONTWAIT, 0);
                                break;
                            }
                        }
                    } else if (g_focused_window_id > 0) {
                        ntfs_msg_t kmsg;
                        memset(&kmsg, 0, sizeof(kmsg));
                        kmsg.type = NTFS_MSG_KEY_EVENT;
                        kmsg.window_id = g_focused_window_id;
                        kmsg.buttons_or_key = (uint32_t)ukey;
                        for (int w = 0; w < MAX_WINDOWS; w++) {
                            if (g_windows[w].active && g_windows[w].id == g_focused_window_id) {
                                syscall(SYS_SEND, g_windows[w].client_fd, (uint64_t)(uintptr_t)&kmsg, sizeof(kmsg), MSG_DONTWAIT, 0);
                                break;
                            }
                        }
                    }
                    kc = 0;
                }
            }
        }

        /* Handle messages from connected clients */
        for (int i = 1; i < kbd_idx; i++) {
            if (poll_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                remove_client(poll_fds[i].fd);
                continue;
            }

            if (poll_fds[i].revents & POLLIN) {
                ntfs_msg_t msg;
                int64_t n = syscall(SYS_RECV, poll_fds[i].fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), MSG_DONTWAIT, 0);
                if (n <= 0) {
                    if (n == 0 || (n < 0 && n != -EAGAIN)) {
                        remove_client(poll_fds[i].fd);
                    }
                    continue;
                }

                if (n == (int64_t)sizeof(msg)) {
                    switch (msg.type) {
                        case NTFS_MSG_REGISTER_ROLE:
                            if (msg.flags == NTFS_ROLE_COMPOSITOR) {
                                g_compositor_fd = poll_fds[i].fd;
                                if (msg.shm_id > 0) {
                                    g_compositor_shm_id = (int)msg.shm_id;
                                    g_compositor_canvas = (uint32_t *)(uintptr_t)syscall(SYS_SHM_MAP, g_compositor_shm_id, 0, SHM_READ, 0, 0);
                                }
                                for (int w = 0; w < MAX_WINDOWS; w++) {
                                    if (g_windows[w].active) {
                                        ntfs_msg_t wmsg;
                                        memset(&wmsg, 0, sizeof(wmsg));
                                        wmsg.type = NTFS_MSG_CREATE_WINDOW;
                                        wmsg.window_id = g_windows[w].id;
                                        wmsg.width = g_windows[w].width;
                                        wmsg.height = g_windows[w].height;
                                        wmsg.shm_id = (uint32_t)g_windows[w].shm_id;
                                        wmsg.flags = g_windows[w].flags;
                                        strncpy(wmsg.title, g_windows[w].title, sizeof(wmsg.title) - 1);
                                        syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&wmsg, sizeof(wmsg), 0, 0);

                                        memset(&wmsg, 0, sizeof(wmsg));
                                        wmsg.type = NTFS_MSG_SET_GEOMETRY;
                                        wmsg.window_id = g_windows[w].id;
                                        wmsg.x = g_windows[w].x;
                                        wmsg.y = g_windows[w].y;
                                        wmsg.width = g_windows[w].width;
                                        wmsg.height = g_windows[w].height;
                                        wmsg.flags = (uint32_t)g_windows[w].z_order;
                                        syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&wmsg, sizeof(wmsg), 0, 0);
                                    }
                                }
                            } else if (msg.flags == NTFS_ROLE_WM) {
                                g_wm_fd = poll_fds[i].fd;
                                for (int w = 0; w < MAX_WINDOWS; w++) {
                                    if (g_windows[w].active) {
                                        ntfs_msg_t wmsg;
                                        memset(&wmsg, 0, sizeof(wmsg));
                                        wmsg.type = NTFS_MSG_CREATE_WINDOW;
                                        wmsg.window_id = g_windows[w].id;
                                        wmsg.width = g_windows[w].width;
                                        wmsg.height = g_windows[w].height;
                                        wmsg.shm_id = (uint32_t)g_windows[w].shm_id;
                                        wmsg.flags = g_windows[w].flags;
                                        strncpy(wmsg.title, g_windows[w].title, sizeof(wmsg.title) - 1);
                                        syscall(SYS_SEND, g_wm_fd, (uint64_t)(uintptr_t)&wmsg, sizeof(wmsg), 0, 0);

                                        memset(&wmsg, 0, sizeof(wmsg));
                                        wmsg.type = NTFS_MSG_SET_GEOMETRY;
                                        wmsg.window_id = g_windows[w].id;
                                        wmsg.x = g_windows[w].x;
                                        wmsg.y = g_windows[w].y;
                                        wmsg.width = g_windows[w].width;
                                        wmsg.height = g_windows[w].height;
                                        wmsg.flags = (uint32_t)g_windows[w].z_order;
                                        syscall(SYS_SEND, g_wm_fd, (uint64_t)(uintptr_t)&wmsg, sizeof(wmsg), 0, 0);
                                    }
                                }
                            }
                            break;

                        case NTFS_MSG_GET_SCREEN_INFO: {
                            ntfs_msg_t reply;
                            memset(&reply, 0, sizeof(reply));
                            reply.type = NTFS_MSG_SCREEN_INFO;
                            reply.width = g_screen_width;
                            reply.height = g_screen_height;
                            syscall(SYS_SEND, poll_fds[i].fd, (uint64_t)(uintptr_t)&reply, sizeof(reply), 0, 0);
                            break;
                        }

                        case NTFS_MSG_CREATE_WINDOW: {
                            int slot = -1;
                            for (int w = 0; w < MAX_WINDOWS; w++) {
                                if (!g_windows[w].active) {
                                    slot = w;
                                    break;
                                }
                            }

                            if (slot >= 0) {
                                uint32_t wid = g_next_window_id++;
                                size_t shm_bytes = (size_t)msg.width * (size_t)msg.height * 4;
                                if (!(msg.flags & (NTFS_WIN_BACKGROUND | NTFS_WIN_TASKBAR | NTFS_WIN_SPLASH))) {
                                    size_t max_bytes = (size_t)g_screen_width * (size_t)g_screen_height * 4;
                                    if (shm_bytes < max_bytes) {
                                        shm_bytes = max_bytes;
                                    }
                                }
                                char shm_name[32];
                                /* Generate region name shm:win_X */
                                shm_name[0] = 'w'; shm_name[1] = 'i'; shm_name[2] = 'n'; shm_name[3] = '_';
                                uint32_t tmp = wid;
                                int digits = 0;
                                char numbuf[10];
                                if (tmp == 0) numbuf[digits++] = '0';
                                while (tmp > 0) { numbuf[digits++] = '0' + (tmp % 10); tmp /= 10; }
                                for (int d = 0; d < digits; d++) shm_name[4 + d] = numbuf[digits - 1 - d];
                                shm_name[4 + digits] = '\0';

                                int win_shm = (int)syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)shm_name, shm_bytes, 0, 0, 0);

                                g_windows[slot].active = 1;
                                g_windows[slot].id = wid;
                                g_windows[slot].client_fd = poll_fds[i].fd;
                                g_windows[slot].width = msg.width;
                                g_windows[slot].height = msg.height;
                                g_windows[slot].shm_id = win_shm;
                                g_windows[slot].flags = msg.flags;
                                if (msg.flags & NTFS_WIN_BACKGROUND) {
                                    g_windows[slot].x = 0;
                                    g_windows[slot].y = 0;
                                    g_windows[slot].z_order = -100;
                                } else if (msg.flags & NTFS_WIN_TASKBAR) {
                                    g_windows[slot].x = 0;
                                    g_windows[slot].y = (int32_t)g_screen_height - (int32_t)TASKBAR_HEIGHT;
                                    g_windows[slot].z_order = 1000;
                                } else {
                                    g_windows[slot].x = 0;
                                    g_windows[slot].y = 0;
                                    g_windows[slot].z_order = 10;
                                }
                                strncpy(g_windows[slot].title, msg.title, sizeof(g_windows[slot].title) - 1);

                                /* Reply to client */
                                ntfs_msg_t rep;
                                memset(&rep, 0, sizeof(rep));
                                rep.type = NTFS_MSG_WINDOW_CREATED;
                                rep.window_id = wid;
                                rep.width = msg.width;
                                rep.height = msg.height;
                                rep.shm_id = (uint32_t)win_shm;
                                syscall(SYS_SEND, poll_fds[i].fd, (uint64_t)(uintptr_t)&rep, sizeof(rep), 0, 0);

                                /* Forward to WM and Compositor */
                                msg.window_id = wid;
                                msg.shm_id = (uint32_t)win_shm;
                                msg.x = g_windows[slot].x;
                                msg.y = g_windows[slot].y;
                                if (g_wm_fd >= 0) syscall(SYS_SEND, g_wm_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                                if (g_compositor_fd >= 0) syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            }
                            break;
                        }

                        case NTFS_MSG_DESTROY_WINDOW: {
                            for (int w = 0; w < MAX_WINDOWS; w++) {
                                if (g_windows[w].active && g_windows[w].id == msg.window_id) {
                                    if (g_windows[w].shm_id > 0) syscall(SYS_SHM_CLOSE, g_windows[w].shm_id, 0, 0, 0, 0);
                                    g_windows[w].active = 0;
                                    break;
                                }
                            }
                            if (g_wm_fd >= 0) syscall(SYS_SEND, g_wm_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            if (g_compositor_fd >= 0) syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            break;
                        }

                        case NTFS_MSG_DAMAGE:
                            /* Client notified damage -> forward to compositor */
                            if (g_compositor_fd >= 0) {
                                syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            }
                            break;

                        case NTFS_MSG_SET_WIREFRAME:
                            /* Forward wireframe geometry to compositor */
                            if (g_compositor_fd >= 0) {
                                syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            }
                            break;

                        case NTFS_MSG_SET_GEOMETRY: {
                            int target_client_fd = -1;
                            int size_changed = 0;
                            uint32_t new_w = msg.width;
                            uint32_t new_h = msg.height;

                            for (int w = 0; w < MAX_WINDOWS; w++) {
                                if (g_windows[w].active && g_windows[w].id == msg.window_id) {
                                    if ((msg.width > 0 && msg.width != g_windows[w].width) ||
                                        (msg.height > 0 && msg.height != g_windows[w].height)) {
                                        size_changed = 1;
                                        target_client_fd = g_windows[w].client_fd;
                                    }
                                    g_windows[w].x = msg.x;
                                    g_windows[w].y = msg.y;
                                    if (msg.width > 0) g_windows[w].width = msg.width;
                                    if (msg.height > 0) g_windows[w].height = msg.height;
                                    if (msg.flags > 0) g_windows[w].z_order = (int)msg.flags;
                                    break;
                                }
                            }
                            if (g_compositor_fd >= 0) {
                                syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            }
                            if (g_wm_fd >= 0 && poll_fds[i].fd != g_wm_fd) {
                                syscall(SYS_SEND, g_wm_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            }
                            /* Notify client application if its size changed */
                            if (size_changed && target_client_fd >= 0) {
                                ntfs_msg_t rmsg;
                                memset(&rmsg, 0, sizeof(rmsg));
                                rmsg.type = NTFS_MSG_WINDOW_RESIZED;
                                rmsg.window_id = msg.window_id;
                                rmsg.x = msg.x;
                                rmsg.y = msg.y;
                                rmsg.width = new_w;
                                rmsg.height = new_h;
                                syscall(SYS_SEND, target_client_fd, (uint64_t)(uintptr_t)&rmsg, sizeof(rmsg), MSG_DONTWAIT, 0);
                            }
                            break;
                        }

                        case NTFS_MSG_FOCUS_WINDOW:
                            g_focused_window_id = msg.window_id;
                            if (g_compositor_fd >= 0) {
                                syscall(SYS_SEND, g_compositor_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                            }
                            break;

                        case NTFS_MSG_CLOSE_WINDOW:
                            /* WM ordered close -> forward to window client */
                            for (int w = 0; w < MAX_WINDOWS; w++) {
                                if (g_windows[w].active && g_windows[w].id == msg.window_id) {
                                    syscall(SYS_SEND, g_windows[w].client_fd, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
                                    break;
                                }
                            }
                            break;

                        case NTFS_MSG_FRAME_READY:
                            g_last_buf_idx = (int)(msg.flags & 1);
                            /* Compositor finished canvas -> Blit from compositor backbuffer to /dev/fb0 if TTY1 is active */
                            if (active_tty == 1 && g_compositor_canvas && g_fb_ptr) {
                                int dx = msg.x < 0 ? 0 : msg.x;
                                int dy = msg.y < 0 ? 0 : msg.y;
                                int dw = (dx + (int)msg.width > (int)g_screen_width) ? (int)g_screen_width - dx : (int)msg.width;
                                int dh = (dy + (int)msg.height > (int)g_screen_height) ? (int)g_screen_height - dy : (int)msg.height;

                                if (dw > 0 && dh > 0) {
                                    cursor_hide();
                                    uint32_t *src_canvas = &g_compositor_canvas[g_last_buf_idx * (g_screen_width * g_screen_height)];
                                    for (int r = 0; r < dh; r++) {
                                        uint32_t *src_row = &src_canvas[(dy + r) * g_screen_width + dx];
                                        uint32_t *dst_row = &g_fb_ptr[(dy + r) * g_screen_pitch + dx];
                                        memcpy(dst_row, src_row, dw * sizeof(uint32_t));
                                    }
                                    cursor_show();
                                }
                            }
                            break;

                        default:
                            break;
                    }
                }
            }
        }

        /* 5. Drain Hardware Mouse Queue */
        int mouse_had_event = 0;
        int32_t total_wheel_dz = 0;
        while (syscall(SYS_GET_MOUSE_EVENT, (uint64_t)(uintptr_t)&mouse_ev, 0, 0, 0, 0) == 1) {
            mouse_had_event = 1;
            total_wheel_dz += mouse_ev.dz;
            if (mouse_ev.buttons != prev_mouse_btns || mouse_ev.dz != 0) {
                break;
            }
        }

        if (mouse_had_event) {
            uint32_t mx = (uint32_t)mouse_ev.x;
            uint32_t my = (uint32_t)mouse_ev.y;
            uint8_t btns = mouse_ev.buttons;

            /* Instantly update hardware cursor directly on physical framebuffer */
            cursor_move((int32_t)mx, (int32_t)my);

            if (mx != prev_mouse_x || my != prev_mouse_y || btns != prev_mouse_btns || total_wheel_dz != 0) {
                uint8_t old_btns = prev_mouse_btns;
                prev_mouse_x = mx;
                prev_mouse_y = my;
                prev_mouse_btns = btns;

                uint32_t btn_flags = (uint32_t)btns;
                if (total_wheel_dz > 0) btn_flags |= NTFS_MOUSE_WHEEL_UP;
                else if (total_wheel_dz < 0) btn_flags |= NTFS_MOUSE_WHEEL_DOWN;

                ntfs_msg_t mev;
                memset(&mev, 0, sizeof(mev));
                mev.type = NTFS_MSG_MOUSE_EVENT;
                mev.x = (int32_t)mx;
                mev.y = (int32_t)my;
                mev.buttons_or_key = btn_flags;

                /* Inform WM for window dragging, hit tests, and focus switching */
                if (g_wm_fd >= 0) {
                    syscall(SYS_SEND, g_wm_fd, (uint64_t)(uintptr_t)&mev, sizeof(mev), MSG_DONTWAIT, 0);
                }

                /* Dismiss any frameless popups (context/start menu) if click is pressed outside */
                uint8_t click_pressed = (btns & (NTFS_MOUSE_BTN_LEFT | NTFS_MOUSE_BTN_RIGHT)) &&
                                        !(old_btns & (NTFS_MOUSE_BTN_LEFT | NTFS_MOUSE_BTN_RIGHT));
                if (click_pressed) {
                    for (int w = 0; w < MAX_WINDOWS; w++) {
                        if (g_windows[w].active && (g_windows[w].flags & NTFS_WIN_FRAMELESS)) {
                            int inside = ((int32_t)mx >= g_windows[w].x && (int32_t)mx < g_windows[w].x + (int)g_windows[w].width &&
                                          (int32_t)my >= g_windows[w].y && (int32_t)my < g_windows[w].y + (int)g_windows[w].height);
                            if (!inside) {
                                ntfs_msg_t cmsg;
                                memset(&cmsg, 0, sizeof(cmsg));
                                cmsg.type = NTFS_MSG_CLOSE_WINDOW;
                                cmsg.window_id = g_windows[w].id;
                                syscall(SYS_SEND, g_windows[w].client_fd, (uint64_t)(uintptr_t)&cmsg, sizeof(cmsg), MSG_DONTWAIT, 0);
                            }
                        }
                    }
                }

                /* Find topmost window under mouse that is not background */
                int top_hit = -1;
                int top_z = -999;
                for (int w = 0; w < MAX_WINDOWS; w++) {
                    if (g_windows[w].active && !(g_windows[w].flags & NTFS_WIN_BACKGROUND)) {
                        int wx = g_windows[w].x;
                        int wy = g_windows[w].y;
                        int ww = (int)g_windows[w].width;
                        int wh = (int)g_windows[w].height;
                        if ((int32_t)mx >= wx && (int32_t)mx < wx + ww &&
                            (int32_t)my >= wy && (int32_t)my < wy + wh) {
                            if (g_windows[w].z_order > top_z) {
                                top_z = g_windows[w].z_order;
                                top_hit = w;
                            }
                        }
                    }
                }

                /* If no normal window was hit, route to background surface */
                if (top_hit < 0) {
                    for (int w = 0; w < MAX_WINDOWS; w++) {
                        if (g_windows[w].active && (g_windows[w].flags & NTFS_WIN_BACKGROUND)) {
                            top_hit = w;
                            break;
                        }
                    }
                }

                /* Forward mouse event to topmost window under mouse or focused window */
                for (int w = 0; w < MAX_WINDOWS; w++) {
                    if (g_windows[w].active) {
                        int is_top = (w == top_hit);
                        int is_inside = ((int32_t)mx >= g_windows[w].x && (int32_t)mx < g_windows[w].x + (int)g_windows[w].width &&
                                         (int32_t)my >= g_windows[w].y && (int32_t)my < g_windows[w].y + (int)g_windows[w].height);
                        if (is_top || (g_windows[w].id == g_focused_window_id && is_inside && btns != 0)) {
                            ntfs_msg_t c_mev = mev;
                            c_mev.x = (int32_t)mx - g_windows[w].x;
                            c_mev.y = (int32_t)my - g_windows[w].y;
                            syscall(SYS_SEND, g_windows[w].client_fd, (uint64_t)(uintptr_t)&c_mev, sizeof(c_mev), MSG_DONTWAIT, 0);
                        }
                    }
                }
            }
        } else if (ready == 0) {
            syscall(SYS_YIELD, 0, 0, 0, 0, 0);
        }
    }
}
