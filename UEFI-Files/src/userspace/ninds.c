#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
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

static char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

typedef struct {
    int active;
    uint32_t id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    int z_order;
} ninds_window_t;

static ninds_window_t g_windows[MAX_WINDOWS];
static uint32_t g_screen_width = 1280;
static uint32_t g_screen_height = 720;
static int g_cascade_idx = 0;
static int g_top_z = 10;

static uint32_t g_drag_win_id = 0;
static int32_t g_drag_offset_x = 0;
static int32_t g_drag_offset_y = 0;
static uint8_t g_prev_buttons = 0;

#define RESIZE_NONE   0
#define RESIZE_RIGHT  1
#define RESIZE_BOTTOM 2
#define RESIZE_LEFT   4
#define RESIZE_TOP    8

static uint32_t g_resize_win_id = 0;
static int      g_resize_edges = 0;
static int32_t  g_resize_start_x = 0;
static int32_t  g_resize_start_y = 0;
static uint32_t g_resize_start_w = 0;
static uint32_t g_resize_start_h = 0;
static int32_t  g_resize_start_mx = 0;
static int32_t  g_resize_start_my = 0;
static int32_t  g_wireframe_x = 0;
static int32_t  g_wireframe_y = 0;
static uint32_t g_wireframe_w = 0;
static uint32_t g_wireframe_h = 0;

static void send_set_geometry(int sock, ninds_window_t *win) {
    ntfs_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = NTFS_MSG_SET_GEOMETRY;
    msg.window_id = win->id;
    msg.x = win->x;
    msg.y = win->y;
    msg.width = win->width;
    msg.height = win->height;
    msg.flags = (uint32_t)win->z_order;
    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
}

static void send_focus(int sock, uint32_t wid) {
    ntfs_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = NTFS_MSG_FOCUS_WINDOW;
    msg.window_id = wid;
    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&msg, sizeof(msg), 0, 0);
}

static void bring_to_front(int sock, ninds_window_t *win) {
    if (!win) return;
    g_top_z++;
    win->z_order = g_top_z;
    send_set_geometry(sock, win);
    send_focus(sock, win->id);
}

void ninds_main(void) {
    puts("[ninds] NINDS Is Not a Display Server (WM) starting up...\n");
    memset(g_windows, 0, sizeof(g_windows));

    /* 1. Connect to display server */
    int sock = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (sock < 0) {
        puts("[ninds] ERROR: socket creation failed\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    sockaddr_un_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, NTFS_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int connected = 0;
    for (int retry = 0; retry < 20; retry++) {
        if (syscall(SYS_CONNECT, sock, (uint64_t)(uintptr_t)&addr, sizeof(addr), 0, 0) == 0) {
            connected = 1;
            break;
        }
        syscall(SYS_SLEEP, 100, 0, 0, 0, 0);
    }

    if (!connected) {
        puts("[ninds] ERROR: failed to connect to /tmp/ntfs.sock\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    /* 2. Register role as Window Manager */
    ntfs_msg_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.type = NTFS_MSG_REGISTER_ROLE;
    reg.flags = NTFS_ROLE_WM;
    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&reg, sizeof(reg), 0, 0);

    /* 3. Query screen dimensions */
    ntfs_msg_t qmsg;
    memset(&qmsg, 0, sizeof(qmsg));
    qmsg.type = NTFS_MSG_GET_SCREEN_INFO;
    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&qmsg, sizeof(qmsg), 0, 0);

    ntfs_msg_t rmsg;
    if (syscall(SYS_RECV, sock, (uint64_t)(uintptr_t)&rmsg, sizeof(rmsg), 0, 0) == (int64_t)sizeof(rmsg)) {
        if (rmsg.width > 0 && rmsg.height > 0) {
            g_screen_width = rmsg.width;
            g_screen_height = rmsg.height;
        }
    }

    puts("[ninds] window manager registered and ready\n");

    /* 4. Event loop */
    pollfd_t pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;

    while (1) {
        pfd.revents = 0;
        int ready = (int)syscall(SYS_POLL, (uint64_t)(uintptr_t)&pfd, 1, 20, 0, 0);

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            syscall(SYS_EXIT, 0, 0, 0, 0, 0);
            return;
        }

        if (ready > 0 && (pfd.revents & POLLIN)) {
            ntfs_msg_t msg;
            while (1) {
                int64_t r = syscall(SYS_RECV, sock, (uint64_t)(uintptr_t)&msg, sizeof(msg), MSG_DONTWAIT, 0);
                if (r == (int64_t)sizeof(msg)) {
                    switch (msg.type) {
                    case NTFS_MSG_CREATE_WINDOW: {
                        int slot = -1;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (!g_windows[i].active) {
                                slot = i;
                                break;
                            }
                        }
                        if (slot >= 0) {
                            g_windows[slot].active = 1;
                            g_windows[slot].id = msg.window_id;
                            g_windows[slot].width = msg.width;
                            g_windows[slot].height = msg.height;
                            g_windows[slot].flags = msg.flags;

                            if (msg.flags & NTFS_WIN_SPLASH) {
                                g_windows[slot].x = 0;
                                g_windows[slot].y = 0;
                                g_windows[slot].z_order = 5000;
                            } else if (msg.flags & NTFS_WIN_BACKGROUND) {
                                g_windows[slot].x = 0;
                                g_windows[slot].y = 0;
                                g_windows[slot].z_order = -100;
                            } else if (msg.flags & NTFS_WIN_TASKBAR) {
                                g_windows[slot].x = 0;
                                g_windows[slot].y = (int32_t)g_screen_height - (int32_t)TASKBAR_HEIGHT;
                                g_windows[slot].z_order = 1000;
                                send_set_geometry(sock, &g_windows[slot]);
                            } else if (msg.flags & NTFS_WIN_FRAMELESS) {
                                g_windows[slot].x = msg.x;
                                g_windows[slot].y = msg.y;
                                g_windows[slot].z_order = 2000;
                            } else {
                                /* Cascade normal windows */
                                int max_x = (int)g_screen_width - (int)msg.width - 40;
                                int max_y = (int)g_screen_height - (int)msg.height - 60;
                                if (max_x < 60) max_x = 60;
                                if (max_y < 50) max_y = 50;

                                g_windows[slot].x = 50 + (g_cascade_idx * 30) % max_x;
                                g_windows[slot].y = 40 + (g_cascade_idx * 25) % max_y;
                                g_cascade_idx++;

                                g_top_z++;
                                g_windows[slot].z_order = g_top_z;
                                send_set_geometry(sock, &g_windows[slot]);
                            }

                            if (!(msg.flags & (NTFS_WIN_BACKGROUND | NTFS_WIN_TASKBAR | NTFS_WIN_SPLASH))) {
                                send_focus(sock, g_windows[slot].id);
                            }
                        } else {
                            puts("[ninds] ERROR: window table full (MAX_WINDOWS reached)\n");
                        }
                        break;
                    }

                    case NTFS_MSG_DESTROY_WINDOW:
                    case NTFS_MSG_WINDOW_DESTROYED: {
                        int was_focused = 0;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (g_windows[i].active && g_windows[i].id == msg.window_id) {
                                g_windows[i].active = 0;
                                was_focused = 1;
                                break;
                            }
                        }
                        if (g_drag_win_id == msg.window_id) {
                            g_drag_win_id = 0;
                        }

                        if (was_focused) {
                            /* Find window with highest z-order to focus */
                            int best_slot = -1;
                            int best_z = -999;
                            for (int i = 0; i < MAX_WINDOWS; i++) {
                                if (g_windows[i].active && !(g_windows[i].flags & (NTFS_WIN_BACKGROUND | NTFS_WIN_TASKBAR))) {
                                    if (g_windows[i].z_order > best_z) {
                                        best_z = g_windows[i].z_order;
                                        best_slot = i;
                                    }
                                }
                            }
                            if (best_slot >= 0) {
                                send_focus(sock, g_windows[best_slot].id);
                            }
                        }
                        break;
                    }

                    case NTFS_MSG_SET_GEOMETRY: {
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (g_windows[i].active && g_windows[i].id == msg.window_id) {
                                g_windows[i].x = msg.x;
                                g_windows[i].y = msg.y;
                                if (msg.width > 0) g_windows[i].width = msg.width;
                                if (msg.height > 0) g_windows[i].height = msg.height;
                                if (msg.flags > 0) g_windows[i].z_order = (int)msg.flags;
                                break;
                            }
                        }
                        break;
                    }

                    case NTFS_MSG_MOUSE_EVENT: {
                        int32_t mx = msg.x;
                        int32_t my = msg.y;
                        uint8_t btns = (uint8_t)msg.buttons_or_key;
                        uint8_t left_down = (btns & NTFS_MOUSE_BTN_LEFT);
                        uint8_t left_pressed = (left_down && !(g_prev_buttons & NTFS_MOUSE_BTN_LEFT));
                        uint8_t left_released = (!left_down && (g_prev_buttons & NTFS_MOUSE_BTN_LEFT));

                        if (left_pressed) {
                            /* Find topmost window under mouse */
                            int hit_slot = -1;
                            int hit_z = -999;
                            int hit_edge = RESIZE_NONE;

                            for (int i = 0; i < MAX_WINDOWS; i++) {
                                if (g_windows[i].active && !(g_windows[i].flags & (NTFS_WIN_BACKGROUND | NTFS_WIN_TASKBAR | NTFS_WIN_SPLASH))) {
                                    int wx = g_windows[i].x;
                                    int wy = g_windows[i].y;
                                    int ww = (int)g_windows[i].width;
                                    int wh = (int)g_windows[i].height;
                                    int fx = wx - BORDER_WIDTH;
                                    int fy = wy - TITLEBAR_HEIGHT;
                                    int fw = ww + 2 * BORDER_WIDTH;
                                    int fh = wh + TITLEBAR_HEIGHT + BORDER_WIDTH;

                                    /* Check entire window including borders with 3px grab tolerance */
                                    if (mx >= fx - 3 && mx <= fx + fw + 3 && my >= fy - 3 && my <= fy + fh + 3) {
                                        if (g_windows[i].z_order > hit_z) {
                                            hit_z = g_windows[i].z_order;
                                            hit_slot = i;

                                            hit_edge = RESIZE_NONE;
                                            if (!(g_windows[i].flags & NTFS_WIN_FIXED_SIZE)) {
                                                /* Bottom-right corner (16x16 corner zone) */
                                                if (mx >= wx + ww - 14 && my >= wy + wh - 14) {
                                                    hit_edge = RESIZE_RIGHT | RESIZE_BOTTOM;
                                                } else if (mx >= wx + ww - 2) {
                                                    hit_edge = RESIZE_RIGHT;
                                                } else if (my >= wy + wh - 2) {
                                                    hit_edge = RESIZE_BOTTOM;
                                                } else if (mx <= wx + 2) {
                                                    hit_edge = RESIZE_LEFT;
                                                } else if (my <= fy + 4) {
                                                    hit_edge = RESIZE_TOP;
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (hit_slot >= 0) {
                                ninds_window_t *target = &g_windows[hit_slot];
                                bring_to_front(sock, target);

                                /* Check close button [X] */
                                int bx = target->x + (int)target->width - 17;
                                int by = target->y - TITLEBAR_HEIGHT + 5;
                                if (mx >= bx && mx < bx + 14 && my >= by && my < by + 14) {
                                    ntfs_msg_t cmsg;
                                    memset(&cmsg, 0, sizeof(cmsg));
                                    cmsg.type = NTFS_MSG_CLOSE_WINDOW;
                                    cmsg.window_id = target->id;
                                    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&cmsg, sizeof(cmsg), 0, 0);
                                } else if (hit_edge != RESIZE_NONE) {
                                    /* Initiate resize drag */
                                    g_resize_win_id = target->id;
                                    g_resize_edges = hit_edge;
                                    g_resize_start_x = target->x;
                                    g_resize_start_y = target->y;
                                    g_resize_start_w = target->width;
                                    g_resize_start_h = target->height;
                                    g_resize_start_mx = mx;
                                    g_resize_start_my = my;
                                    g_wireframe_x = target->x - BORDER_WIDTH;
                                    g_wireframe_y = target->y - TITLEBAR_HEIGHT;
                                    g_wireframe_w = target->width + 2 * BORDER_WIDTH;
                                    g_wireframe_h = target->height + TITLEBAR_HEIGHT + BORDER_WIDTH;

                                    ntfs_msg_t wf_msg;
                                    memset(&wf_msg, 0, sizeof(wf_msg));
                                    wf_msg.type = NTFS_MSG_SET_WIREFRAME;
                                    wf_msg.x = g_wireframe_x;
                                    wf_msg.y = g_wireframe_y;
                                    wf_msg.width = g_wireframe_w;
                                    wf_msg.height = g_wireframe_h;
                                    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&wf_msg, sizeof(wf_msg), 0, 0);
                                } else if (my < target->y) {
                                    /* Clicked on titlebar -> initiate drag */
                                    g_drag_win_id = target->id;
                                    g_drag_offset_x = mx - target->x;
                                    g_drag_offset_y = my - target->y;
                                }
                            }
                        }

                        if (left_down && g_resize_win_id > 0) {
                            int dx = mx - g_resize_start_mx;
                            int dy = my - g_resize_start_my;
                            int32_t new_x = g_resize_start_x;
                            int32_t new_y = g_resize_start_y;
                            int32_t new_w = (int32_t)g_resize_start_w;
                            int32_t new_h = (int32_t)g_resize_start_h;

                            if (g_resize_edges & RESIZE_RIGHT) {
                                new_w += dx;
                            }
                            if (g_resize_edges & RESIZE_BOTTOM) {
                                new_h += dy;
                            }
                            if (g_resize_edges & RESIZE_LEFT) {
                                new_w -= dx;
                                new_x += dx;
                            }
                            if (g_resize_edges & RESIZE_TOP) {
                                new_h -= dy;
                                new_y += dy;
                            }

                            if (new_w < 200) {
                                if (g_resize_edges & RESIZE_LEFT) new_x -= (200 - new_w);
                                new_w = 200;
                            }
                            if (new_h < 120) {
                                if (g_resize_edges & RESIZE_TOP) new_y -= (120 - new_h);
                                new_h = 120;
                            }

                            if (new_x < BORDER_WIDTH) new_x = BORDER_WIDTH;
                            if (new_y < TITLEBAR_HEIGHT) new_y = TITLEBAR_HEIGHT;

                            g_wireframe_x = new_x - BORDER_WIDTH;
                            g_wireframe_y = new_y - TITLEBAR_HEIGHT;
                            g_wireframe_w = (uint32_t)(new_w + 2 * BORDER_WIDTH);
                            g_wireframe_h = (uint32_t)(new_h + TITLEBAR_HEIGHT + BORDER_WIDTH);

                            ntfs_msg_t wf_msg;
                            memset(&wf_msg, 0, sizeof(wf_msg));
                            wf_msg.type = NTFS_MSG_SET_WIREFRAME;
                            wf_msg.x = g_wireframe_x;
                            wf_msg.y = g_wireframe_y;
                            wf_msg.width = g_wireframe_w;
                            wf_msg.height = g_wireframe_h;
                            syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&wf_msg, sizeof(wf_msg), 0, 0);
                        } else if (left_down && g_drag_win_id > 0) {
                            for (int i = 0; i < MAX_WINDOWS; i++) {
                                if (g_windows[i].active && g_windows[i].id == g_drag_win_id) {
                                    int32_t nx = mx - g_drag_offset_x;
                                    int32_t ny = my - g_drag_offset_y;
                                    if (ny < TITLEBAR_HEIGHT) ny = TITLEBAR_HEIGHT;
                                    if (ny > (int32_t)g_screen_height - 30) ny = (int32_t)g_screen_height - 30;

                                    g_windows[i].x = nx;
                                    g_windows[i].y = ny;
                                    send_set_geometry(sock, &g_windows[i]);
                                    break;
                                }
                            }
                        }

                        if (left_released) {
                            if (g_resize_win_id > 0) {
                                /* Clear wireframe on compositor */
                                ntfs_msg_t wf_msg;
                                memset(&wf_msg, 0, sizeof(wf_msg));
                                wf_msg.type = NTFS_MSG_SET_WIREFRAME;
                                wf_msg.width = 0;
                                wf_msg.height = 0;
                                syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&wf_msg, sizeof(wf_msg), 0, 0);

                                /* Commit new window dimensions */
                                for (int i = 0; i < MAX_WINDOWS; i++) {
                                    if (g_windows[i].active && g_windows[i].id == g_resize_win_id) {
                                        int32_t committed_w = (int32_t)g_wireframe_w - 2 * BORDER_WIDTH;
                                        int32_t committed_h = (int32_t)g_wireframe_h - TITLEBAR_HEIGHT - BORDER_WIDTH;
                                        int32_t committed_x = g_wireframe_x + BORDER_WIDTH;
                                        int32_t committed_y = g_wireframe_y + TITLEBAR_HEIGHT;

                                        if (committed_w >= 200 && committed_h >= 120) {
                                            g_windows[i].x = committed_x;
                                            g_windows[i].y = committed_y;
                                            g_windows[i].width = (uint32_t)committed_w;
                                            g_windows[i].height = (uint32_t)committed_h;
                                            send_set_geometry(sock, &g_windows[i]);
                                        }
                                        break;
                                    }
                                }
                                g_resize_win_id = 0;
                            }
                            g_drag_win_id = 0;
                        }

                        g_prev_buttons = btns;
                        break;
                    }

                    default:
                        break;
                }
            } else if (r == 0 || (r < 0 && r != -EAGAIN)) {
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                return;
            } else {
                break;
            }
        }
    }
    }
}
