#include "drivers.h"
#include "io.h"
#include "lib.h"

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

void keyboard_isr_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);
    if (status & 0x01) {
        uint8_t scancode = inb(PS2_DATA_PORT);
        uint8_t next_head = (g_queue_head + 1) & 0xFF;
        if (next_head != g_queue_tail) {
            g_key_queue[g_queue_head] = scancode;
            g_queue_head = next_head;
        }
    }
}

void keyboard_init(void) {
    g_queue_head = 0;
    g_queue_tail = 0;
    g_shift_pressed = 0;
    g_caps_lock = 0;

    /* Flush PS/2 controller buffer */
    while (inb(PS2_STATUS_PORT) & 0x01) {
        inb(PS2_DATA_PORT);
    }
}

static char translate_scancode(uint8_t scancode) {
    /* Handle key releases (scancode bit 7 set) */
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            g_shift_pressed = 0;
        }
        return 0;
    }

    /* Handle key presses */
    if (scancode == 0x2A || scancode == 0x36) {
        g_shift_pressed = 1;
        return 0;
    }
    if (scancode == 0x3A) {
        g_caps_lock = !g_caps_lock;
        return 0;
    }

    if (scancode >= 128) return 0;

    char c = 0;
    int use_upper = (g_shift_pressed ^ g_caps_lock);

    if (g_shift_pressed) {
        c = g_scancode_table_shifted[scancode];
    } else {
        c = g_scancode_table_normal[scancode];
        if (use_upper && c >= 'a' && c <= 'z') {
            c -= 32;
        }
    }

    return c;
}

char keyboard_getchar(void) {
    while (1) {
        /* 1. Check Serial Port */
        if (inb(SERIAL_LSR) & 0x01) {
            char c = (char)inb(SERIAL_DATA);
            if (c == '\r') return '\n';
            if (c == 0x7F) return '\b';
            return c;
        }

        /* 2. Check PS/2 Interrupt Queue */
        if (g_queue_head != g_queue_tail) {
            uint8_t scancode = g_key_queue[g_queue_tail];
            g_queue_tail = (g_queue_tail + 1) & 0xFF;
            char c = translate_scancode(scancode);
            if (c != 0) return c;
        }

        /* Wait for interrupt or serial activity */
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
