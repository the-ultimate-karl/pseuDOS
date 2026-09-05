#include "fs.h"
#include "storage.h"
#include "bootinfo.h"
#include "drivers.h"
#include "lib.h"

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

static StorageDevice *g_sync_dev = NULL;
static uint64_t g_sync_part_lba = 0;
static uint32_t g_sync_spc = 8;
static uint32_t g_sync_bps = 512;
static uint32_t g_sync_cluster_size = 4096;
static uint32_t g_sync_reserved_sectors = 32;
static uint32_t g_sync_num_fats = 2;
static uint32_t g_sync_fat_size_sectors = 1024;
static uint64_t g_sync_data_lba_base = 0;
static uint32_t g_sync_root_cluster = 2;
static int g_is_fat32_mounted = 0;

int fat32_is_mounted(void) {
    return g_is_fat32_mounted;
}

static uint64_t cluster_to_lba(uint32_t cluster) {
    if (cluster < 2) cluster = 2;
    return g_sync_data_lba_base + ((uint64_t)(cluster - 2) * g_sync_spc);
}

static int read_cluster(uint32_t cluster, void *buf) {
    if (!g_sync_dev || !g_sync_dev->read_sectors) return -1;
    uint64_t lba = cluster_to_lba(cluster);
    return g_sync_dev->read_sectors(g_sync_dev, lba, g_sync_spc, buf);
}

static int write_cluster(uint32_t cluster, const void *buf) {
    if (!g_sync_dev || !g_sync_dev->write_sectors) return -1;
    uint64_t lba = cluster_to_lba(cluster);
    return g_sync_dev->write_sectors(g_sync_dev, lba, g_sync_spc, buf);
}

static uint32_t get_fat_entry(uint32_t cluster) {
    if (!g_sync_dev || !g_sync_dev->read_sectors) return 0x0FFFFFFF;
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_sync_part_lba + g_sync_reserved_sectors + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    uint8_t sec[512];
    if (g_sync_dev->read_sectors(g_sync_dev, fat_sector, 1, sec) != 0) {
        return 0x0FFFFFFF;
    }
    return (*(uint32_t *)&sec[entry_offset]) & 0x0FFFFFFF;
}

static void set_fat_entry(uint32_t cluster, uint32_t val) {
    if (!g_sync_dev || !g_sync_dev->read_sectors || !g_sync_dev->write_sectors) return;
    uint32_t fat_offset = cluster * 4;
    uint32_t sec_offset = fat_offset / 512;
    uint32_t entry_offset = fat_offset % 512;

    uint8_t sec[512];
    /* Update FAT1 */
    uint32_t fat1_sec = g_sync_part_lba + g_sync_reserved_sectors + sec_offset;
    if (g_sync_dev->read_sectors(g_sync_dev, fat1_sec, 1, sec) == 0) {
        *(uint32_t *)&sec[entry_offset] = val & 0x0FFFFFFF;
        g_sync_dev->write_sectors(g_sync_dev, fat1_sec, 1, sec);
    }

    /* Update FAT2 */
    uint32_t fat2_sec = g_sync_part_lba + g_sync_reserved_sectors + g_sync_fat_size_sectors + sec_offset;
    if (g_sync_dev->read_sectors(g_sync_dev, fat2_sec, 1, sec) == 0) {
        *(uint32_t *)&sec[entry_offset] = val & 0x0FFFFFFF;
        g_sync_dev->write_sectors(g_sync_dev, fat2_sec, 1, sec);
    }
}

static void update_fsinfo_free_clusters(int delta) {
    if (!g_sync_dev || !g_sync_dev->read_sectors || !g_sync_dev->write_sectors) return;
    uint8_t sec[512];
    uint64_t fsinfo_lba = g_sync_part_lba + 1;
    if (g_sync_dev->read_sectors(g_sync_dev, fsinfo_lba, 1, sec) == 0) {
        uint32_t lead_sig = *(uint32_t *)&sec[0];
        uint32_t struct_sig = *(uint32_t *)&sec[484];
        if (lead_sig == 0x41615252 && struct_sig == 0x61417272) {
            uint32_t free_c = *(uint32_t *)&sec[488];
            if (free_c != 0xFFFFFFFF) {
                if (delta < 0 && (uint32_t)(-delta) <= free_c) {
                    free_c -= (uint32_t)(-delta);
                } else if (delta > 0) {
                    free_c += (uint32_t)delta;
                }
                *(uint32_t *)&sec[488] = free_c;
                g_sync_dev->write_sectors(g_sync_dev, fsinfo_lba, 1, sec);
            }
        }
    }
}

static uint32_t alloc_free_cluster(void) {
    if (!g_sync_dev) return 0;
    uint8_t sec[512];

    for (uint32_t s = 0; s < g_sync_fat_size_sectors; s++) {
        uint64_t fat_sec = g_sync_part_lba + g_sync_reserved_sectors + s;
        if (g_sync_dev->read_sectors(g_sync_dev, fat_sec, 1, sec) != 0) continue;

        uint32_t *entries = (uint32_t *)sec;
        for (uint32_t i = 0; i < 128; i++) {
            uint32_t cluster_idx = s * 128 + i;
            if (cluster_idx >= 2 && (entries[i] & 0x0FFFFFFF) == 0) {
                /* Allocate cluster */
                set_fat_entry(cluster_idx, 0x0FFFFFFF);

                /* Zero the allocated cluster */
                uint8_t zero_buf[4096];
                memset(zero_buf, 0, sizeof(zero_buf));
                write_cluster(cluster_idx, zero_buf);

                update_fsinfo_free_clusters(-1);
                return cluster_idx;
            }
        }
    }
    return 0;
}

static void free_cluster_chain(uint32_t start_cluster) {
    uint32_t curr = start_cluster;
    int freed_count = 0;

    while (curr >= 2 && curr < 0x0FFFFFF8) {
        uint32_t next = get_fat_entry(curr);
        set_fat_entry(curr, 0x00000000);
        freed_count++;
        curr = next;
    }

    if (freed_count > 0) {
        update_fsinfo_free_clusters(freed_count);
    }
}

static void to_dos_name(const char *src, char *dos_name) {
    memset(dos_name, ' ', 11);
    if (!src || src[0] == '\0') return;

    if (strcmp(src, ".") == 0) {
        dos_name[0] = '.';
        return;
    }
    if (strcmp(src, "..") == 0) {
        dos_name[0] = '.';
        dos_name[1] = '.';
        return;
    }

    const char *dot = strrchr(src, '.');
    size_t name_len = dot ? (size_t)(dot - src) : strlen(src);
    if (name_len > 8) name_len = 8;

    for (size_t i = 0; i < name_len; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        dos_name[i] = c;
    }

    if (dot) {
        dot++;
        size_t ext_len = strlen(dot);
        if (ext_len > 3) ext_len = 3;
        for (size_t i = 0; i < ext_len; i++) {
            char c = dot[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            dos_name[8 + i] = c;
        }
    }
}

static void from_dos_name(const char *dos_name, char *out_name) {
    char name_part[9];
    char ext_part[4];
    int n_len = 0, e_len = 0;

    for (int i = 0; i < 8; i++) {
        if (dos_name[i] != ' ') {
            name_part[n_len++] = dos_name[i];
        }
    }
    name_part[n_len] = '\0';

    for (int i = 8; i < 11; i++) {
        if (dos_name[i] != ' ') {
            ext_part[e_len++] = dos_name[i];
        }
    }
    ext_part[e_len] = '\0';

    if (e_len > 0) {
        snprintf(out_name, 64, "%s.%s", name_part, ext_part);
    } else {
        snprintf(out_name, 64, "%s", name_part);
    }
}

static uint32_t find_path_parent_cluster(const char *path, char *out_child_name) {
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) {
        if (out_child_name) out_child_name[0] = '\0';
        return g_sync_root_cluster;
    }

    char norm[256];
    strncpy(norm, path, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = '\0';

    char *last_slash = strrchr(norm, '/');
    if (!last_slash) {
        last_slash = strrchr(norm, '\\');
    }

    if (!last_slash || last_slash == norm) {
        if (out_child_name) {
            strcpy(out_child_name, last_slash ? last_slash + 1 : norm);
        }
        return g_sync_root_cluster;
    }

    *last_slash = '\0';
    if (out_child_name) {
        strcpy(out_child_name, last_slash + 1);
    }

    /* Traverse down from root cluster */
    uint32_t curr_cluster = g_sync_root_cluster;
    char *token = norm;
    if (*token == '/' || *token == '\\') token++;

    while (*token) {
        char *slash = strchr(token, '/');
        if (!slash) slash = strchr(token, '\\');
        if (slash) *slash = '\0';

        char dos_target[11];
        to_dos_name(token, dos_target);

        uint32_t c = curr_cluster;
        uint32_t next_cluster = 0;
        uint8_t cluster_buf[4096];

        while (c >= 2 && c < 0x0FFFFFF8) {
            if (read_cluster(c, cluster_buf) != 0) break;
            FatDirEntry *entries = (FatDirEntry *)cluster_buf;
            uint32_t num_entries = g_sync_cluster_size / sizeof(FatDirEntry);

            for (uint32_t i = 0; i < num_entries; i++) {
                if ((uint8_t)entries[i].name[0] == 0x00) break;
                if ((uint8_t)entries[i].name[0] == 0xE5) continue;
                if (entries[i].attr == 0x0F) continue;

                if ((entries[i].attr & 0x10) && memcmp(entries[i].name, dos_target, 11) == 0) {
                    next_cluster = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
                    break;
                }
            }
            if (next_cluster != 0) break;
            c = get_fat_entry(c);
        }

        if (next_cluster == 0) {
            return g_sync_root_cluster;
        }
        curr_cluster = next_cluster;

        if (!slash) break;
        token = slash + 1;
    }

    return curr_cluster;
}

int fat32_sync_create_file(const char *path) {
    if (!g_is_fat32_mounted || !path) return -1;

    char file_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(path, file_name);
    if (file_name[0] == '\0') return -1;

    char dos_name[11];
    to_dos_name(file_name, dos_name);

    uint32_t c = parent_cluster;
    uint8_t cluster_buf[4096];

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) return -1;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = g_sync_cluster_size / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                /* Found slot, allocate 1 initial cluster for the file */
                uint32_t new_cluster = alloc_free_cluster();
                if (new_cluster == 0) return -1;

                memset(&entries[i], 0, sizeof(FatDirEntry));
                memcpy(entries[i].name, dos_name, 11);
                entries[i].attr = 0x20; /* Archive / File */
                entries[i].fst_clus_hi = (uint16_t)(new_cluster >> 16);
                entries[i].fst_clus_lo = (uint16_t)(new_cluster & 0xFFFF);
                entries[i].file_size = 0;

                write_cluster(c, cluster_buf);
                return 0;
            } else if (memcmp(entries[i].name, dos_name, 11) == 0) {
                return 0; /* Already exists */
            }
        }

        uint32_t next = get_fat_entry(c);
        if (next >= 0x0FFFFFF8) {
            /* Allocate new cluster for directory */
            uint32_t new_dir_clus = alloc_free_cluster();
            if (new_dir_clus == 0) return -1;
            set_fat_entry(c, new_dir_clus);
            c = new_dir_clus;
        } else {
            c = next;
        }
    }

    return -1;
}

int fat32_sync_write_file(const char *path, const char *text, int append) {
    if (!g_is_fat32_mounted || !path || !text) return -1;

    char file_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(path, file_name);
    if (file_name[0] == '\0') return -1;

    char dos_name[11];
    to_dos_name(file_name, dos_name);

    uint32_t c = parent_cluster;
    uint8_t cluster_buf[4096];

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) return -1;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = g_sync_cluster_size / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;

            if (memcmp(entries[i].name, dos_name, 11) == 0) {
                uint32_t file_cluster = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
                size_t text_len = strlen(text);

                if (file_cluster < 2) {
                    file_cluster = alloc_free_cluster();
                    if (file_cluster == 0) return -1;
                    entries[i].fst_clus_hi = (uint16_t)(file_cluster >> 16);
                    entries[i].fst_clus_lo = (uint16_t)(file_cluster & 0xFFFF);
                }

                uint8_t data_buf[4096];
                memset(data_buf, 0, sizeof(data_buf));

                if (append && entries[i].file_size > 0) {
                    read_cluster(file_cluster, data_buf);
                    size_t cur_sz = entries[i].file_size;
                    size_t to_copy = text_len;
                    if (cur_sz + to_copy > sizeof(data_buf)) {
                        to_copy = sizeof(data_buf) - cur_sz;
                    }
                    memcpy(data_buf + cur_sz, text, to_copy);
                    entries[i].file_size = (uint32_t)(cur_sz + to_copy);
                } else {
                    size_t to_copy = text_len > sizeof(data_buf) ? sizeof(data_buf) : text_len;
                    memcpy(data_buf, text, to_copy);
                    entries[i].file_size = (uint32_t)to_copy;
                }

                write_cluster(file_cluster, data_buf);
                write_cluster(c, cluster_buf);
                return (int)entries[i].file_size;
            }
        }
        c = get_fat_entry(c);
    }

    /* If file doesn't exist, create it then write */
    if (fat32_sync_create_file(path) == 0) {
        return fat32_sync_write_file(path, text, 0);
    }

    return -1;
}

int fat32_sync_mkdir(const char *path) {
    if (!g_is_fat32_mounted || !path) return -1;

    char dir_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(path, dir_name);
    if (dir_name[0] == '\0') return -1;

    char dos_name[11];
    to_dos_name(dir_name, dos_name);

    uint32_t new_dir_clus = alloc_free_cluster();
    if (new_dir_clus == 0) return -1;

    /* Initialize new directory cluster with . and .. */
    uint8_t dir_buf[4096];
    memset(dir_buf, 0, sizeof(dir_buf));
    FatDirEntry *dir_entries = (FatDirEntry *)dir_buf;

    /* Entry 0: . */
    memcpy(dir_entries[0].name, ".          ", 11);
    dir_entries[0].attr = 0x10;
    dir_entries[0].fst_clus_hi = (uint16_t)(new_dir_clus >> 16);
    dir_entries[0].fst_clus_lo = (uint16_t)(new_dir_clus & 0xFFFF);

    /* Entry 1: .. */
    memcpy(dir_entries[1].name, "..         ", 11);
    dir_entries[1].attr = 0x10;
    uint32_t p_clus = (parent_cluster == g_sync_root_cluster) ? 0 : parent_cluster;
    dir_entries[1].fst_clus_hi = (uint16_t)(p_clus >> 16);
    dir_entries[1].fst_clus_lo = (uint16_t)(p_clus & 0xFFFF);

    write_cluster(new_dir_clus, dir_buf);

    /* Insert into parent directory cluster */
    uint32_t c = parent_cluster;
    uint8_t cluster_buf[4096];

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) return -1;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = g_sync_cluster_size / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                memset(&entries[i], 0, sizeof(FatDirEntry));
                memcpy(entries[i].name, dos_name, 11);
                entries[i].attr = 0x10; /* Directory */
                entries[i].fst_clus_hi = (uint16_t)(new_dir_clus >> 16);
                entries[i].fst_clus_lo = (uint16_t)(new_dir_clus & 0xFFFF);

                write_cluster(c, cluster_buf);
                return 0;
            }
        }
        uint32_t next = get_fat_entry(c);
        if (next >= 0x0FFFFFF8) {
            uint32_t ext_clus = alloc_free_cluster();
            if (ext_clus == 0) return -1;
            set_fat_entry(c, ext_clus);
            c = ext_clus;
        } else {
            c = next;
        }
    }
    return -1;
}

static void recursive_free_dir_clusters(uint32_t dir_cluster) {
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8) return;

    uint8_t cluster_buf[4096];
    uint32_t c = dir_cluster;

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) break;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = g_sync_cluster_size / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr == 0x0F) continue;
            if (memcmp(entries[i].name, ".          ", 11) == 0) continue;
            if (memcmp(entries[i].name, "..         ", 11) == 0) continue;

            uint32_t child_clus = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
            if (entries[i].attr & 0x10) {
                recursive_free_dir_clusters(child_clus);
            }
            free_cluster_chain(child_clus);
        }
        c = get_fat_entry(c);
    }

    free_cluster_chain(dir_cluster);
}

int fat32_sync_delete_node(const char *path, int is_dir) {
    if (!g_is_fat32_mounted || !path) return -1;

    char node_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(path, node_name);
    if (node_name[0] == '\0') return -1;

    char dos_name[11];
    to_dos_name(node_name, dos_name);

    uint32_t c = parent_cluster;
    uint8_t cluster_buf[4096];

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) return -1;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = g_sync_cluster_size / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;

            if (memcmp(entries[i].name, dos_name, 11) == 0) {
                uint32_t target_cluster = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;

                /* 1. Mark entry as deleted (0xE5) */
                entries[i].name[0] = (char)0xE5;
                write_cluster(c, cluster_buf);

                /* 2. Free physical clusters on disk */
                if (is_dir || (entries[i].attr & 0x10)) {
                    recursive_free_dir_clusters(target_cluster);
                } else {
                    free_cluster_chain(target_cluster);
                }
                return 0;
            }
        }
        c = get_fat_entry(c);
    }

    return -1;
}

static void load_fat32_dir_recursive(uint32_t dir_cluster, const char *vfs_parent_path) {
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8) return;

    uint8_t cluster_buf[4096];
    uint32_t c = dir_cluster;

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) break;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = g_sync_cluster_size / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr == 0x0F) continue;
            if (memcmp(entries[i].name, ".          ", 11) == 0) continue;
            if (memcmp(entries[i].name, "..         ", 11) == 0) continue;

            char entry_name[64];
            from_dos_name(entries[i].name, entry_name);

            char sub_path[256];
            if (strcmp(vfs_parent_path, "/") == 0) {
                snprintf(sub_path, sizeof(sub_path), "/%s", entry_name);
            } else {
                snprintf(sub_path, sizeof(sub_path), "%s/%s", vfs_parent_path, entry_name);
            }

            uint32_t start_cluster = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;

            if (entries[i].attr & 0x10) {
                /* Directory */
                vfs_node_t *dir_node = vfs_mkdir(sub_path);
                if (strcasecmp(entry_name, "EFI") == 0 || strcasecmp(entry_name, "protected") == 0) {
                    if (dir_node) dir_node->is_protected = 1;
                }
                load_fat32_dir_recursive(start_cluster, sub_path);
            } else {
                /* File */
                vfs_node_t *file_node = vfs_create_file(sub_path);
                if (file_node) {
                    file_node->size = entries[i].file_size;
                    /* Read file content into memory if text/config file */
                    if (entries[i].file_size > 0 && entries[i].file_size <= 65536) {
                        uint8_t file_buf[4096];
                        if (read_cluster(start_cluster, file_buf) == 0) {
                            file_node->capacity = (entries[i].file_size + 1 + 255) & ~255;
                            file_node->content = (char *)kmalloc(file_node->capacity);
                            if (file_node->content) {
                                size_t to_copy = entries[i].file_size > sizeof(file_buf) ? sizeof(file_buf) : entries[i].file_size;
                                memcpy(file_node->content, file_buf, to_copy);
                                file_node->content[to_copy] = '\0';
                            }
                        }
                    }
                }
            }
        }
        c = get_fat_entry(c);
    }
}

int fat32_mount_disk(StorageDevice *dev) {
    if (!dev || !dev->read_sectors || !dev->write_sectors) return -1;

    StorageFsInfo info;
    if (storage_inspect_fs(dev, &info) != 0 || !info.has_filesystem) {
        return -1;
    }

    g_sync_dev = dev;
    g_sync_part_lba = info.part_start_lba;
    g_sync_spc = info.sectors_per_cluster;
    g_sync_bps = info.bytes_per_sector;
    g_sync_cluster_size = info.cluster_size;
    g_sync_reserved_sectors = info.reserved_sectors;
    g_sync_num_fats = info.num_fats;
    g_sync_fat_size_sectors = info.fat_size_sectors;
    g_sync_root_cluster = info.root_cluster > 0 ? info.root_cluster : 2;
    g_sync_data_lba_base = g_sync_part_lba + g_sync_reserved_sectors + (uint64_t)g_sync_num_fats * g_sync_fat_size_sectors;
    g_is_fat32_mounted = 0; /* Keep disabled during initial load */

    /* Load actual directory tree from disk sectors */
    load_fat32_dir_recursive(g_sync_root_cluster, "/");
    vfs_chdir("/");

    /* Enable live sync write-through */
    g_is_fat32_mounted = 1;

    return 0;
}
