#include "fs.h"
#include "lib.h"

BootLocationInfo g_boot_location = {
    .base_hardware_path = "PciRoot(0x0)/Pci(0x1,0x1)/Ata(0x0)/CDROM(0x0)",
    .partition_boot_file = "\\EFI\\pseuDOS\\kernel.bin",
    .mode = DEVPATH_MODE_FIRMWARE
};

void fs_init_devpath(const BootInfo *boot_info) {
    if (!boot_info) return;

    if (boot_info->hardware_devpath[0] != '\0') {
        strncpy(g_boot_location.base_hardware_path, boot_info->hardware_devpath, sizeof(g_boot_location.base_hardware_path) - 1);
        g_boot_location.base_hardware_path[sizeof(g_boot_location.base_hardware_path) - 1] = '\0';
    }
    if (boot_info->boot_file_path[0] != '\0') {
        strncpy(g_boot_location.partition_boot_file, boot_info->boot_file_path, sizeof(g_boot_location.partition_boot_file) - 1);
        g_boot_location.partition_boot_file[sizeof(g_boot_location.partition_boot_file) - 1] = '\0';
    }
}

void devpath_set_mode(devpath_mode_t mode) {
    g_boot_location.mode = mode;
}

devpath_mode_t devpath_get_mode(void) {
    return g_boot_location.mode;
}

void fs_get_prompt_path(char *out_buf, size_t max_len) {
    if (!out_buf || max_len == 0) return;

    const char *cwd = vfs_getcwd();

    char dos_cwd[256];
    size_t i = 0;
    while (cwd[i] != '\0' && i < sizeof(dos_cwd) - 1) {
        dos_cwd[i] = (cwd[i] == '/') ? '\\' : cwd[i];
        i++;
    }
    dos_cwd[i] = '\0';
    if (dos_cwd[0] == '\0') {
        dos_cwd[0] = '\\';
        dos_cwd[1] = '\0';
    }

    switch (g_boot_location.mode) {
        case DEVPATH_MODE_HARDWARE:
            if (strcmp(dos_cwd, "\\") == 0) {
                snprintf(out_buf, max_len, "%s\\", g_boot_location.base_hardware_path);
            } else {
                snprintf(out_buf, max_len, "%s%s", g_boot_location.base_hardware_path, dos_cwd);
            }
            break;
        case DEVPATH_MODE_SOFTWARE:
            strncpy(out_buf, cwd, max_len - 1);
            break;
        case DEVPATH_MODE_FIRMWARE:
        default:
            strncpy(out_buf, dos_cwd, max_len - 1);
            break;
    }
    out_buf[max_len - 1] = '\0';
}
