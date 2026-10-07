#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
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

/* Screen state */
static uint32_t g_screen_width = 1280;
static uint32_t g_screen_height = 720;
static uint32_t *g_canvas = NULL;
static int g_canvas_shm_id = -1;


/* Window surfaces table */
typedef struct {
    int active;
    uint32_t id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    int shm_id;
    uint32_t *pixels;
    int z_order;
    uint32_t flags;
    char title[32];
    int has_geometry;
} lack_surface_t;

static lack_surface_t g_surfaces[MAX_WINDOWS];
static uint32_t g_focused_id = 0;

static int g_current_buf = 0;
static int g_wireframe_active = 0;
static int32_t g_wireframe_x = 0;
static int32_t g_wireframe_y = 0;
static uint32_t g_wireframe_w = 0;
static uint32_t g_wireframe_h = 0;

static void render_scene(int sock) {
    if (!g_canvas) return;

    /* Render into the backbuffer to avoid screen tearing */
    int back_idx = g_current_buf ^ 1;
    uint32_t screen_pixels = g_screen_width * g_screen_height;
    uint32_t *cur_canvas = &g_canvas[back_idx * screen_pixels];

    /* 1. Sort surfaces by z_order ascending (lowest first, highest last) */
    int order[MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_surfaces[i].active && g_surfaces[i].pixels && g_surfaces[i].has_geometry) {
            order[count++] = i;
        }
    }

    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (g_surfaces[order[j]].z_order > g_surfaces[order[j + 1]].z_order) {
                int temp = order[j];
                order[j] = order[j + 1];
                order[j + 1] = temp;
            }
        }
    }

    /* 2. Clear background only if bottom surface is not fullscreen */
    int has_fullscreen_bg = (count > 0 && g_surfaces[order[0]].x == 0 && g_surfaces[order[0]].y == 0 &&
                             g_surfaces[order[0]].width == g_screen_width && g_surfaces[order[0]].height == g_screen_height);
    if (!has_fullscreen_bg) {
        for (uint32_t i = 0; i < screen_pixels; i++) {
            cur_canvas[i] = COLOR_DESKTOP;
        }
    }

    /* 3. Composite surfaces onto canvas */
    for (int s = 0; s < count; s++) {
        lack_surface_t *surf = &g_surfaces[order[s]];
        int sx = surf->x;
        int sy = surf->y;
        int sw = (int)surf->width;
        int sh = (int)surf->height;

        /* Window frame and titlebar decorations for normal windows */
        if (!(surf->flags & (NTFS_WIN_FRAMELESS | NTFS_WIN_BACKGROUND | NTFS_WIN_TASKBAR | NTFS_WIN_SPLASH))) {
            int fx = sx - BORDER_WIDTH;
            int fy = sy - TITLEBAR_HEIGHT;
            int fw = sw + 2 * BORDER_WIDTH;
            int fh = sh + TITLEBAR_HEIGHT + BORDER_WIDTH;

            gfx_fill_rect(cur_canvas, g_screen_width, g_screen_height, fx, fy, fw, fh, COLOR_WIN_BG);
            gfx_draw_bevel(cur_canvas, g_screen_width, g_screen_height, fx, fy, fw, fh, 0);

            uint32_t title_bg = (surf->id == g_focused_id) ? COLOR_WIN_TITLE : COLOR_WIN_TITLE_INACTIVE;
            gfx_fill_rect(cur_canvas, g_screen_width, g_screen_height, sx, sy - TITLEBAR_HEIGHT + 3, sw, TITLEBAR_HEIGHT - 4, title_bg);
            gfx_draw_string(cur_canvas, g_screen_width, g_screen_height, sx + 5, sy - TITLEBAR_HEIGHT + 5, surf->title, COLOR_TITLE_TEXT, 0, 1);

            int bx = sx + sw - 17;
            int by = sy - TITLEBAR_HEIGHT + 5;
            gfx_fill_rect(cur_canvas, g_screen_width, g_screen_height, bx, by, 14, 14, COLOR_BTN_FACE);
            gfx_draw_bevel(cur_canvas, g_screen_width, g_screen_height, bx, by, 14, 14, 0);
            gfx_draw_char(cur_canvas, g_screen_width, g_screen_height, bx + 3, by - 1, 'X', COLOR_BLACK, 0, 1);

            gfx_draw_bevel(cur_canvas, g_screen_width, g_screen_height, sx - 1, sy - 1, sw + 2, sh + 2, 1);

            /* Classic diagonal sizing grip in bottom-right corner of window border */
            if (!(surf->flags & NTFS_WIN_FIXED_SIZE)) {
                int gx = fx + fw - 4;
                int gy = fy + fh - 4;
                for (int gi = 0; gi < 3; gi++) {
                    int d = gi * 4;
                    gfx_fill_rect(cur_canvas, g_screen_width, g_screen_height, gx - d, gy, 2, 2, COLOR_WHITE);
                    gfx_fill_rect(cur_canvas, g_screen_width, g_screen_height, gx - d - 1, gy - 1, 2, 2, COLOR_BTN_SHADOW);
                    gfx_fill_rect(cur_canvas, g_screen_width, g_screen_height, gx, gy - d, 2, 2, COLOR_WHITE);
                    gfx_fill_rect(cur_canvas, g_screen_width, g_screen_height, gx - 1, gy - d - 1, 2, 2, COLOR_BTN_SHADOW);
                }
            }
        }

        int x0 = (sx < 0) ? 0 : sx;
        int y0 = (sy < 0) ? 0 : sy;
        int x1 = (sx + sw > (int)g_screen_width) ? (int)g_screen_width : sx + sw;
        int y1 = (sy + sh > (int)g_screen_height) ? (int)g_screen_height : sy + sh;

        int copy_w = x1 - x0;
        if (copy_w > 0) {
            for (int y = y0; y < y1; y++) {
                int src_y = y - sy;
                uint32_t *src_row = &surf->pixels[src_y * surf->width + (x0 - sx)];
                uint32_t *dst_row = &cur_canvas[y * g_screen_width + x0];
                __builtin_memcpy(dst_row, src_row, copy_w * sizeof(uint32_t));
            }
        }
    }

    /* 3b. Draw classic Windows 95 dashed ghost wireframe resize box */
    if (g_wireframe_active && g_wireframe_w > 0 && g_wireframe_h > 0) {
        int wx = g_wireframe_x;
        int wy = g_wireframe_y;
        int ww = (int)g_wireframe_w;
        int wh = (int)g_wireframe_h;

        for (int x = wx; x < wx + ww; x++) {
            if (x < 0 || x >= (int)g_screen_width) continue;
            if (wy >= 0 && wy < (int)g_screen_height && ((x + wy) & 2)) {
                cur_canvas[wy * g_screen_width + x] = COLOR_BLACK;
            }
            if (wy + 1 >= 0 && wy + 1 < (int)g_screen_height && ((x + wy + 1) & 2)) {
                cur_canvas[(wy + 1) * g_screen_width + x] = COLOR_WHITE;
            }
            if (wy + wh - 1 >= 0 && wy + wh - 1 < (int)g_screen_height && ((x + wy + wh - 1) & 2)) {
                cur_canvas[(wy + wh - 1) * g_screen_width + x] = COLOR_BLACK;
            }
            if (wy + wh - 2 >= 0 && wy + wh - 2 < (int)g_screen_height && ((x + wy + wh - 2) & 2)) {
                cur_canvas[(wy + wh - 2) * g_screen_width + x] = COLOR_WHITE;
            }
        }

        for (int y = wy; y < wy + wh; y++) {
            if (y < 0 || y >= (int)g_screen_height) continue;
            if (wx >= 0 && wx < (int)g_screen_width && ((wx + y) & 2)) {
                cur_canvas[y * g_screen_width + wx] = COLOR_BLACK;
            }
            if (wx + 1 >= 0 && wx + 1 < (int)g_screen_width && ((wx + 1 + y) & 2)) {
                cur_canvas[y * g_screen_width + wx + 1] = COLOR_WHITE;
            }
            if (wx + ww - 1 >= 0 && wx + ww - 1 < (int)g_screen_width && ((wx + ww - 1 + y) & 2)) {
                cur_canvas[y * g_screen_width + wx + ww - 1] = COLOR_BLACK;
            }
            if (wx + ww - 2 >= 0 && wx + ww - 2 < (int)g_screen_width && ((wx + ww - 2 + y) & 2)) {
                cur_canvas[y * g_screen_width + wx + ww - 2] = COLOR_WHITE;
            }
        }
    }

    /* Flip buffer index */
    g_current_buf = back_idx;

    /* 4. Notify display server that entire frame is ready, indicating which buffer index in flags */
    ntfs_msg_t ready_msg;
    memset(&ready_msg, 0, sizeof(ready_msg));
    ready_msg.type = NTFS_MSG_FRAME_READY;
    ready_msg.x = 0;
    ready_msg.y = 0;
    ready_msg.width = g_screen_width;
    ready_msg.height = g_screen_height;
    ready_msg.flags = (uint32_t)g_current_buf;
    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&ready_msg, sizeof(ready_msg), 0, 0);
}

void lack_main(void) {
    puts("[lack] LAckluster Compositor Kit starting up...\n");
    memset(g_surfaces, 0, sizeof(g_surfaces));

    /* 1. Connect to display server */
    int sock = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (sock < 0) {
        puts("[lack] ERROR: unable to create socket\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    sockaddr_un_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, NTFS_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* Retry connect up to 20 times (2 seconds) while server initializes */
    int connected = 0;
    for (int retry = 0; retry < 20; retry++) {
        if (syscall(SYS_CONNECT, sock, (uint64_t)(uintptr_t)&addr, sizeof(addr), 0, 0) == 0) {
            connected = 1;
            break;
        }
        syscall(SYS_SLEEP, 100, 0, 0, 0, 0);
    }

    if (!connected) {
        puts("[lack] ERROR: could not connect to /tmp/ntfs.sock\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    /* 2. Query screen resolution */
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

    /* 3. Allocate shared composition canvas (double buffered) */
    size_t canvas_bytes = (size_t)g_screen_width * (size_t)g_screen_height * 4;
    g_canvas_shm_id = (int)syscall(SYS_SHM_CREATE, (uint64_t)(uintptr_t)"shm:lack_canvas", canvas_bytes * 2, 0, 0, 0);
    if (g_canvas_shm_id < 0) {
        puts("[lack] ERROR: could not allocate shm:lack_canvas\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    g_canvas = (uint32_t *)(uintptr_t)syscall(SYS_SHM_MAP, g_canvas_shm_id, 0, SHM_READ | SHM_WRITE, 0, 0);
    if (!g_canvas) {
        puts("[lack] ERROR: could not map canvas shared memory\n");
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    /* 4. Register role as compositor */
    ntfs_msg_t reg_msg;
    memset(&reg_msg, 0, sizeof(reg_msg));
    reg_msg.type = NTFS_MSG_REGISTER_ROLE;
    reg_msg.flags = NTFS_ROLE_COMPOSITOR;
    reg_msg.shm_id = (uint32_t)g_canvas_shm_id;
    syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&reg_msg, sizeof(reg_msg), 0, 0);

    puts("[lack] compositor initialized and connected\n");

    /* Initial frame */
    render_scene(sock);

    /* 5. Main event processing loop */
    pollfd_t pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;

    int needs_redraw = 0;

    while (1) {
        pfd.revents = 0;
        int ready = (int)syscall(SYS_POLL, (uint64_t)(uintptr_t)&pfd, 1, 1, 0, 0);

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
                            if (!g_surfaces[i].active) {
                                slot = i;
                                break;
                            }
                        }
                        if (slot >= 0) {
                            g_surfaces[slot].active = 1;
                            g_surfaces[slot].id = msg.window_id;
                            g_surfaces[slot].x = msg.x;
                            g_surfaces[slot].y = msg.y;
                            g_surfaces[slot].width = msg.width;
                            g_surfaces[slot].height = msg.height;
                            g_surfaces[slot].shm_id = (int)msg.shm_id;
                            g_surfaces[slot].z_order = 0;
                            g_surfaces[slot].flags = msg.flags;
                            if (msg.flags & (NTFS_WIN_BACKGROUND | NTFS_WIN_TASKBAR | NTFS_WIN_SPLASH)) {
                                g_surfaces[slot].has_geometry = 1;
                                if (msg.flags & NTFS_WIN_SPLASH) {
                                    g_surfaces[slot].z_order = 5000;
                                } else if (msg.flags & NTFS_WIN_TASKBAR) {
                                    g_surfaces[slot].x = 0;
                                    g_surfaces[slot].y = (int)g_screen_height - (int)TASKBAR_HEIGHT;
                                    g_surfaces[slot].z_order = 1000;
                                }
                            } else {
                                g_surfaces[slot].has_geometry = 0;
                            }
                            strncpy(g_surfaces[slot].title, msg.title, sizeof(g_surfaces[slot].title) - 1);
                            if (msg.shm_id > 0) {
                                g_surfaces[slot].pixels = (uint32_t *)(uintptr_t)syscall(SYS_SHM_MAP, msg.shm_id, 0, SHM_READ, 0, 0);
                            }
                            needs_redraw = 1;
                        }
                        break;
                    }

                    case NTFS_MSG_DESTROY_WINDOW:
                    case NTFS_MSG_WINDOW_DESTROYED: {
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (g_surfaces[i].active && g_surfaces[i].id == msg.window_id) {
                                if (g_surfaces[i].pixels) {
                                    syscall(SYS_SHM_UNMAP, (uint64_t)(uintptr_t)g_surfaces[i].pixels, 0, 0, 0, 0);
                                }
                                if (g_surfaces[i].shm_id > 0) {
                                    syscall(SYS_SHM_CLOSE, g_surfaces[i].shm_id, 0, 0, 0, 0);
                                }
                                g_surfaces[i].active = 0;
                                g_surfaces[i].has_geometry = 0;
                                g_surfaces[i].pixels = NULL;
                                g_surfaces[i].shm_id = -1;
                                needs_redraw = 1;
                                break;
                            }
                        }
                        break;
                    }

                    case NTFS_MSG_SET_GEOMETRY: {
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (g_surfaces[i].active && g_surfaces[i].id == msg.window_id) {
                                g_surfaces[i].x = msg.x;
                                g_surfaces[i].y = msg.y;
                                if (msg.width > 0) g_surfaces[i].width = msg.width;
                                if (msg.height > 0) g_surfaces[i].height = msg.height;
                                g_surfaces[i].z_order = (int)msg.flags;
                                g_surfaces[i].has_geometry = 1;
                                needs_redraw = 1;
                                break;
                            }
                        }
                        break;
                    }

                    case NTFS_MSG_FOCUS_WINDOW: {
                        g_focused_id = msg.window_id;
                        needs_redraw = 1;
                        break;
                    }

                    case NTFS_MSG_DAMAGE: {
                        needs_redraw = 1;
                        break;
                    }

                    case NTFS_MSG_SET_WIREFRAME: {
                        if (msg.width > 0 && msg.height > 0) {
                            g_wireframe_active = 1;
                            g_wireframe_x = msg.x;
                            g_wireframe_y = msg.y;
                            g_wireframe_w = msg.width;
                            g_wireframe_h = msg.height;
                        } else {
                            g_wireframe_active = 0;
                        }
                        needs_redraw = 1;
                        break;
                    }

                    case NTFS_MSG_MOUSE_EVENT:
                        /* Hardware cursor is rendered directly by display server */
                        break;

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

        if (needs_redraw) {
            render_scene(sock);
            needs_redraw = 0;
        } else if (ready == 0) {
            syscall(SYS_YIELD, 0, 0, 0, 0, 0);
        }
    }
}
