#include "storage.h"
#include "bootinfo.h"
#include "fs.h"
#include "lib.h"
#include "klog.h"

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

typedef struct {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t fst_clus_lo;
    uint16_t name3[2];
} __attribute__((packed)) FatLfnEntry;

static uint8_t gpt_lfn_checksum(const char dos_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)dos_name[i]);
    }
    return sum;
}

static void make_dir_entry(FatDirEntry *entry, const char name11[11], uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(entry, 0, sizeof(FatDirEntry));
    memcpy(entry->name, name11, 11);
    entry->attr = attr;
    entry->fst_clus_hi = (uint16_t)(cluster >> 16);
    entry->fst_clus_lo = (uint16_t)(cluster & 0xFFFF);
    entry->file_size = size;
}

static void make_lfn_entry(FatDirEntry *entries, int *inout_idx, const char *long_name, const char name11[11], uint8_t attr, uint32_t cluster, uint32_t size) {
    if (!long_name || strcmp(long_name, ".") == 0 || strcmp(long_name, "..") == 0) {
        make_dir_entry(&entries[*inout_idx], name11, attr, cluster, size);
        (*inout_idx)++;
        return;
    }

    uint8_t chk = gpt_lfn_checksum(name11);
    size_t name_len = strlen(long_name);
    int num_chunks = (int)((name_len + 12) / 13);
    if (num_chunks < 1) num_chunks = 1;

    for (int seq = num_chunks; seq >= 1; seq--) {
        FatLfnEntry *lfn = (FatLfnEntry *)&entries[*inout_idx];
        memset(lfn, 0, sizeof(FatLfnEntry));
        lfn->order = (uint8_t)seq;
        if (seq == num_chunks) lfn->order |= 0x40;
        lfn->attr = 0x0F;
        lfn->type = 0x00;
        lfn->checksum = chk;
        lfn->fst_clus_lo = 0;

        int start_char = (seq - 1) * 13;
        uint16_t chars[13];
        for (int k = 0; k < 13; k++) {
            size_t pos = start_char + k;
            if (pos < name_len) {
                chars[k] = (uint16_t)(uint8_t)long_name[pos];
            } else if (pos == name_len) {
                chars[k] = 0x0000;
            } else {
                chars[k] = 0xFFFF;
            }
        }
        memcpy(lfn->name1, &chars[0], 5 * sizeof(uint16_t));
        memcpy(lfn->name2, &chars[5], 6 * sizeof(uint16_t));
        memcpy(lfn->name3, &chars[11], 2 * sizeof(uint16_t));
        (*inout_idx)++;
    }

    make_dir_entry(&entries[*inout_idx], name11, attr, cluster, size);
    (*inout_idx)++;
}

static GptInstallErrorInfo g_last_install_error;

const GptInstallErrorInfo *gpt_fat32_get_last_error(void) {
    return &g_last_install_error;
}

static void set_install_error(const char *stage, const char *details, uint64_t lba, uint32_t count, int code) {
    memset(&g_last_install_error, 0, sizeof(g_last_install_error));
    if (stage) strncpy(g_last_install_error.stage, stage, sizeof(g_last_install_error.stage) - 1);
    if (details) strncpy(g_last_install_error.details, details, sizeof(g_last_install_error.details) - 1);
    g_last_install_error.failed_lba = lba;
    g_last_install_error.sector_count = count;
    g_last_install_error.error_code = code;
}

void flash_format_error_screen(char *buf, size_t buf_sz, const StorageDevice *dev) {
    if (!buf || buf_sz == 0) return;
    const GptInstallErrorInfo *err = &g_last_install_error;

    const char *dev_name = (dev && dev->name[0]) ? dev->name : "Unknown Device";
    const char *dev_type = (dev && dev->type_str[0]) ? dev->type_str : "Block Device";
    const char *dev_size = (dev && dev->size_str[0]) ? dev->size_str : "Unknown Size";
    const char *dev_path = (dev && dev->devpath[0]) ? dev->devpath : "Unknown Path";
    const char *bus_speed = (dev && dev->bus_speed[0]) ? dev->bus_speed : "Standard";

    const char *stage = (err->stage[0]) ? err->stage : "Storage Subsystem Initialization";
    const char *details = (err->details[0]) ? err->details : "Device I/O write rejected or timed out";

    snprintf(buf, buf_sz,
        "flash: fatal error: installation failed due to disk write error!\n"
        "INFO: Hardware disk write error (%s)\n"
        "      Stage:         %s\n"
        "      Target Sector: LBA %llu (Count: %u sector%s)\n"
        "      Target Device: %s (%s, %s, %s)\n"
        "      Device Path:   %s\n",
        details,
        stage,
        (unsigned long long)err->failed_lba, err->sector_count, (err->sector_count == 1) ? "" : "s",
        dev_name, dev_type, dev_size, bus_speed,
        dev_path
    );
}

int gpt_fat32_format_and_install(StorageDevice *dev, install_progress_cb_t progress_cb) {
    memset(&g_last_install_error, 0, sizeof(g_last_install_error));
    if (!dev || !dev->write_sectors || dev->total_sectors < 4096) {
        set_install_error("Validating Storage Geometry", "Drive capacity under 2 MB or invalid sector size", 0, 0, -1);
        return -1;
    }

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

    if (dev->write_sectors(dev, 0, 1, sector_buf) != 0) {
        set_install_error("Writing Protective MBR (LBA 0)", "Device I/O write rejected or timed out", 0, 1, -1);
        return -1;
    }

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
    if (!entries_buf) {
        set_install_error("Allocating ESP Partition Table", "Out of kernel memory", 2, 32, -1);
        return -1;
    }
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
    if (dev->write_sectors(dev, 2, 32, entries_buf) != 0) {
        klog_warn("Failed to write Primary GPT partition array");
        set_install_error("Writing Primary GPT Partition Array (LBA 2-33)", "Device I/O write rejected or timed out", 2, 32, -1);
        kfree(entries_buf);
        return -1;
    }

    /* Write Secondary (Backup) Partition Array (LBA dev->total_sectors - 33 to -2) */
    uint64_t backup_entries_lba = dev->total_sectors - 33;
    if (dev->write_sectors(dev, backup_entries_lba, 32, entries_buf) != 0) {
        klog_warn("Failed to write Backup GPT partition array");
        set_install_error("Writing Secondary (Backup) GPT Partition Array", "Device I/O write rejected or timed out", backup_entries_lba, 32, -1);
        kfree(entries_buf);
        return -1;
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

    if (dev->write_sectors(dev, 1, 1, sector_buf) != 0) {
        klog_warn("Failed to write Primary GPT header");
        set_install_error("Writing Primary GPT Header (LBA 1)", "Device I/O write rejected or timed out", 1, 1, -1);
        return -1;
    }

    /* Secondary (Backup) GPT Header (LBA dev->total_sectors - 1) */
    gpt_hdr->my_lba = dev->total_sectors - 1;
    gpt_hdr->alternate_lba = 1;
    gpt_hdr->partition_entry_lba = backup_entries_lba;
    gpt_hdr->header_crc32 = 0;
    gpt_hdr->header_crc32 = calculate_crc32(gpt_hdr, 92);

    if (dev->write_sectors(dev, dev->total_sectors - 1, 1, sector_buf) != 0) {
        klog_warn("Failed to write Backup GPT header");
        set_install_error("Writing Secondary (Backup) GPT Header", "Device I/O write rejected or timed out", dev->total_sectors - 1, 1, -1);
        return -1;
    }

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

    enum {
        PAYLOAD_BOOTLOADER = 0,
        PAYLOAD_RECOVERY,
        PAYLOAD_KERNEL,
        PAYLOAD_AUTOINIT,
        PAYLOAD_BSHSS,
        PAYLOAD_GMAN,
        PAYLOAD_SUPERGLUE,
        PAYLOAD_NTFS,
        PAYLOAD_LACK,
        PAYLOAD_NINDS,
        PAYLOAD_SPLASH,
        PAYLOAD_GSHSS,
        PAYLOAD_SHELL,
        PAYLOAD_SYSMON,
        PAYLOAD_CALC,
        PAYLOAD_NOTEPAD,
        PAYLOAD_PAINT,
        PAYLOAD_CLOCK,
        PAYLOAD_VIEW,
        PAYLOAD_FILEMGR,
        PAYLOAD_INITRAMFS,

        /* Test Media in \home\user */
        PAYLOAD_IMG1_JPG,
        PAYLOAD_IMG1_PNG,
        PAYLOAD_IMG1_BMP,
        PAYLOAD_IMG2_PNG,
        PAYLOAD_SAMPLE_PNG,
        PAYLOAD_SAMPLE_BMP,

        /* Wallpapers in \home\user\Wallpapers */
        PAYLOAD_WALL_UNDER_CONSTRUCTION,
        PAYLOAD_WALL_ACCURATE,
        PAYLOAD_WALL_BLUESCREEN,
        PAYLOAD_WALL_FALLBACK,
        PAYLOAD_WALL_MEOW,
        PAYLOAD_WALL_SPARKLYMEOW,
        PAYLOAD_WALL_SPARKLYMEOW_ALT,
        PAYLOAD_WALL_SPARKLYMEOW_ALTCIRCLES,
        PAYLOAD_WALL_SPARKLYMEOW_ALTLINES,
        PAYLOAD_WALL_SPARKLYMEOW_ALTLINES2,
        NUM_INSTALL_PAYLOADS
    };

    DynamicPayload payloads[NUM_INSTALL_PAYLOADS] = {
        [PAYLOAD_BOOTLOADER] = { "\\EFI\\BOOT\\BOOTX64.EFI boot manager", NULL, 0, 0, 0 },
        [PAYLOAD_RECOVERY]   = { "\\EFI\\BOOT\\recovery.efi recovery console", NULL, 0, 0, 0 },
        [PAYLOAD_KERNEL]     = { "\\protected\\krnl\\vpkernel bare-metal payload", NULL, 0, 0, 0 },
        [PAYLOAD_AUTOINIT]   = { "\\protected\\krnl\\autoinit.exe userland init process", NULL, 0, 0, 0 },
        [PAYLOAD_BSHSS]      = { "\\protected\\crit\\bshss.exe shell subsystem", NULL, 0, 0, 0 },
        [PAYLOAD_GMAN]       = { "\\protected\\gui\\gman.exe graphical manager", NULL, 0, 0, 0 },
        [PAYLOAD_SUPERGLUE]  = { "\\protected\\gui\\superglue.exe window compositor", NULL, 0, 0, 0 },
        [PAYLOAD_NTFS]       = { "\\protected\\gui\\ntfs.exe display server", NULL, 0, 0, 0 },
        [PAYLOAD_LACK]       = { "\\protected\\gui\\lack.exe login/auth session", NULL, 0, 0, 0 },
        [PAYLOAD_NINDS]      = { "\\protected\\gui\\ninds.exe window manager", NULL, 0, 0, 0 },
        [PAYLOAD_SPLASH]     = { "\\protected\\gui\\splash.exe splash loader", NULL, 0, 0, 0 },
        [PAYLOAD_GSHSS]      = { "\\protected\\gui\\gshss.exe GUI desktop shell", NULL, 0, 0, 0 },
        [PAYLOAD_SHELL]      = { "\\protected\\apps\\shell.exe terminal app", NULL, 0, 0, 0 },
        [PAYLOAD_SYSMON]     = { "\\protected\\apps\\sysmon.exe system monitor", NULL, 0, 0, 0 },
        [PAYLOAD_CALC]       = { "\\protected\\apps\\calc.exe calculator app", NULL, 0, 0, 0 },
        [PAYLOAD_NOTEPAD]    = { "\\protected\\apps\\notepad.exe notepad app", NULL, 0, 0, 0 },
        [PAYLOAD_PAINT]      = { "\\protected\\apps\\paint.exe paint app", NULL, 0, 0, 0 },
        [PAYLOAD_CLOCK]      = { "\\protected\\apps\\clock.exe clock app", NULL, 0, 0, 0 },
        [PAYLOAD_VIEW]       = { "\\protected\\apps\\view.exe image viewer app", NULL, 0, 0, 0 },
        [PAYLOAD_FILEMGR]    = { "\\protected\\apps\\filemgr.exe file manager app", NULL, 0, 0, 0 },
        [PAYLOAD_INITRAMFS]  = { "\\protected\\krnl\\initramfs.img archive", NULL, 0, 0, 0 },

        [PAYLOAD_IMG1_JPG]   = { "\\home\\user\\image1.jpg test image", NULL, 0, 0, 0 },
        [PAYLOAD_IMG1_PNG]   = { "\\home\\user\\image1.png test image", NULL, 0, 0, 0 },
        [PAYLOAD_IMG1_BMP]   = { "\\home\\user\\image1.bmp test image", NULL, 0, 0, 0 },
        [PAYLOAD_IMG2_PNG]   = { "\\home\\user\\image2.png test image", NULL, 0, 0, 0 },
        [PAYLOAD_SAMPLE_PNG] = { "\\home\\user\\sample.png test image", NULL, 0, 0, 0 },
        [PAYLOAD_SAMPLE_BMP] = { "\\home\\user\\sample.bmp test image", NULL, 0, 0, 0 },

        [PAYLOAD_WALL_UNDER_CONSTRUCTION]    = { "\\home\\user\\Wallpapers\\Under Construction.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_ACCURATE]              = { "\\home\\user\\Wallpapers\\accurate.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_BLUESCREEN]            = { "\\home\\user\\Wallpapers\\bluescreen.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_FALLBACK]              = { "\\home\\user\\Wallpapers\\fallback.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_MEOW]                  = { "\\home\\user\\Wallpapers\\meow.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_SPARKLYMEOW]           = { "\\home\\user\\Wallpapers\\sparklymeow.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_SPARKLYMEOW_ALT]       = { "\\home\\user\\Wallpapers\\sparklymeow_alt.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_SPARKLYMEOW_ALTCIRCLES]= { "\\home\\user\\Wallpapers\\sparklymeow_altcircles.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_SPARKLYMEOW_ALTLINES]  = { "\\home\\user\\Wallpapers\\sparklymeow_altlines.png wallpaper", NULL, 0, 0, 0 },
        [PAYLOAD_WALL_SPARKLYMEOW_ALTLINES2] = { "\\home\\user\\Wallpapers\\sparklymeow_altlines2.png wallpaper", NULL, 0, 0, 0 },
    };
    int num_payloads = NUM_INSTALL_PAYLOADS;

    payloads[PAYLOAD_BOOTLOADER].data = payload_get_bootloader(&payloads[PAYLOAD_BOOTLOADER].size);
    payloads[PAYLOAD_RECOVERY].data   = payload_get_recovery(&payloads[PAYLOAD_RECOVERY].size);
    payloads[PAYLOAD_KERNEL].data     = payload_get_kernel(&payloads[PAYLOAD_KERNEL].size);
    payloads[PAYLOAD_AUTOINIT].data   = payload_get_autoinit(&payloads[PAYLOAD_AUTOINIT].size);
    payloads[PAYLOAD_BSHSS].data      = payload_get_bshss(&payloads[PAYLOAD_BSHSS].size);
    payloads[PAYLOAD_GMAN].data       = payload_get_gman(&payloads[PAYLOAD_GMAN].size);
    payloads[PAYLOAD_SUPERGLUE].data  = payload_get_superglue(&payloads[PAYLOAD_SUPERGLUE].size);
    payloads[PAYLOAD_NTFS].data       = payload_get_ntfs(&payloads[PAYLOAD_NTFS].size);
    payloads[PAYLOAD_LACK].data       = payload_get_lack(&payloads[PAYLOAD_LACK].size);
    payloads[PAYLOAD_NINDS].data      = payload_get_ninds(&payloads[PAYLOAD_NINDS].size);
    payloads[PAYLOAD_SPLASH].data     = payload_get_splash(&payloads[PAYLOAD_SPLASH].size);
    payloads[PAYLOAD_GSHSS].data      = payload_get_gshss(&payloads[PAYLOAD_GSHSS].size);
    payloads[PAYLOAD_SHELL].data      = payload_get_shell(&payloads[PAYLOAD_SHELL].size);
    payloads[PAYLOAD_SYSMON].data     = payload_get_sysmon(&payloads[PAYLOAD_SYSMON].size);
    payloads[PAYLOAD_CALC].data       = payload_get_calc(&payloads[PAYLOAD_CALC].size);
    payloads[PAYLOAD_NOTEPAD].data    = payload_get_notepad(&payloads[PAYLOAD_NOTEPAD].size);
    payloads[PAYLOAD_PAINT].data      = payload_get_paint(&payloads[PAYLOAD_PAINT].size);
    payloads[PAYLOAD_CLOCK].data      = payload_get_clock(&payloads[PAYLOAD_CLOCK].size);
    payloads[PAYLOAD_VIEW].data       = payload_get_view(&payloads[PAYLOAD_VIEW].size);
    payloads[PAYLOAD_FILEMGR].data    = payload_get_filemgr(&payloads[PAYLOAD_FILEMGR].size);
    payloads[PAYLOAD_INITRAMFS].data  = payload_get_initramfs(&payloads[PAYLOAD_INITRAMFS].size);

    /* Test images in /home/user */
    payloads[PAYLOAD_IMG1_JPG].data   = payload_get_by_path("/home/user/image1.jpg", &payloads[PAYLOAD_IMG1_JPG].size);
    if (!payloads[PAYLOAD_IMG1_JPG].data) payloads[PAYLOAD_IMG1_JPG].data = payload_get_by_path("/home/user/image1_testmedia.jpg", &payloads[PAYLOAD_IMG1_JPG].size);
    payloads[PAYLOAD_IMG1_PNG].data   = payload_get_by_path("/home/user/image1.png", &payloads[PAYLOAD_IMG1_PNG].size);
    if (!payloads[PAYLOAD_IMG1_PNG].data) payloads[PAYLOAD_IMG1_PNG].data = payload_get_by_path("/home/user/image1_testmedia.png", &payloads[PAYLOAD_IMG1_PNG].size);
    payloads[PAYLOAD_IMG1_BMP].data   = payload_get_by_path("/home/user/image1.bmp", &payloads[PAYLOAD_IMG1_BMP].size);
    if (!payloads[PAYLOAD_IMG1_BMP].data) payloads[PAYLOAD_IMG1_BMP].data = payload_get_by_path("/home/user/image1_testmedia.bmp", &payloads[PAYLOAD_IMG1_BMP].size);
    payloads[PAYLOAD_IMG2_PNG].data   = payload_get_by_path("/home/user/image2.png", &payloads[PAYLOAD_IMG2_PNG].size);
    if (!payloads[PAYLOAD_IMG2_PNG].data) payloads[PAYLOAD_IMG2_PNG].data = payload_get_by_path("/home/user/image2_testmedia.png", &payloads[PAYLOAD_IMG2_PNG].size);
    payloads[PAYLOAD_SAMPLE_PNG].data = payload_get_by_path("/home/user/sample.png", &payloads[PAYLOAD_SAMPLE_PNG].size);
    payloads[PAYLOAD_SAMPLE_BMP].data = payload_get_by_path("/home/user/sample.bmp", &payloads[PAYLOAD_SAMPLE_BMP].size);

    /* Wallpapers in /home/user/Wallpapers */
    payloads[PAYLOAD_WALL_UNDER_CONSTRUCTION].data    = payload_get_by_path("/home/user/Wallpapers/Under Construction.png", &payloads[PAYLOAD_WALL_UNDER_CONSTRUCTION].size);
    payloads[PAYLOAD_WALL_ACCURATE].data              = payload_get_by_path("/home/user/Wallpapers/accurate.png", &payloads[PAYLOAD_WALL_ACCURATE].size);
    payloads[PAYLOAD_WALL_BLUESCREEN].data            = payload_get_by_path("/home/user/Wallpapers/bluescreen.png", &payloads[PAYLOAD_WALL_BLUESCREEN].size);
    payloads[PAYLOAD_WALL_FALLBACK].data              = payload_get_by_path("/home/user/Wallpapers/fallback.png", &payloads[PAYLOAD_WALL_FALLBACK].size);
    payloads[PAYLOAD_WALL_MEOW].data                  = payload_get_by_path("/home/user/Wallpapers/meow.png", &payloads[PAYLOAD_WALL_MEOW].size);
    payloads[PAYLOAD_WALL_SPARKLYMEOW].data           = payload_get_by_path("/home/user/Wallpapers/sparklymeow.png", &payloads[PAYLOAD_WALL_SPARKLYMEOW].size);
    payloads[PAYLOAD_WALL_SPARKLYMEOW_ALT].data       = payload_get_by_path("/home/user/Wallpapers/sparklymeow_alt.png", &payloads[PAYLOAD_WALL_SPARKLYMEOW_ALT].size);
    payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTCIRCLES].data= payload_get_by_path("/home/user/Wallpapers/sparklymeow_altcircles.png", &payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTCIRCLES].size);
    payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES].data  = payload_get_by_path("/home/user/Wallpapers/sparklymeow_altlines.png", &payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES].size);
    payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES2].data = payload_get_by_path("/home/user/Wallpapers/sparklymeow_altlines2.png", &payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES2].size);

    uint32_t cluster_size_bytes = spc * 512;
    uint32_t next_cluster = 21; /* Clusters 2..20 reserved for directories and static config files */

    for (int p = 0; p < num_payloads; p++) {
        if (payloads[p].size == 0) {
            payloads[p].clusters = 0;
            payloads[p].start_cluster = 0;
            continue;
        }
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

    if (dev->write_sectors(dev, fat32_start_lba, 1, sector_buf) != 0) {
        klog_warn("Failed to write FAT32 VBR");
        set_install_error("Writing FAT32 VBR (Volume Boot Record)", "Device I/O write rejected or timed out", fat32_start_lba, 1, -1);
        return -1;
    }
    if (dev->write_sectors(dev, fat32_start_lba + 6, 1, sector_buf) != 0) { /* Backup Boot Sector */
        klog_warn("Failed to write backup FAT32 VBR");
        set_install_error("Writing Backup FAT32 VBR", "Device I/O write rejected or timed out", fat32_start_lba + 6, 1, -1);
        return -1;
    }

    /* 8. Write Dynamic FSInfo Sector */
    memset(sector_buf, 0, 512);
    *(uint32_t *)&sector_buf[0] = 0x41615252;
    *(uint32_t *)&sector_buf[484] = 0x61417272;
    *(uint32_t *)&sector_buf[488] = free_clusters;
    *(uint32_t *)&sector_buf[492] = next_free_cluster;
    *(uint32_t *)&sector_buf[508] = 0xAA550000;
    if (dev->write_sectors(dev, fat32_start_lba + 1, 1, sector_buf) != 0) {
        klog_warn("Failed to write FAT32 FSInfo sector");
        set_install_error("Writing FAT32 FSInfo Sector", "Device I/O write rejected or timed out", fat32_start_lba + 1, 1, -1);
        return -1;
    }
    if (dev->write_sectors(dev, fat32_start_lba + 7, 1, sector_buf) != 0) { /* Backup FSInfo */
        klog_warn("Failed to write backup FAT32 FSInfo sector");
        set_install_error("Writing Backup FAT32 FSInfo Sector", "Device I/O write rejected or timed out", fat32_start_lba + 7, 1, -1);
        return -1;
    }

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
            } else if (c >= 2 && c <= 20) {
                entries[i] = 0x0FFFFFFF; /* EOF for system directories and static config files */
            } else {
                int matched = 0;
                for (int p = 0; p < num_payloads; p++) {
                    if (payloads[p].clusters > 0 &&
                        c >= payloads[p].start_cluster &&
                        c < payloads[p].start_cluster + payloads[p].clusters) {
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
        uint32_t secs_to_write = chunk_sectors;
        if (cur_sector_offset + secs_to_write > fat_size_sectors) {
            secs_to_write = fat_size_sectors - cur_sector_offset;
        }

        if (secs_to_write > 0) {
            /* Write to FAT1 */
            if (dev->write_sectors(dev, fat32_start_lba + reserved_sectors + cur_sector_offset, secs_to_write, fat_chunk) != 0) {
                klog_warn("Failed to write FAT1 chunk at offset %u", cur_sector_offset);
                set_install_error("Writing Primary Allocation Table (FAT1)", "Device I/O write rejected or timed out", fat32_start_lba + reserved_sectors + cur_sector_offset, secs_to_write, -1);
                return -1;
            }
            /* Write to FAT2 */
            if (dev->write_sectors(dev, fat32_start_lba + reserved_sectors + fat_size_sectors + cur_sector_offset, secs_to_write, fat_chunk) != 0) {
                klog_warn("Failed to write FAT2 chunk at offset %u", cur_sector_offset);
                set_install_error("Writing Backup Allocation Table (FAT2)", "Device I/O write rejected or timed out", fat32_start_lba + reserved_sectors + fat_size_sectors + cur_sector_offset, secs_to_write, -1);
                return -1;
            }
        }

        current_cluster += entries_in_chunk;
        written_fat_bytes += sizeof(fat_chunk);
    }

    uint64_t data_lba_base = fat32_start_lba + reserved_sectors + (uint64_t)num_fats * fat_size_sectors;

    #define WRITE_CLUSTER(c, buf) \
        do { \
            uint64_t _clba = data_lba_base + ((uint64_t)(c) - 2) * spc; \
            int _rc = dev->write_sectors(dev, _clba, spc, (buf)); \
            if (_rc != 0) { \
                klog_warn("WRITE_CLUSTER failed: cluster=%u lba=%llu", (uint32_t)(c), (unsigned long long)_clba); \
                char _cstage[96]; \
                snprintf(_cstage, sizeof(_cstage), "Writing Filesystem Metadata (Cluster %u)", (uint32_t)(c)); \
                set_install_error(_cstage, "Device I/O write rejected or timed out", _clba, spc, _rc); \
                kfree(cluster_buf); \
                return -1; \
            } \
        } while (0)

    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size_bytes);
    if (!cluster_buf) {
        set_install_error("Allocating Cluster Buffer", "Out of kernel memory", 0, 0, -1);
        return -1;
    }

    /* 10. Deploying System Directories & Configuration Files */
    if (progress_cb) progress_cb("creating filesystem directories and boot configuration...", 1);

    /* Cluster 2: Root Directory contains "\EFI", "startup.nsh", "\protected", "\home", "\tmp", "\services" */
    memset(cluster_buf, 0, cluster_size_bytes);
    FatDirEntry *entries = (FatDirEntry *)cluster_buf;
    int idx = 0;
    make_lfn_entry(entries, &idx, "EFI",         "EFI        ", 0x10, 3, 0);
    make_lfn_entry(entries, &idx, "startup.nsh", "STARTUP NSH", 0x20, 6, 24);
    make_lfn_entry(entries, &idx, "protected",   "PROTEC~1   ", 0x10, 7, 0);
    make_lfn_entry(entries, &idx, "home",        "HOME       ", 0x10, 16, 0);
    make_lfn_entry(entries, &idx, "tmp",         "TMP        ", 0x10, 17, 0);
    make_lfn_entry(entries, &idx, "services",    "SERVICES   ", 0x10, 18, 0);
    WRITE_CLUSTER(2, cluster_buf);

    /* Cluster 6: \startup.nsh file */
    memset(cluster_buf, 0, cluster_size_bytes);
    strcpy((char *)cluster_buf, "\\EFI\\BOOT\\BOOTX64.EFI\r\n");
    WRITE_CLUSTER(6, cluster_buf);

    /* Cluster 3: \EFI Directory contains "BOOT" and "pseuDOS" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",       ".          ", 0x10, 3, 0);
    make_lfn_entry(entries, &idx, "..",      "..         ", 0x10, 0, 0);
    make_lfn_entry(entries, &idx, "BOOT",    "BOOT       ", 0x10, 4, 0);
    make_lfn_entry(entries, &idx, "pseuDOS", "PSEUDOS    ", 0x10, 5, 0);
    WRITE_CLUSTER(3, cluster_buf);

    /* Cluster 4: \EFI\BOOT contains "BOOTX64.EFI" and "recovery.efi" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",           ".          ", 0x10, 4, 0);
    make_lfn_entry(entries, &idx, "..",          "..         ", 0x10, 3, 0);
    make_lfn_entry(entries, &idx, "BOOTX64.EFI", "BOOTX64 EFI", 0x20, payloads[PAYLOAD_BOOTLOADER].start_cluster, (uint32_t)payloads[PAYLOAD_BOOTLOADER].size);
    if (payloads[PAYLOAD_RECOVERY].clusters > 0) {
        make_lfn_entry(entries, &idx, "recovery.efi", "RECOVERYEFI", 0x20, payloads[PAYLOAD_RECOVERY].start_cluster, (uint32_t)payloads[PAYLOAD_RECOVERY].size);
    }
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
        "VERSION=\"0.7.2\"\r\n"
        "PRETTY_NAME=\"pseuDOS v0.7.2 (x86_64 UEFI)\"\r\n"
        "HOME_URL=\"https://github.com/the-ultimate-karl/pseuDOS\"\r\n";
    uint32_t osrelease_len = (uint32_t)strlen(osrelease_content);
    memset(cluster_buf, 0, cluster_size_bytes);
    memcpy(cluster_buf, osrelease_content, osrelease_len);
    WRITE_CLUSTER(13, cluster_buf);

    /* Cluster 5: \EFI\pseuDOS contains BOOTX64.EFI, silo.efi, recovery.efi, fallback vpkernel, grub.cfg, os-release */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",           ".          ", 0x10, 5, 0);
    make_lfn_entry(entries, &idx, "..",          "..         ", 0x10, 3, 0);
    make_lfn_entry(entries, &idx, "BOOTX64.EFI", "BOOTX64 EFI", 0x20, payloads[PAYLOAD_BOOTLOADER].start_cluster, (uint32_t)payloads[PAYLOAD_BOOTLOADER].size);
    make_lfn_entry(entries, &idx, "silo.efi",    "SILO    EFI", 0x20, payloads[PAYLOAD_BOOTLOADER].start_cluster, (uint32_t)payloads[PAYLOAD_BOOTLOADER].size);
    if (payloads[PAYLOAD_RECOVERY].clusters > 0) {
        make_lfn_entry(entries, &idx, "recovery.efi", "RECOVERYEFI", 0x20, payloads[PAYLOAD_RECOVERY].start_cluster, (uint32_t)payloads[PAYLOAD_RECOVERY].size);
    }
    make_lfn_entry(entries, &idx, "vpkernel",    "VPKERNEL   ", 0x20, payloads[PAYLOAD_KERNEL].start_cluster, (uint32_t)payloads[PAYLOAD_KERNEL].size);
    make_lfn_entry(entries, &idx, "grub.cfg",    "GRUB    CFG", 0x20, 12, grub_len);
    make_lfn_entry(entries, &idx, "os-release",  "OS-REL~1   ", 0x20, 13, osrelease_len);
    WRITE_CLUSTER(5, cluster_buf);

    /* Cluster 7: \protected Directory contains "krnl", "bootmgr", "crit", "gui", "apps" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",       ".          ", 0x10, 7, 0);
    make_lfn_entry(entries, &idx, "..",      "..         ", 0x10, 0, 0);
    make_lfn_entry(entries, &idx, "krnl",    "KRNL       ", 0x10, 8, 0);
    make_lfn_entry(entries, &idx, "bootmgr", "BOOTMGR    ", 0x10, 9, 0);
    make_lfn_entry(entries, &idx, "crit",    "CRIT       ", 0x10, 11, 0);
    make_lfn_entry(entries, &idx, "gui",     "GUI        ", 0x10, 14, 0);
    make_lfn_entry(entries, &idx, "apps",    "APPS       ", 0x10, 15, 0);
    WRITE_CLUSTER(7, cluster_buf);

    /* Cluster 8: \protected\krnl contains "vpkernel", "autoinit.exe", and optionally "initramfs.img" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",            ".          ", 0x10, 8, 0);
    make_lfn_entry(entries, &idx, "..",           "..         ", 0x10, 7, 0);
    make_lfn_entry(entries, &idx, "vpkernel",     "VPKERNEL   ", 0x20, payloads[PAYLOAD_KERNEL].start_cluster, (uint32_t)payloads[PAYLOAD_KERNEL].size);
    make_lfn_entry(entries, &idx, "autoinit.exe", "AUTOINITEXE", 0x20, payloads[PAYLOAD_AUTOINIT].start_cluster, (uint32_t)payloads[PAYLOAD_AUTOINIT].size);
    if (payloads[PAYLOAD_INITRAMFS].clusters > 0) {
        make_lfn_entry(entries, &idx, "initramfs.img", "INITRAMFIMG", 0x20, payloads[PAYLOAD_INITRAMFS].start_cluster, (uint32_t)payloads[PAYLOAD_INITRAMFS].size);
    }
    WRITE_CLUSTER(8, cluster_buf);

    /* Cluster 11: \protected\crit contains "bshss.exe" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",         ".          ", 0x10, 11, 0);
    make_lfn_entry(entries, &idx, "..",        "..         ", 0x10, 7, 0);
    make_lfn_entry(entries, &idx, "bshss.exe", "BSHSS   EXE", 0x20, payloads[PAYLOAD_BSHSS].start_cluster, (uint32_t)payloads[PAYLOAD_BSHSS].size);
    WRITE_CLUSTER(11, cluster_buf);

    /* Cluster 10: \protected\bootmgr\boot.cfg file content */
    char cfg_content[512];
    if (payloads[PAYLOAD_INITRAMFS].clusters > 0) {
        snprintf(cfg_content, sizeof(cfg_content),
            "# pseuDOS Boot Configuration\r\n"
            "kernel=\\protected\\krnl\\vpkernel\r\n"
            "initramfs=\\protected\\krnl\\initramfs.img\r\n"
            "autoinit=\\protected\\krnl\\autoinit.exe\r\n"
            "shell=\\protected\\gui\\gman.exe\r\n"
            "cmdline=quiet devpath=hardware\r\n"
            "default_resolution=auto\r\n"
            "bootmgr_version=1.1.0\r\n");
    } else {
        snprintf(cfg_content, sizeof(cfg_content),
            "# pseuDOS Boot Configuration\r\n"
            "kernel=\\protected\\krnl\\vpkernel\r\n"
            "autoinit=\\protected\\krnl\\autoinit.exe\r\n"
            "shell=\\protected\\gui\\gman.exe\r\n"
            "cmdline=quiet devpath=hardware\r\n"
            "default_resolution=auto\r\n"
            "bootmgr_version=1.1.0\r\n");
    }
    uint32_t cfg_len = (uint32_t)strlen(cfg_content);

    memset(cluster_buf, 0, cluster_size_bytes);
    memcpy(cluster_buf, cfg_content, cfg_len);
    WRITE_CLUSTER(10, cluster_buf);

    /* Cluster 9: \protected\bootmgr contains "boot.cfg" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",        ".          ", 0x10, 9, 0);
    make_lfn_entry(entries, &idx, "..",       "..         ", 0x10, 7, 0);
    make_lfn_entry(entries, &idx, "boot.cfg", "BOOT    CFG", 0x20, 10, cfg_len);
    WRITE_CLUSTER(9, cluster_buf);

    /* Cluster 14: \protected\gui contains gman, superglue, ntfs, lack, ninds, splash, gshss */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",             ".          ", 0x10, 14, 0);
    make_lfn_entry(entries, &idx, "..",            "..         ", 0x10, 7, 0);
    make_lfn_entry(entries, &idx, "gman.exe",      "GMAN    EXE", 0x20, payloads[PAYLOAD_GMAN].start_cluster, (uint32_t)payloads[PAYLOAD_GMAN].size);
    make_lfn_entry(entries, &idx, "superglue.exe", "SUPERGLUEXE", 0x20, payloads[PAYLOAD_SUPERGLUE].start_cluster, (uint32_t)payloads[PAYLOAD_SUPERGLUE].size);
    make_lfn_entry(entries, &idx, "ntfs.exe",      "NTFS    EXE", 0x20, payloads[PAYLOAD_NTFS].start_cluster, (uint32_t)payloads[PAYLOAD_NTFS].size);
    make_lfn_entry(entries, &idx, "lack.exe",      "LACK    EXE", 0x20, payloads[PAYLOAD_LACK].start_cluster, (uint32_t)payloads[PAYLOAD_LACK].size);
    make_lfn_entry(entries, &idx, "ninds.exe",     "NINDS   EXE", 0x20, payloads[PAYLOAD_NINDS].start_cluster, (uint32_t)payloads[PAYLOAD_NINDS].size);
    make_lfn_entry(entries, &idx, "splash.exe",    "SPLASH  EXE", 0x20, payloads[PAYLOAD_SPLASH].start_cluster, (uint32_t)payloads[PAYLOAD_SPLASH].size);
    make_lfn_entry(entries, &idx, "gshss.exe",     "GSHSS   EXE", 0x20, payloads[PAYLOAD_GSHSS].start_cluster, (uint32_t)payloads[PAYLOAD_GSHSS].size);
    WRITE_CLUSTER(14, cluster_buf);

    /* Cluster 15: \protected\apps contains shell, sysmon, calc, notepad, paint, clock, view, filemgr */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",           ".          ", 0x10, 15, 0);
    make_lfn_entry(entries, &idx, "..",          "..         ", 0x10, 7, 0);
    make_lfn_entry(entries, &idx, "shell.exe",   "SHELL   EXE", 0x20, payloads[PAYLOAD_SHELL].start_cluster, (uint32_t)payloads[PAYLOAD_SHELL].size);
    make_lfn_entry(entries, &idx, "sysmon.exe",  "SYSMON  EXE", 0x20, payloads[PAYLOAD_SYSMON].start_cluster, (uint32_t)payloads[PAYLOAD_SYSMON].size);
    make_lfn_entry(entries, &idx, "calc.exe",    "CALC    EXE", 0x20, payloads[PAYLOAD_CALC].start_cluster, (uint32_t)payloads[PAYLOAD_CALC].size);
    make_lfn_entry(entries, &idx, "notepad.exe", "NOTEPAD EXE", 0x20, payloads[PAYLOAD_NOTEPAD].start_cluster, (uint32_t)payloads[PAYLOAD_NOTEPAD].size);
    make_lfn_entry(entries, &idx, "paint.exe",   "PAINT   EXE", 0x20, payloads[PAYLOAD_PAINT].start_cluster, (uint32_t)payloads[PAYLOAD_PAINT].size);
    make_lfn_entry(entries, &idx, "clock.exe",   "CLOCK   EXE", 0x20, payloads[PAYLOAD_CLOCK].start_cluster, (uint32_t)payloads[PAYLOAD_CLOCK].size);
    make_lfn_entry(entries, &idx, "view.exe",    "VIEW    EXE", 0x20, payloads[PAYLOAD_VIEW].start_cluster, (uint32_t)payloads[PAYLOAD_VIEW].size);
    make_lfn_entry(entries, &idx, "filemgr.exe", "FILEMGR EXE", 0x20, payloads[PAYLOAD_FILEMGR].start_cluster, (uint32_t)payloads[PAYLOAD_FILEMGR].size);
    WRITE_CLUSTER(15, cluster_buf);

    /* Cluster 16: \home contains "user" */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",    ".          ", 0x10, 16, 0);
    make_lfn_entry(entries, &idx, "..",   "..         ", 0x10, 0, 0);
    make_lfn_entry(entries, &idx, "user", "USER       ", 0x10, 19, 0);
    WRITE_CLUSTER(16, cluster_buf);

    /* Cluster 17: \tmp (empty directory) */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",  ".          ", 0x10, 17, 0);
    make_lfn_entry(entries, &idx, "..", "..         ", 0x10, 0, 0);
    WRITE_CLUSTER(17, cluster_buf);

    /* Cluster 18: \services (empty directory) */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",  ".          ", 0x10, 18, 0);
    make_lfn_entry(entries, &idx, "..", "..         ", 0x10, 0, 0);
    WRITE_CLUSTER(18, cluster_buf);

    /* Cluster 19: \home\user contains "Wallpapers" and test media */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",          ".          ", 0x10, 19, 0);
    make_lfn_entry(entries, &idx, "..",         "..         ", 0x10, 16, 0);
    make_lfn_entry(entries, &idx, "Wallpapers", "WALLPAPE   ", 0x10, 20, 0);
    if (payloads[PAYLOAD_IMG1_JPG].clusters > 0)
        make_lfn_entry(entries, &idx, "image1.jpg", "IMAGE1  JPG", 0x20, payloads[PAYLOAD_IMG1_JPG].start_cluster, (uint32_t)payloads[PAYLOAD_IMG1_JPG].size);
    if (payloads[PAYLOAD_IMG1_PNG].clusters > 0)
        make_lfn_entry(entries, &idx, "image1.png", "IMAGE1  PNG", 0x20, payloads[PAYLOAD_IMG1_PNG].start_cluster, (uint32_t)payloads[PAYLOAD_IMG1_PNG].size);
    if (payloads[PAYLOAD_IMG1_BMP].clusters > 0)
        make_lfn_entry(entries, &idx, "image1.bmp", "IMAGE1  BMP", 0x20, payloads[PAYLOAD_IMG1_BMP].start_cluster, (uint32_t)payloads[PAYLOAD_IMG1_BMP].size);
    if (payloads[PAYLOAD_IMG2_PNG].clusters > 0)
        make_lfn_entry(entries, &idx, "image2.png", "IMAGE2  PNG", 0x20, payloads[PAYLOAD_IMG2_PNG].start_cluster, (uint32_t)payloads[PAYLOAD_IMG2_PNG].size);
    if (payloads[PAYLOAD_SAMPLE_PNG].clusters > 0)
        make_lfn_entry(entries, &idx, "sample.png", "SAMPLE  PNG", 0x20, payloads[PAYLOAD_SAMPLE_PNG].start_cluster, (uint32_t)payloads[PAYLOAD_SAMPLE_PNG].size);
    if (payloads[PAYLOAD_SAMPLE_BMP].clusters > 0)
        make_lfn_entry(entries, &idx, "sample.bmp", "SAMPLE  BMP", 0x20, payloads[PAYLOAD_SAMPLE_BMP].start_cluster, (uint32_t)payloads[PAYLOAD_SAMPLE_BMP].size);
    WRITE_CLUSTER(19, cluster_buf);

    /* Cluster 20: \home\user\Wallpapers contains all wallpaper assets */
    memset(cluster_buf, 0, cluster_size_bytes);
    entries = (FatDirEntry *)cluster_buf;
    idx = 0;
    make_lfn_entry(entries, &idx, ".",  ".          ", 0x10, 20, 0);
    make_lfn_entry(entries, &idx, "..", "..         ", 0x10, 19, 0);
    if (payloads[PAYLOAD_WALL_UNDER_CONSTRUCTION].clusters > 0)
        make_lfn_entry(entries, &idx, "Under Construction.png",     "UNDERC~1PNG", 0x20, payloads[PAYLOAD_WALL_UNDER_CONSTRUCTION].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_UNDER_CONSTRUCTION].size);
    if (payloads[PAYLOAD_WALL_ACCURATE].clusters > 0)
        make_lfn_entry(entries, &idx, "accurate.png",               "ACCURATEPNG", 0x20, payloads[PAYLOAD_WALL_ACCURATE].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_ACCURATE].size);
    if (payloads[PAYLOAD_WALL_BLUESCREEN].clusters > 0)
        make_lfn_entry(entries, &idx, "bluescreen.png",             "BLUESC~1PNG", 0x20, payloads[PAYLOAD_WALL_BLUESCREEN].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_BLUESCREEN].size);
    if (payloads[PAYLOAD_WALL_FALLBACK].clusters > 0)
        make_lfn_entry(entries, &idx, "fallback.png",               "FALLBACKPNG", 0x20, payloads[PAYLOAD_WALL_FALLBACK].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_FALLBACK].size);
    if (payloads[PAYLOAD_WALL_MEOW].clusters > 0)
        make_lfn_entry(entries, &idx, "meow.png",                   "MEOW    PNG", 0x20, payloads[PAYLOAD_WALL_MEOW].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_MEOW].size);
    if (payloads[PAYLOAD_WALL_SPARKLYMEOW].clusters > 0)
        make_lfn_entry(entries, &idx, "sparklymeow.png",            "SPARKL~1PNG", 0x20, payloads[PAYLOAD_WALL_SPARKLYMEOW].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_SPARKLYMEOW].size);
    if (payloads[PAYLOAD_WALL_SPARKLYMEOW_ALT].clusters > 0)
        make_lfn_entry(entries, &idx, "sparklymeow_alt.png",        "SPARKL~2PNG", 0x20, payloads[PAYLOAD_WALL_SPARKLYMEOW_ALT].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_SPARKLYMEOW_ALT].size);
    if (payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTCIRCLES].clusters > 0)
        make_lfn_entry(entries, &idx, "sparklymeow_altcircles.png", "SPARKL~3PNG", 0x20, payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTCIRCLES].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTCIRCLES].size);
    if (payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES].clusters > 0)
        make_lfn_entry(entries, &idx, "sparklymeow_altlines.png",   "SPARKL~4PNG", 0x20, payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES].size);
    if (payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES2].clusters > 0)
        make_lfn_entry(entries, &idx, "sparklymeow_altlines2.png",  "SPARKL~5PNG", 0x20, payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES2].start_cluster, (uint32_t)payloads[PAYLOAD_WALL_SPARKLYMEOW_ALTLINES2].size);
    WRITE_CLUSTER(20, cluster_buf);

    /* 11. Deploying all Payloads (Boot manager, Kernel, Init, Shell, GUI stack, Apps) */
    #define BULK_CHUNK_SECTORS 128
    uint8_t *bulk_buf = (uint8_t *)kmalloc(BULK_CHUNK_SECTORS * 512);
    if (!bulk_buf) {
        set_install_error("Allocating Payload Buffer", "Out of kernel memory", 0, 0, -1);
        kfree(cluster_buf);
        return -1;
    }

    for (int p = 0; p < num_payloads; p++) {
        if (!payloads[p].data || payloads[p].size == 0 || payloads[p].clusters == 0) {
            continue;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "copying %s...", payloads[p].desc);
        if (progress_cb) progress_cb(msg, 1);

        uint32_t total_sectors = payloads[p].clusters * spc;
        uint64_t cur_payload_lba = data_lba_base + ((uint64_t)payloads[p].start_cluster - 2) * spc;
        size_t bytes_written = 0;

        while (total_sectors > 0) {
            uint32_t cur_chunk = total_sectors > BULK_CHUNK_SECTORS ? BULK_CHUNK_SECTORS : total_sectors;
            uint32_t chunk_bytes = cur_chunk * 512;
            memset(bulk_buf, 0, chunk_bytes);

            if (payloads[p].data && bytes_written < payloads[p].size) {
                size_t avail = payloads[p].size - bytes_written;
                size_t to_copy = avail < chunk_bytes ? avail : chunk_bytes;
                memcpy(bulk_buf, payloads[p].data + bytes_written, to_copy);
                bytes_written += to_copy;
            }

            int rc = dev->write_sectors(dev, cur_payload_lba, cur_chunk, bulk_buf);
            if (rc != 0) {
                klog_warn("Bulk write failed: payload=%s lba=%llu count=%u",
                          payloads[p].desc, (unsigned long long)cur_payload_lba, cur_chunk);
                char pstage[96];
                snprintf(pstage, sizeof(pstage), "Streaming Payload: %s", payloads[p].desc);
                set_install_error(pstage, "Device I/O write rejected or timed out", cur_payload_lba, cur_chunk, rc);
                kfree(bulk_buf);
                kfree(cluster_buf);
                return -1;
            }

            cur_payload_lba += cur_chunk;
            total_sectors -= cur_chunk;
        }
    }

    kfree(bulk_buf);
    kfree(cluster_buf);

    /* 12. Synchronizing Storage Cache */
    if (progress_cb) progress_cb("synchronizing disk cache and finalizing installation...", 1);
    if (dev->flush) {
        dev->flush(dev);
    }

    return 0;
}
