#include "storage.h"
#include "drivers.h"
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

int storage_inspect_fs(StorageDevice *dev, StorageFsInfo *out_info) {
    if (!dev || !out_info || !dev->read_sectors) return -1;

    memset(out_info, 0, sizeof(StorageFsInfo));
    strcpy(out_info->part_table_type, "None");
    strcpy(out_info->part_type_name, "Unpartitioned");
    strcpy(out_info->fs_type, "RAW / Unformatted");
    strcpy(out_info->vol_label, "NO NAME");
    strcpy(out_info->oem_name, "UNKNOWN");
    strcpy(out_info->health_status, "Unformatted");

    uint8_t sector[512];
    uint64_t part_start_lba = 0;
    uint64_t part_end_lba = 0;

    /* 1. Read LBA 0 (Protective MBR / Legacy MBR) */
    if (dev->read_sectors(dev, 0, 1, sector) != 0) {
        return -1;
    }

    int has_valid_mbr = (sector[510] == 0x55 && sector[511] == 0xAA);
    int is_gpt = 0;

    if (has_valid_mbr) {
        /* Check if Partition 1 has type 0xEE (GPT Protective) */
        if (sector[446 + 4] == 0xEE) {
            is_gpt = 1;
        }
    }

    if (is_gpt) {
        /* 2. Read LBA 1 (GPT Header) */
        if (dev->read_sectors(dev, 1, 1, sector) == 0) {
            if (memcmp(sector, "EFI PART", 8) == 0) {
                out_info->has_partition_table = 1;
                strcpy(out_info->part_table_type, "GPT");

                /* 3. Read LBA 2 (GPT Partition Entries) */
                if (dev->read_sectors(dev, 2, 1, sector) == 0) {
                    /* Check Partition 1 Type GUID (offset 0..15) */
                    static const uint8_t esp_guid[16] = {
                        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
                    };

                    if (memcmp(sector, esp_guid, 16) == 0) {
                        strcpy(out_info->part_type_name, "EFI System Partition (ESP)");
                    } else {
                        strcpy(out_info->part_type_name, "Basic Data Partition");
                    }

                    part_start_lba = *(uint64_t *)&sector[32];
                    part_end_lba = *(uint64_t *)&sector[40];

                    if (part_end_lba >= part_start_lba && part_start_lba > 0) {
                        out_info->part_start_lba = part_start_lba;
                        out_info->part_end_lba = part_end_lba;
                        out_info->part_total_sectors = (part_end_lba - part_start_lba + 1);
                    }
                }
            }
        }
    } else if (has_valid_mbr) {
        /* Legacy MBR */
        uint8_t part_type = sector[446 + 4];
        if (part_type != 0) {
            out_info->has_partition_table = 1;
            strcpy(out_info->part_table_type, "MBR");
            snprintf(out_info->part_type_name, sizeof(out_info->part_type_name), "Type 0x%02X", part_type);
            part_start_lba = *(uint32_t *)&sector[446 + 8];
            uint32_t total_sec = *(uint32_t *)&sector[446 + 12];
            out_info->part_start_lba = part_start_lba;
            out_info->part_end_lba = part_start_lba + total_sec - 1;
            out_info->part_total_sectors = total_sec;
        }
    }

    if (part_start_lba == 0) {
        part_start_lba = 2048; /* Standard default ESP offset */
    }

    /* 4. Read BPB Boot Sector at partition start */
    if (dev->read_sectors(dev, part_start_lba, 1, sector) == 0) {
        if (sector[510] == 0x55 && sector[511] == 0xAA) {
            char fs_id[9];
            memcpy(fs_id, &sector[82], 8);
            fs_id[8] = '\0';

            char oem[9];
            memcpy(oem, &sector[3], 8);
            oem[8] = '\0';
            /* Strip trailing spaces */
            for (int i = 7; i >= 0 && oem[i] == ' '; i--) oem[i] = '\0';
            strncpy(out_info->oem_name, oem, sizeof(out_info->oem_name) - 1);

            uint16_t bps = *(uint16_t *)&sector[11];
            uint8_t spc = sector[13];
            uint16_t rsvd = *(uint16_t *)&sector[14];
            uint8_t fats = sector[16];
            uint32_t tot_sec_32 = *(uint32_t *)&sector[32];
            uint16_t tot_sec_16 = *(uint16_t *)&sector[19];
            uint32_t total_sec = tot_sec_32 != 0 ? tot_sec_32 : tot_sec_16;
            uint32_t fat_sz = *(uint32_t *)&sector[36];
            uint32_t root_clus = *(uint32_t *)&sector[44];

            if (bps >= 512 && spc > 0 && fats > 0 && fat_sz > 0) {
                out_info->has_filesystem = 1;
                strcpy(out_info->fs_type, "FAT32");
                out_info->bytes_per_sector = bps;
                out_info->sectors_per_cluster = spc;
                out_info->cluster_size = bps * spc;
                out_info->reserved_sectors = rsvd;
                out_info->num_fats = fats;
                out_info->fat_size_sectors = fat_sz;
                out_info->root_cluster = root_clus;

                char label[12];
                memcpy(label, &sector[71], 11);
                label[11] = '\0';
                for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = '\0';
                if (label[0] != '\0') {
                    strncpy(out_info->vol_label, label, sizeof(out_info->vol_label) - 1);
                }

                uint32_t data_sec = total_sec > (rsvd + fats * fat_sz) ? (total_sec - (rsvd + fats * fat_sz)) : 0;
                out_info->total_clusters = data_sec / spc;
                out_info->free_clusters = out_info->total_clusters;
                out_info->used_clusters = 0;

                /* 5. Read FSInfo Sector */
                uint8_t fsinfo[512];
                if (dev->read_sectors(dev, part_start_lba + 1, 1, fsinfo) == 0) {
                    uint32_t lead_sig = *(uint32_t *)&fsinfo[0];
                    uint32_t struct_sig = *(uint32_t *)&fsinfo[484];
                    if (lead_sig == 0x41615252 && struct_sig == 0x61417272) {
                        uint32_t free_c = *(uint32_t *)&fsinfo[488];
                        if (free_c != 0xFFFFFFFF && free_c <= out_info->total_clusters) {
                            out_info->free_clusters = free_c;
                            out_info->used_clusters = out_info->total_clusters - free_c;
                        }
                    }
                }

                out_info->total_bytes = (uint64_t)out_info->total_clusters * out_info->cluster_size;
                out_info->free_bytes = (uint64_t)out_info->free_clusters * out_info->cluster_size;
                out_info->used_bytes = (uint64_t)out_info->used_clusters * out_info->cluster_size;

                storage_format_size(out_info->total_bytes, out_info->total_str, sizeof(out_info->total_str));
                storage_format_size(out_info->free_bytes, out_info->free_str, sizeof(out_info->free_str));
                storage_format_size(out_info->used_bytes, out_info->used_str, sizeof(out_info->used_str));

                strcpy(out_info->health_status, "Clean / Healthy");
            }
        }
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
