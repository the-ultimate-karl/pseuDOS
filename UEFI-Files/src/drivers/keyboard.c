#include "drivers.h"
#include "io.h"
#include "lib.h"
#include "scheduler.h"
#include "mice.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64

#define SERIAL_PORT      0x3F8
#define SERIAL_LSR       (SERIAL_PORT + 5)
#define SERIAL_DATA      SERIAL_PORT

static volatile uint8_t g_key_queue[256];
static volatile uint8_t g_queue_head = 0;
static volatile uint8_t g_queue_tail = 0;

static int g_shift_pressed = 0;
static int g_caps_lock = 0;
static int g_alt_pressed = 0;
static int g_ctrl_pressed = 0;

static const char g_scancode_table_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char g_scancode_table_shifted[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

void keyboard_post_scancode(uint8_t scancode) {
    uint8_t next_head = (g_queue_head + 1) & 0xFF;
    if (next_head != g_queue_tail) {
        g_key_queue[g_queue_head] = scancode;
        g_queue_head = next_head;
    }
}

void keyboard_isr_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (status & 0x01) {
        if (status & 0x20) {
            mice_handle_irq();
            return;
        }
        uint8_t scancode = inb(PS2_DATA_PORT);
        keyboard_post_scancode(scancode);
    }
}

static inline uint64_t get_rflags(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static int ps2_wait_write(void) {
    int timeout = 100000;
    while ((inb(PS2_STATUS_PORT) & 0x02) && --timeout > 0) {
        __asm__ volatile ("pause");
    }
    return (timeout > 0) ? 0 : -1;
}

static int ps2_wait_read(void) {
    int timeout = 100000;
    while (!(inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        __asm__ volatile ("pause");
    }
    return (timeout > 0) ? 0 : -1;
}

void keyboard_init(void) {
    uint64_t saved_flags = get_rflags();
    __asm__ volatile ("cli");

    g_queue_head = 0;
    g_queue_tail = 0;
    g_shift_pressed = 0;
    g_caps_lock = 0;
    g_alt_pressed = 0;
    g_ctrl_pressed = 0;

    /* Flush PS/2 controller buffer with bounded timeout */
    int timeout = 1000;
    while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        inb(PS2_DATA_PORT);
    }

    /* 1. Enable first PS/2 port (keyboard) */
    if (ps2_wait_write() == 0) {
        outb(PS2_STATUS_PORT, 0xAE);
    }

    /* 2. Read Controller Configuration Byte (command 0x20) */
    uint8_t config = 0x47; /* Standard default: Port 1 enable, IRQ 1 enable, translation enable */
    if (ps2_wait_write() == 0) {
        outb(PS2_STATUS_PORT, 0x20);
        if (ps2_wait_read() == 0) {
            uint8_t read_cfg = inb(PS2_DATA_PORT);
            if (read_cfg != 0x00 && read_cfg != 0xFF && read_cfg != 0xFA) {
                config = read_cfg;
            }
        }
    }

    /* Enable IRQ 1 (bit 0), enable Set 1 translation (bit 6 = 0x40), disable clock inhibit (bit 4 = 0) */
    config |= 0x41;  /* Bit 0: Port 1 IRQ enable, Bit 6: Set 1 translation enable */
    config &= ~0x10; /* Bit 4: First PS/2 port clock enabled */

    /* 3. Write Controller Configuration Byte (command 0x60) */
    if (ps2_wait_write() == 0) {
        outb(PS2_STATUS_PORT, 0x60);
        if (ps2_wait_write() == 0) {
            outb(PS2_DATA_PORT, config);
        }
    }

    /* 4. Enable Keyboard Scanning (0xF4) and drain ACK */
    if (ps2_wait_write() == 0) {
        outb(PS2_DATA_PORT, 0xF4);
        if (ps2_wait_read() == 0) {
            (void)inb(PS2_DATA_PORT);
        }
    }

    /* Flush any trailing response bytes */
    timeout = 256;
    while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout > 0) {
        inb(PS2_DATA_PORT);
    }

    if (saved_flags & 0x200) {
        __asm__ volatile ("sti");
    }
}

void ps2_poll_controller(void) {
    static volatile int s_ps2_polling = 0;
    if (s_ps2_polling) return;
    s_ps2_polling = 1;

    for (int i = 0; i < 8; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & 0x01)) break;

        if (status & 0x20) {
            mice_handle_irq();
        } else {
            uint8_t scancode = inb(PS2_DATA_PORT);
            if (scancode != 0xFA && scancode != 0xAA && scancode != 0xFE) {
                keyboard_post_scancode(scancode);
            }
        }
    }

    s_ps2_polling = 0;
}

static char g_char_buf[8];
static int g_char_buf_len = 0;
static int g_char_buf_pos = 0;
static int g_extended_scancode = 0;
static int g_set2_mode = 0;
static int g_set2_release = 0;

static uint8_t scancode_set2_to_set1(uint8_t sc2) {
    switch (sc2) {
        case 0x76: return 0x01; /* Esc */
        case 0x16: return 0x02; /* 1 */
        case 0x1E: return 0x03; /* 2 */
        case 0x26: return 0x04; /* 3 */
        case 0x25: return 0x05; /* 4 */
        case 0x2E: return 0x06; /* 5 */
        case 0x36: return 0x07; /* 6 */
        case 0x3D: return 0x08; /* 7 */
        case 0x3E: return 0x09; /* 8 */
        case 0x46: return 0x0A; /* 9 */
        case 0x45: return 0x0B; /* 0 */
        case 0x4E: return 0x0C; /* - */
        case 0x55: return 0x0D; /* = */
        case 0x66: return 0x0E; /* Backspace */
        case 0x0D: return 0x0F; /* Tab */
        case 0x15: return 0x10; /* Q */
        case 0x1D: return 0x11; /* W */
        case 0x24: return 0x12; /* E */
        case 0x2D: return 0x13; /* R */
        case 0x2C: return 0x14; /* T */
        case 0x35: return 0x15; /* Y */
        case 0x3C: return 0x16; /* U */
        case 0x43: return 0x17; /* I */
        case 0x44: return 0x18; /* O */
        case 0x4D: return 0x19; /* P */
        case 0x54: return 0x1A; /* [ */
        case 0x5B: return 0x1B; /* ] */
        case 0x5A: return 0x1C; /* Enter */
        case 0x14: return 0x1D; /* Left/Right Ctrl */
        case 0x1C: return 0x1E; /* A */
        case 0x1B: return 0x1F; /* S */
        case 0x23: return 0x20; /* D */
        case 0x2B: return 0x21; /* F */
        case 0x34: return 0x22; /* G */
        case 0x33: return 0x23; /* H */
        case 0x3B: return 0x24; /* J */
        case 0x42: return 0x25; /* K */
        case 0x4B: return 0x26; /* L */
        case 0x4C: return 0x27; /* ; */
        case 0x52: return 0x28; /* ' */
        case 0x0E: return 0x29; /* ` */
        case 0x12: return 0x2A; /* Left Shift */
        case 0x5D: return 0x2B; /* \ */
        case 0x1A: return 0x2C; /* Z */
        case 0x22: return 0x2D; /* X */
        case 0x21: return 0x2E; /* C */
        case 0x2A: return 0x2F; /* V */
        case 0x32: return 0x30; /* B */
        case 0x31: return 0x31; /* N */
        case 0x3A: return 0x32; /* M */
        case 0x41: return 0x33; /* , */
        case 0x49: return 0x34; /* . */
        case 0x4A: return 0x35; /* / */
        case 0x59: return 0x36; /* Right Shift */
        case 0x11: return 0x38; /* Alt */
        case 0x29: return 0x39; /* Space */
        case 0x58: return 0x3A; /* Caps Lock */
        case 0x05: return 0x3B; /* F1 */
        case 0x06: return 0x3C; /* F2 */
        case 0x04: return 0x3D; /* F3 */
        case 0x75: return 0x48; /* Up Arrow */
        case 0x72: return 0x50; /* Down Arrow */
        case 0x6B: return 0x4B; /* Left Arrow */
        case 0x74: return 0x4D; /* Right Arrow */
        case 0x7D: return 0x49; /* Page Up */
        case 0x7A: return 0x51; /* Page Down */
        case 0x1F: return 0x5B; /* Left GUI */
        case 0x27: return 0x5C; /* Right GUI */
        default:   return 0;
    }
}

static char translate_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        g_extended_scancode = 1;
        return 0;
    }
    if (scancode == 0xF0) {
        g_set2_mode = 1;
        g_set2_release = 1;
        return 0;
    }
    if (g_set2_mode) {
        uint8_t mapped = scancode_set2_to_set1(scancode);
        if (mapped == 0) {
            g_set2_release = 0;
            g_extended_scancode = 0;
            return 0;
        }
        scancode = g_set2_release ? (uint8_t)(mapped | 0x80) : mapped;
        g_set2_release = 0;
    }

    if (g_extended_scancode) {
        g_extended_scancode = 0;
        if (scancode & 0x80) {
            uint8_t ext_rel = scancode & 0x7F;
            if (ext_rel == 0x38) g_alt_pressed = 0;
            if (ext_rel == 0x1D) g_ctrl_pressed = 0;
            return 0;
        }
        if (scancode == 0x38) {
            g_alt_pressed = 1;
            return 0;
        }
        if (scancode == 0x1D) {
            g_ctrl_pressed = 1;
            return 0;
        }
        if (scancode == 0x5B || scancode == 0x5C || scancode == 0x5D) { /* Left Win, Right Win, Menu */
            return (char)KEY_WIN;
        }
        if (scancode == 0x48) { /* Up Arrow */
            if (g_alt_pressed) {
                mice_post_event(0, g_shift_pressed ? -12 : -5, 0, 0);
                return 0;
            }
            if (g_shift_pressed) {
                g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = '5'; g_char_buf[3] = '~';
                g_char_buf_len = 4; g_char_buf_pos = 1;
            } else {
                g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = 'A';
                g_char_buf_len = 3; g_char_buf_pos = 1;
            }
            return 27;
        } else if (scancode == 0x50) { /* Down Arrow */
            if (g_alt_pressed) {
                mice_post_event(0, g_shift_pressed ? 12 : 5, 0, 0);
                return 0;
            }
            if (g_shift_pressed) {
                g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = '6'; g_char_buf[3] = '~';
                g_char_buf_len = 4; g_char_buf_pos = 1;
            } else {
                g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = 'B';
                g_char_buf_len = 3; g_char_buf_pos = 1;
            }
            return 27;
        } else if (scancode == 0x4D) { /* Right Arrow */
            if (g_alt_pressed) {
                mice_post_event(g_shift_pressed ? 12 : 5, 0, 0, 0);
                return 0;
            }
            g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = 'C';
            g_char_buf_len = 3; g_char_buf_pos = 1;
            return 27;
        } else if (scancode == 0x4B) { /* Left Arrow */
            if (g_alt_pressed) {
                mice_post_event(g_shift_pressed ? -12 : -5, 0, 0, 0);
                return 0;
            }
            g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = 'D';
            g_char_buf_len = 3; g_char_buf_pos = 1;
            return 27;
        } else if (scancode == 0x49) { /* Page Up */
            g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = '5'; g_char_buf[3] = '~';
            g_char_buf_len = 4; g_char_buf_pos = 1;
            return 27;
        } else if (scancode == 0x51) { /* Page Down */
            g_char_buf[0] = 27; g_char_buf[1] = '['; g_char_buf[2] = '6'; g_char_buf[3] = '~';
            g_char_buf_len = 4; g_char_buf_pos = 1;
            return 27;
        }
        return 0;
    }

    /* Handle key releases (scancode bit 7 set) */
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            g_shift_pressed = 0;
        } else if (released == 0x38) {
            g_alt_pressed = 0;
        } else if (released == 0x1D) {
            g_ctrl_pressed = 0;
        } else if (released == 0x1C && g_alt_pressed) {
            mice_post_event(0, 0, 0, 0); /* Release Left Click */
        } else if (released == 0x0E && g_alt_pressed) {
            mice_post_event(0, 0, 0, 0); /* Release Right Click */
        }
        return 0;
    }

    /* Handle key presses */
    if (scancode == 0x2A || scancode == 0x36) {
        g_shift_pressed = 1;
        return 0;
    }
    if (scancode == 0x38) {
        g_alt_pressed = 1;
        return 0;
    }
    if (scancode == 0x1D) {
        g_ctrl_pressed = 1;
        return 0;
    }
    if (scancode == 0x3A) {
        g_caps_lock = !g_caps_lock;
        return 0;
    }

    /* Mouse Key Fallbacks: Alt+Enter = Left Click, Alt+Backspace = Right Click, Alt+Numpad/WASD/Arrows */
    if (g_alt_pressed) {
        if (scancode == 0x1C) { /* Alt+Enter: Left Click */
            mice_post_event(0, 0, 0, 1);
            return 0;
        }
        if (scancode == 0x0E) { /* Alt+Backspace: Right Click */
            mice_post_event(0, 0, 0, 2);
            return 0;
        }
        if (scancode == 0x48) { mice_post_event(0, g_shift_pressed ? -12 : -5, 0, 0); return 0; }
        if (scancode == 0x50) { mice_post_event(0, g_shift_pressed ? 12 : 5, 0, 0); return 0; }
        if (scancode == 0x4D) { mice_post_event(g_shift_pressed ? 12 : 5, 0, 0, 0); return 0; }
        if (scancode == 0x4B) { mice_post_event(g_shift_pressed ? -12 : -5, 0, 0, 0); return 0; }
    }

    /* Hotkey: Alt+F3 -> Switch to TTY3 (Console) */
    if (scancode == 0x3D) {
        if (g_alt_pressed) {
            tty_switch(3);
            return 0;
        }
    }

    /* Hotkey: Alt+F1 -> Switch to TTY1 (GUI Desktop) */
    if (scancode == 0x3B) {
        if (g_alt_pressed) {
            tty_switch(1);
            return 0;
        }
    }

    /* Hotkey: Ctrl+Esc -> Windows key */
    if (scancode == 0x01 && g_ctrl_pressed) {
        return (char)KEY_WIN;
    }

    if (scancode >= 128) return 0;

    char c = 0;
    int use_upper = (g_shift_pressed ^ g_caps_lock);

    if (g_shift_pressed) {
        c = g_scancode_table_shifted[scancode];
        if (g_caps_lock && c >= 'A' && c <= 'Z') {
            c += 32;
        }
    } else {
        c = g_scancode_table_normal[scancode];
        if (use_upper && c >= 'a' && c <= 'z') {
            c -= 32;
        }
    }

    return c;
}

int keyboard_has_char(void) {
    extern void usb_hid_poll(void);
    ps2_poll_controller();
    usb_hid_poll();

    if (g_char_buf_pos < g_char_buf_len) return 1;
    if (uart_is_present() && (inb(SERIAL_LSR) & 0x01)) return 1;
    if (g_queue_head != g_queue_tail) return 1;
    return 0;
}

char keyboard_getchar_nonblock(void) {
    extern void usb_hid_poll(void);
    ps2_poll_controller();
    usb_hid_poll();

    if (g_char_buf_pos < g_char_buf_len) {
        return g_char_buf[g_char_buf_pos++];
    }

    if (uart_is_present() && (inb(SERIAL_LSR) & 0x01)) {
        char c = (char)inb(SERIAL_DATA);
        if (c == '\r') return '\n';
        if (c == 0x7F) return '\b';
        return c;
    }

    while (g_queue_head != g_queue_tail) {
        uint8_t scancode = g_key_queue[g_queue_tail];
        g_queue_tail = (g_queue_tail + 1) & 0xFF;
        char c = translate_scancode(scancode);
        if (c != 0) return c;
    }

    return 0;
}

char keyboard_getchar(void) {
    extern void usb_hid_poll(void);
    while (1) {
        ps2_poll_controller();
        usb_hid_poll();

        /* Check buffered characters (e.g. from multi-byte escape sequences) */
        if (g_char_buf_pos < g_char_buf_len) {
            return g_char_buf[g_char_buf_pos++];
        }

        /* 1. Check Serial Port (only if physical UART is detected) */
        if (uart_is_present() && (inb(SERIAL_LSR) & 0x01)) {
            char c = (char)inb(SERIAL_DATA);
            if (c == '\r') return '\n';
            if (c == 0x7F) return '\b';
            return c;
        }

        /* 2. Check PS/2 Interrupt / Poll Queue */
        if (g_queue_head != g_queue_tail) {
            uint8_t scancode = g_key_queue[g_queue_tail];
            g_queue_tail = (g_queue_tail + 1) & 0xFF;
            char c = translate_scancode(scancode);
            if (c != 0) return c;
            continue;
        }

        /* Wait for interrupt or serial activity */
        if (scheduler_is_enabled()) {
            scheduler_yield();
        }
        __asm__ volatile ("pause");
    }
}

void keyboard_readline(char *buffer, size_t max_len, const char *prompt) {
    if (!buffer || max_len == 0) return;

    if (prompt) {
        console_puts(prompt);
    }

    size_t idx = 0;
    buffer[0] = '\0';

    while (1) {
        char c = keyboard_getchar();

        if (c == '\n') {
            console_putc('\n');
            buffer[idx] = '\0';
            return;
        }

        if (c == '\b') {
            if (idx > 0) {
                idx--;
                buffer[idx] = '\0';
                console_putc('\b');
            }
            continue;
        }

        if (c >= 32 && c <= 126) {
            if (idx + 1 < max_len) {
                buffer[idx++] = c;
                buffer[idx] = '\0';
                console_putc(c);
            }
        }
    }
}
