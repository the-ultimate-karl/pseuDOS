#include "kernel.h"
#include "bootinfo.h"
#include "drivers.h"
#include "fs.h"
#include "lib.h"

static const BootInfo *g_boot_info = NULL;

static void cmd_help(void) {
    console_puts("===========================\n");
    console_puts("help              :     display available kernel commands\n");
    console_puts("ls                :     list files and directories in current path\n");
    console_puts("cd                :     change current working directory\n");
    console_puts("pwd               :     print current working directory\n");
    console_puts("cat               :     display plaintext file contents\n");
    console_puts("mkdir             :     create a new directory in filesystem\n");
    console_puts("touch             :     create an empty file\n");
    console_puts("write             :     write or append text to a file\n");
    console_puts("del               :     delete a file or directory (-r, -f, -rf)\n");
    console_puts("fs                :     display active filesystem and mount information\n");
    console_puts("cpu               :     display CPU model, vendor, and feature flags\n");
    console_puts("mem               :     display physical memory map and statistics\n");
    console_puts("pci               :     scan and list connected PCI / PCIe bus devices\n");
    console_puts("devpath           :     display and toggle boot device hardware path\n");
    console_puts("clear             :     clear the screen console\n");
    console_puts("reboot            :     perform bare-metal system reset\n");
    console_puts("shutdown          :     perform bare-metal ACPI system power-off\n");
    console_puts("halt              :     halt CPU execution\n");
    console_puts("===========================\n");
}

static void cmd_fs(void) {
    console_puts("filesystem information:\n");
    console_puts("  current root (/)  : initramfs (in-memory ramdisk vfs)\n");
    console_puts("  filesystem type   : tmpfs / initramfs\n");
    console_puts("  storage medium    : volatile system RAM\n");
    console_puts("  mount point       : /\n");
    console_printf("  storage pool size : %lu MB\n", heap_get_total() / (1024 * 1024));
    console_printf("  used storage      : %lu KB\n", heap_get_used() / 1024);
    console_puts("  status            : active (read/write)\n");
    console_puts("  boot stage        : stage-1 early boot filesystem (awaiting switch_root)\n");
}

static void cmd_devpath(const char *arg) {
    if (!arg || arg[0] == '\0') {
        devpath_mode_t cur = devpath_get_mode();
        console_printf("active devpath mode: %s\n",
            cur == DEVPATH_MODE_HARDWARE ? "--absolute-hardware" :
            (cur == DEVPATH_MODE_SOFTWARE ? "--absolute-software" : "--absolute-firmware"));
        console_printf("  hardware: %s\\\n", g_boot_location.base_hardware_path);
        console_printf("  firmware: %s\n", vfs_getcwd());
        console_printf("  software: %s\n", vfs_getcwd());
        console_puts("usage: devpath [--absolute-hardware | --absolute-firmware | --absolute-software]\n");
        return;
    }

    if (strcmp(arg, "--absolute-hardware") == 0) {
        devpath_set_mode(DEVPATH_MODE_HARDWARE);
        console_puts("file path changed to absolute hardware path\n");
    } else if (strcmp(arg, "--absolute-firmware") == 0) {
        devpath_set_mode(DEVPATH_MODE_FIRMWARE);
        console_puts("file path changed to absolute firmware path\n");
    } else if (strcmp(arg, "--absolute-software") == 0) {
        devpath_set_mode(DEVPATH_MODE_SOFTWARE);
        console_puts("file path changed to absolute software path\n");
    } else {
        devpath_mode_t cur = devpath_get_mode();
        if (cur == DEVPATH_MODE_FIRMWARE) {
            devpath_set_mode(DEVPATH_MODE_HARDWARE);
            console_puts("file path changed to absolute hardware path\n");
        } else if (cur == DEVPATH_MODE_HARDWARE) {
            devpath_set_mode(DEVPATH_MODE_SOFTWARE);
            console_puts("file path changed to absolute software path\n");
        } else {
            devpath_set_mode(DEVPATH_MODE_FIRMWARE);
            console_puts("file path changed to absolute firmware path\n");
        }
    }
}

static void cmd_ls(const char *arg) {
    vfs_listdir(arg);
}

static void cmd_cd(const char *arg) {
    if (!arg || arg[0] == '\0') {
        vfs_chdir("/");
        return;
    }
    if (vfs_chdir(arg) != 0) {
        console_printf("cd: no such file or directory: %s\n", arg);
    }
}

static void cmd_pwd(void) {
    console_printf("%s\n", vfs_getcwd());
}

static void cmd_cat(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("cat: missing file operand\n");
        return;
    }
    char buf[2048];
    int bytes = vfs_read_file(arg, buf, sizeof(buf));
    if (bytes < 0) {
        console_printf("cat: %s: no such file or directory\n", arg);
    } else {
        console_puts(buf);
        if (bytes > 0 && buf[bytes - 1] != '\n') {
            console_putc('\n');
        }
    }
}

static void cmd_mkdir(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("mkdir: missing operand\n");
        return;
    }
    if (!vfs_mkdir(arg)) {
        console_printf("mkdir: cannot create directory '%s': file or directory exists or invalid parent\n", arg);
    }
}

static void cmd_touch(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("touch: missing file operand\n");
        return;
    }
    if (!vfs_create_file(arg)) {
        console_printf("touch: cannot create file '%s'\n", arg);
    }
}

static void cmd_write(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("usage: write <file> <text>\n");
        return;
    }

    char file[64];
    size_t i = 0;
    while (arg[i] != '\0' && arg[i] != ' ' && i < sizeof(file) - 1) {
        file[i] = arg[i];
        i++;
    }
    file[i] = '\0';

    while (arg[i] == ' ') i++;

    const char *text = &arg[i];
    int res = vfs_write_file(file, text, 0);
    if (res < 0) {
        console_printf("write: cannot write to '%s'\n", file);
    } else {
        console_printf("wrote %d bytes to %s\n", res, file);
    }
}

static void cmd_del(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("del: missing operand\n");
        console_puts("usage: del [-r] [-f] <path>\n");
        return;
    }

    int recursive = 0;
    int force = 0;
    char target_path[256];
    target_path[0] = '\0';

    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = buf;
    while (*token) {
        while (*token == ' ') token++;
        if (*token == '\0') break;

        char *next_space = strchr(token, ' ');
        if (next_space) {
            *next_space = '\0';
        }

        if (token[0] == '-') {
            for (size_t i = 1; token[i] != '\0'; i++) {
                if (token[i] == 'r' || token[i] == 'R') recursive = 1;
                else if (token[i] == 'f' || token[i] == 'F') force = 1;
            }
        } else {
            strncpy(target_path, token, sizeof(target_path) - 1);
            target_path[sizeof(target_path) - 1] = '\0';
        }

        if (!next_space) break;
        token = next_space + 1;
    }

    if (target_path[0] == '\0') {
        console_puts("del: missing file or directory operand\n");
        return;
    }

    int res = vfs_remove_node_ex(target_path, recursive, force);
    if (res == -1) {
        console_printf("del: cannot remove '%s': no such file or directory\n", target_path);
    } else if (res == -2) {
        console_printf("del: cannot remove '%s': directory not empty\n", target_path);
    } else if (res == -3) {
        console_printf("del: cannot remove '%s': invalid argument\n", target_path);
    } else if (res == -4) {
        console_puts("del: recursive deletion error: targeted directory is protected! please use the force (-f) flag to override.\n");
    }
}

static void cmd_halt(void) {
    console_puts("halt instruction executed!\n");
    console_puts("system halted!\n");

    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void shell_init(const BootInfo *boot_info) {
    g_boot_info = boot_info;
}

void shell_run(const BootInfo *boot_info) {
    if (boot_info) {
        g_boot_info = boot_info;
    }
    char line_buf[256];
    char prompt_buf[512];
    char prompt_path[256];

    console_puts("pseuDOS kernel v0.4.0-baremetal (x86_64 uefi / bare-metal)\n");
    console_puts("what's new (kernel version 0.4.0):\n");
    console_puts("- full bare-metal execution via ExitBootServices()\n");
    console_puts("- 1280x720 32-bit linear framebuffer console\n");
    console_puts("- embedded 8x16 bitmap font engine\n");
    console_puts("- 64-bit IDT (Interrupt Descriptor Table) & exception handlers\n");
    console_puts("- remapped 8259 PIC & IRQ 1 PS/2 keyboard interrupt driver\n");
    console_puts("- linux-style 2-stage initramfs in-memory VFS\n");
    console_puts("- physical RAM and PCIe MMIO aperture memory reporting\n");
    console_puts("- bare-metal ACPI shutdown and hardware reboot\n\n");
    console_puts("type 'help' to view available commands.\n\n");

    while (1) {
        fs_get_prompt_path(prompt_path, sizeof(prompt_path));
        snprintf(prompt_buf, sizeof(prompt_buf), "kernel@pseuDOS [%s] > ", prompt_path);

        keyboard_readline(line_buf, sizeof(line_buf), prompt_buf);
        char *cmd = trim(line_buf);

        if (cmd[0] == '\0') {
            continue;
        }

        char *arg = strchr(cmd, ' ');
        if (arg) {
            *arg = '\0';
            arg = trim(arg + 1);
        }

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "fs") == 0 || strcmp(cmd, "mount") == 0 || strcmp(cmd, "df") == 0) {
            cmd_fs();
        } else if (strcmp(cmd, "devpath") == 0) {
            cmd_devpath(arg);
        } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
            cmd_ls(arg);
        } else if (strcmp(cmd, "cd") == 0) {
            cmd_cd(arg);
        } else if (strcmp(cmd, "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
            cmd_cat(arg);
        } else if (strcmp(cmd, "mkdir") == 0) {
            cmd_mkdir(arg);
        } else if (strcmp(cmd, "touch") == 0) {
            cmd_touch(arg);
        } else if (strcmp(cmd, "write") == 0) {
            cmd_write(arg);
        } else if (strcmp(cmd, "del") == 0 || strcmp(cmd, "rm") == 0) {
            cmd_del(arg);
        } else if (strcmp(cmd, "cpu") == 0) {
            cpu_print_info();
        } else if (strcmp(cmd, "mem") == 0) {
            if (g_boot_info) {
                memory_print_info(&g_boot_info->mem);
            } else {
                console_puts("mem: boot memory map not available\n");
            }
        } else if (strcmp(cmd, "pci") == 0) {
            pci_scan_bus();
        } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
            console_clear();
        } else if (strcmp(cmd, "reboot") == 0) {
            acpi_reboot();
        } else if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) {
            acpi_shutdown();
        } else if (strcmp(cmd, "halt") == 0) {
            cmd_halt();
        } else {
            console_printf("unknown command '%s'. type 'help' for available commands.\n", cmd);
        }
    }
}
