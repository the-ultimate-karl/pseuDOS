#include "kernel.h"
#include "bootinfo.h"
#include "drivers.h"
#include "storage.h"
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
    console_puts("fs                :     query filesystem, in-memory VFS stats, and disk partitions\n");
    console_puts("screenres         :     adjust or display screen resolution\n");
    console_puts("switch-target     :     switch storage target (--internal | --external)\n");
    console_puts("attached-drives   :     list attached storage drives (--internal | --external | --all)\n");
    console_puts("flash             :     install pseuDOS onto a selected mass storage drive\n");
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

static void cmd_switch_target(const char *arg) {
    if (!arg || arg[0] == '\0') {
        TargetFilterMode cur = storage_get_target_mode();
        console_printf("active storage target: %s\n", cur == TARGET_MODE_INTERNAL ? "internal storage devices only" : "external USB devices only");
        console_puts("usage: switch-target [--internal | --external]\n");
        return;
    }

    if (strcmp(arg, "--internal") == 0) {
        storage_set_target_mode(TARGET_MODE_INTERNAL);
        console_puts("switch-target: active storage target switched to internal devices only\n");
    } else if (strcmp(arg, "--external") == 0) {
        storage_set_target_mode(TARGET_MODE_EXTERNAL);
        console_puts("switch-target: active storage target switched to external USB devices only\n");
    } else {
        console_printf("switch-target: unknown parameter '%s'\n", arg);
        console_puts("usage: switch-target [--internal | --external]\n");
    }
}

static void cmd_attached_drives(const char *arg) {
    int show_internal = 0;
    int show_external = 0;

    if (!arg || arg[0] == '\0') {
        TargetFilterMode cur = storage_get_target_mode();
        if (cur == TARGET_MODE_INTERNAL) show_internal = 1;
        else show_external = 1;
    } else if (strcmp(arg, "--internal") == 0) {
        show_internal = 1;
    } else if (strcmp(arg, "--external") == 0) {
        show_external = 1;
    } else if (strcmp(arg, "--all") == 0) {
        show_internal = 1;
        show_external = 1;
    } else {
        console_printf("attached-drives: unknown option '%s'\n", arg);
        console_puts("usage: attached-drives [--internal | --external | --all]\n");
        return;
    }

    uint32_t count = storage_get_device_count();
    uint32_t match_count = 0;

    console_puts("\n[NO]  |  [TYPE]    |  [DEVICE_NAME]                    |  [SIZE]    |  [BUS_SPEED]\n");
    console_puts("------+------------+-----------------------------------+------------+--------------------\n");

    for (uint32_t i = 0; i < count; i++) {
        StorageDevice *dev = storage_get_device(i);
        if (!dev) continue;

        int is_int = (dev->type == STORAGE_TYPE_INTERNAL_SATA || dev->type == STORAGE_TYPE_INTERNAL_NVME);
        int is_ext = (dev->type == STORAGE_TYPE_EXTERNAL_USB);

        if ((is_int && show_internal) || (is_ext && show_external)) {
            match_count++;
            console_printf("%-5u |  %-9s |  %-32s |  %-9s |  %s\n",
                match_count, dev->type_str, dev->name, dev->size_str, dev->bus_speed);
        }
    }

    if (match_count == 0) {
        console_puts("  (no matching mass storage devices detected)\n");
    }
    console_puts("\n");
}

static void install_progress_handler(const char *step_name, int is_ok) {
    if (is_ok) {
        console_printf("flash: %s [ok]\n", step_name);
    } else {
        console_printf("flash: %s [failed]\n", step_name);
    }
}

static void cmd_flash(const char *arg) {
    (void)arg;

    console_puts("flash: searching for attached internal mass storage devices... ");
    uint32_t count = storage_get_device_count();

    StorageDevice *matching[32];
    uint32_t match_count = 0;
    int is_external_view = 0;

    /* Check internal drives first */
    for (uint32_t i = 0; i < count; i++) {
        StorageDevice *dev = storage_get_device(i);
        if (dev && (dev->type == STORAGE_TYPE_INTERNAL_SATA || dev->type == STORAGE_TYPE_INTERNAL_NVME)) {
            matching[match_count++] = dev;
        }
    }

    if (match_count > 0) {
        console_puts("done\n");
        console_puts("flash: identifying mass storage devices... done\n");
        console_puts("flash: displaying options for internal mass storage devices\n\n");
    } else {
        console_puts("none found\n");
        console_puts("flash: querying universal serial bus (usb) for any connected mass storage devices... ");

        for (uint32_t i = 0; i < count; i++) {
            StorageDevice *dev = storage_get_device(i);
            if (dev && dev->type == STORAGE_TYPE_EXTERNAL_USB) {
                matching[match_count++] = dev;
            }
        }

        if (match_count > 0) {
            console_puts("done\n");
            console_puts("flash: identifying mass storage devices... done\n");
            console_puts("flash: displaying options for external mass storage devices\n\n");
            is_external_view = 1;
        } else {
            console_puts("none found\n");
            console_puts("flash: error: no mass storage devices detected on system!\n");
            return;
        }
    }

    console_puts("                                [pseuDOS Installation]\n");
    console_puts("==========================================================================\n");
    console_puts("choose the mass storage device you want to install pseuDOS on:\n\n");
    console_puts("[NO]    |    [DEVICE_NAME]                      |    [SIZE]\n");
    console_puts("--------+---------------------------------------+------------\n");

    for (uint32_t i = 0; i < match_count; i++) {
        console_printf("%-7u |    %-34s |    %s\n", i + 1, matching[i]->name, matching[i]->size_str);
    }
    console_puts("\n");

    StorageDevice *selected_dev = NULL;
    char line_buf[128];

    while (1) {
        keyboard_readline(line_buf, sizeof(line_buf), "flash > ");
        char *input = trim(line_buf);

        if (input[0] == '\0') {
            continue;
        }

        if (strcmp(input, "cancel") == 0 || strcmp(input, "stop") == 0) {
            console_puts("flash: cancelling installation...\n");
            return;
        }

        long sel_num = strtol(input, NULL, 10);
        if (sel_num < 1 || sel_num > (long)match_count) {
            console_puts("flash: error: unknown command\n");
            continue;
        }

        selected_dev = matching[sel_num - 1];
        break;
    }

    /* USB 3.1 Gen 1 minimum speed verification */
    if (is_external_view || selected_dev->type == STORAGE_TYPE_EXTERNAL_USB) {
        if (selected_dev->usb_version < 0x0310) {
            console_puts("ATTENTION! you are attempting to install pseuDOS to an external universal serial bus drive that does not meet the minimum requirement of USB 3.1 Gen 1. it is highly recommended to use a faster drive to make sure installation does not crawl, and to ensure boot times are at max.\n");
            console_puts("are you sure you want to do this?\n(y/N) ");
            keyboard_readline(line_buf, sizeof(line_buf), "");
            char *ans = trim(line_buf);
            if (ans[0] != 'y' && ans[0] != 'Y') {
                console_puts("flash: cancelling installation...\n");
                return;
            }
        }
    }

    /* Safety Confirmation Warning */
    console_puts("WARNING!!! ensure you have selected the proper target, as this command will\n");
    console_puts("erase EVERYTHING on the selected drive!!\n");
    console_printf("are you sure you want to erase and format %s?\n(y/N) ", selected_dev->name);
    keyboard_readline(line_buf, sizeof(line_buf), "");
    char *ans = trim(line_buf);
    if (ans[0] != 'y' && ans[0] != 'Y') {
        console_puts("flash: cancelling installation...\n");
        return;
    }

    console_puts("\n");
    int res = gpt_fat32_format_and_install(selected_dev, install_progress_handler);
    if (res != 0) {
        console_puts("flash: fatal error: installation failed due to hardware disk write error!\n");
        return;
    }

    console_printf("\nflash: successfully installed pseuDOS to %s (devpath: %s)\n", selected_dev->name, selected_dev->devpath);
    console_puts("flash: please remove the installation media and press ENTER\n");

    /* Strict wait for ENTER key only */
    while (1) {
        char c = keyboard_getchar();
        if (c == '\r' || c == '\n') {
            break;
        }
    }

    console_puts("rebooting system into newly installed pseuDOS...\n");
    acpi_reboot();
}

static void cmd_screenres(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_printf("active screen resolution: %ux%u\n", fb_get_width(), fb_get_height());
        console_puts("usage: screenres <width> <height>\n");
        console_puts("example: screenres 1920 1080\n");
        return;
    }

    char w_str[32];
    char h_str[32];
    w_str[0] = '\0';
    h_str[0] = '\0';

    size_t i = 0;
    size_t w_idx = 0;
    while (arg[i] != '\0' && arg[i] != ' ' && w_idx < sizeof(w_str) - 1) {
        w_str[w_idx++] = arg[i++];
    }
    w_str[w_idx] = '\0';

    while (arg[i] == ' ') i++;

    size_t h_idx = 0;
    while (arg[i] != '\0' && arg[i] != ' ' && h_idx < sizeof(h_str) - 1) {
        h_str[h_idx++] = arg[i++];
    }
    h_str[h_idx] = '\0';

    long target_w = strtol(w_str, NULL, 10);
    long target_h = strtol(h_str, NULL, 10);

    if (target_w <= 0 || target_h <= 0) {
        console_printf("active screen resolution: %ux%u\n", fb_get_width(), fb_get_height());
        console_puts("usage: screenres <width> <height>\n");
        console_puts("example: screenres 1920 1080\n");
        return;
    }

    /* Tier 1: Attempt requested resolution */
    int res = fb_set_resolution((uint32_t)target_w, (uint32_t)target_h);
    if (res == 0) {
        console_printf("screenres: successfully switched resolution to %ux%u\n", (uint32_t)target_w, (uint32_t)target_h);
        return;
    }

    /* Tier 2: Attempt restoring last known good resolution */
    console_printf("screenres: failed to switch to %ldx%ld\n", target_w, target_h);
    console_puts("screenres: switching back to last known good resolution...\n");
    uint32_t last_w = fb_get_last_good_width();
    uint32_t last_h = fb_get_last_good_height();
    res = fb_set_resolution(last_w, last_h);
    if (res == 0) {
        console_printf("screenres: successfully switched back to %ux%u\n", last_w, last_h);
        return;
    }

    /* Tier 3: Attempt universal Safe-Mode resolution (800x600) */
    console_puts("screenres: failed to restore last known resolution!\n");
    console_puts("screenres: recovering in safe-mode (800x600)...\n");
    res = fb_set_resolution(800, 600);
    if (res == 0) {
        console_puts("screenres: successfully recovered in safe-mode 800x600\n");
        return;
    }

    /* Tier 4: Fatal Error - System Halt */
    console_puts("screenres: fatal graphics error: unable to restore video mode!\n");
    console_puts("screenres: system halted to prevent display corruption.\n");
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}

static void cmd_fs(const char *arg) {
    if (!arg || arg[0] == '\0') {
        /* 1. Live in-memory VFS Statistics */
        uint32_t total_nodes = 0, total_dirs = 0, total_files = 0;
        uint64_t total_bytes = 0;
        vfs_get_stats(&total_nodes, &total_dirs, &total_files, &total_bytes);

        console_puts("active root filesystem information:\n");
        console_puts("  current root (/)  : initramfs (in-memory ramdisk vfs)\n");
        console_puts("  filesystem type   : tmpfs / initramfs\n");
        console_puts("  storage medium    : volatile system RAM\n");
        console_puts("  mount point       : /\n");
        console_printf("  live directory cnt: %u directories\n", total_dirs);
        console_printf("  live file count   : %u files\n", total_files);
        console_printf("  stored file data  : %lu B (%lu KB)\n", total_bytes, total_bytes / 1024);
        console_printf("  storage pool size : %lu MB (dynamic kernel heap)\n", heap_get_total() / (1024 * 1024));
        console_printf("  allocated storage : %lu KB / %lu MB\n", heap_get_used() / 1024, heap_get_total() / (1024 * 1024));
        console_puts("  status            : active (read/write)\n");
        console_puts("  boot stage        : stage-1 early boot filesystem (awaiting switch_root)\n\n");

        /* 2. Attached Block Device Partition Summary */
        console_puts("attached storage partition summary:\n");
        uint32_t dev_count = storage_get_device_count();
        if (dev_count == 0) {
            console_puts("  (no attached block devices detected)\n");
        } else {
            for (uint32_t i = 0; i < dev_count; i++) {
                StorageDevice *dev = storage_get_device(i);
                if (!dev) continue;

                StorageFsInfo info;
                if (storage_inspect_fs(dev, &info) == 0 && info.has_filesystem) {
                    console_printf("  drive %u (%s - %s): %s [%s / %s] - %s free / %s total (%s)\n",
                        i + 1, dev->type_str, dev->name, dev->size_str,
                        info.fs_type, info.vol_label, info.free_str, info.total_str, info.health_status);
                } else if (info.has_partition_table) {
                    console_printf("  drive %u (%s - %s): %s [%s / %s] - (unformatted filesystem)\n",
                        i + 1, dev->type_str, dev->name, dev->size_str,
                        info.part_table_type, info.part_type_name);
                } else {
                    console_printf("  drive %u (%s - %s): %s (unpartitioned / RAW)\n",
                        i + 1, dev->type_str, dev->name, dev->size_str);
                }
            }
        }
        console_puts("type 'fs --drives' or 'fs <drive_no>' for detailed partition analysis.\n");
        return;
    }

    if (strcmp(arg, "--drives") == 0 || strcmp(arg, "--all") == 0 || strcmp(arg, "-d") == 0) {
        uint32_t dev_count = storage_get_device_count();
        console_puts("\n[NO]  |  [TYPE]    |  [DEVICE_NAME]                    |  [PARTITION]  |  [FS_TYPE]  |  [LABEL]       |  [FREE / TOTAL]\n");
        console_puts("------+------------+-----------------------------------+---------------+-------------+----------------+--------------------\n");

        if (dev_count == 0) {
            console_puts("  (no mass storage devices detected)\n\n");
            return;
        }

        for (uint32_t i = 0; i < dev_count; i++) {
            StorageDevice *dev = storage_get_device(i);
            if (!dev) continue;

            StorageFsInfo info;
            storage_inspect_fs(dev, &info);

            char part_display[16];
            if (info.has_partition_table) {
                if (strcmp(info.part_table_type, "GPT") == 0) strcpy(part_display, "GPT / ESP");
                else strcpy(part_display, "MBR / Part");
            } else {
                strcpy(part_display, "RAW / None");
            }

            char cap_display[24];
            if (info.has_filesystem) {
                snprintf(cap_display, sizeof(cap_display), "%s / %s", info.free_str, info.total_str);
            } else {
                snprintf(cap_display, sizeof(cap_display), "- / %s", dev->size_str);
            }

            console_printf("%-5u |  %-9s |  %-34s |  %-12s |  %-10s |  %-14s |  %-18s\n",
                i + 1, dev->type_str, dev->name, part_display, info.fs_type, info.vol_label, cap_display);
        }
        console_puts("\n");
        return;
    }

    /* Drive number inspection */
    int drive_num = atoi(arg);
    uint32_t dev_count = storage_get_device_count();
    if (drive_num >= 1 && (uint32_t)drive_num <= dev_count) {
        StorageDevice *dev = storage_get_device(drive_num - 1);
        if (!dev) return;

        StorageFsInfo info;
        storage_inspect_fs(dev, &info);

        console_printf("\nfilesystem inspection: Drive %d (%s - %s)\n", drive_num, dev->name, dev->type_str);
        console_printf("  hardware devpath    : %s\n", dev->devpath);
        console_printf("  raw capacity        : %s (%lu sectors @ %uB)\n", dev->size_str, dev->total_sectors, dev->sector_size);
        console_printf("  bus speed           : %s\n", dev->bus_speed);
        console_printf("  partition scheme    : %s\n", info.part_table_type);
        console_printf("  partition type      : %s\n", info.part_type_name);
        if (info.has_partition_table) {
            console_printf("  partition range     : LBA %lu - %lu (%lu sectors / %s)\n",
                info.part_start_lba, info.part_end_lba, info.part_total_sectors, info.total_str);
        }
        console_printf("  filesystem format   : %s\n", info.fs_type);
        if (info.has_filesystem) {
            console_printf("  volume label        : %s\n", info.vol_label);
            console_printf("  oem identifier      : %s\n", info.oem_name);
            console_printf("  bytes per sector    : %u bytes\n", info.bytes_per_sector);
            console_printf("  sectors per cluster : %u (%u bytes per cluster)\n", info.sectors_per_cluster, info.cluster_size);
            console_printf("  reserved sectors    : %u (FSInfo @ LBA %lu)\n", info.reserved_sectors, info.part_start_lba + 1);
            console_printf("  number of FATs      : %u (%u sectors per FAT)\n", info.num_fats, info.fat_size_sectors);
            console_printf("  root cluster        : %u\n", info.root_cluster);
            console_printf("  total data clusters : %u clusters (%s)\n", info.total_clusters, info.total_str);
            console_printf("  free clusters       : %u clusters (%s free)\n", info.free_clusters, info.free_str);
            console_printf("  used clusters       : %u clusters (%s used)\n", info.used_clusters, info.used_str);
            console_printf("  filesystem health   : %s\n", info.health_status);
        }
        console_puts("\n");
    } else {
        console_printf("fs: invalid drive number '%s'. type 'fs --drives' to view available drives.\n", arg);
    }
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

    console_puts("pseuDOS kernel v0.5.0-baremetal (x86_64 uefi / bare-metal)\n");
    console_puts("what's new (kernel version 0.5.0):\n");
    console_puts("- bare-metal AHCI SATA & NVMe PCIe SSD DMA storage drivers\n");
    console_puts("- USB 3.x xHCI / USB 2.0 EHCI device discovery & bus speed policy\n");
    console_puts("- GPT partitioning & FAT32 EFI System Partition self-installer engine (flash)\n");
    console_puts("- storage target filtering (switch-target) & attached drive listing (attached-drives)\n");
    console_puts("- live VFS metrics traversal & block-device partition deep inspector (fs)\n");
    console_puts("- strict bare-metal hardware simulator with IOMMU & NUMA memory holes (run_realistic.sh)\n");
    console_puts("- 500 MB persistent virtual drive options (--sata, --nvme, --usb, --usb2, --all)\n\n");
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
        } else if (strcmp(cmd, "screenres") == 0) {
            cmd_screenres(arg);
        } else if (strcmp(cmd, "switch-target") == 0) {
            cmd_switch_target(arg);
        } else if (strcmp(cmd, "attached-drives") == 0) {
            cmd_attached_drives(arg);
        } else if (strcmp(cmd, "flash") == 0) {
            cmd_flash(arg);
        } else if (strcmp(cmd, "fs") == 0 || strcmp(cmd, "mount") == 0 || strcmp(cmd, "df") == 0) {
            cmd_fs(arg);
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
