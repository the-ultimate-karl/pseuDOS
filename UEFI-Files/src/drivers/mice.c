#include "mice.h"
#include "io.h"
#include "lib.h"
#include "klog.h"
#include "pit.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define MOUSE_QUEUE_SIZE 128

static volatile mouse_event_t g_event_queue[MOUSE_QUEUE_SIZE];
static volatile uint8_t g_queue_head = 0;
static volatile uint8_t g_queue_tail = 0;

static volatile int32_t g_mouse_x = 640;
static volatile int32_t g_mouse_y = 360;
static volatile uint8_t g_buttons = 0;
static volatile int8_t  g_scroll_wheel = 0;

static uint32_t g_max_x = 1279;
static uint32_t g_max_y = 719;

static int g_has_wheel = 0;
static int g_packet_size = 3;
static uint8_t g_packet[4];
static int g_packet_idx = 0;
static uint64_t g_last_byte_tick = 0;
static int g_mice_initialized = 0;

static int mice_wait_write(void) {
    int timeout = 100000;
    while ((inb(PS2_STATUS_PORT) & 0x02) && --timeout > 0) {
        __asm__ volatile ("pause");
    }
    return timeout > 0;
}

static int mice_wait_read(void) {
    int timeout = 100000;
    while (!(inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        __asm__ volatile ("pause");
    }
    return timeout > 0;
}

static void mice_write_cmd(uint8_t cmd) {
    mice_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

static void mice_write_data(uint8_t data) {
    mice_wait_write();
    outb(PS2_DATA_PORT, data);
}

static uint8_t mice_read_data(void) {
    if (mice_wait_read()) {
        return inb(PS2_DATA_PORT);
    }
    return 0;
}

static uint8_t mice_send_to_mouse(uint8_t val) {
    mice_write_cmd(0xD4);
    mice_write_data(val);
    return mice_read_data();
}

void mice_set_bounds(uint32_t max_x, uint32_t max_y) {
    if (max_x > 0) g_max_x = max_x - 1;
    if (max_y > 0) g_max_y = max_y - 1;
    if (g_mouse_x > (int32_t)g_max_x) g_mouse_x = (int32_t)g_max_x;
    if (g_mouse_y > (int32_t)g_max_y) g_mouse_y = (int32_t)g_max_y;
}

void mice_init(const FramebufferInfo *fb) {
    if (fb && fb->width > 0 && fb->height > 0) {
        g_max_x = fb->width - 1;
        g_max_y = fb->height - 1;
        g_mouse_x = (int32_t)(fb->width / 2);
        g_mouse_y = (int32_t)(fb->height / 2);
    }

    g_packet_idx = 0;
    g_queue_head = 0;
    g_queue_tail = 0;

    /* 1. Enable Auxiliary Device */
    mice_write_cmd(0xA8);

    /* 2. Configure Controller Command Byte:
     *    Bit 0: Enable IRQ 1 (keyboard)
     *    Bit 1: Enable IRQ 12 (mouse)
     *    Bit 4: Disable keyboard clock inhibit (0 = clock on)
     *    Bit 5: Disable mouse clock inhibit (0 = clock on)
     *    Bit 6: Translation enable (Scan Code Set 1 translation)
     */
    mice_write_cmd(0x20);
    uint8_t config = mice_read_data();
    if (config == 0) {
        config = 0x47;
    }
    config |= 0x43;  /* Bits 0, 1, 6 */
    config &= ~0x30; /* Clear bits 4 and 5 */
    mice_write_cmd(0x60);
    mice_write_data(config);

    /* Flush output buffer */
    int timeout = 1000;
    while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        inb(PS2_DATA_PORT);
    }

    /* 3. Reset Mouse */
    mice_send_to_mouse(0xFF);
    mice_read_data(); /* 0xAA */
    mice_read_data(); /* 0x00 */

    /* 4. IntelliMouse sequence */
    mice_send_to_mouse(0xF3); mice_send_to_mouse(200);
    mice_send_to_mouse(0xF3); mice_send_to_mouse(100);
    mice_send_to_mouse(0xF3); mice_send_to_mouse(80);

    mice_send_to_mouse(0xF2);
    uint8_t mouse_id = mice_read_data();
    if (mouse_id == 3 || mouse_id == 4) {
        g_has_wheel = 1;
        g_packet_size = 4;
        klog_info("PS/2 IntelliMouse detected (ID %u, 4-byte wheel packets armed)", mouse_id);
    } else {
        g_has_wheel = 0;
        g_packet_size = 3;
        klog_info("Standard PS/2 mouse detected (ID %u, 3-byte packets armed)", mouse_id);
    }

    /* 5. Set defaults and enable reporting */
    mice_send_to_mouse(0xF6);
    mice_send_to_mouse(0xF4);

    /* Flush any leftover bytes in controller buffer */
    timeout = 1000;
    while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        inb(PS2_DATA_PORT);
    }

    g_mice_initialized = 1;
    klog_info("Mice subsystem armed (bounds: %ux%u, initial pos: %d,%d)", g_max_x + 1, g_max_y + 1, g_mouse_x, g_mouse_y);
}

void mice_handle_irq(void) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & 0x01)) {
        return;
    }
    if (!(status & 0x20)) {
        /* Not mouse data (keyboard) */
        return;
    }

    uint64_t now = pit_get_ticks();
    if (g_packet_idx > 0 && (now - g_last_byte_tick > 20)) {
        /* Desync recovery: discard stale incomplete packet after 200ms idle */
        g_packet_idx = 0;
    }
    g_last_byte_tick = now;

    uint8_t byte = inb(PS2_DATA_PORT);

    /* Packet synchronization: byte 0 must have bit 3 set */
    if (g_packet_idx == 0 && !(byte & 0x08)) {
        return;
    }

    g_packet[g_packet_idx++] = byte;

    if (g_packet_idx >= g_packet_size) {
        g_packet_idx = 0;

        uint8_t flags = g_packet[0];
        int32_t dx = (int32_t)g_packet[1];
        int32_t dy = (int32_t)g_packet[2];
        int32_t dz = 0;

        if (flags & 0x10) dx |= ~0xFF;
        if (flags & 0x20) dy |= ~0xFF;

        if (flags & 0xC0) {
            return;
        }

        if (g_has_wheel) {
            int8_t wheel_val = (int8_t)(g_packet[3] & 0x0F);
            if (wheel_val & 0x08) wheel_val |= ~0x0F;
            dz = (int32_t)wheel_val;
            g_scroll_wheel += wheel_val;
        }

        uint8_t btns = flags & 0x07;
        uint8_t prev_btns = g_buttons;
        g_buttons = btns;

        g_mouse_x += dx;
        g_mouse_y -= dy;

        if (g_mouse_x < 0) g_mouse_x = 0;
        if (g_mouse_x > (int32_t)g_max_x) g_mouse_x = (int32_t)g_max_x;
        if (g_mouse_y < 0) g_mouse_y = 0;
        if (g_mouse_y > (int32_t)g_max_y) g_mouse_y = (int32_t)g_max_y;

        mouse_event_t ev;
        ev.x = g_mouse_x;
        ev.y = g_mouse_y;
        ev.dx = dx;
        ev.dy = dy;
        ev.dz = dz;
        ev.buttons = btns;
        ev.reserved = 0;

        if (btns != prev_btns) {
            ev.event_type = MOUSE_EVENT_BUTTON;
        } else if (dz != 0) {
            ev.event_type = MOUSE_EVENT_WHEEL;
        } else {
            ev.event_type = MOUSE_EVENT_MOVE;
        }

        uint8_t next_head = (g_queue_head + 1) % MOUSE_QUEUE_SIZE;
        if (next_head != g_queue_tail) {
            g_event_queue[g_queue_head] = ev;
            g_queue_head = next_head;
        }
    }
}

int mice_get_event(mouse_event_t *ev) {
    if (!ev) return 0;
    if (g_queue_head == g_queue_tail) {
        return 0;
    }
    *ev = g_event_queue[g_queue_tail];
    g_queue_tail = (g_queue_tail + 1) % MOUSE_QUEUE_SIZE;
    return 1;
}

void mice_get_state(mouse_state_t *state) {
    if (!state) return;
    state->x = g_mouse_x;
    state->y = g_mouse_y;
    state->buttons = g_buttons;
    state->scroll_wheel = g_scroll_wheel;
    state->max_x = g_max_x;
    state->max_y = g_max_y;
    state->has_wheel = g_has_wheel;
}
