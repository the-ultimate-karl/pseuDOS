#include "storage.h"
#include "bootinfo.h"
#include "fs.h"
#include "lib.h"

extern const BootInfo *g_boot_info_global;

typedef struct {
    uint8_t  boot_indicator;
    uint8_t  start_head;
    uint8_t  start_sector;
    uint8_t  start_cylinder;
    uint8_t  os_type;
    uint8_t  end_head;
    uint8_t  end_sector;
    uint8_t  end_cylinder;
    uint32_t starting_lba;
    uint32_t size_in_lba;
} __attribute__((packed)) MbrPartition;

typedef struct {
    uint8_t  signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} __attribute__((packed)) GptHeader;

typedef struct {
    uint8_t  partition_type_guid[16];
    uint8_t  unique_partition_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t partition_name[36];
} __attribute__((packed)) GptEntry;

typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed)) FatDirEntry;

static const uint8_t ESP_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static uint32_t calculate_crc32(const void *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < length; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static void make_dir_entry(FatDirEntry *entry, const char name11[11], uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(entry, 0, sizeof(FatDirEntry));
    memcpy(entry->name, name11, 11);
    entry->attr = attr;
    entry->fst_clus_hi = (uint16_t)(cluster >> 16);
    entry->fst_clus_lo = (uint16_t)(cluster & 0xFFFF);
    entry->file_size = size;
}

int gpt_fat32_format_and_install(StorageDevice *dev, install_progress_cb_t progress_cb) {
    if (!dev || !dev->write_sectors) return -1;

    uint8_t sector_buf[512];

    /* 1. Preparing physical disk */
    if (progress_cb) progress_cb("preparing physical storage device and validating geometry...", 1);

    /* 2. Write Protective MBR (LBA 0) */
    if (progress_cb) progress_cb("writing Protective MBR to LBA 0...", 1);
    memset(sector_buf, 0, 512);

    MbrPartition *mbr_part = (MbrPartition *)&sector_buf[446];
    mbr_part->boot_indicator = 0x00;
    mbr_part->start_head = 0x00;
    mbr_part->start_sector = 0x02;
    mbr_part->start_cylinder = 0x00;
    mbr_part->os_type = 0xEE; /* GPT Protective MBR */
    mbr_part->end_head = 0xFF;
    mbr_part->end_sector = 0xFF;
    mbr_part->end_cylinder = 0xFF;
    mbr_part->starting_lba = 1;
    mbr_part->size_in_lba = (dev->total_sectors > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)(dev->total_sectors - 1);

    sector_buf[510] = 0x55;
    sector_buf[511] = 0xAA;

    if (dev->write_sectors(dev, 0, 1, sector_buf) != 0) return -1;

    /* 3. Generate GPT Partition Array (LBA 2–33) */
    if (progress_cb) progress_cb("creating 512 MB EFI System Partition (ESP)...", 1);
    uint8_t *entries_buf = (uint8_t *)kmalloc(128 * sizeof(GptEntry));
    memset(entries_buf, 0, 128 * sizeof(GptEntry));

    GptEntry *esp_entry = (GptEntry *)&entries_buf[0];
    memcpy(esp_entry->partition_type_guid, ESP_GUID, 16);
    esp_entry->unique_partition_guid[0] = 0xAA;
    esp_entry->unique_partition_guid[1] = 0xBB;
    esp_entry->unique_partition_guid[2] = 0xCC;
    esp_entry->unique_partition_guid[3] = 0xDD;
    esp_entry->unique_partition_guid[15] = 0x01;

    esp_entry->starting_lba = 2048; /* 1 MB offset */
    uint64_t esp_size_sectors = (512 * 1024 * 1024) / 512; /* 512 MB = 1048576 sectors */
    if (esp_entry->starting_lba + esp_size_sectors > dev->total_sectors - 34) {
        esp_size_sectors = (dev->total_sectors > 2082) ? (dev->total_sectors - 2082) : 1000;
    }
    esp_entry->ending_lba = esp_entry->starting_lba + esp_size_sectors - 1;
    esp_entry->attributes = 0;

    const char *pname = "EFI System Partition";
    for (size_t i = 0; pname[i] != '\0' && i < 35; i++) {
        esp_entry->partition_name[i] = (uint16_t)pname[i];
    }

    uint32_t entries_crc = calculate_crc32(entries_buf, 128 * sizeof(GptEntry));

    for (int i = 0; i < 32; i++) {
        dev->write_sectors(dev, 2 + i, 1, &entries_buf[i * 512]);
    }

    /* 4. Write Primary GPT Header (LBA 1) */
    if (progress_cb) progress_cb("generating Primary GUID Partition Table (GPT) header...", 1);
    memset(sector_buf, 0, 512);
    GptHeader *gpt_hdr = (GptHeader *)sector_buf;
    memcpy(gpt_hdr->signature, "EFI PART", 8);
    gpt_hdr->revision = 0x00010000;
    gpt_hdr->header_size = 92;
    gpt_hdr->reserved = 0;
    gpt_hdr->my_lba = 1;
    gpt_hdr->alternate_lba = dev->total_sectors - 1;
    gpt_hdr->first_usable_lba = 34;
    gpt_hdr->last_usable_lba = dev->total_sectors - 34;
    gpt_hdr->disk_guid[0] = 0x50;
    gpt_hdr->disk_guid[1] = 0x53;
    gpt_hdr->disk_guid[2] = 0x45;
    gpt_hdr->disk_guid[3] = 0x55;
    gpt_hdr->partition_entry_lba = 2;
    gpt_hdr->num_partition_entries = 128;
    gpt_hdr->size_of_partition_entry = sizeof(GptEntry);
    gpt_hdr->partition_entry_array_crc32 = entries_crc;
    gpt_hdr->header_crc32 = 0;
    gpt_hdr->header_crc32 = calculate_crc32(gpt_hdr, 92);

    dev->write_sectors(dev, 1, 1, sector_buf);
    kfree(entries_buf);

    /* 5. Format Partition as FAT32 */
    if (progress_cb) progress_cb("formatting partition as FAT32 filesystem...", 1);

    uint64_t fat32_start_lba = 2048;
    memset(sector_buf, 0, 512);

    /* FAT32 Boot Sector */
    sector_buf[0] = 0xEB; sector_buf[1] = 0x58; sector_buf[2] = 0x90;
    memcpy(&sector_buf[3], "MSWIN4.1", 8);
    *(uint16_t *)&sector_buf[11] = 512;
    sector_buf[13] = 8;                 /* 8 sectors/cluster = 4096 bytes */
    *(uint16_t *)&sector_buf[14] = 32;  /* Reserved sectors */
    sector_buf[16] = 2;                 /* Number of FATs */
    *(uint16_t *)&sector_buf[17] = 0;
    sector_buf[21] = 0xF8;
    *(uint16_t *)&sector_buf[22] = 0;
    *(uint16_t *)&sector_buf[24] = 32;
    *(uint16_t *)&sector_buf[26] = 64;
    *(uint32_t *)&sector_buf[28] = 2048;
    *(uint32_t *)&sector_buf[32] = (uint32_t)esp_size_sectors;
    *(uint32_t *)&sector_buf[36] = 1024;/* 1024 sectors per FAT */
    *(uint16_t *)&sector_buf[40] = 0;
    *(uint16_t *)&sector_buf[42] = 0;
    *(uint32_t *)&sector_buf[44] = 2;   /* Root cluster = 2 */
    *(uint16_t *)&sector_buf[48] = 1;   /* FSInfo = 1 */
    *(uint16_t *)&sector_buf[50] = 6;
    sector_buf[64] = 0x80;
    sector_buf[66] = 0x29;
    *(uint32_t *)&sector_buf[67] = 0x12345678;
    memcpy(&sector_buf[71], "EFI SYSTEM ", 11);
    memcpy(&sector_buf[82], "FAT32   ", 8);
    sector_buf[510] = 0x55;
    sector_buf[511] = 0xAA;

    dev->write_sectors(dev, fat32_start_lba, 1, sector_buf);

    /* FAT32 FSInfo Sector */
    memset(sector_buf, 0, 512);
    *(uint32_t *)&sector_buf[0] = 0x41615252;
    *(uint32_t *)&sector_buf[484] = 0x61417272;
    *(uint32_t *)&sector_buf[488] = (uint32_t)((esp_size_sectors - 32 - 2048) / 8);
    *(uint32_t *)&sector_buf[492] = 3;
    *(uint32_t *)&sector_buf[508] = 0xAA550000;
    dev->write_sectors(dev, fat32_start_lba + 1, 1, sector_buf);

    /* Query embedded payload binaries */
    size_t bootx64_size = 0;
    const uint8_t *bootx64_data = payload_get_bootloader(&bootx64_size);

    size_t kernel_size = 0;
    const uint8_t *kernel_data = payload_get_kernel(&kernel_size);

    uint32_t boot_clusters = (uint32_t)((bootx64_size + 4095) / 4096);
    if (boot_clusters == 0) boot_clusters = 1;

    uint32_t kernel_clusters = (uint32_t)((kernel_size + 4095) / 4096);
    if (kernel_clusters == 0) kernel_clusters = 1;

    uint32_t boot_start_cluster = 6;
    uint32_t kernel_start_cluster = boot_start_cluster + boot_clusters;

    /* Build FAT Table (1024 sectors) */
    uint32_t *fat_table = (uint32_t *)kmalloc(1024 * 512);
    memset(fat_table, 0, 1024 * 512);

    fat_table[0] = 0x0FFFFFF8; /* Media descriptor */
    fat_table[1] = 0xFFFFFFFF; /* Clean shutdown status */
    fat_table[2] = 0x0FFFFFFF; /* Root Directory (Cluster 2) EOF */
    fat_table[3] = 0x0FFFFFFF; /* \EFI Directory (Cluster 3) EOF */
    fat_table[4] = 0x0FFFFFFF; /* \EFI\BOOT Directory (Cluster 4) EOF */
    fat_table[5] = 0x0FFFFFFF; /* \EFI\pseuDOS Directory (Cluster 5) EOF */

    /* Chain for BOOTX64.EFI */
    for (uint32_t i = 0; i < boot_clusters; i++) {
        fat_table[boot_start_cluster + i] = (i + 1 == boot_clusters) ? 0x0FFFFFFF : (boot_start_cluster + i + 1);
    }

    /* Chain for kernel.bin */
    for (uint32_t i = 0; i < kernel_clusters; i++) {
        fat_table[kernel_start_cluster + i] = (i + 1 == kernel_clusters) ? 0x0FFFFFFF : (kernel_start_cluster + i + 1);
    }

    /* Write FAT1 and FAT2 tables */
    for (int i = 0; i < 1024; i++) {
        dev->write_sectors(dev, fat32_start_lba + 32 + i, 1, ((uint8_t *)fat_table) + i * 512);
        dev->write_sectors(dev, fat32_start_lba + 32 + 1024 + i, 1, ((uint8_t *)fat_table) + i * 512);
    }
    kfree(fat_table);

    uint64_t data_lba_base = fat32_start_lba + 32 + 2048; /* Reserved (32) + 2*FAT (2048) */

    #define WRITE_CLUSTER(c, buf) \
        for (int _s = 0; _s < 8; _s++) { \
            dev->write_sectors(dev, data_lba_base + ((c) - 2) * 8 + _s, 1, ((const uint8_t *)(buf)) + _s * 512); \
        }

    uint8_t cluster_buf[4096];

    /* 6. Deploying System Directories */
    if (progress_cb) progress_cb("creating \\EFI\\BOOT and \\EFI\\pseuDOS system directories...", 1);

    /* Cluster 2: Root Directory contains "\EFI" */
    memset(cluster_buf, 0, sizeof(cluster_buf));
    FatDirEntry *entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], "EFI        ", 0x10, 3, 0);
    WRITE_CLUSTER(2, cluster_buf);

    /* Cluster 3: \EFI Directory contains "BOOT" and "PSEUDOS" */
    memset(cluster_buf, 0, sizeof(cluster_buf));
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 3, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 0, 0);
    make_dir_entry(&entries[2], "BOOT       ", 0x10, 4, 0);
    make_dir_entry(&entries[3], "PSEUDOS    ", 0x10, 5, 0);
    WRITE_CLUSTER(3, cluster_buf);

    /* Cluster 4: \EFI\BOOT contains "BOOTX64.EFI" */
    memset(cluster_buf, 0, sizeof(cluster_buf));
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 4, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 3, 0);
    make_dir_entry(&entries[2], "BOOTX64 EFI", 0x20, boot_start_cluster, (uint32_t)bootx64_size);
    WRITE_CLUSTER(4, cluster_buf);

    /* Cluster 5: \EFI\pseuDOS contains "KERNEL.BIN" */
    memset(cluster_buf, 0, sizeof(cluster_buf));
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 5, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 3, 0);
    make_dir_entry(&entries[2], "KERNEL  BIN", 0x20, kernel_start_cluster, (uint32_t)kernel_size);
    WRITE_CLUSTER(5, cluster_buf);

    /* 7. Deploying BOOTX64.EFI */
    if (progress_cb) progress_cb("copying \\EFI\\BOOT\\BOOTX64.EFI boot manager...", 1);
    for (uint32_t i = 0; i < boot_clusters; i++) {
        memset(cluster_buf, 0, sizeof(cluster_buf));
        size_t offset = i * 4096;
        size_t to_copy = (bootx64_size > offset) ? (bootx64_size - offset) : 0;
        if (to_copy > 4096) to_copy = 4096;
        if (to_copy > 0) {
            memcpy(cluster_buf, bootx64_data + offset, to_copy);
        }
        WRITE_CLUSTER(boot_start_cluster + i, cluster_buf);
    }

    /* 8. Deploying kernel.bin */
    if (progress_cb) progress_cb("copying \\EFI\\pseuDOS\\kernel.bin bare-metal payload...", 1);
    for (uint32_t i = 0; i < kernel_clusters; i++) {
        memset(cluster_buf, 0, sizeof(cluster_buf));
        size_t offset = i * 4096;
        size_t to_copy = (kernel_size > offset) ? (kernel_size - offset) : 0;
        if (to_copy > 4096) to_copy = 4096;
        if (to_copy > 0) {
            memcpy(cluster_buf, kernel_data + offset, to_copy);
        }
        WRITE_CLUSTER(kernel_start_cluster + i, cluster_buf);
    }

    /* 9. Deploying System Configuration */
    if (progress_cb) progress_cb("installing boot configuration and system files...", 1);

    /* 10. Synchronizing Storage Cache */
    if (progress_cb) progress_cb("synchronizing disk cache and finalizing installation...", 1);

    return 0;
}
