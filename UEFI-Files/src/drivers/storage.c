#include "storage.h"
#include "drivers.h"
#include "fs.h"
#include "lib.h"

#define MAX_STORAGE_DEVICES 32

static StorageDevice g_devices[MAX_STORAGE_DEVICES];
static uint32_t g_device_count = 0;
static TargetFilterMode g_target_mode = TARGET_MODE_INTERNAL;

void storage_format_size(uint64_t bytes, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;

    if (bytes >= (uint64_t)1024 * 1024 * 1024 * 1024) {
        uint64_t tb = bytes / ((uint64_t)1024 * 1024 * 1024 * 1024);
        snprintf(buf, buf_size, "%lu TB", tb);
    } else if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        uint64_t gb = bytes / ((uint64_t)1024 * 1024 * 1024);
        snprintf(buf, buf_size, "%lu GB", gb);
    } else if (bytes >= (uint64_t)1024 * 1024) {
        uint64_t mb = bytes / ((uint64_t)1024 * 1024);
        snprintf(buf, buf_size, "%lu MB", mb);
    } else if (bytes >= 1024) {
        uint64_t kb = bytes / 1024;
        snprintf(buf, buf_size, "%lu KB", kb);
    } else {
        snprintf(buf, buf_size, "%lu B", bytes);
    }
}

int storage_register_device(const StorageDevice *dev) {
    if (!dev || g_device_count >= MAX_STORAGE_DEVICES) return -1;
    memcpy(&g_devices[g_device_count], dev, sizeof(StorageDevice));
    g_device_count++;
    return 0;
}

uint32_t storage_get_device_count(void) {
    return g_device_count;
}

StorageDevice *storage_get_device(uint32_t index) {
    if (index >= g_device_count) return NULL;
    return &g_devices[index];
}

TargetFilterMode storage_get_target_mode(void) {
    return g_target_mode;
}

void storage_set_target_mode(TargetFilterMode mode) {
    g_target_mode = mode;
}

static void parse_partition_filesystem(StorageDevice *dev, uint64_t part_start_lba, uint64_t part_end_lba, StoragePartitionInfo *p) {
    (void)part_end_lba;
    p->has_fs = 0;
    strcpy(p->fs_type, "RAW / Unformatted");
    strcpy(p->vol_label, "NO NAME");
    strcpy(p->oem_name, "UNKNOWN");
    strcpy(p->health_status, "Unformatted");

    if (!dev || !dev->read_sectors) return;

    uint8_t sector[512];
    if (dev->read_sectors(dev, part_start_lba, 1, sector) != 0) {
        return;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return;
    }

    char oem[9];
    memcpy(oem, &sector[3], 8);
    oem[8] = '\0';
    for (int i = 7; i >= 0 && oem[i] == ' '; i--) oem[i] = '\0';
    if (oem[0] != '\0') {
        strncpy(p->oem_name, oem, sizeof(p->oem_name) - 1);
    }

    uint16_t bps = *(uint16_t *)&sector[11];
    uint8_t spc = sector[13];
    uint16_t rsvd = *(uint16_t *)&sector[14];
    uint8_t fats = sector[16];
    uint16_t tot_sec_16 = *(uint16_t *)&sector[19];
    uint16_t fat_sz_16 = *(uint16_t *)&sector[22];
    uint32_t tot_sec_32 = *(uint32_t *)&sector[32];
    uint32_t total_sec = tot_sec_32 != 0 ? tot_sec_32 : tot_sec_16;
    uint32_t fat_sz_32 = *(uint32_t *)&sector[36];

    if (bps >= 512 && spc > 0 && fats > 0 && fat_sz_32 > 0) {
        /* FAT32 Volume */
        p->has_fs = 1;
        strcpy(p->fs_type, "FAT32");
        p->bytes_per_sector = bps;
        p->sectors_per_cluster = spc;
        p->cluster_size = bps * spc;
        p->reserved_sectors = rsvd;
        p->num_fats = fats;
        p->fat_size_sectors = fat_sz_32;
        p->root_cluster = *(uint32_t *)&sector[44];

        /* BPB_FSInfo dynamically from offset 0x30 (byte 48) */
        uint16_t fsinfo_rel_sec = *(uint16_t *)&sector[48];
        if (fsinfo_rel_sec == 0 || fsinfo_rel_sec >= rsvd) {
            fsinfo_rel_sec = 1;
        }

        char label[12];
        memcpy(label, &sector[71], 11);
        label[11] = '\0';
        for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = '\0';
        if (label[0] != '\0') {
            strncpy(p->vol_label, label, sizeof(p->vol_label) - 1);
        }

        uint32_t data_sec = total_sec > (rsvd + fats * fat_sz_32) ? (total_sec - (rsvd + fats * fat_sz_32)) : 0;
        p->total_clusters = data_sec / spc;
        p->free_clusters = p->total_clusters;
        p->used_clusters = 0;

        /* Read FSInfo sector dynamically from partition start + fsinfo_rel_sec */
        uint8_t fsinfo[512];
        if (dev->read_sectors(dev, part_start_lba + fsinfo_rel_sec, 1, fsinfo) == 0) {
            uint32_t lead_sig = *(uint32_t *)&fsinfo[0];
            uint32_t struct_sig = *(uint32_t *)&fsinfo[484];
            if (lead_sig == 0x41615252 && struct_sig == 0x61417272) {
                uint32_t free_c = *(uint32_t *)&fsinfo[488];
                if (free_c != 0xFFFFFFFF && free_c <= p->total_clusters) {
                    p->free_clusters = free_c;
                    p->used_clusters = p->total_clusters - free_c;
                }
            }
        }

        p->total_bytes = (uint64_t)p->total_clusters * p->cluster_size;
        p->free_bytes = (uint64_t)p->free_clusters * p->cluster_size;
        p->used_bytes = (uint64_t)p->used_clusters * p->cluster_size;

        storage_format_size(p->total_bytes, p->total_str, sizeof(p->total_str));
        storage_format_size(p->free_bytes, p->free_str, sizeof(p->free_str));
        storage_format_size(p->used_bytes, p->used_str, sizeof(p->used_str));

        strcpy(p->health_status, "Clean / Healthy");
    } else if (bps >= 512 && spc > 0 && fats > 0 && fat_sz_16 > 0) {
        /* FAT16 / FAT12 Volume */
        p->has_fs = 1;
        strcpy(p->fs_type, "FAT16");
        p->bytes_per_sector = bps;
        p->sectors_per_cluster = spc;
        p->cluster_size = bps * spc;
        p->reserved_sectors = rsvd;
        p->num_fats = fats;
        p->fat_size_sectors = fat_sz_16;

        char label[12];
        memcpy(label, &sector[43], 11);
        label[11] = '\0';
        for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = '\0';
        if (label[0] != '\0') {
            strncpy(p->vol_label, label, sizeof(p->vol_label) - 1);
        }

        uint32_t root_dir_sectors = ((uint32_t)*(uint16_t *)&sector[17] * 32 + (bps - 1)) / bps;
        uint32_t data_sec = total_sec > (rsvd + fats * fat_sz_16 + root_dir_sectors) ?
            (total_sec - (rsvd + fats * fat_sz_16 + root_dir_sectors)) : 0;
        p->total_clusters = data_sec / spc;
        p->free_clusters = p->total_clusters;
        p->used_clusters = 0;
        p->total_bytes = (uint64_t)p->total_clusters * p->cluster_size;
        p->free_bytes = p->total_bytes;
        p->used_bytes = 0;

        storage_format_size(p->total_bytes, p->total_str, sizeof(p->total_str));
        storage_format_size(p->free_bytes, p->free_str, sizeof(p->free_str));
        storage_format_size(p->used_bytes, p->used_str, sizeof(p->used_str));

        strcpy(p->health_status, "Clean / Healthy");
    }
}

int storage_inspect_drive(StorageDevice *dev, StorageDriveInfo *out_drive_info) {
    if (!dev || !out_drive_info || !dev->read_sectors) return -1;

    memset(out_drive_info, 0, sizeof(StorageDriveInfo));
    strcpy(out_drive_info->part_table_type, "None");
    out_drive_info->partition_count = 0;

    uint8_t mbr[512];
    if (dev->read_sectors(dev, 0, 1, mbr) != 0) {
        return -1;
    }

    int has_valid_mbr = (mbr[510] == 0x55 && mbr[511] == 0xAA);
    int is_gpt = 0;

    if (has_valid_mbr && mbr[446 + 4] == 0xEE) {
        is_gpt = 1;
    }

    if (is_gpt) {
        /* Read LBA 1 (GPT Header) */
        uint8_t gpt_hdr[512];
        if (dev->read_sectors(dev, 1, 1, gpt_hdr) == 0 && memcmp(gpt_hdr, "EFI PART", 8) == 0) {
            out_drive_info->has_partition_table = 1;
            strcpy(out_drive_info->part_table_type, "GPT");

            uint64_t part_lba = *(uint64_t *)&gpt_hdr[72];
            uint32_t num_entries = *(uint32_t *)&gpt_hdr[80];
            uint32_t entry_size = *(uint32_t *)&gpt_hdr[84];

            if (part_lba == 0) part_lba = 2;
            if (num_entries == 0 || num_entries > 128) num_entries = 128;
            if (entry_size == 0) entry_size = 128;

            static const uint8_t esp_guid[16] = {
                0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
            };
            static const uint8_t basic_data_guid[16] = {
                0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
            };
            static const uint8_t linux_fs_guid[16] = {
                0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
            };
            static const uint8_t msr_guid[16] = {
                0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d,
                0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae
            };

            uint32_t entries_per_sec = 512 / entry_size;
            if (entries_per_sec == 0) entries_per_sec = 4;
            uint32_t total_sec = (num_entries + entries_per_sec - 1) / entries_per_sec;

            uint8_t sec_buf[512];
            uint32_t checked_entries = 0;

            for (uint32_t s = 0; s < total_sec && checked_entries < num_entries; s++) {
                if (dev->read_sectors(dev, part_lba + s, 1, sec_buf) != 0) {
                    break;
                }

                for (uint32_t e = 0; e < entries_per_sec && checked_entries < num_entries; e++) {
                    uint8_t *entry = &sec_buf[e * entry_size];
                    checked_entries++;

                    int is_unused = 1;
                    for (int b = 0; b < 16; b++) {
                        if (entry[b] != 0) {
                            is_unused = 0;
                            break;
                        }
                    }
                    if (is_unused) continue;

                    uint64_t start = *(uint64_t *)&entry[32];
                    uint64_t end = *(uint64_t *)&entry[40];
                    if (end < start || start == 0) continue;

                    if (out_drive_info->partition_count < 16) {
                        StoragePartitionInfo *p = &out_drive_info->partitions[out_drive_info->partition_count];
                        memset(p, 0, sizeof(StoragePartitionInfo));
                        p->part_index = out_drive_info->partition_count + 1;
                        p->start_lba = start;
                        p->end_lba = end;
                        p->total_sectors = (end - start + 1);

                        if (memcmp(entry, esp_guid, 16) == 0) {
                            strcpy(p->part_type_name, "EFI System Partition (ESP)");
                        } else if (memcmp(entry, basic_data_guid, 16) == 0) {
                            strcpy(p->part_type_name, "Basic Data Partition");
                        } else if (memcmp(entry, linux_fs_guid, 16) == 0) {
                            strcpy(p->part_type_name, "Linux Filesystem Data");
                        } else if (memcmp(entry, msr_guid, 16) == 0) {
                            strcpy(p->part_type_name, "Microsoft Reserved (MSR)");
                        } else {
                            strcpy(p->part_type_name, "General Data Partition");
                        }

                        storage_format_size(p->total_sectors * dev->sector_size, p->total_str, sizeof(p->total_str));
                        parse_partition_filesystem(dev, p->start_lba, p->end_lba, p);
                        out_drive_info->partition_count++;
                    }
                }
            }
        }
    } else if (has_valid_mbr) {
        /* Legacy MBR partition inspection */
        for (int i = 0; i < 4; i++) {
            uint8_t *pte = &mbr[446 + i * 16];
            uint8_t part_type = pte[4];
            if (part_type == 0) continue;

            out_drive_info->has_partition_table = 1;
            strcpy(out_drive_info->part_table_type, "MBR");

            if (out_drive_info->partition_count < 16) {
                StoragePartitionInfo *p = &out_drive_info->partitions[out_drive_info->partition_count];
                memset(p, 0, sizeof(StoragePartitionInfo));
                p->part_index = out_drive_info->partition_count + 1;
                p->start_lba = *(uint32_t *)&pte[8];
                p->total_sectors = *(uint32_t *)&pte[12];
                p->end_lba = (p->total_sectors > 0) ? (p->start_lba + p->total_sectors - 1) : p->start_lba;

                switch (part_type) {
                    case 0xEF:
                        strcpy(p->part_type_name, "EFI System Partition");
                        break;
                    case 0x07:
                        strcpy(p->part_type_name, "NTFS / exFAT Partition");
                        break;
                    case 0x0B:
                    case 0x0C:
                        strcpy(p->part_type_name, "FAT32 Data Partition");
                        break;
                    case 0x06:
                    case 0x0E:
                        strcpy(p->part_type_name, "FAT16 Data Partition");
                        break;
                    case 0x83:
                        strcpy(p->part_type_name, "Linux Native Partition");
                        break;
                    default:
                        snprintf(p->part_type_name, sizeof(p->part_type_name), "MBR Partition (Type 0x%02X)", part_type);
                        break;
                }

                storage_format_size(p->total_sectors * dev->sector_size, p->total_str, sizeof(p->total_str));
                parse_partition_filesystem(dev, p->start_lba, p->end_lba, p);
                out_drive_info->partition_count++;
            }
        }
    }

    /* If no partition table found, check if disk is directly formatted (superfloppy style) */
    if (out_drive_info->partition_count == 0 && has_valid_mbr) {
        uint16_t bps = *(uint16_t *)&mbr[11];
        uint8_t spc = mbr[13];
        if (bps >= 512 && spc > 0) {
            StoragePartitionInfo *p = &out_drive_info->partitions[0];
            memset(p, 0, sizeof(StoragePartitionInfo));
            p->part_index = 1;
            p->start_lba = 0;
            p->end_lba = (dev->total_sectors > 0) ? (dev->total_sectors - 1) : 0;
            p->total_sectors = dev->total_sectors;
            strcpy(p->part_type_name, "Superfloppy / Raw Volume");
            storage_format_size(p->total_sectors * dev->sector_size, p->total_str, sizeof(p->total_str));
            parse_partition_filesystem(dev, 0, p->end_lba, p);
            if (p->has_fs) {
                out_drive_info->partition_count = 1;
            }
        }
    }

    return 0;
}

int storage_inspect_fs(StorageDevice *dev, StorageFsInfo *out_info) {
    if (!dev || !out_info) return -1;

    memset(out_info, 0, sizeof(StorageFsInfo));

    StorageDriveInfo drive_info;
    if (storage_inspect_drive(dev, &drive_info) != 0) return -1;

    out_info->has_partition_table = drive_info.has_partition_table;
    strncpy(out_info->part_table_type, drive_info.part_table_type, sizeof(out_info->part_table_type) - 1);

    if (drive_info.partition_count > 0) {
        StoragePartitionInfo *selected = &drive_info.partitions[0];
        for (uint32_t i = 0; i < drive_info.partition_count; i++) {
            if (drive_info.partitions[i].has_fs) {
                selected = &drive_info.partitions[i];
                break;
            }
        }

        strncpy(out_info->part_type_name, selected->part_type_name, sizeof(out_info->part_type_name) - 1);
        out_info->part_start_lba = selected->start_lba;
        out_info->part_end_lba = selected->end_lba;
        out_info->part_total_sectors = selected->total_sectors;

        out_info->has_filesystem = selected->has_fs;
        strncpy(out_info->fs_type, selected->fs_type, sizeof(out_info->fs_type) - 1);
        strncpy(out_info->vol_label, selected->vol_label, sizeof(out_info->vol_label) - 1);
        strncpy(out_info->oem_name, selected->oem_name, sizeof(out_info->oem_name) - 1);
        out_info->bytes_per_sector = selected->bytes_per_sector;
        out_info->sectors_per_cluster = selected->sectors_per_cluster;
        out_info->cluster_size = selected->cluster_size;
        out_info->reserved_sectors = selected->reserved_sectors;
        out_info->num_fats = selected->num_fats;
        out_info->fat_size_sectors = selected->fat_size_sectors;
        out_info->root_cluster = selected->root_cluster;

        out_info->total_clusters = selected->total_clusters;
        out_info->free_clusters = selected->free_clusters;
        out_info->used_clusters = selected->used_clusters;

        out_info->total_bytes = selected->total_bytes;
        out_info->free_bytes = selected->free_bytes;
        out_info->used_bytes = selected->used_bytes;

        strncpy(out_info->total_str, selected->total_str, sizeof(out_info->total_str) - 1);
        strncpy(out_info->free_str, selected->free_str, sizeof(out_info->free_str) - 1);
        strncpy(out_info->used_str, selected->used_str, sizeof(out_info->used_str) - 1);
        strncpy(out_info->health_status, selected->health_status, sizeof(out_info->health_status) - 1);
    } else {
        strcpy(out_info->part_type_name, "Unpartitioned");
        strcpy(out_info->fs_type, "RAW / Unformatted");
        strcpy(out_info->vol_label, "NO NAME");
        strcpy(out_info->oem_name, "UNKNOWN");
        strcpy(out_info->health_status, "Unformatted");
    }

    return 0;
}

void storage_init(void) {
    g_device_count = 0;
    memset(g_devices, 0, sizeof(g_devices));

    /* 1. Discover Internal AHCI SATA Controllers and Drives */
    ahci_init();

    /* 2. Discover Internal NVMe PCIe Controllers and Namespaces */
    nvme_init();

    /* 3. Discover External USB Host Controllers and Mass Storage Drives */
    usb_storage_init();
}

StorageDevice *storage_get_boot_device(const char *boot_devpath, int *out_drive_index) {
    if (out_drive_index) *out_drive_index = 0;
    if (!boot_devpath || boot_devpath[0] == '\0') {
        boot_devpath = g_boot_location.base_hardware_path;
    }
    if (!boot_devpath || boot_devpath[0] == '\0') return NULL;

    /* Optical CD-ROM / DVD-ROM media is an ISO volume, not a registered writable disk */
    if (strstr(boot_devpath, "CDROM") || strstr(boot_devpath, "CdRom") || strstr(boot_devpath, "cdrom")) {
        return NULL;
    }

    /* 1. Try exact or substring match with registered device devpaths */
    for (uint32_t i = 0; i < g_device_count; i++) {
        StorageDevice *dev = &g_devices[i];
        if (dev->devpath[0] != '\0' && strstr(boot_devpath, dev->devpath) != NULL) {
            if (out_drive_index) *out_drive_index = (int)(i + 1);
            return dev;
        }
    }

    /* 2. Match by protocol identifier if devpath formatting differs across firmware versions */
    if (strstr(boot_devpath, "Sata(") || strstr(boot_devpath, "SATA(") || strstr(boot_devpath, "sata(")) {
        for (uint32_t i = 0; i < g_device_count; i++) {
            if (g_devices[i].type == STORAGE_TYPE_INTERNAL_SATA) {
                if (out_drive_index) *out_drive_index = (int)(i + 1);
                return &g_devices[i];
            }
        }
    } else if (strstr(boot_devpath, "NVMe(") || strstr(boot_devpath, "Nvme(") || strstr(boot_devpath, "nvme(")) {
        for (uint32_t i = 0; i < g_device_count; i++) {
            if (g_devices[i].type == STORAGE_TYPE_INTERNAL_NVME) {
                if (out_drive_index) *out_drive_index = (int)(i + 1);
                return &g_devices[i];
            }
        }
    } else if (strstr(boot_devpath, "Usb(") || strstr(boot_devpath, "USB(") || strstr(boot_devpath, "usb(")) {
        for (uint32_t i = 0; i < g_device_count; i++) {
            if (g_devices[i].type == STORAGE_TYPE_EXTERNAL_USB) {
                if (out_drive_index) *out_drive_index = (int)(i + 1);
                return &g_devices[i];
            }
        }
    }

    return NULL;
}
