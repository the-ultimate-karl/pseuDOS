#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    STORAGE_TYPE_INTERNAL_SATA,
    STORAGE_TYPE_INTERNAL_NVME,
    STORAGE_TYPE_EXTERNAL_USB
} StorageDeviceType;

typedef enum {
    TARGET_MODE_INTERNAL,
    TARGET_MODE_EXTERNAL
} TargetFilterMode;

typedef struct StorageDevice {
    char name[64];           /* Model / Product name */
    char type_str[16];       /* "NVMe", "SATA", "USB" */
    char bus_speed[32];      /* "PCIe 3.0 x4", "SATA 6.0 Gbps", "USB 3.1 Gen 1", "USB 2.0 High-Speed" */
    char size_str[32];       /* "250 GB", "1 TB", "32 GB", "10 GB" */
    char devpath[160];       /* Absolute Hardware Device Path */
    StorageDeviceType type;
    uint64_t total_sectors;  /* Total LBA addressable sectors */
    uint32_t sector_size;    /* Bytes per sector (512 or 4096) */
    uint32_t usb_version;    /* e.g. 0x0200 (USB 2.0), 0x0300 (USB 3.0), 0x0310 (USB 3.1 Gen 1) */
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_func;
    void *driver_priv;
    int (*read_sectors)(struct StorageDevice *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write_sectors)(struct StorageDevice *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct StorageDevice *dev);
    void (*shutdown)(struct StorageDevice *dev);
} StorageDevice;

/* Storage Subsystem Initialization & Device Enumeration */
void storage_init(void);
uint32_t storage_get_device_count(void);
StorageDevice *storage_get_device(uint32_t index);
void storage_format_size(uint64_t bytes, char *buf, size_t buf_size);
void storage_flush_all(void);
void storage_shutdown_all(void);

/* Target Filter Mode (Internal vs External) */
TargetFilterMode storage_get_target_mode(void);
void storage_set_target_mode(TargetFilterMode mode);

/* Individual Driver Probing APIs */
void ahci_init(void);
void nvme_init(void);
void usb_storage_init(void);

/* Disk Formatting & Installer API */
typedef void (*install_progress_cb_t)(const char *step_name, int is_ok);
int gpt_fat32_format_and_install(StorageDevice *dev, install_progress_cb_t progress_cb);

/* Block Device Filesystem Inspection API */
typedef struct {
    int has_partition_table;       /* 1 if valid GPT or MBR found */
    char part_table_type[16];      /* "GPT", "MBR", "None" */
    char part_type_name[48];       /* "EFI System Partition (ESP)", "Basic Data", etc. */
    uint64_t part_start_lba;
    uint64_t part_end_lba;
    uint64_t part_total_sectors;

    int has_filesystem;            /* 1 if recognized filesystem */
    char fs_type[32];              /* "FAT32", "FAT16", "RAW / Unformatted" */
    char vol_label[32];            /* "PSEUDOS ESP", etc. */
    char oem_name[16];             /* "MSWIN4.1" */
    uint32_t bytes_per_sector;     /* e.g. 512 */
    uint32_t sectors_per_cluster;  /* e.g. 8 (4096 bytes) */
    uint32_t cluster_size;         /* e.g. 4096 */
    uint32_t reserved_sectors;     /* e.g. 32 */
    uint32_t num_fats;             /* e.g. 2 */
    uint32_t fat_size_sectors;     /* e.g. 1024 */
    uint32_t root_cluster;         /* e.g. 2 */

    uint32_t total_clusters;
    uint32_t free_clusters;
    uint32_t used_clusters;

    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;

    char total_str[32];
    char free_str[32];
    char used_str[32];
    char health_status[32];        /* "Clean / Healthy", "Dirty / Unclean", "Unformatted" */
} StorageFsInfo;

typedef struct {
    uint32_t part_index;
    char part_type_name[48];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t total_sectors;
    int has_fs;
    char fs_type[32];
    char vol_label[32];
    char oem_name[16];
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint32_t free_clusters;
    uint32_t used_clusters;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    char total_str[32];
    char free_str[32];
    char used_str[32];
    char health_status[32];
} StoragePartitionInfo;

typedef struct {
    int has_partition_table;
    char part_table_type[16];      /* "GPT", "MBR", "None" */
    uint32_t partition_count;
    StoragePartitionInfo partitions[16];
} StorageDriveInfo;

int storage_inspect_fs(StorageDevice *dev, StorageFsInfo *out_info);
int storage_inspect_drive(StorageDevice *dev, StorageDriveInfo *out_drive_info);
StorageDevice *storage_get_boot_device(const char *boot_devpath, int *out_drive_index);

/* Embedded Installer Payloads */
const uint8_t *payload_get_bootloader(size_t *out_size);
const uint8_t *payload_get_kernel(size_t *out_size);

#endif /* STORAGE_H */
