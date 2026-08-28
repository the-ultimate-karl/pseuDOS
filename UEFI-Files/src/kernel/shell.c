#include "kernel.h"
#include "drivers.h"
#include "fs.h"
#include "lib.h"
#include "io.h"

typedef struct {
    const char *name;
    const char *description;
} CommandEntry;

static const CommandEntry g_commands[] = {
    {"help",    "display available kernel commands"},
    {"cpu",     "display cpu model, vendor, and feature flags"},
    {"mem",     "display physical memory map and statistics"},
    {"pci",     "scan and list connected pci bus devices"},
    {"devpath", "display and toggle uefi boot device hardware path"},
    {"clear",   "clear the screen console"},
    {"reboot",  "perform system reset"},
    {"halt",    "halt cpu execution"},
    {NULL, NULL}
};

static void cmd_help(void) {
    console_printf("===========================\n");
    for (int i = 0; g_commands[i].name != NULL; i++) {
        char cmd_buf[32];
        char desc_buf[128];
        strncpy(cmd_buf, g_commands[i].name, sizeof(cmd_buf) - 1);
        strncpy(desc_buf, g_commands[i].description, sizeof(desc_buf) - 1);
        cmd_buf[sizeof(cmd_buf) - 1] = '\0';
        desc_buf[sizeof(desc_buf) - 1] = '\0';

        /* Ensure strictly lowercase output */
        to_lowercase(cmd_buf);
        to_lowercase(desc_buf);

        /* Format: 18 chars left-aligned, colon, 5 spaces, description */
        char pad[32];
        size_t nlen = strlen(cmd_buf);
        size_t pad_count = (nlen < 18) ? (18 - nlen) : 0;
        for (size_t p = 0; p < pad_count; p++) {
            pad[p] = ' ';
        }
        pad[pad_count] = '\0';

        console_printf("%s%s:     %s\n", cmd_buf, pad, desc_buf);
    }
    console_printf("===========================\n");
}

static void cmd_devpath(void) {
    int mode = fs_toggle_path_mode();
    if (mode == 1) {
        console_printf("file path changed from partition file path to absolute hardware path\n");
    } else {
        console_printf("file path changed from absolute hardware path to partition file path\n");
    }
}

static void cmd_halt(void) {
    console_printf("halt instruction executed!\n");
    console_printf("System halted!\n");

    /* Disable interrupts and halt CPU in infinite loop */
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}

static void cmd_reboot(EFI_SYSTEM_TABLE *SystemTable) {
    console_printf("Rebooting system...\n");
    if (SystemTable && SystemTable->RuntimeServices && SystemTable->RuntimeServices->ResetSystem) {
        SystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    }

    /* Fallback to PS/2 Keyboard Controller pulse reset (Port 0x64 -> 0xFE) */
    outb(0x64, 0xFE);

    /* Triple fault fallback */
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}

void shell_run(EFI_SYSTEM_TABLE *SystemTable) {
    char input_line[256];
    char prompt[512];

    while (1) {
        const char *active_path = fs_get_active_path();
        snprintf(prompt, sizeof(prompt), "kernel@pseuDOS [%s] > ", active_path);

        keyboard_readline(input_line, sizeof(input_line), prompt);

        char *cmd = trim(input_line);
        if (strlen(cmd) == 0) {
            continue;
        }

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "cpu") == 0) {
            cpu_print_info();
        } else if (strcmp(cmd, "mem") == 0 || strcmp(cmd, "memory") == 0) {
            memory_print_info(SystemTable);
        } else if (strcmp(cmd, "pci") == 0) {
            pci_scan_bus();
        } else if (strcmp(cmd, "devpath") == 0 || strcmp(cmd, "path") == 0) {
            cmd_devpath();
        } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
            console_clear();
        } else if (strcmp(cmd, "reboot") == 0) {
            cmd_reboot(SystemTable);
        } else if (strcmp(cmd, "halt") == 0) {
            cmd_halt();
        } else {
            console_printf("Unknown command: '%s'. Type 'help' for available commands.\n", cmd);
        }
    }
}
