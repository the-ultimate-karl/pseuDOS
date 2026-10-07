#include "storage.h"
#include "bootinfo.h"
#include "fs.h"
#include "lib.h"

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

static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void generate_guid(uint8_t out_guid[16], uint32_t seed) {
    uint64_t tsc = read_tsc() ^ (uint64_t)seed;
    for (int i = 0; i < 8; i++) {
        out_guid[i] = (uint8_t)(tsc >> (i * 8));
    }
    tsc = read_tsc() ^ (~(uint64_t)seed);
    for (int i = 0; i < 8; i++) {
        out_guid[8 + i] = (uint8_t)(tsc >> (i * 8));
    }
    out_guid[6] = (out_guid[6] & 0x0F) | 0x40; /* Version 4 */
    out_guid[8] = (out_guid[8] & 0x3F) | 0x80; /* Variant 1 */
}

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
    if (!dev || !dev->write_sectors || dev->total_sectors < 4096) return -1;

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

    /* 3. Compute Dynamic Partition Geometry */
    uint64_t start_lba = 2048; /* Standard 1 MB alignment offset */
    uint64_t last_usable_lba = dev->total_sectors - 34; /* Leave 33 LBAs for Backup GPT + Header */
    
    uint64_t esp_size_sectors = 0;
    if (dev->total_sectors <= 2097152) {
        /* Under 1 GB: Allocate entire remaining disk capacity for ESP */
        esp_size_sectors = (last_usable_lba >= start_lba) ? (last_usable_lba - start_lba + 1) : 1000;
    } else {
        /* Over 1 GB: Standard 512 MB (1048576 sectors) EFI System Partition */
        esp_size_sectors = 1048576;
    }
    uint64_t end_lba = start_lba + esp_size_sectors - 1;

    /* 4. Generate GPT Partition Array (128 entries = 32 sectors) */
    if (progress_cb) progress_cb("creating dynamic EFI System Partition (ESP)...", 1);
    uint8_t *entries_buf = (uint8_t *)kmalloc(128 * sizeof(GptEntry));
    if (!entries_buf) return -1;
    memset(entries_buf, 0, 128 * sizeof(GptEntry));

    GptEntry *esp_entry = (GptEntry *)&entries_buf[0];
    memcpy(esp_entry->partition_type_guid, ESP_GUID, 16);
    generate_guid(esp_entry->unique_partition_guid, 0x12345678);

    esp_entry->starting_lba = start_lba;
    esp_entry->ending_lba = end_lba;
    esp_entry->attributes = 0;

    const char *pname = "EFI System Partition";
    for (size_t i = 0; pname[i] != '\0' && i < 35; i++) {
        esp_entry->partition_name[i] = (uint16_t)pname[i];
    }

    uint32_t entries_crc = calculate_crc32(entries_buf, 128 * sizeof(GptEntry));

    /* Write Primary Partition Array (LBA 2–33) */
    for (int i = 0; i < 32; i++) {
        dev->write_sectors(dev, 2 + i, 1, &entries_buf[i * 512]);
    }

    /* Write Secondary (Backup) Partition Array (LBA dev->total_sectors - 33 to -2) */
    uint64_t backup_entries_lba = dev->total_sectors - 33;
    for (int i = 0; i < 32; i++) {
        dev->write_sectors(dev, backup_entries_lba + i, 1, &entries_buf[i * 512]);
    }
    kfree(entries_buf);

    /* 5. Write Primary and Backup GPT Headers */
    if (progress_cb) progress_cb("generating Primary & Backup GUID Partition Table headers...", 1);
    
    uint8_t disk_guid[16];
    generate_guid(disk_guid, 0x87654321);

    /* Primary GPT Header (LBA 1) */
    memset(sector_buf, 0, 512);
    GptHeader *gpt_hdr = (GptHeader *)sector_buf;
    memcpy(gpt_hdr->signature, "EFI PART", 8);
    gpt_hdr->revision = 0x00010000;
    gpt_hdr->header_size = 92;
    gpt_hdr->reserved = 0;
    gpt_hdr->my_lba = 1;
    gpt_hdr->alternate_lba = dev->total_sectors - 1;
    gpt_hdr->first_usable_lba = 34;
    gpt_hdr->last_usable_lba = last_usable_lba;
    memcpy(gpt_hdr->disk_guid, disk_guid, 16);
    gpt_hdr->partition_entry_lba = 2;
    gpt_hdr->num_partition_entries = 128;
    gpt_hdr->size_of_partition_entry = sizeof(GptEntry);
    gpt_hdr->partition_entry_array_crc32 = entries_crc;
    gpt_hdr->header_crc32 = 0;
    gpt_hdr->header_crc32 = calculate_crc32(gpt_hdr, 92);

    dev->write_sectors(dev, 1, 1, sector_buf);

    /* Secondary (Backup) GPT Header (LBA dev->total_sectors - 1) */
    gpt_hdr->my_lba = dev->total_sectors - 1;
    gpt_hdr->alternate_lba = 1;
    gpt_hdr->partition_entry_lba = backup_entries_lba;
    gpt_hdr->header_crc32 = 0;
    gpt_hdr->header_crc32 = calculate_crc32(gpt_hdr, 92);

    dev->write_sectors(dev, dev->total_sectors - 1, 1, sector_buf);

    /* 6. Dynamic FAT32 Calculations */
    if (progress_cb) progress_cb("calculating dynamic FAT32 cluster geometry & parameters...", 1);

    uint8_t spc = 8; /* 8 sectors/cluster = 4096 bytes */
    if (esp_size_sectors > 33554432) {
        spc = 16;    /* > 16 GB */
    }

    uint16_t reserved_sectors = 32;
    uint8_t num_fats = 2;
    
    /* Dynamic FAT size calculation (Microsoft FAT32 Specification Formula) */
    uint64_t tmp_val1 = esp_size_sectors - reserved_sectors;
    uint64_t tmp_val2 = (uint64_t)(256 * (uint32_t)spc) + num_fats;
    uint32_t fat_size_sectors = (uint32_t)((tmp_val1 + (tmp_val2 - 1)) / tmp_val2);
    if (fat_size_sectors < 32) fat_size_sectors = 32;

    uint32_t total_data_sectors = (uint32_t)(esp_size_sectors - (reserved_sectors + num_fats * fat_size_sectors));
    uint32_t total_clusters = total_data_sectors / spc;

    /* Query embedded payload binaries dynamically */
    typedef struct {
        const char *desc;
        const uint8_t *data;
        size_t size;
        uint32_t clusters;
        uint32_t start_cluster;
    } DynamicPayload;

    DynamicPayload payloads[] = {
        { "\\EFI\\BOOT\\BOOTX64.EFI boot manager", NULL, 0, 0, 0 },
        { "\\protected\\krnl\\vpkernel bare-metal payload", NULL, 0, 0, 0 },
        { "\\protected\\krnl\\autoinit.exe userland init process", NULL, 0, 0, 0 },
        { "\\protected\\crit\\xshss.exe shell subsystem", NULL, 0, 0, 0 },
        { "\\protected\\gui\\superglue.exe display compositor", NULL, 0, 0, 0 },
        { "\\protected\\gui\\ntfs.exe display server", NULL, 0, 0, 0 },
        { "\\protected\\gui\\lack.exe login/auth session", NULL, 0, 0, 0 },
        { "\\protected\\gui\\ninds.exe window manager", NULL, 0, 0, 0 },
        { "\\protected\\gui\\splash.exe splash loader", NULL, 0, 0, 0 },
        { "\\protected\\gui\\gshss.exe GUI desktop shell", NULL, 0, 0, 0 },
        { "\\protected\\apps\\shell.exe terminal app", NULL, 0, 0, 0 },
        { "\\protected\\apps\\sysmon.exe system monitor", NULL, 0, 0, 0 },
        { "\\protected\\apps\\calc.exe calculator app", NULL, 0, 0, 0 },
        { "\\protected\\apps\\notepad.exe notepad app", NULL, 0, 0, 0 },
        { "\\protected\\apps\\paint.exe paint app", NULL, 0, 0, 0 },
        { "\\protected\\apps\\clock.exe clock app", NULL, 0, 0, 0 }
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    payloads[0].data = payload_get_bootloader(&payloads[0].size);
    payloads[1].data = payload_get_kernel(&payloads[1].size);
    payloads[2].data = payload_get_autoinit(&payloads[2].size);
    payloads[3].data = payload_get_xshss(&payloads[3].size);
    payloads[4].data = payload_get_superglue(&payloads[4].size);
    payloads[5].data = payload_get_ntfs(&payloads[5].size);
    payloads[6].data = payload_get_lack(&payloads[6].size);
    payloads[7].data = payload_get_ninds(&payloads[7].size);
    payloads[8].data = payload_get_splash(&payloads[8].size);
    payloads[9].data = payload_get_gshss(&payloads[9].size);
    payloads[10].data = payload_get_shell(&payloads[10].size);
    payloads[11].data = payload_get_sysmon(&payloads[11].size);
    payloads[12].data = payload_get_calc(&payloads[12].size);
    payloads[13].data = payload_get_notepad(&payloads[13].size);
    payloads[14].data = payload_get_paint(&payloads[14].size);
    payloads[15].data = payload_get_clock(&payloads[15].size);

    uint32_t cluster_size_bytes = spc * 512;
    uint32_t next_cluster = 20; /* Clusters 2..19 reserved for directories and static config files */

    for (int p = 0; p < num_payloads; p++) {
        payloads[p].clusters = (uint32_t)((payloads[p].size + cluster_size_bytes - 1) / cluster_size_bytes);
        if (payloads[p].clusters == 0) payloads[p].clusters = 1;
        payloads[p].start_cluster = next_cluster;
        next_cluster += payloads[p].clusters;
    }

    uint32_t total_allocated_clusters = next_cluster - 2;
    uint32_t free_clusters = (total_clusters > total_allocated_clusters) ? (total_clusters - total_allocated_clusters) : 0;
    uint32_t next_free_cluster = next_cluster;

    /* 7. Write FAT32 Boot Sector */
    if (progress_cb) progress_cb("formatting partition as FAT32 filesystem...", 1);

    uint64_t fat32_start_lba = start_lba;
    memset(sector_buf, 0, 512);

    sector_buf[0] = 0xEB; sector_buf[1] = 0x58; sector_buf[2] = 0x90;
    memcpy(&sector_buf[3], "MSWIN4.1", 8);
    *(uint16_t *)&sector_buf[11] = 512;
    sector_buf[13] = spc;
    *(uint16_t *)&sector_buf[14] = reserved_sectors;
    sector_buf[16] = num_fats;
    *(uint16_t *)&sector_buf[17] = 0;
    sector_buf[21] = 0xF8;
    *(uint16_t *)&sector_buf[22] = 0;
    *(uint16_t *)&sector_buf[24] = 32;
    *(uint16_t *)&sector_buf[26] = 64;
    *(uint32_t *)&sector_buf[28] = (uint32_t)start_lba;
    *(uint32_t *)&sector_buf[32] = (uint32_t)esp_size_sectors;
    *(uint32_t *)&sector_buf[36] = fat_size_sectors;
    *(uint16_t *)&sector_buf[40] = 0;
    *(uint16_t *)&sector_buf[42] = 0;
    *(uint32_t *)&sector_buf[44] = 2;   /* Root cluster = 2 */
    *(uint16_t *)&sector_buf[48] = 1;   /* FSInfo = 1 */
    *(uint16_t *)&sector_buf[50] = 6;
    sector_buf[64] = 0x80;
    sector_buf[66] = 0x29;
    *(uint32_t *)&sector_buf[67] = (uint32_t)read_tsc();
    memcpy(&sector_buf[71], "EFI SYSTEM ", 11);
    memcpy(&sector_buf[82], "FAT32   ", 8);
    sector_buf[510] = 0x55;
    sector_buf[511] = 0xAA;

    dev->write_sectors(dev, fat32_start_lba, 1, sector_buf);

    /* 8. Write Dynamic FSInfo Sector */
    memset(sector_buf, 0, 512);
    *(uint32_t *)&sector_buf[0] = 0x41615252;
    *(uint32_t *)&sector_buf[484] = 0x61417272;
    *(uint32_t *)&sector_buf[488] = free_clusters;
    *(uint32_t *)&sector_buf[492] = next_free_cluster;
    *(uint32_t *)&sector_buf[508] = 0xAA550000;
    dev->write_sectors(dev, fat32_start_lba + 1, 1, sector_buf);

    /* 9. Stream Write FAT1 & FAT2 Tables (4KB chunks) */
    if (progress_cb) progress_cb("writing dynamic FAT allocation tables (FAT1 & FAT2)...", 1);

    uint8_t fat_chunk[4096];
    uint32_t total_fat_bytes = fat_size_sectors * 512;
    uint32_t written_fat_bytes = 0;
    uint32_t current_cluster = 0;

    while (written_fat_bytes < total_fat_bytes) {
        memset(fat_chunk, 0, sizeof(fat_chunk));
        uint32_t *entries = (uint32_t *)fat_chunk;
        uint32_t entries_in_chunk = sizeof(fat_chunk) / sizeof(uint32_t); /* 1024 entries */

        for (uint32_t i = 0; i < entries_in_chunk; i++) {
            uint32_t c = current_cluster + i;
            if (c == 0) {
                entries[i] = 0x0FFFFFF8; /* Media descriptor */
            } else if (c == 1) {
                entries[i] = 0xFFFFFFFF; /* Clean shutdown status */
            } else if (c >= 2 && c <= 19) {
                entries[i] = 0x0FFFFFFF; /* EOF for system directories and static config files */
            } else {
                int matched = 0;
                for (int p = 0; p < num_payloads; p++) {
                    if (c >= payloads[p].start_cluster && c < payloads[p].start_cluster + payloads[p].clusters) {
                        uint32_t offset = c - payloads[p].start_cluster;
                        entries[i] = (offset + 1 == payloads[p].clusters) ? 0x0FFFFFFF : (c + 1);
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    entries[i] = 0x00000000; /* Free cluster */
                }
            }
        }

        uint32_t chunk_sectors = sizeof(fat_chunk) / 512;
        uint32_t cur_sector_offset = written_fat_bytes / 512;

        for (uint32_t s = 0; s < chunk_sectors && (cur_sector_offset + s) < fat_size_sectors; s++) {
            /* Write to FAT1 */
            dev->write_sectors(dev, fat32_start_lba + reserved_sectors + cur_sector_offset + s, 1, fat_chunk + s * 512);
            /* Write to FAT2 */
            dev->write_sectors(dev, fat32_start_lba + reserved_sectors + fat_size_sectors + cur_sector_offset + s, 1, fat_chunk + s * 512);
        }

        current_cluster += entries_in_chunk;
        written_fat_bytes += sizeof(fat_chunk);
    }

    uint64_t data_lba_base = fat32_start_lba + reserved_sectors + (uint64_t)num_fats * fat_size_sectors;

    #define WRITE_CLUSTER(c, buf) \
        for (int _s = 0; _s < spc; _s++) { \
            dev->write_sectors(dev, data_lba_base + ((uint64_t)(c) - 2) * spc + _s, 1, ((const uint8_t *)(buf)) + _s * 512); \
        }

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size_bytes);
    if (!cluster_buf) return -1;

    /* 10. Deploying System Directories & Configuration Files */
    if (progress_cb) progress_cb("creating filesystem directories and boot configuration...", 1);

    /* Cluster 2: Root Directory contains "\EFI", "STARTUP.NSH", "\PROTECT", "\HOME", "\TMP", "\SERVICES" */
    memset(cluster_buf, 0, cluster_size_bytes);
    FatDirEntry *entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], "EFI        ", 0x10, 3, 0);
    make_dir_entry(&entries[1], "STARTUP NSH", 0x20, 6, 24);
    make_dir_entry(&entries[2], "PROTECT    ", 0x10, 7, 0);
    make_dir_entry(&entries[3], "HOME       ", 0x10, 16, 0);
    make_dir_entry(&entries[4], "TMP        ", 0x10, 17, 0);
    make_dir_entry(&entries[5], "SERVICES   ", 0x10, 18, 0);
    WRITE_CLUSTER(2, cluster_buf);

    /* Cluster 6: \startup.nsh file */
    memset(cluster_buf, 0, cluster_size_bytes);
    strcpy((char *)cluster_buf, "\\EFI\\BOOT\\BOOTX64.EFI\r\n");
    WRITE_CLUSTER(6, cluster_buf);

    /* Cluster 3: \EFI Directory contains "BOOT" and "PSEUDOS" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 3, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 0, 0);
    make_dir_entry(&entries[2], "BOOT       ", 0x10, 4, 0);
    make_dir_entry(&entries[3], "PSEUDOS    ", 0x10, 5, 0);
    WRITE_CLUSTER(3, cluster_buf);

    /* Cluster 4: \EFI\BOOT contains "BOOTX64.EFI" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 4, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 3, 0);
    make_dir_entry(&entries[2], "BOOTX64 EFI", 0x20, payloads[0].start_cluster, (uint32_t)payloads[0].size);
    WRITE_CLUSTER(4, cluster_buf);

    /* Cluster 12: \EFI\pseuDOS\grub.cfg file */
    const char *grub_content =
        "# GRUB 2 configuration snippet for pseuDOS\r\n"
        "menuentry \"pseuDOS x86_64\" {\r\n"
        "    insmod fat\r\n"
        "    insmod chain\r\n"
        "    search --no-floppy --set=root --file /EFI/pseuDOS/BOOTX64.EFI\r\n"
        "    chainloader /EFI/pseuDOS/BOOTX64.EFI\r\n"
        "}\r\n";
    uint32_t grub_len = (uint32_t)strlen(grub_content);
    memset(cluster_buf, 0, cluster_size_bytes);
    memcpy(cluster_buf, grub_content, grub_len);
    WRITE_CLUSTER(12, cluster_buf);

    /* Cluster 13: \EFI\pseuDOS\os-release file */
    const char *osrelease_content =
        "NAME=\"pseuDOS\"\r\n"
        "ID=pseudos\r\n"
        "VERSION=\"0.6.0\"\r\n"
        "PRETTY_NAME=\"pseuDOS v0.6.0 (x86_64 UEFI)\"\r\n"
        "HOME_URL=\"https://github.com/the-ultimate-karl/pseuDOS\"\r\n";
    uint32_t osrelease_len = (uint32_t)strlen(osrelease_content);
    memset(cluster_buf, 0, cluster_size_bytes);
    memcpy(cluster_buf, osrelease_content, osrelease_len);
    WRITE_CLUSTER(13, cluster_buf);

    /* Cluster 5: \EFI\pseuDOS contains BOOTX64.EFI, PSEUDOS.EFI, fallback VPKERNEL, GRUB.CFG, OS-RELEA */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 5, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 3, 0);
    make_dir_entry(&entries[2], "BOOTX64 EFI", 0x20, payloads[0].start_cluster, (uint32_t)payloads[0].size);
    make_dir_entry(&entries[3], "PSEUDOS EFI", 0x20, payloads[0].start_cluster, (uint32_t)payloads[0].size);
    make_dir_entry(&entries[4], "VPKERNEL   ", 0x20, payloads[1].start_cluster, (uint32_t)payloads[1].size);
    make_dir_entry(&entries[5], "GRUB    CFG", 0x20, 12, grub_len);
    make_dir_entry(&entries[6], "OS-RELEA   ", 0x20, 13, osrelease_len);
    WRITE_CLUSTER(5, cluster_buf);

    /* Cluster 7: \protected Directory contains "KRNL", "BOOTMGR", "CRIT", "GUI", "APPS" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 7, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 0, 0);
    make_dir_entry(&entries[2], "KRNL       ", 0x10, 8, 0);
    make_dir_entry(&entries[3], "BOOTMGR    ", 0x10, 9, 0);
    make_dir_entry(&entries[4], "CRIT       ", 0x10, 11, 0);
    make_dir_entry(&entries[5], "GUI        ", 0x10, 14, 0);
    make_dir_entry(&entries[6], "APPS       ", 0x10, 15, 0);
    WRITE_CLUSTER(7, cluster_buf);

    /* Cluster 8: \protected\krnl contains "VPKERNEL" and "AUTOINIT.EXE" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 8, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 7, 0);
    make_dir_entry(&entries[2], "VPKERNEL   ", 0x20, payloads[1].start_cluster, (uint32_t)payloads[1].size);
    make_dir_entry(&entries[3], "AUTOINITEXE", 0x20, payloads[2].start_cluster, (uint32_t)payloads[2].size);
    WRITE_CLUSTER(8, cluster_buf);

    /* Cluster 11: \protected\crit contains "XSHSS.EXE" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 11, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 7, 0);
    make_dir_entry(&entries[2], "XSHSS   EXE", 0x20, payloads[3].start_cluster, (uint32_t)payloads[3].size);
    WRITE_CLUSTER(11, cluster_buf);

    /* Cluster 10: \protected\bootmgr\boot.cfg file content */
    const char *cfg_content =
        "# pseuDOS Boot Configuration\r\n"
        "kernel=\\protected\\krnl\\vpkernel\r\n"
        "autoinit=\\protected\\krnl\\autoinit.exe\r\n"
        "shell=\\protected\\gui\\superglue.exe\r\n"
        "cmdline=quiet devpath=hardware\r\n"
        "default_resolution=1280x720\r\n"
        "bootmgr_version=1.1.0\r\n";
    uint32_t cfg_len = (uint32_t)strlen(cfg_content);

    memset(cluster_buf, 0, cluster_size_bytes);
    memcpy(cluster_buf, cfg_content, cfg_len);
    WRITE_CLUSTER(10, cluster_buf);

    /* Cluster 9: \protected\bootmgr contains "BOOT.CFG" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 9, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 7, 0);
    make_dir_entry(&entries[2], "BOOT    CFG", 0x20, 10, cfg_len);
    WRITE_CLUSTER(9, cluster_buf);

    /* Cluster 14: \protected\gui contains superglue, ntfs, lack, ninds, splash, gshss */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 14, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 7, 0);
    make_dir_entry(&entries[2], "SUPERGLUEXE", 0x20, payloads[4].start_cluster, (uint32_t)payloads[4].size);
    make_dir_entry(&entries[3], "NTFS    EXE", 0x20, payloads[5].start_cluster, (uint32_t)payloads[5].size);
    make_dir_entry(&entries[4], "LACK    EXE", 0x20, payloads[6].start_cluster, (uint32_t)payloads[6].size);
    make_dir_entry(&entries[5], "NINDS   EXE", 0x20, payloads[7].start_cluster, (uint32_t)payloads[7].size);
    make_dir_entry(&entries[6], "SPLASH  EXE", 0x20, payloads[8].start_cluster, (uint32_t)payloads[8].size);
    make_dir_entry(&entries[7], "GSHSS   EXE", 0x20, payloads[9].start_cluster, (uint32_t)payloads[9].size);
    WRITE_CLUSTER(14, cluster_buf);

    /* Cluster 15: \protected\apps contains shell, sysmon, calc, notepad, paint, clock */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 15, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 7, 0);
    make_dir_entry(&entries[2], "SHELL   EXE", 0x20, payloads[10].start_cluster, (uint32_t)payloads[10].size);
    make_dir_entry(&entries[3], "SYSMON  EXE", 0x20, payloads[11].start_cluster, (uint32_t)payloads[11].size);
    make_dir_entry(&entries[4], "CALC    EXE", 0x20, payloads[12].start_cluster, (uint32_t)payloads[12].size);
    make_dir_entry(&entries[5], "NOTEPAD EXE", 0x20, payloads[13].start_cluster, (uint32_t)payloads[13].size);
    make_dir_entry(&entries[6], "PAINT   EXE", 0x20, payloads[14].start_cluster, (uint32_t)payloads[14].size);
    make_dir_entry(&entries[7], "CLOCK   EXE", 0x20, payloads[15].start_cluster, (uint32_t)payloads[15].size);
    WRITE_CLUSTER(15, cluster_buf);

    /* Cluster 16: \HOME contains "USER" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 16, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 0, 0);
    make_dir_entry(&entries[2], "USER       ", 0x10, 19, 0);
    WRITE_CLUSTER(16, cluster_buf);

    /* Cluster 17: \TMP (empty directory) */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 17, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 0, 0);
    WRITE_CLUSTER(17, cluster_buf);

    /* Cluster 18: \SERVICES (empty directory) */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 18, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 0, 0);
    WRITE_CLUSTER(18, cluster_buf);

    /* Cluster 19: \HOME\USER (empty directory) */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    make_dir_entry(&entries[0], ".          ", 0x10, 19, 0);
    make_dir_entry(&entries[1], "..         ", 0x10, 16, 0);
    WRITE_CLUSTER(19, cluster_buf);

    /* 11. Deploying all Payloads (Boot manager, Kernel, Init, Shell, GUI stack, Apps) */
    for (int p = 0; p < num_payloads; p++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "copying %s...", payloads[p].desc);
        if (progress_cb) progress_cb(msg, 1);

        for (uint32_t i = 0; i < payloads[p].clusters; i++) {
            memset(cluster_buf, 0, cluster_size_bytes);
            size_t offset = (size_t)i * cluster_size_bytes;
            size_t to_copy = (payloads[p].size > offset) ? (payloads[p].size - offset) : 0;
            if (to_copy > cluster_size_bytes) to_copy = cluster_size_bytes;
            if (to_copy > 0 && payloads[p].data) {
                memcpy(cluster_buf, payloads[p].data + offset, to_copy);
            }
            WRITE_CLUSTER(payloads[p].start_cluster + i, cluster_buf);
        }
    }

    kfree(cluster_buf);

    /* 12. Synchronizing Storage Cache */
    if (progress_cb) progress_cb("synchronizing disk cache and finalizing installation...", 1);
    if (dev->flush) {
        dev->flush(dev);
    }

    return 0;
}
