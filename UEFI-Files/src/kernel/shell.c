#include "kernel.h"
#include "bootinfo.h"
#include "drivers.h"
#include "storage.h"
#include "fs.h"
#include "rtc.h"
#include "klog.h"
#include "lib.h"
#include "process.h"
#include "scheduler.h"
#include "syscall.h"
#include "pit.h"

static const BootInfo *g_boot_info = NULL;

static void cmd_help(void) {
    console_puts("===========================\n");
    console_puts("help              :     display available kernel commands\n");
    console_puts("ls                :     list files and directories in current path\n");
    console_puts("cd                :     change current working directory\n");
    console_puts("pwd               :     print current working directory\n");
    console_puts("cat               :     display plaintext file contents\n");
    console_puts("more / less       :     view plaintext file contents page-by-page\n");
    console_puts("data              :     display file creation, access, and metadata\n");
    console_puts("cp                :     copy a file to a new destination\n");
    console_puts("mv                :     move or rename a file\n");
    console_puts("mkdir             :     create a new directory in filesystem\n");
    console_puts("touch             :     create an empty file\n");
    console_puts("write             :     write or append text to a file\n");
    console_puts("del               :     delete a file or directory (-r, -f, -rf)\n");
    console_puts("fs                :     query filesystem, in-memory VFS stats, and disk partitions\n");
    console_puts("ps                :     list active processes and CPU tick usage\n");
    console_puts("kill              :     terminate a process by PID\n");
    console_puts("proctest          :     test preemptive multitasking with concurrent tasks\n");
    console_puts("syscalltest       :     test x86_64 syscall interface and privileges\n");
    console_puts("screenres         :     adjust or display screen resolution\n");
    console_puts("switch-target     :     switch storage target (--internal | --external)\n");
    console_puts("attached-drives   :     list attached storage drives (--internal | --external | --all)\n");
    console_puts("flash             :     install pseuDOS onto a selected mass storage drive\n");
    console_puts("grub              :     display GRUB 2 chainloader config & setup\n");
    console_puts("cpu               :     display CPU model, vendor, and feature flags\n");
    console_puts("mem               :     display physical memory map and statistics\n");
    console_puts("pci               :     scan and list connected PCI / PCIe bus devices\n");
    console_puts("devpath           :     display and toggle boot device hardware path\n");
    console_puts("dmesg             :     dump kernel log ring buffer\n");
    console_puts("clear             :     clear the screen console\n");
    console_puts("reboot            :     perform bare-metal system reset\n");
    console_puts("shutdown          :     perform bare-metal ACPI system power-off\n");
    console_puts("halt              :     halt CPU execution\n");
    console_puts("===========================\n");
}

void cmd_switch_target(const char *arg) {
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

void cmd_attached_drives(const char *arg) {
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

void cmd_flash(const char *arg) {
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

void cmd_screenres(const char *arg) {
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
    while (arg[i] != '\0' && arg[i] != ' ') {
        if (w_idx < sizeof(w_str) - 1) {
            w_str[w_idx++] = arg[i];
        }
        i++;
    }
    w_str[w_idx] = '\0';

    while (arg[i] == ' ') i++;

    size_t h_idx = 0;
    while (arg[i] != '\0' && arg[i] != ' ') {
        if (h_idx < sizeof(h_str) - 1) {
            h_str[h_idx++] = arg[i];
        }
        i++;
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

    if (!fb_is_runtime_switch_supported()) {
        console_puts("screenres: runtime video mode switching is not supported by current display hardware.\n");
        console_printf("active display: %ux%u (native UEFI linear framebuffer TrueColor)\n", fb_get_width(), fb_get_height());
        console_puts("note: on bare-metal hardware (Intel/AMD/Nvidia), display resolution is set natively by UEFI GOP during boot.\n");
        return;
    }

    int res = fb_set_resolution((uint32_t)target_w, (uint32_t)target_h);
    if (res == 0) {
        console_printf("screenres: successfully switched resolution to %ux%u\n", (uint32_t)target_w, (uint32_t)target_h);
        return;
    }

    console_printf("screenres: display controller rejected mode %ldx%ld; keeping active mode %ux%u\n",
        target_w, target_h, fb_get_width(), fb_get_height());
}

void cmd_fs(const char *arg) {
    if (!arg || arg[0] == '\0') {
        /* 1. Live Root Filesystem & Boot Medium Status */
        uint32_t total_nodes = 0, total_dirs = 0, total_files = 0;
        uint64_t total_bytes = 0;
        vfs_get_stats(&total_nodes, &total_dirs, &total_files, &total_bytes);

        int boot_drive_num = 0;
        StorageDevice *boot_dev = storage_get_boot_device(g_boot_location.base_hardware_path, &boot_drive_num);

        console_puts("active root filesystem information:\n");

        if (boot_dev) {
            StorageFsInfo boot_fs;
            storage_inspect_fs(boot_dev, &boot_fs);

            console_printf("  current root (/)  : Drive %d (%s - %s)\n", boot_drive_num, boot_dev->name, boot_dev->type_str);
            console_printf("  storage medium    : Non-Volatile %s Block Storage (%s)\n", boot_dev->type_str, boot_dev->bus_speed);
            console_printf("  boot partition    : %s (%s)\n", boot_fs.has_partition_table ? boot_fs.part_type_name : "RAW", boot_fs.part_table_type);
            console_printf("  filesystem type   : %s (ESP) + In-Memory VFS Tree\n", boot_fs.fs_type);
            console_puts("  mount point       : /\n");
            console_printf("  hardware devpath  : %s\n", g_boot_location.base_hardware_path);
            if (boot_fs.has_filesystem) {
                console_printf("  partition capacity: %s free / %s total (%s)\n", boot_fs.free_str, boot_fs.total_str, boot_fs.health_status);
            }
        } else if (strstr(g_boot_location.base_hardware_path, "CDROM") || strstr(g_boot_location.base_hardware_path, "Ata(")) {
            console_puts("  current root (/)  : Optical UEFI Boot ISO (CD-ROM)\n");
            console_puts("  storage medium    : Optical ISO9660 Media + In-Memory VFS\n");
            console_puts("  boot partition    : UEFI El Torito Boot Image\n");
            console_puts("  filesystem type   : ISO9660 / FAT32 EFI\n");
            console_puts("  mount point       : /\n");
            console_printf("  hardware devpath  : %s\n", g_boot_location.base_hardware_path);
        } else {
            console_puts("  current root (/)  : In-Memory VFS Root\n");
            console_puts("  storage medium    : Volatile System RAM\n");
            console_puts("  filesystem type   : tmpfs / VFS\n");
            console_puts("  mount point       : /\n");
            console_printf("  hardware devpath  : %s\n", g_boot_location.base_hardware_path);
        }

        console_printf("  live directory cnt: %u directories\n", total_dirs);
        console_printf("  live file count   : %u files\n", total_files);
        console_printf("  stored file data  : %lu B (%lu KB)\n", total_bytes, total_bytes / 1024);
        console_printf("  storage pool size : %lu MB (dynamic kernel heap)\n", heap_get_total() / (1024 * 1024));
        console_printf("  allocated storage : %lu KB / %lu MB\n", heap_get_used() / 1024, heap_get_total() / (1024 * 1024));
        console_puts("  filesystem status : active (read/write)\n\n");
        console_puts("use 'fs <drive_no>' to inspect partitions on an attached disk, or 'attached-drives' to list drives.\n");
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

        StorageDriveInfo *drive_info = (StorageDriveInfo *)kmalloc(sizeof(StorageDriveInfo));
        if (!drive_info) return;

        storage_inspect_drive(dev, drive_info);

        console_printf("\nfilesystem inspection: Drive %d (%s - %s)\n", drive_num, dev->name, dev->type_str);
        console_printf("  hardware devpath    : %s\n", dev->devpath);
        console_printf("  raw capacity        : %s (%lu sectors @ %uB)\n", dev->size_str, dev->total_sectors, dev->sector_size);
        console_printf("  bus speed           : %s\n", dev->bus_speed);
        console_printf("  partition scheme    : %s (%u partition(s) found)\n",
            drive_info->part_table_type, drive_info->partition_count);

        if (drive_info->partition_count == 0) {
            console_puts("  (no partition table or unformatted disk)\n");
        } else {
            for (uint32_t p = 0; p < drive_info->partition_count; p++) {
                StoragePartitionInfo *part = &drive_info->partitions[p];
                console_printf("\n  [Partition %u]: %s\n", part->part_index, part->part_type_name);
                console_printf("    LBA range         : %lu - %lu (%lu sectors / %s)\n",
                    part->start_lba, part->end_lba, part->total_sectors, part->total_str);
                console_printf("    filesystem format : %s\n", part->fs_type);
                if (part->has_fs) {
                    console_printf("    volume label      : %s\n", part->vol_label);
                    console_printf("    oem identifier    : %s\n", part->oem_name);
                    console_printf("    bytes per sector  : %u bytes\n", part->bytes_per_sector);
                    console_printf("    cluster geometry  : %u sectors/cluster (%u bytes per cluster)\n",
                        part->sectors_per_cluster, part->cluster_size);
                    console_printf("    reserved sectors  : %u (FAT size: %u sectors, %u FATs)\n",
                        part->reserved_sectors, part->fat_size_sectors, part->num_fats);
                    console_printf("    root cluster      : %u\n", part->root_cluster);
                    console_printf("    cluster counts    : %u total, %u free, %u used\n",
                        part->total_clusters, part->free_clusters, part->used_clusters);
                    console_printf("    volume allocation : %s free / %s total (%s)\n",
                        part->free_str, part->total_str, part->health_status);
                }
            }
        }
        kfree(drive_info);
        console_puts("\n");
    } else {
        console_printf("fs: invalid drive number '%s'. type 'attached-drives' to view available drives.\n", arg);
    }
}

void cmd_devpath(const char *arg) {
    if (arg && (strcmp(arg, "--info") == 0 || strcmp(arg, "--status") == 0 || strcmp(arg, "-i") == 0 || strcmp(arg, "info") == 0 || strcmp(arg, "status") == 0)) {
        devpath_mode_t cur = devpath_get_mode();
        console_printf("active devpath mode: %s\n",
            cur == DEVPATH_MODE_HARDWARE ? "--absolute-hardware" :
            (cur == DEVPATH_MODE_SOFTWARE ? "--absolute-software" : "--absolute-firmware"));
        console_printf("  hardware: %s\\\n", g_boot_location.base_hardware_path);
        console_printf("  firmware: %s\n", vfs_getcwd());
        console_printf("  software: %s\n", vfs_getcwd());
        console_puts("usage: devpath [--absolute-hardware | --absolute-firmware | --absolute-software | --info]\n");
        return;
    }

    if (arg && (strcmp(arg, "--absolute-hardware") == 0 || strcmp(arg, "hardware") == 0 || strcmp(arg, "-h") == 0)) {
        devpath_set_mode(DEVPATH_MODE_HARDWARE);
        console_puts("file path changed to absolute hardware path\n");
    } else if (arg && (strcmp(arg, "--absolute-firmware") == 0 || strcmp(arg, "firmware") == 0 || strcmp(arg, "-f") == 0)) {
        devpath_set_mode(DEVPATH_MODE_FIRMWARE);
        console_puts("file path changed to absolute firmware path\n");
    } else if (arg && (strcmp(arg, "--absolute-software") == 0 || strcmp(arg, "software") == 0 || strcmp(arg, "-s") == 0)) {
        devpath_set_mode(DEVPATH_MODE_SOFTWARE);
        console_puts("file path changed to absolute software path\n");
    } else {
        /* Default / toggle behavior: cycle SOFTWARE -> HARDWARE -> FIRMWARE -> SOFTWARE */
        devpath_mode_t cur = devpath_get_mode();
        if (cur == DEVPATH_MODE_SOFTWARE) {
            devpath_set_mode(DEVPATH_MODE_HARDWARE);
            console_puts("file path changed to absolute hardware path\n");
        } else if (cur == DEVPATH_MODE_HARDWARE) {
            devpath_set_mode(DEVPATH_MODE_FIRMWARE);
            console_puts("file path changed to absolute firmware path\n");
        } else {
            devpath_set_mode(DEVPATH_MODE_SOFTWARE);
            console_puts("file path changed to absolute software path\n");
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

static void cmd_more_less(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("more: missing file operand\n");
        return;
    }

    vfs_node_t *node = vfs_find_node(arg);
    if (!node || node->type != VFS_NODE_FILE) {
        console_printf("more: %s: no such file or directory\n", arg);
        return;
    }

    if (!node->content || node->size == 0) {
        return;
    }

    const char *ptr = node->content;
    size_t line_count = 0;
    const size_t PAGE_LINES = 22;

    while (*ptr) {
        char ch = *ptr++;
        console_putc(ch);
        if (ch == '\n') {
            line_count++;
            if (line_count >= PAGE_LINES && *ptr != '\0') {
                console_puts("-- More -- (Space: next page, Enter: next line, Q: quit) ");
                while (1) {
                    char key = keyboard_getchar();
                    if (key == ' ' || key == 'f' || key == 'F') {
                        line_count = 0;
                        console_puts("\r                                                         \r");
                        break;
                    } else if (key == '\r' || key == '\n') {
                        line_count = PAGE_LINES - 1;
                        console_puts("\r                                                         \r");
                        break;
                    } else if (key == 'q' || key == 'Q') {
                        console_puts("\r                                                         \r");
                        return;
                    }
                }
            }
        }
    }
    if (line_count > 0 && node->content[node->size - 1] != '\n') {
        console_putc('\n');
    }
}

static void cmd_data(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("data: missing file operand\n");
        console_puts("usage: data <file>\n");
        return;
    }

    vfs_node_t *node = vfs_find_node(arg);
    if (!node) {
        console_printf("data: %s: no such file or directory\n", arg);
        return;
    }

    const char *parent_dir = "/";
    if (node->parent && node->parent->name[0] != '\0') {
        parent_dir = node->parent->name;
    }

    console_puts("File metadata:\n");
    console_printf("    Date created:       %s\n", node->date_created[0] ? node->date_created : "N/A");
    console_printf("    Date last accessed: %s\n", node->date_accessed[0] ? node->date_accessed : "N/A");
    console_printf("    Date modified:      %s\n", node->date_modified[0] ? node->date_modified : "N/A");
    console_printf("    On directory:       %s\n", parent_dir);
    console_printf("    File name:          %s\n", node->name);
    console_printf("    File size:          %lu bytes\n", (unsigned long)node->size);
    console_printf("    Node type:          %s\n", node->type == VFS_NODE_DIRECTORY ? "Directory" : "Plaintext file");
    console_printf("    Protected:          %s\n", node->is_protected ? "Yes" : "No");
}

static void resolve_copy_dest(const char *src, const char *dst, char *out_dst, size_t max_len) {
    vfs_node_t *dst_node = vfs_find_node(dst);
    size_t dst_len = strlen(dst);
    int is_dir = (dst_node && dst_node->type == VFS_NODE_DIRECTORY);
    int ends_slash = (dst_len > 0 && (dst[dst_len - 1] == '/' || dst[dst_len - 1] == '\\'));

    if (is_dir || ends_slash) {
        /* Extract base name from src */
        const char *base = strrchr(src, '/');
        if (!base) base = strrchr(src, '\\');
        base = base ? base + 1 : src;

        if (strcmp(dst, "/") == 0 || strcmp(dst, "\\") == 0) {
            snprintf(out_dst, max_len, "/%s", base);
        } else if (ends_slash) {
            snprintf(out_dst, max_len, "%s%s", dst, base);
        } else {
            snprintf(out_dst, max_len, "%s/%s", dst, base);
        }
    } else {
        strncpy(out_dst, dst, max_len - 1);
        out_dst[max_len - 1] = '\0';
    }
}

static void cmd_cp(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("cp: missing file operand\n");
        console_puts("usage: cp <source> <destination>\n");
        return;
    }

    char src[128];
    char dst[128];
    size_t i = 0, j = 0;

    while (arg[i] == ' ') i++;
    if (arg[i] == '"') {
        i++;
        while (arg[i] != '\0' && arg[i] != '"' && j < sizeof(src) - 1) {
            src[j++] = arg[i++];
        }
        if (arg[i] == '"') i++;
    } else {
        while (arg[i] != '\0' && arg[i] != ' ' && j < sizeof(src) - 1) {
            src[j++] = arg[i++];
        }
    }
    src[j] = '\0';

    while (arg[i] == ' ') i++;

    j = 0;
    if (arg[i] == '"') {
        i++;
        while (arg[i] != '\0' && arg[i] != '"' && j < sizeof(dst) - 1) {
            dst[j++] = arg[i++];
        }
        if (arg[i] == '"') i++;
    } else {
        while (arg[i] != '\0' && arg[i] != ' ' && j < sizeof(dst) - 1) {
            dst[j++] = arg[i++];
        }
    }
    dst[j] = '\0';

    if (src[0] == '\0' || dst[0] == '\0') {
        console_puts("cp: missing destination file operand\n");
        console_puts("usage: cp <source> <destination>\n");
        return;
    }

    vfs_node_t *src_node = vfs_find_node(src);
    if (!src_node || src_node->type != VFS_NODE_FILE) {
        console_printf("cp: cannot stat '%s': no such file\n", src);
        return;
    }

    char resolved_dst[256];
    resolve_copy_dest(src, dst, resolved_dst, sizeof(resolved_dst));

    vfs_node_t *existing_dst = vfs_find_node(resolved_dst);
    if (existing_dst == src_node) {
        console_printf("cp: '%s' and '%s' are the same file\n", src, resolved_dst);
        return;
    }

    int written = vfs_write_file_bytes(resolved_dst, src_node->content, src_node->size, 0);
    if (written < 0) {
        console_printf("cp: cannot copy to '%s': destination path invalid\n", resolved_dst);
    } else {
        console_printf("cp: copied '%s' -> '%s' (%lu bytes)\n", src, resolved_dst, (unsigned long)src_node->size);
    }
}

static void cmd_mv(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("mv: missing file operand\n");
        console_puts("usage: mv <source> <destination>\n");
        return;
    }

    char src[128];
    char dst[128];
    size_t i = 0, j = 0;

    while (arg[i] == ' ') i++;
    if (arg[i] == '"') {
        i++;
        while (arg[i] != '\0' && arg[i] != '"' && j < sizeof(src) - 1) {
            src[j++] = arg[i++];
        }
        if (arg[i] == '"') i++;
    } else {
        while (arg[i] != '\0' && arg[i] != ' ' && j < sizeof(src) - 1) {
            src[j++] = arg[i++];
        }
    }
    src[j] = '\0';

    while (arg[i] == ' ') i++;

    j = 0;
    if (arg[i] == '"') {
        i++;
        while (arg[i] != '\0' && arg[i] != '"' && j < sizeof(dst) - 1) {
            dst[j++] = arg[i++];
        }
        if (arg[i] == '"') i++;
    } else {
        while (arg[i] != '\0' && arg[i] != ' ' && j < sizeof(dst) - 1) {
            dst[j++] = arg[i++];
        }
    }
    dst[j] = '\0';

    if (src[0] == '\0' || dst[0] == '\0') {
        console_puts("mv: missing destination file operand\n");
        console_puts("usage: mv <source> <destination>\n");
        return;
    }

    vfs_node_t *src_node = vfs_find_node(src);
    if (!src_node || src_node->type != VFS_NODE_FILE) {
        console_printf("mv: cannot stat '%s': no such file\n", src);
        return;
    }

    char resolved_dst[256];
    resolve_copy_dest(src, dst, resolved_dst, sizeof(resolved_dst));

    vfs_node_t *existing_dst = vfs_find_node(resolved_dst);
    if (existing_dst == src_node) {
        console_printf("mv: '%s' and '%s' are the same file\n", src, resolved_dst);
        return;
    }

    int written = vfs_write_file_bytes(resolved_dst, src_node->content, src_node->size, 0);
    if (written < 0) {
        console_printf("mv: cannot move to '%s': destination path invalid\n", resolved_dst);
        return;
    }

    vfs_remove_node_ex(src, 0, 1);
    console_printf("mv: moved '%s' -> '%s'\n", src, resolved_dst);
}

static void cmd_dmesg(void) {
    klog_dmesg();
}

static void cmd_ps(void) {
    process_dump_list();
}

static void cmd_kill(const char *arg) {
    if (!arg || arg[0] == '\0') {
        console_puts("kill: missing operand\n");
        console_puts("usage: kill <pid>\n");
        return;
    }
    uint32_t pid = (uint32_t)atoi(arg);
    if (pid == 0 && strcmp(arg, "0") != 0) {
        console_printf("kill: invalid PID '%s'\n", arg);
        return;
    }
    process_kill(pid);
}

static void task_worker_a(void) {
    for (int i = 0; i < 4; i++) {
        klog_info("[Task A] working iteration %d/4 (PID %u)", i + 1, process_get_current()->pid);
        pit_sleep_ms(30);
    }
    klog_info("[Task A] completed.");
}

static void task_worker_b(void) {
    for (int i = 0; i < 4; i++) {
        klog_info("[Task B] working iteration %d/4 (PID %u)", i + 1, process_get_current()->pid);
        pit_sleep_ms(30);
    }
    klog_info("[Task B] completed.");
}

void cmd_proctest(void) {
    console_puts("proctest: Spawning background tasks worker_a and worker_b...\n");
    process_t *pa = process_create("worker_a", task_worker_a, PRIV_KERNEL);
    process_t *pb = process_create("worker_b", task_worker_b, PRIV_KERNEL);
    if (pa && pb) {
        console_printf("proctest: Tasks spawned: PID %u (worker_a) and PID %u (worker_b)\n", pa->pid, pb->pid);
        console_puts("proctest: Tasks are running concurrently under preemptive scheduling.\n");
        console_puts("proctest: Run 'ps' or 'dmesg' to monitor their progress!\n");
    } else {
        console_puts("proctest: Failed to spawn test tasks.\n");
    }
}

static void cmd_syscalltest(void) {
    console_puts("--- Testing Syscall Interface via 'syscall' instruction ---\n");
    int64_t pid = pseu_syscall(SYS_GETPID, 0, 0, 0, 0, 0);
    console_printf("1. sys_getpid() returned PID: %lld\n", (long long)pid);

    rtc_datetime_t t;
    int64_t ret = pseu_syscall(SYS_TIME, (uint64_t)(uintptr_t)&t, 0, 0, 0, 0);
    console_printf("2. sys_time() returned: %lld -> %04u-%02u-%02u %02u:%02u:%02u\n",
                   (long long)ret, (uint32_t)t.year, (uint32_t)t.month, (uint32_t)t.day,
                   (uint32_t)t.hours, (uint32_t)t.minutes, (uint32_t)t.seconds);

    const char *msg = "3. sys_write(1, msg): Hello from syscall!\n";
    pseu_syscall(SYS_WRITE, 1, (uint64_t)(uintptr_t)msg, strlen(msg), 0, 0);

    int64_t priv_before = pseu_syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
    console_printf("4. sys_get_privilege() before elevate: %s\n", priv_before == 1 ? "KERNEL" : "USER");

    pseu_syscall(SYS_ELEVATE, 0, 0, 0, 0, 0);
    int64_t priv_after = pseu_syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
    console_printf("5. sys_get_privilege() after sys_elevate(): %s\n", priv_after == 1 ? "KERNEL" : "USER");

    pseu_syscall(SYS_DROP_PRIVILEGES, 0, 0, 0, 0, 0);
    int64_t priv_drop = pseu_syscall(SYS_GET_PRIVILEGE, 0, 0, 0, 0, 0);
    console_printf("6. sys_get_privilege() after sys_drop_privileges(): %s\n", priv_drop == 1 ? "KERNEL" : "USER");

    console_puts("--- All Syscall Tests Passed! ---\n");
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

static void cmd_del(const char *cmd_name, const char *arg) {
    if (!cmd_name) cmd_name = "del";
    if (!arg || arg[0] == '\0') {
        console_printf("%s: missing operand\n", cmd_name);
        console_printf("usage: %s [-r] [-f] <path...>\n", cmd_name);
        return;
    }

    int recursive = 0;
    int force = 0;
    int operand_count = 0;

    char buf[512];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = buf;
    int stop_flags = 0;

    while (*token) {
        while (*token == ' ') token++;
        if (*token == '\0') break;

        char *next_space = strchr(token, ' ');
        if (next_space) {
            *next_space = '\0';
        }

        if (!stop_flags && token[0] == '-' && token[1] != '\0') {
            if (strcmp(token, "--") == 0) {
                stop_flags = 1;
            } else {
                for (size_t i = 1; token[i] != '\0'; i++) {
                    if (token[i] == 'r' || token[i] == 'R') recursive = 1;
                    else if (token[i] == 'f' || token[i] == 'F') force = 1;
                }
            }
        } else {
            operand_count++;
            int res = vfs_remove_node_ex(token, recursive, force);
            if (res == -1) {
                if (!force) {
                    console_printf("%s: cannot remove '%s': no such file or directory\n", cmd_name, token);
                }
            } else if (res == -2) {
                console_printf("%s: cannot remove '%s': directory not empty\n", cmd_name, token);
            } else if (res == -3) {
                console_printf("%s: cannot remove '%s': invalid argument\n", cmd_name, token);
            } else if (res == -4) {
                console_printf("%s: recursive deletion error: targeted directory is protected! please use the force (-f) flag to override.\n", cmd_name);
            } else if (res == -5) {
                console_printf("%s: cannot remove '%s': disk I/O synchronization error\n", cmd_name, token);
            }
        }

        if (!next_space) break;
        token = next_space + 1;
    }

    if (operand_count == 0) {
        console_printf("%s: missing file or directory operand\n", cmd_name);
    }
}

static void cmd_grub(void) {
    console_puts("=================================================================\n");
    console_puts(" pseuDOS GRUB 2 Integration & Chainloader Configuration\n");
    console_puts("=================================================================\n");
    console_puts("EFI Bootloader Targets:\n");
    console_puts("  /EFI/pseuDOS/BOOTX64.EFI\n");
    console_puts("  /EFI/pseuDOS/pseudos.efi\n\n");
    console_puts("GRUB 2 Menuentry Snippet (/etc/grub.d/40_custom or /boot/grub/grub.cfg):\n");
    console_puts("  menuentry \"pseuDOS x86_64\" {\n");
    console_puts("      insmod fat\n");
    console_puts("      insmod chain\n");
    console_puts("      search --no-floppy --set=root --file /EFI/pseuDOS/BOOTX64.EFI\n");
    console_puts("      chainloader /EFI/pseuDOS/BOOTX64.EFI\n");
    console_puts("  }\n\n");
    console_puts("Linux Installation Helper:\n");
    console_puts("  1. Append the menuentry snippet above to /etc/grub.d/40_custom\n");
    console_puts("  2. Run 'sudo update-grub' (or 'grub2-mkconfig -o /boot/grub/grub.cfg')\n");
    console_puts("  3. On reboot, select 'pseuDOS x86_64' from the GRUB boot menu\n");
    console_puts("  4. Live snippet also available at /EFI/pseuDOS/grub.cfg\n");
    console_puts("=================================================================\n");
}

void cmd_halt(void) {
    console_puts("halt instruction executed!\n");
    console_puts("system halted!\n");

    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void cmd_mem(void) {
    extern const BootInfo *g_boot_info_global;
    const BootInfo *bi = g_boot_info ? g_boot_info : g_boot_info_global;
    if (bi) {
        memory_print_info(&bi->mem);
    } else {
        console_puts("mem: boot memory map not available\n");
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

    console_puts("pseuDOS kernel v0.5.4-baremetal (x86_64 uefi / bare-metal)\n");
    console_puts("what's new (kernel version 0.5.4):\n");
    console_puts("- fast text-buffer console scrolling & 64-bit memory copy routines (memcpy/memmove/memset)\n");
    console_puts("- dynamic PE32+ base relocation engine in bootloader (.reloc DIR64 / HIGHLOW)\n");
    console_puts("- full 256-vector assembly ISR dispatch table with CPU exception stack normalization\n");
    console_puts("- 8259 PIC spurious IRQ 7 / IRQ 15 handling and EOI suppression per Intel spec\n");
    console_puts("- i8042 PS/2 controller IRQ 1 enablement (command byte 0x60) & non-racing poll fallback\n");
    console_puts("- hardware UART scratch register (0x3F8+7) detection preventing COM1 floating hangs\n");
    console_puts("- xHCI mass-storage class (0x08) verification protecting USB HID keyboards from reset\n");
    console_puts("- pure Windows 11 host toolchain migration & native PowerShell runner scripts\n");
    console_puts("- persistent FAT32 block-device synchronization with immediate write-through\n");
    console_puts("- dynamic on-disk FAT32 directory tree loading on permanent storage boot (SATA/NVMe/USB)\n");
    console_puts("- live sector synchronization for write, touch, mkdir, and del commands\n");
    console_puts("- dynamic boot device resolution & filesystem health reporting (fs)\n");
    console_puts("- VMware AHCI memory alignment fix (1024B CLB, 256B FB, 128B CTBA static pools)\n");
    console_puts("- AHCI BIOS/OS handoff (BOHC) & bounded timeout loops to prevent hypervisor lockups\n");
    console_puts("- EFI auto-boot hook (startup.nsh) for instant standalone disk booting\n");
    console_puts("- dynamic ACPI hardware table parser (RSDP -> XSDT/RSDT -> FADT -> DSDT AML)\n");
    console_puts("- native ACPI S5 shutdown & VMware backdoor (0x5658) poweroff support\n");
    console_puts("- bare-metal AHCI SATA & NVMe PCIe SSD DMA storage drivers\n");
    console_puts("- GPT partitioning & FAT32 EFI System Partition self-installer (flash)\n\n");
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
        } else if (strcmp(cmd, "more") == 0 || strcmp(cmd, "less") == 0) {
            cmd_more_less(arg);
        } else if (strcmp(cmd, "data") == 0) {
            cmd_data(arg);
        } else if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "copy") == 0) {
            cmd_cp(arg);
        } else if (strcmp(cmd, "mv") == 0 || strcmp(cmd, "move") == 0 || strcmp(cmd, "ren") == 0) {
            cmd_mv(arg);
        } else if (strcmp(cmd, "dmesg") == 0) {
            cmd_dmesg();
        } else if (strcmp(cmd, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(cmd, "kill") == 0) {
            cmd_kill(arg);
        } else if (strcmp(cmd, "proctest") == 0) {
            cmd_proctest();
        } else if (strcmp(cmd, "syscalltest") == 0) {
            cmd_syscalltest();
        } else if (strcmp(cmd, "grub") == 0) {
            cmd_grub();
        } else if (strcmp(cmd, "mkdir") == 0) {
            cmd_mkdir(arg);
        } else if (strcmp(cmd, "touch") == 0) {
            cmd_touch(arg);
        } else if (strcmp(cmd, "write") == 0) {
            cmd_write(arg);
        } else if (strcmp(cmd, "del") == 0 || strcmp(cmd, "rm") == 0) {
            cmd_del(cmd, arg);
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
