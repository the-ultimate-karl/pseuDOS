#include "drivers.h"
#include "io.h"
#include "lib.h"

static EFI_SYSTEM_TABLE *g_st = NULL;

void keyboard_init(EFI_SYSTEM_TABLE *SystemTable) {
    g_st = SystemTable;
    if (g_st && g_st->ConIn && g_st->ConIn->Reset) {
        g_st->ConIn->Reset(g_st->ConIn, 0);
    }
}

char keyboard_getc(void) {
    while (1) {
        /* Check UEFI ConIn first */
        if (g_st && g_st->ConIn) {
            EFI_INPUT_KEY key;
            EFI_STATUS status = g_st->ConIn->ReadKeyStroke(g_st->ConIn, &key);
            if (status == EFI_SUCCESS) {
                if (key.UnicodeChar != 0) {
                    char ch = (char)(key.UnicodeChar & 0x7F);
                    if (ch == '\r') ch = '\n';
                    return ch;
                }
            }
        } else {
            /* Check UART COM1 RX ready if ConIn is absent */
            if (inb(0x3F8 + 5) & 0x01) {
                uint8_t ch = inb(0x3F8);
                if (ch == '\r') ch = '\n';
                if (ch == 0x7F) ch = '\b'; /* DEL to Backspace */
                return (char)ch;
            }
        }

        /* Short CPU pause to prevent burning cycles in polling */
        __asm__ volatile ("pause");
    }
}

int keyboard_readline(char *buffer, size_t max_len, const char *prompt) {
    if (!buffer || max_len == 0) return 0;

    if (prompt) {
        console_puts(prompt);
    }

    size_t idx = 0;
    buffer[0] = '\0';

    while (1) {
        char ch = keyboard_getc();

        if (ch == '\n' || ch == '\r') {
            console_putc('\n');
            buffer[idx] = '\0';
            return (int)idx;
        } else if (ch == '\b' || ch == 0x7F) {
            if (idx > 0) {
                idx--;
                buffer[idx] = '\0';
                /* Visual backspace */
                console_puts("\b \b");
            }
        } else if ((unsigned char)ch >= 32 && (unsigned char)ch <= 126) {
            if (idx + 1 < max_len) {
                buffer[idx++] = ch;
                buffer[idx] = '\0';
                console_putc(ch);
            }
        }
    }
}
