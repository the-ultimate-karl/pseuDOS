#include "fs.h"
#include "storage.h"
#include "bootinfo.h"
#include "drivers.h"
#include "lib.h"
#include "klog.h"
#include "syscall.h"

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

typedef struct {
    int active;
    char mount_path[64];
    StorageDevice *dev;
    uint64_t part_lba;
    uint32_t spc;
    uint32_t bps;
    uint32_t cluster_size;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint64_t data_lba_base;
    uint32_t next_alloc_cluster;
} Fat32MountEntry;

#define MAX_FAT32_MOUNTS 16
static Fat32MountEntry g_fat32_mounts[MAX_FAT32_MOUNTS];
static int g_fat32_loading = 0;

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

static inline uint32_t get_clus_buf_size(void) {
    uint32_t sz = g_sync_cluster_size;
    if (sz < 4096) sz = 4096;
    if (sz > 65536) sz = 65536;
    return sz;
}

static uint64_t cluster_to_lba(uint32_t cluster) {
    if (cluster < 2 || cluster >= 0x0FFFFFF8) return 0;
    if (g_sync_fat_size_sectors > 0) {
        uint32_t max_clusters = (g_sync_fat_size_sectors * 512) / 4;
        if (cluster >= max_clusters) return 0;
    }
    return g_sync_data_lba_base + ((uint64_t)(cluster - 2) * g_sync_spc);
}

static int read_cluster(uint32_t cluster, void *buf) {
    if (!g_sync_dev || !g_sync_dev->read_sectors || !buf) return -1;
    if (g_sync_spc == 0 || g_sync_spc > 128) return -1;
    uint64_t lba = cluster_to_lba(cluster);
    if (lba == 0) return -1;
    if (g_sync_dev->total_sectors > 0 && lba + g_sync_spc > g_sync_dev->total_sectors) return -1;
    return g_sync_dev->read_sectors(g_sync_dev, lba, g_sync_spc, buf);
}

static int write_cluster(uint32_t cluster, const void *buf) {
    if (!g_sync_dev || !g_sync_dev->write_sectors || !buf) return -1;
    if (g_sync_spc == 0 || g_sync_spc > 128) return -1;
    uint64_t lba = cluster_to_lba(cluster);
    if (lba == 0) return -1;
    if (g_sync_dev->total_sectors > 0 && lba + g_sync_spc > g_sync_dev->total_sectors) return -1;
    return g_sync_dev->write_sectors(g_sync_dev, lba, g_sync_spc, buf);
}

static uint32_t s_cached_fat_sec = 0xFFFFFFFF;
static uint8_t s_fat_cache[512];
static int s_fat_cache_dirty = 0;

static int flush_fat_cache(void) {
    if (!g_sync_dev || !g_sync_dev->write_sectors || s_cached_fat_sec == 0xFFFFFFFF || !s_fat_cache_dirty) {
        return 0;
    }
    int ret = 0;
    /* Write to FAT1 */
    uint32_t fat1_sec = s_cached_fat_sec;
    if (g_sync_dev->write_sectors(g_sync_dev, fat1_sec, 1, s_fat_cache) != 0) ret = -1;
    /* Write to FAT2 */
    uint32_t sec_offset = fat1_sec - (g_sync_part_lba + g_sync_reserved_sectors);
    uint32_t fat2_sec = g_sync_part_lba + g_sync_reserved_sectors + g_sync_fat_size_sectors + sec_offset;
    if (g_sync_dev->write_sectors(g_sync_dev, fat2_sec, 1, s_fat_cache) != 0) ret = -1;

    s_fat_cache_dirty = 0;
    return ret;
}

static uint32_t get_fat_entry(uint32_t cluster) {
    if (!g_sync_dev || !g_sync_dev->read_sectors) return 0x0FFFFFFF;
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_sync_part_lba + g_sync_reserved_sectors + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    if (s_cached_fat_sec != fat_sector) {
        flush_fat_cache();
        if (g_sync_dev->read_sectors(g_sync_dev, fat_sector, 1, s_fat_cache) != 0) {
            return 0x0FFFFFFF;
        }
        s_cached_fat_sec = fat_sector;
    }
    return (*(uint32_t *)&s_fat_cache[entry_offset]) & 0x0FFFFFFF;
}

static int set_fat_entry(uint32_t cluster, uint32_t val) {
    if (!g_sync_dev || !g_sync_dev->read_sectors || !g_sync_dev->write_sectors) return -1;
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_sync_part_lba + g_sync_reserved_sectors + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    if (s_cached_fat_sec != fat_sector) {
        flush_fat_cache();
        if (g_sync_dev->read_sectors(g_sync_dev, fat_sector, 1, s_fat_cache) != 0) {
            return -1;
        }
        s_cached_fat_sec = fat_sector;
    }

    *(uint32_t *)&s_fat_cache[entry_offset] = val & 0x0FFFFFFF;
    s_fat_cache_dirty = 1;
    return 0;
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

static uint32_t s_next_alloc_cluster = 2;

static uint32_t alloc_free_cluster(void) {
    if (!g_sync_dev) return 0;
    uint8_t sec[512];

    uint32_t start_s = (s_next_alloc_cluster * 4) / 512;
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t s = start_s; s < g_sync_fat_size_sectors; s++) {
            uint64_t fat_sec = g_sync_part_lba + g_sync_reserved_sectors + s;
            if (s_cached_fat_sec == fat_sec) {
                memcpy(sec, s_fat_cache, 512);
            } else {
                flush_fat_cache();
                if (g_sync_dev->read_sectors(g_sync_dev, fat_sec, 1, sec) != 0) continue;
            }

            uint32_t *entries = (uint32_t *)sec;
            for (uint32_t i = 0; i < 128; i++) {
                uint32_t cluster_idx = s * 128 + i;
                if (cluster_idx >= 2 && (entries[i] & 0x0FFFFFFF) == 0) {
                    /* Allocate cluster */
                    set_fat_entry(cluster_idx, 0x0FFFFFFF);
                    s_next_alloc_cluster = cluster_idx + 1;
                    return cluster_idx;
                }
            }
        }
        start_s = 0; /* Wrap around */
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
        flush_fat_cache();
        update_fsinfo_free_clusters(freed_count);
    }
}

static int select_fat32_mount_for_path(const char *full_path, char *out_rel_path, size_t rel_cap) {
    if (g_fat32_loading || !full_path || full_path[0] != '/') return 0;

    Fat32MountEntry *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < MAX_FAT32_MOUNTS; i++) {
        if (!g_fat32_mounts[i].active) continue;
        const char *mp = g_fat32_mounts[i].mount_path;
        size_t mlen = strlen(mp);

        if (strcmp(mp, "/") == 0) {
            if (strncmp(full_path, "/tmp", 4) == 0 && (full_path[4] == '/' || full_path[4] == '\0')) continue;
            if (strncmp(full_path, "/dev", 4) == 0 && (full_path[4] == '/' || full_path[4] == '\0')) continue;
            if (strncmp(full_path, "/proc", 5) == 0 && (full_path[5] == '/' || full_path[5] == '\0')) continue;
            if (strncmp(full_path, "/mounts", 7) == 0 && (full_path[7] == '/' || full_path[7] == '\0')) continue;
            if (best_len < 1) {
                best = &g_fat32_mounts[i];
                best_len = 1;
            }
        } else if (mlen > 1) {
            if (strncmp(full_path, mp, mlen) == 0 && (full_path[mlen] == '/' || full_path[mlen] == '\0')) {
                if (mlen > best_len) {
                    best = &g_fat32_mounts[i];
                    best_len = mlen;
                }
            }
        }
    }

    if (!best) return 0;

    char rel[256];
    if (strcmp(best->mount_path, "/") == 0) {
        strncpy(rel, full_path, sizeof(rel) - 1);
        rel[sizeof(rel) - 1] = '\0';
    } else {
        if (full_path[best_len] == '\0') {
            strcpy(rel, "/");
        } else {
            strncpy(rel, full_path + best_len, sizeof(rel) - 1);
            rel[sizeof(rel) - 1] = '\0';
        }
    }

    if (strcmp(rel, "/") == 0 || strcmp(rel, "/.raw") == 0 || strcmp(rel, "/.root") == 0) {
        return 0;
    }

    if (out_rel_path && rel_cap > 0) {
        strncpy(out_rel_path, rel, rel_cap - 1);
        out_rel_path[rel_cap - 1] = '\0';
    }

    if (g_sync_dev != best->dev || g_sync_part_lba != best->part_lba) {
        s_cached_fat_sec = 0xFFFFFFFF;
    }
    g_sync_dev = best->dev;
    g_sync_part_lba = best->part_lba;
    g_sync_spc = best->spc;
    g_sync_bps = best->bps;
    g_sync_cluster_size = best->cluster_size;
    g_sync_reserved_sectors = best->reserved_sectors;
    g_sync_num_fats = best->num_fats;
    g_sync_fat_size_sectors = best->fat_size_sectors;
    g_sync_root_cluster = best->root_cluster;
    g_sync_data_lba_base = best->data_lba_base;
    return 1;
}

int fat32_has_mount_for_path(const char *path) {
    char rel[256];
    return select_fat32_mount_for_path(path, rel, sizeof(rel));
}

void fat32_unmount_path(const char *mount_path) {
    if (!mount_path) return;
    for (int i = 0; i < MAX_FAT32_MOUNTS; i++) {
        if (g_fat32_mounts[i].active && strcmp(g_fat32_mounts[i].mount_path, mount_path) == 0) {
            g_fat32_mounts[i].active = 0;
        }
    }
}

static uint8_t fat_lfn_checksum(const char dos_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)dos_name[i]);
    }
    return sum;
}

typedef struct {
    char     buf[256];
    uint8_t  checksum;
    int      valid;
    int      max_seq;
    uint32_t start_idx;
} FatLfnCollector;

static void lfn_collector_init(FatLfnCollector *col) {
    memset(col, 0, sizeof(*col));
}

static void lfn_collector_feed(FatLfnCollector *col, const FatLfnEntry *lfn, uint32_t entry_idx) {
    uint8_t order = lfn->order;
    if (order == 0xE5) {
        col->valid = 0;
        return;
    }
    int seq = order & 0x1F;
    if (seq < 1 || seq > 20) {
        col->valid = 0;
        return;
    }

    if (order & 0x40) {
        memset(col->buf, 0, sizeof(col->buf));
        col->checksum = lfn->checksum;
        col->max_seq = seq;
        col->start_idx = entry_idx;
        col->valid = 1;
    } else {
        if (!col->valid || lfn->checksum != col->checksum) {
            col->valid = 0;
            return;
        }
    }

    int base_idx = (seq - 1) * 13;
    if (base_idx + 13 > (int)sizeof(col->buf)) {
        col->valid = 0;
        return;
    }

    uint16_t raw_chars[13];
    memcpy(&raw_chars[0], lfn->name1, 5 * sizeof(uint16_t));
    memcpy(&raw_chars[5], lfn->name2, 6 * sizeof(uint16_t));
    memcpy(&raw_chars[11], lfn->name3, 2 * sizeof(uint16_t));

    for (int k = 0; k < 13; k++) {
        uint16_t u = raw_chars[k];
        if (u == 0x0000 || u == 0xFFFF) {
            col->buf[base_idx + k] = '\0';
        } else {
            col->buf[base_idx + k] = (char)(u < 128 ? u : '?');
        }
    }
}

static void from_dos_name(const char *dos_name, char *out_name) {
    char name_part[9];
    char ext_part[4];
    int n_len = 0, e_len = 0;

    for (int i = 0; i < 8; i++) {
        if (dos_name[i] != ' ') {
            char c = dos_name[i];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            name_part[n_len++] = c;
        }
    }
    name_part[n_len] = '\0';

    for (int i = 8; i < 11; i++) {
        if (dos_name[i] != ' ') {
            char c = dos_name[i];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            ext_part[e_len++] = c;
        }
    }
    ext_part[e_len] = '\0';

    if (e_len > 0) {
        snprintf(out_name, 64, "%s.%s", name_part, ext_part);
    } else {
        snprintf(out_name, 64, "%s", name_part);
    }
}

static void generate_dos_short_name(const char *src, char dos_name[11], int tail_num) {
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

    const char *last_dot = strrchr(src, '.');
    size_t base_len = last_dot ? (size_t)(last_dot - src) : strlen(src);

    char clean_base[16];
    size_t cb_len = 0;
    for (size_t i = 0; i < base_len && cb_len < sizeof(clean_base) - 1; i++) {
        char c = src[i];
        if (c == ' ' || c == '.' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') continue;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        clean_base[cb_len++] = c;
    }
    if (cb_len == 0) {
        clean_base[0] = '_';
        cb_len = 1;
    }
    clean_base[cb_len] = '\0';

    char clean_ext[4];
    size_t ce_len = 0;
    if (last_dot) {
        const char *ext = last_dot + 1;
        for (size_t i = 0; ext[i] != '\0' && ce_len < 3; i++) {
            char c = ext[i];
            if (c == ' ' || c == '.' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') continue;
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            clean_ext[ce_len++] = c;
        }
    }
    clean_ext[ce_len] = '\0';

    if (tail_num > 0) {
        char tail_str[8];
        snprintf(tail_str, sizeof(tail_str), "~%d", tail_num);
        size_t tail_len = strlen(tail_str);
        size_t copy_base = (tail_len < 8) ? (8 - tail_len) : 1;
        if (copy_base > cb_len) copy_base = cb_len;

        for (size_t i = 0; i < copy_base; i++) {
            dos_name[i] = clean_base[i];
        }
        for (size_t i = 0; i < tail_len && (copy_base + i) < 8; i++) {
            dos_name[copy_base + i] = tail_str[i];
        }
    } else {
        size_t n = cb_len > 8 ? 8 : cb_len;
        for (size_t i = 0; i < n; i++) dos_name[i] = clean_base[i];
    }

    for (size_t i = 0; i < ce_len && i < 3; i++) {
        dos_name[8 + i] = clean_ext[i];
    }
}

static void get_resolved_entry_name(const FatDirEntry *entry, FatLfnCollector *col, char *out_name, size_t out_cap) {
    if (col->valid && col->buf[0] != '\0' && fat_lfn_checksum(entry->name) == col->checksum) {
        strncpy(out_name, col->buf, out_cap - 1);
        out_name[out_cap - 1] = '\0';
    } else {
        from_dos_name(entry->name, out_name);
    }
    col->valid = 0;
}

static int find_entry_in_dir_cluster(uint32_t dir_cluster, const char *target_name, int require_dir,
                                     uint32_t *out_cluster, uint32_t *out_idx, uint32_t *out_lfn_start_idx,
                                     FatDirEntry *out_entry) {
    if (dir_cluster < 2 || !target_name || target_name[0] == '\0') return -1;

    char dos_match[11];
    generate_dos_short_name(target_name, dos_match, 0);

    uint32_t c = dir_cluster;
    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return -1;

    FatLfnCollector col;

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }

        lfn_collector_init(&col);
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = c_sz / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) {
                kfree(cluster_buf);
                return -1;
            }
            if ((uint8_t)entries[i].name[0] == 0xE5) {
                col.valid = 0;
                continue;
            }
            if (entries[i].attr == 0x0F) {
                lfn_collector_feed(&col, (const FatLfnEntry *)&entries[i], i);
                continue;
            }
            if (entries[i].attr & 0x08) {
                col.valid = 0;
                continue;
            }

            uint32_t lfn_start = (col.valid && fat_lfn_checksum(entries[i].name) == col.checksum) ? col.start_idx : i;
            char resolved_name[256];
            get_resolved_entry_name(&entries[i], &col, resolved_name, sizeof(resolved_name));

            if (require_dir == 1 && !(entries[i].attr & 0x10)) continue;
            if (require_dir == 0 && (entries[i].attr & 0x10)) continue;

            if (strcasecmp(resolved_name, target_name) == 0 ||
                memcmp(entries[i].name, dos_match, 11) == 0) {
                if (out_cluster) *out_cluster = c;
                if (out_idx) *out_idx = i;
                if (out_lfn_start_idx) *out_lfn_start_idx = lfn_start;
                if (out_entry) *out_entry = entries[i];
                kfree(cluster_buf);
                return 0;
            }
        }

        c = get_fat_entry(c);
    }

    kfree(cluster_buf);
    return -1;
}

static int short_name_exists_in_dir(uint32_t dir_cluster, const char dos_name[11]) {
    uint32_t c = dir_cluster;
    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return 0;

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) break;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = c_sz / sizeof(FatDirEntry);
        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) {
                kfree(cluster_buf);
                return 0;
            }
            if ((uint8_t)entries[i].name[0] == 0xE5 || entries[i].attr == 0x0F) continue;
            if (memcmp(entries[i].name, dos_name, 11) == 0) {
                kfree(cluster_buf);
                return 1;
            }
        }
        c = get_fat_entry(c);
    }
    kfree(cluster_buf);
    return 0;
}

static void write_lfn_and_short_entries(FatDirEntry *entries, uint32_t start_slot,
                                        const char *long_name, const char dos_name[11],
                                        int num_lfn, uint8_t attr, uint32_t target_cluster, uint32_t file_size) {
    uint8_t chk = fat_lfn_checksum(dos_name);
    size_t name_len = strlen(long_name);

    for (int seq = num_lfn; seq >= 1; seq--) {
        uint32_t slot_idx = start_slot + (uint32_t)(num_lfn - seq);
        FatLfnEntry *lfn = (FatLfnEntry *)&entries[slot_idx];
        memset(lfn, 0, sizeof(FatLfnEntry));
        lfn->order = (uint8_t)seq;
        if (seq == num_lfn) lfn->order |= 0x40;
        lfn->attr = 0x0F;
        lfn->type = 0x00;
        lfn->checksum = chk;
        lfn->fst_clus_lo = 0;

        int start_char = (seq - 1) * 13;
        uint16_t chars[13];
        for (int k = 0; k < 13; k++) {
            size_t pos = (size_t)start_char + (size_t)k;
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
    }

    uint32_t short_idx = start_slot + (uint32_t)num_lfn;
    memset(&entries[short_idx], 0, sizeof(FatDirEntry));
    memcpy(entries[short_idx].name, dos_name, 11);
    entries[short_idx].attr = attr;
    entries[short_idx].fst_clus_hi = (uint16_t)(target_cluster >> 16);
    entries[short_idx].fst_clus_lo = (uint16_t)(target_cluster & 0xFFFF);
    entries[short_idx].file_size = file_size;
}

static int insert_entry_in_dir_cluster(uint32_t dir_cluster, const char *name, uint8_t attr,
                                       uint32_t target_cluster, uint32_t file_size) {
    if (dir_cluster < 2 || !name || name[0] == '\0') return -1;

    char dos_name[11];
    int is_dot = (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
    if (is_dot) {
        generate_dos_short_name(name, dos_name, 0);
    } else {
        /* Try clean 8.3 first if name fits within 8.3, otherwise or on collision use ~N tail */
        const char *dot = strrchr(name, '.');
        size_t blen = dot ? (size_t)(dot - name) : strlen(name);
        size_t elen = dot ? strlen(dot + 1) : 0;
        int has_space = (strchr(name, ' ') != NULL);
        int start_tail = (blen > 8 || elen > 3 || has_space) ? 1 : 0;

        for (int tail = start_tail; tail <= 999; tail++) {
            generate_dos_short_name(name, dos_name, tail);
            if (!short_name_exists_in_dir(dir_cluster, dos_name)) {
                break;
            }
        }
    }

    int num_lfn = is_dot ? 0 : (int)((strlen(name) + 12) / 13);
    if (num_lfn < 0) num_lfn = 0;
    uint32_t needed_slots = (uint32_t)num_lfn + 1;

    uint32_t c = dir_cluster;
    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return -1;

    uint32_t last_c = dir_cluster;

    while (c >= 2 && c < 0x0FFFFFF8) {
        last_c = c;
        if (read_cluster(c, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }

        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = c_sz / sizeof(FatDirEntry);

        for (uint32_t i = 0; i + needed_slots <= num_entries; i++) {
            int fits = 1;
            for (uint32_t k = 0; k < needed_slots; k++) {
                uint8_t first_b = (uint8_t)entries[i + k].name[0];
                if (first_b != 0x00 && first_b != 0xE5) {
                    fits = 0;
                    break;
                }
            }

            if (fits) {
                write_lfn_and_short_entries(entries, i, name, dos_name, num_lfn, attr, target_cluster, file_size);
                write_cluster(c, cluster_buf);
                if (g_sync_dev && g_sync_dev->flush) {
                    g_sync_dev->flush(g_sync_dev);
                }
                kfree(cluster_buf);
                return 0;
            }
        }

        c = get_fat_entry(c);
    }

    /* Expand directory with a new cluster */
    uint32_t ext_clus = alloc_free_cluster();
    if (ext_clus == 0) {
        kfree(cluster_buf);
        return -1;
    }
    set_fat_entry(last_c, ext_clus);

    memset(cluster_buf, 0, c_sz);
    FatDirEntry *entries = (FatDirEntry *)cluster_buf;
    write_lfn_and_short_entries(entries, 0, name, dos_name, num_lfn, attr, target_cluster, file_size);

    write_cluster(ext_clus, cluster_buf);
    if (g_sync_dev && g_sync_dev->flush) {
        g_sync_dev->flush(g_sync_dev);
    }
    kfree(cluster_buf);
    return 0;
}

static int ensure_subdir_in_cluster(uint32_t parent_cluster, const char *dir_name, uint32_t *out_clus) {
    if (parent_cluster < 2 || !dir_name || dir_name[0] == '\0') return -1;

    FatDirEntry found;
    if (find_entry_in_dir_cluster(parent_cluster, dir_name, 1, NULL, NULL, NULL, &found) == 0) {
        if (out_clus) *out_clus = ((uint32_t)found.fst_clus_hi << 16) | found.fst_clus_lo;
        return 0;
    }

    uint32_t new_dir_clus = alloc_free_cluster();
    if (new_dir_clus == 0) return -1;

    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return -1;

    memset(cluster_buf, 0, c_sz);
    FatDirEntry *dir_entries = (FatDirEntry *)cluster_buf;
    memcpy(dir_entries[0].name, ".          ", 11);
    dir_entries[0].attr = 0x10;
    dir_entries[0].fst_clus_hi = (uint16_t)(new_dir_clus >> 16);
    dir_entries[0].fst_clus_lo = (uint16_t)(new_dir_clus & 0xFFFF);

    memcpy(dir_entries[1].name, "..         ", 11);
    dir_entries[1].attr = 0x10;
    uint32_t p_clus = (parent_cluster == g_sync_root_cluster) ? 0 : parent_cluster;
    dir_entries[1].fst_clus_hi = (uint16_t)(p_clus >> 16);
    dir_entries[1].fst_clus_lo = (uint16_t)(p_clus & 0xFFFF);

    write_cluster(new_dir_clus, cluster_buf);
    kfree(cluster_buf);

    if (insert_entry_in_dir_cluster(parent_cluster, dir_name, 0x10, new_dir_clus, 0) != 0) {
        return -1;
    }
    if (out_clus) *out_clus = new_dir_clus;
    return 0;
}

static uint32_t find_path_parent_cluster(const char *path, char *out_child_name, int create_missing) {
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

    uint32_t curr_cluster = g_sync_root_cluster;
    char *token = norm;
    if (*token == '/' || *token == '\\') token++;

    while (*token) {
        char *slash = strchr(token, '/');
        if (!slash) slash = strchr(token, '\\');
        if (slash) *slash = '\0';

        FatDirEntry found;
        uint32_t next_cluster = 0;
        if (find_entry_in_dir_cluster(curr_cluster, token, 1, NULL, NULL, NULL, &found) == 0) {
            next_cluster = ((uint32_t)found.fst_clus_hi << 16) | found.fst_clus_lo;
        } else if (create_missing) {
            if (ensure_subdir_in_cluster(curr_cluster, token, &next_cluster) != 0 || next_cluster == 0) {
                return 0;
            }
        } else {
            return 0;
        }

        curr_cluster = next_cluster;
        if (!slash) break;
        token = slash + 1;
    }

    return curr_cluster;
}

int fat32_sync_create_file(const char *path) {
    char rel_path[256];
    if (!select_fat32_mount_for_path(path, rel_path, sizeof(rel_path))) return -1;

    char file_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(rel_path, file_name, 1);
    if (parent_cluster < 2 || file_name[0] == '\0') return -1;

    FatDirEntry found;
    if (find_entry_in_dir_cluster(parent_cluster, file_name, 0, NULL, NULL, NULL, &found) == 0) {
        return 0;
    }

    uint32_t new_cluster = alloc_free_cluster();
    if (new_cluster == 0) return -1;

    return insert_entry_in_dir_cluster(parent_cluster, file_name, 0x20, new_cluster, 0);
}

int fat32_sync_write_file_bytes(const char *path, const void *data, size_t size, int append) {
    char rel_path[256];
    if (!select_fat32_mount_for_path(path, rel_path, sizeof(rel_path))) return -1;
    const uint8_t *src_bytes = (const uint8_t *)data;

    char file_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(rel_path, file_name, 1);
    if (parent_cluster < 2 || file_name[0] == '\0') return -1;

    uint32_t entry_clus = 0, entry_idx = 0;
    FatDirEntry entry;
    if (find_entry_in_dir_cluster(parent_cluster, file_name, 0, &entry_clus, &entry_idx, NULL, &entry) != 0) {
        if (fat32_sync_create_file(path) != 0) return -1;
        if (find_entry_in_dir_cluster(parent_cluster, file_name, 0, &entry_clus, &entry_idx, NULL, &entry) != 0) {
            return -1;
        }
    }

    uint32_t file_cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    uint32_t old_size = entry.file_size;
    uint32_t new_size = (append && old_size > 0) ? (old_size + (uint32_t)size) : (uint32_t)size;
    uint32_t c_sz = get_clus_buf_size();

    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return -1;

    if (new_size == 0) {
        if (file_cluster >= 2 && file_cluster < 0x0FFFFFF8) {
            free_cluster_chain(file_cluster);
        }
        if (read_cluster(entry_clus, cluster_buf) == 0) {
            FatDirEntry *entries = (FatDirEntry *)cluster_buf;
            entries[entry_idx].fst_clus_hi = 0;
            entries[entry_idx].fst_clus_lo = 0;
            entries[entry_idx].file_size = 0;
            write_cluster(entry_clus, cluster_buf);
            if (g_sync_dev && g_sync_dev->flush) g_sync_dev->flush(g_sync_dev);
        }
        kfree(cluster_buf);
        return 0;
    }

    uint32_t clusters_needed = (new_size + c_sz - 1) / c_sz;
    if (clusters_needed == 0) clusters_needed = 1;

    if (file_cluster < 2) {
        file_cluster = alloc_free_cluster();
        if (file_cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }
    }

    uint32_t *chain = (uint32_t *)kmalloc(clusters_needed * sizeof(uint32_t));
    uint8_t *data_buf = (uint8_t *)kmalloc(c_sz);
    if (!chain || !data_buf) {
        if (chain) kfree(chain);
        if (data_buf) kfree(data_buf);
        kfree(cluster_buf);
        return -1;
    }

    uint32_t chain_len = 0;
    uint32_t cur_c = file_cluster;
    chain[chain_len++] = cur_c;

    while (chain_len < clusters_needed) {
        uint32_t next = get_fat_entry(cur_c);
        if (next < 2 || next >= 0x0FFFFFF8) {
            uint32_t new_c = alloc_free_cluster();
            if (new_c == 0) break;
            set_fat_entry(cur_c, new_c);
            cur_c = new_c;
        } else {
            cur_c = next;
        }
        chain[chain_len++] = cur_c;
    }

    uint32_t trailing = get_fat_entry(cur_c);
    set_fat_entry(cur_c, 0x0FFFFFFF);
    if (trailing >= 2 && trailing < 0x0FFFFFF8) {
        free_cluster_chain(trailing);
    }

    size_t src_pos = 0;
    for (uint32_t k = 0; k < chain_len; k++) {
        uint32_t clus_id = chain[k];
        uint32_t clus_file_offset = k * c_sz;

        /* Optimize contiguous full clusters for faster multi-sector writes */
        if (!append && src_bytes) {
            uint32_t contig = 1;
            while (k + contig < chain_len && chain[k + contig] == chain[k] + contig && (src_pos + (size_t)(contig + 1) * c_sz) <= size) {
                contig++;
            }
            if (contig > 1 && (src_pos + (size_t)contig * c_sz) <= size) {
                uint64_t lba = cluster_to_lba(chain[k]);
                if (lba > 0 && g_sync_dev->write_sectors(g_sync_dev, lba, contig * g_sync_spc, src_bytes + src_pos) == 0) {
                    src_pos += contig * c_sz;
                    k += contig - 1;
                    continue;
                }
            }
        }

        memset(data_buf, 0, c_sz);

        if (append && old_size > 0 && clus_file_offset < old_size) {
            read_cluster(clus_id, data_buf);
            uint32_t start_in_clus = old_size - clus_file_offset;
            if (start_in_clus < c_sz && src_pos < size) {
                uint32_t to_write = c_sz - start_in_clus;
                if (to_write > (uint32_t)(size - src_pos)) to_write = (uint32_t)(size - src_pos);
                if (src_bytes) memcpy(data_buf + start_in_clus, src_bytes + src_pos, to_write);
                src_pos += to_write;
            }
        } else {
            if (src_pos < size) {
                uint32_t to_write = c_sz;
                if (to_write > (uint32_t)(size - src_pos)) to_write = (uint32_t)(size - src_pos);
                if (src_bytes) memcpy(data_buf, src_bytes + src_pos, to_write);
                src_pos += to_write;
            }
        }
        write_cluster(clus_id, data_buf);
    }

    kfree(data_buf);
    kfree(chain);
    flush_fat_cache();
    update_fsinfo_free_clusters(-(int)chain_len);

    if (read_cluster(entry_clus, cluster_buf) == 0) {
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        entries[entry_idx].fst_clus_hi = (uint16_t)(file_cluster >> 16);
        entries[entry_idx].fst_clus_lo = (uint16_t)(file_cluster & 0xFFFF);
        entries[entry_idx].file_size = new_size;
        write_cluster(entry_clus, cluster_buf);
        if (g_sync_dev && g_sync_dev->flush) {
            g_sync_dev->flush(g_sync_dev);
        }
    }

    kfree(cluster_buf);
    return (int)new_size;
}

int fat32_sync_write_file(const char *path, const char *text, int append) {
    if (!text) return -1;
    return fat32_sync_write_file_bytes(path, text, strlen(text), append);
}

int fat32_sync_mkdir(const char *path) {
    char rel_path[256];
    if (!select_fat32_mount_for_path(path, rel_path, sizeof(rel_path))) return -1;

    char dir_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(rel_path, dir_name, 1);
    if (parent_cluster < 2 || dir_name[0] == '\0') return -1;

    uint32_t new_dir_clus = 0;
    return ensure_subdir_in_cluster(parent_cluster, dir_name, &new_dir_clus);
}

static void recursive_free_dir_clusters_depth(uint32_t dir_cluster, int depth) {
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8 || depth > 8) return;

    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return;

    uint32_t c = dir_cluster;

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (read_cluster(c, cluster_buf) != 0) break;
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = c_sz / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr == 0x0F) continue;
            if (memcmp(entries[i].name, ".          ", 11) == 0) continue;
            if (memcmp(entries[i].name, "..         ", 11) == 0) continue;

            uint32_t child_clus = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
            if (entries[i].attr & 0x10) {
                recursive_free_dir_clusters_depth(child_clus, depth + 1);
            }
            free_cluster_chain(child_clus);
        }
        c = get_fat_entry(c);
    }

    kfree(cluster_buf);
    free_cluster_chain(dir_cluster);
}

int fat32_sync_delete_node(const char *path, int is_dir) {
    char rel_path[256];
    if (!select_fat32_mount_for_path(path, rel_path, sizeof(rel_path))) return -2;

    char node_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(rel_path, node_name, 0);
    if (parent_cluster < 2 || node_name[0] == '\0') return -2;

    uint32_t entry_clus = 0, entry_idx = 0, lfn_start_idx = 0;
    FatDirEntry entry;
    if (find_entry_in_dir_cluster(parent_cluster, node_name, is_dir ? 1 : -1,
                                  &entry_clus, &entry_idx, &lfn_start_idx, &entry) != 0) {
        return -2;
    }

    uint32_t target_cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return -1;

    if (read_cluster(entry_clus, cluster_buf) == 0) {
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        for (uint32_t k = lfn_start_idx; k <= entry_idx; k++) {
            entries[k].name[0] = (char)0xE5;
        }
        if (write_cluster(entry_clus, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -1;
        }
    }
    kfree(cluster_buf);

    if (is_dir || (entry.attr & 0x10)) {
        recursive_free_dir_clusters_depth(target_cluster, 0);
    } else {
        free_cluster_chain(target_cluster);
    }

    if (g_sync_dev && g_sync_dev->flush) {
        g_sync_dev->flush(g_sync_dev);
    }
    return 0;
}

int fat32_sync_read_file_bytes(const char *path, void *buffer, size_t max_len) {
    if (!buffer || max_len == 0) return -1;
    char rel_path[256];
    if (!select_fat32_mount_for_path(path, rel_path, sizeof(rel_path))) return -1;

    char file_name[64];
    uint32_t parent_cluster = find_path_parent_cluster(rel_path, file_name, 0);
    if (parent_cluster < 2 || file_name[0] == '\0') return -1;

    FatDirEntry entry;
    if (find_entry_in_dir_cluster(parent_cluster, file_name, 0, NULL, NULL, NULL, &entry) != 0) {
        return -1;
    }

    uint32_t fc = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    size_t target_size = entry.file_size;
    if (target_size > max_len) target_size = max_len;

    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return -1;

    size_t bytes_read = 0;
    uint8_t *dst = (uint8_t *)buffer;
    while (fc >= 2 && fc < 0x0FFFFFF8 && bytes_read < target_size) {
        size_t needed = target_size - bytes_read;
        if (needed >= c_sz) {
            if (read_cluster(fc, dst + bytes_read) != 0) break;
            bytes_read += c_sz;
        } else {
            if (read_cluster(fc, cluster_buf) != 0) break;
            memcpy(dst + bytes_read, cluster_buf, needed);
            bytes_read += needed;
        }
        fc = get_fat_entry(fc);
    }

    kfree(cluster_buf);
    return (int)bytes_read;
}

static uint32_t s_mount_loaded_nodes = 0;
static size_t   s_mount_loaded_bytes = 0;

static int load_fat32_dir_recursive_depth(uint32_t dir_cluster, const char *vfs_parent_path, int depth) {
    if (dir_cluster < 2 || dir_cluster >= 0x0FFFFFF8 || depth > 8) return -1;

    uint32_t c_sz = get_clus_buf_size();
    uint8_t *cluster_buf = (uint8_t *)kmalloc(c_sz);
    if (!cluster_buf) return -1;

    uint32_t c = dir_cluster;
    int first_cluster = 1;
    uint32_t chain_guard = 0;
    size_t max_file_preload = 512 * 1024;
    size_t max_vol_preload  = 4 * 1024 * 1024;

    FatLfnCollector col;

    while (c >= 2 && c < 0x0FFFFFF8 && chain_guard++ < 1024) {
        if (read_cluster(c, cluster_buf) != 0) {
            if (first_cluster) {
                kfree(cluster_buf);
                return -1;
            }
            break;
        }
        first_cluster = 0;
        lfn_collector_init(&col);
        FatDirEntry *entries = (FatDirEntry *)cluster_buf;
        uint32_t num_entries = c_sz / sizeof(FatDirEntry);

        for (uint32_t i = 0; i < num_entries; i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) {
                col.valid = 0;
                continue;
            }
            if (entries[i].attr == 0x0F) {
                lfn_collector_feed(&col, (const FatLfnEntry *)&entries[i], i);
                continue;
            }
            if (entries[i].attr & 0x08) {
                col.valid = 0;
                continue; /* Skip volume label entry */
            }
            if (memcmp(entries[i].name, ".          ", 11) == 0 ||
                memcmp(entries[i].name, "..         ", 11) == 0) {
                col.valid = 0;
                continue;
            }

            if (s_mount_loaded_nodes >= 512) break;

            char entry_name[64];
            get_resolved_entry_name(&entries[i], &col, entry_name, sizeof(entry_name));
            if (entry_name[0] == '\0') continue;

            char sub_path[256];
            if (strcmp(vfs_parent_path, "/") == 0) {
                snprintf(sub_path, sizeof(sub_path), "/%s", entry_name);
            } else {
                snprintf(sub_path, sizeof(sub_path), "%s/%s", vfs_parent_path, entry_name);
            }

            uint32_t start_cluster = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;

            if (entries[i].attr & 0x10) {
                /* Directory */
                s_mount_loaded_nodes++;
                vfs_node_t *dir_node = vfs_mkdir(sub_path);
                if (strcasecmp(entry_name, "EFI") == 0 || strcasecmp(entry_name, "protected") == 0) {
                    if (dir_node) dir_node->is_protected = 1;
                }
                if (start_cluster >= 2 && start_cluster != dir_cluster) {
                    load_fat32_dir_recursive_depth(start_cluster, sub_path, depth + 1);
                }
            } else {
                /* File */
                s_mount_loaded_nodes++;
                vfs_node_t *file_node = vfs_create_file(sub_path);
                if (file_node) {
                    file_node->size = entries[i].file_size;
                    /* Read file content into memory across FAT cluster chain (within safety budget) */
                    int skip_body = (strcasecmp(entry_name, "vpkernel") == 0 ||
                                     strcasecmp(entry_name, "BOOTX64.EFI") == 0 ||
                                     strcasecmp(entry_name, "recovery.efi") == 0 ||
                                     strcasecmp(entry_name, "EFIBOOT.IMG") == 0 ||
                                     strcasecmp(entry_name, "initramfs.img") == 0);
                    if (!skip_body &&
                        entries[i].file_size > 0 &&
                        entries[i].file_size <= max_file_preload &&
                        s_mount_loaded_bytes + entries[i].file_size <= max_vol_preload) {
                        file_node->capacity = (entries[i].file_size + 1 + 255) & ~255;
                        file_node->content = (char *)kmalloc(file_node->capacity);
                        if (file_node->content) {
                            uint32_t fc = start_cluster;
                            size_t bytes_read = 0;
                            size_t clus_size = c_sz;
                            uint8_t *tail_buf = NULL;

                            while (fc >= 2 && fc < 0x0FFFFFF8 && bytes_read < entries[i].file_size) {
                                size_t needed = entries[i].file_size - bytes_read;

                                /* Count contiguous clusters in the chain */
                                uint32_t contig = 1;
                                uint32_t next_c = get_fat_entry(fc);
                                while (next_c == fc + contig && ((size_t)(contig + 1) * clus_size) <= needed) {
                                    contig++;
                                    next_c = get_fat_entry(fc + contig - 1);
                                }

                                size_t contig_bytes = (size_t)contig * clus_size;
                                if (contig_bytes <= needed) {
                                    uint64_t lba = cluster_to_lba(fc);
                                    uint32_t sec_count = contig * g_sync_spc;
                                    if (g_sync_dev->read_sectors(g_sync_dev, lba, sec_count, file_node->content + bytes_read) != 0) break;
                                    bytes_read += contig_bytes;
                                    fc = next_c;
                                } else {
                                    if (!tail_buf) tail_buf = (uint8_t *)kmalloc(clus_size);
                                    if (!tail_buf || read_cluster(fc, tail_buf) != 0) break;
                                    size_t chunk = needed < clus_size ? needed : clus_size;
                                    memcpy(file_node->content + bytes_read, tail_buf, chunk);
                                    bytes_read += chunk;
                                    fc = next_c;
                                }
                            }
                            if (tail_buf) kfree(tail_buf);
                            file_node->content[bytes_read] = '\0';
                            s_mount_loaded_bytes += bytes_read;
                        } else {
                            file_node->capacity = 0;
                        }
                    }
                }
            }
        }
        if (s_mount_loaded_nodes >= 512) break;
        c = get_fat_entry(c);
    }
    kfree(cluster_buf);
    return 0;
}

static int load_fat32_dir_recursive(uint32_t dir_cluster, const char *vfs_parent_path) {
    s_mount_loaded_nodes = 0;
    s_mount_loaded_bytes = 0;
    return load_fat32_dir_recursive_depth(dir_cluster, vfs_parent_path, 0);
}

int fat32_mount_to_path(StorageDevice *dev, const char *mount_path) {
    if (!dev || !dev->read_sectors || !mount_path) return -1;

    StorageFsInfo info;
    if (storage_inspect_fs(dev, &info) != 0 || !info.has_filesystem || strcmp(info.fs_type, "FAT32") != 0) {
        return -1;
    }

    StorageDevice *prev_dev = g_sync_dev;
    uint64_t prev_part_lba = g_sync_part_lba;
    uint32_t prev_spc = g_sync_spc;
    uint32_t prev_bps = g_sync_bps;
    uint32_t prev_cluster_size = g_sync_cluster_size;
    uint32_t prev_reserved = g_sync_reserved_sectors;
    uint32_t prev_num_fats = g_sync_num_fats;
    uint32_t prev_fat_size = g_sync_fat_size_sectors;
    uint32_t prev_root_cluster = g_sync_root_cluster;
    uint64_t prev_data_lba = g_sync_data_lba_base;

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
    s_cached_fat_sec = 0xFFFFFFFF;
    s_next_alloc_cluster = 2;

    g_fat32_loading = 1;
    vfs_mkdir(mount_path);
    klog_info("FAT32 Filesystem: reading volume files into '%s' (root_clus=%u)...", mount_path, g_sync_root_cluster);
    if (load_fat32_dir_recursive(g_sync_root_cluster, mount_path) != 0) {
        klog_err("fat32: failed to read root directory from volume on '%s'", mount_path);
        g_fat32_loading = 0;
        g_sync_dev = prev_dev;
        g_sync_part_lba = prev_part_lba;
        g_sync_spc = prev_spc;
        g_sync_bps = prev_bps;
        g_sync_cluster_size = prev_cluster_size;
        g_sync_reserved_sectors = prev_reserved;
        g_sync_num_fats = prev_num_fats;
        g_sync_fat_size_sectors = prev_fat_size;
        g_sync_root_cluster = prev_root_cluster;
        g_sync_data_lba_base = prev_data_lba;
        return -1;
    }
    g_fat32_loading = 0;
    klog_info("FAT32 Filesystem: finished loading '%s'", mount_path);

    /* Register mounted FAT32 volume in g_fat32_mounts for write-through sync */
    int slot = -1;
    for (int i = 0; i < MAX_FAT32_MOUNTS; i++) {
        if (g_fat32_mounts[i].active && strcmp(g_fat32_mounts[i].mount_path, mount_path) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_FAT32_MOUNTS; i++) {
            if (!g_fat32_mounts[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot >= 0) {
        g_fat32_mounts[slot].active = 1;
        strncpy(g_fat32_mounts[slot].mount_path, mount_path, sizeof(g_fat32_mounts[slot].mount_path) - 1);
        g_fat32_mounts[slot].mount_path[sizeof(g_fat32_mounts[slot].mount_path) - 1] = '\0';
        g_fat32_mounts[slot].dev = dev;
        g_fat32_mounts[slot].part_lba = g_sync_part_lba;
        g_fat32_mounts[slot].spc = g_sync_spc;
        g_fat32_mounts[slot].bps = g_sync_bps;
        g_fat32_mounts[slot].cluster_size = g_sync_cluster_size;
        g_fat32_mounts[slot].reserved_sectors = g_sync_reserved_sectors;
        g_fat32_mounts[slot].num_fats = g_sync_num_fats;
        g_fat32_mounts[slot].fat_size_sectors = g_sync_fat_size_sectors;
        g_fat32_mounts[slot].root_cluster = g_sync_root_cluster;
        g_fat32_mounts[slot].data_lba_base = g_sync_data_lba_base;
        g_fat32_mounts[slot].next_alloc_cluster = 2;
    }

    if (strcmp(mount_path, "/") == 0) {
        vfs_chdir("/");
        g_is_fat32_mounted = 1;
    }

    return 0;
}

int fat32_mount_disk(StorageDevice *dev) {
    return fat32_mount_to_path(dev, "/");
}

int fat32_fsck_device(StorageDevice *dev, int repair, fsck_result_t *out_res) {
    if (!dev || !dev->read_sectors || !out_res) return -1;
    memset(out_res, 0, sizeof(fsck_result_t));

    StorageFsInfo info;
    if (storage_inspect_fs(dev, &info) != 0 || !info.has_filesystem || strcmp(info.fs_type, "FAT32") != 0) {
        out_res->error_flags |= FSCK_ERR_BPB;
        return -1;
    }

    uint64_t part_lba = info.part_start_lba;
    uint32_t spc = info.sectors_per_cluster;
    uint32_t bps = info.bytes_per_sector;
    uint32_t rsvd = info.reserved_sectors;
    uint32_t fats = info.num_fats;
    uint32_t fat_sz = info.fat_size_sectors;
    uint32_t root_clus = info.root_cluster > 0 ? info.root_cluster : 2;
    uint32_t total_clusters = info.total_clusters;
    out_res->total_clusters = total_clusters;

    uint8_t sec_buf1[512];
    uint8_t sec_buf2[512];

    /* 1. Verify Boot Sector Magic */
    if (dev->read_sectors(dev, part_lba, 1, sec_buf1) != 0 ||
        sec_buf1[510] != 0x55 || sec_buf1[511] != 0xAA) {
        out_res->error_flags |= FSCK_ERR_MAGIC;
        if (repair && dev->write_sectors) {
            if (dev->read_sectors(dev, part_lba + 6, 1, sec_buf2) == 0 &&
                sec_buf2[510] == 0x55 && sec_buf2[511] == 0xAA) {
                dev->write_sectors(dev, part_lba, 1, sec_buf2);
            }
        }
    }

    /* 2. Verify and Compare FAT1 vs FAT2 */
    if (fats >= 2 && fat_sz > 0) {
        uint64_t fat1_lba = part_lba + rsvd;
        uint64_t fat2_lba = part_lba + rsvd + fat_sz;
        for (uint32_t s = 0; s < fat_sz; s++) {
            if (dev->read_sectors(dev, fat1_lba + s, 1, sec_buf1) != 0 ||
                dev->read_sectors(dev, fat2_lba + s, 1, sec_buf2) != 0) {
                out_res->error_flags |= FSCK_ERR_FAT_MISMATCH;
                out_res->fat_mismatches++;
                continue;
            }
            if (memcmp(sec_buf1, sec_buf2, 512) != 0) {
                out_res->error_flags |= FSCK_ERR_FAT_MISMATCH;
                out_res->fat_mismatches++;
                if (repair && dev->write_sectors) {
                    /* Mirror primary FAT1 to secondary FAT2 */
                    dev->write_sectors(dev, fat2_lba + s, 1, sec_buf1);
                }
            }
        }
    }

    /* 3. Validate FSInfo Sector & Count Actual Free Clusters in FAT1 */
    uint32_t actual_free = 0;
    uint64_t fat1_lba = part_lba + rsvd;
    for (uint32_t s = 0; s < fat_sz; s++) {
        if (dev->read_sectors(dev, fat1_lba + s, 1, sec_buf1) != 0) break;
        uint32_t *entries = (uint32_t *)sec_buf1;
        for (uint32_t i = 0; i < 128; i++) {
            uint32_t clus = s * 128 + i;
            if (clus >= 2 && clus < (total_clusters + 2)) {
                if ((entries[i] & 0x0FFFFFFF) == 0) {
                    actual_free++;
                }
            }
        }
    }
    out_res->fsinfo_actual_free = actual_free;

    /* Read FSInfo */
    uint16_t fsinfo_rel = 1;
    if (dev->read_sectors(dev, part_lba, 1, sec_buf1) == 0) {
        uint16_t rel = *(uint16_t *)&sec_buf1[48];
        if (rel > 0 && rel < rsvd) fsinfo_rel = rel;
    }
    if (dev->read_sectors(dev, part_lba + fsinfo_rel, 1, sec_buf1) == 0) {
        uint32_t lead_sig = *(uint32_t *)&sec_buf1[0];
        uint32_t struct_sig = *(uint32_t *)&sec_buf1[484];
        uint32_t trail_sig = *(uint32_t *)&sec_buf1[508];
        uint32_t rep_free = *(uint32_t *)&sec_buf1[488];
        out_res->fsinfo_reported_free = rep_free;

        if (lead_sig != 0x41615252 || struct_sig != 0x61417272 || trail_sig != 0xAA550000 ||
            rep_free != actual_free) {
            out_res->error_flags |= FSCK_ERR_FSINFO;
            if (repair && dev->write_sectors) {
                *(uint32_t *)&sec_buf1[0] = 0x41615252;
                *(uint32_t *)&sec_buf1[484] = 0x61417272;
                *(uint32_t *)&sec_buf1[488] = actual_free;
                *(uint32_t *)&sec_buf1[508] = 0xAA550000;
                dev->write_sectors(dev, part_lba + fsinfo_rel, 1, sec_buf1);
            }
        }
    } else {
        out_res->error_flags |= FSCK_ERR_FSINFO;
    }

    /* 4. Validate Directory Consistency & Cluster Chains */
    StorageDevice *prev_d = g_sync_dev;
    uint64_t prev_lba = g_sync_part_lba;
    uint32_t prev_spc = g_sync_spc;
    uint32_t prev_bps = g_sync_bps;
    uint32_t prev_csz = g_sync_cluster_size;
    uint32_t prev_rsv = g_sync_reserved_sectors;
    uint32_t prev_fats = g_sync_num_fats;
    uint32_t prev_fsz = g_sync_fat_size_sectors;
    uint32_t prev_root = g_sync_root_cluster;
    uint64_t prev_data = g_sync_data_lba_base;

    g_sync_dev = dev;
    g_sync_part_lba = part_lba;
    g_sync_spc = spc;
    g_sync_bps = bps;
    g_sync_cluster_size = spc * bps;
    g_sync_reserved_sectors = rsvd;
    g_sync_num_fats = fats;
    g_sync_fat_size_sectors = fat_sz;
    g_sync_root_cluster = root_clus;
    g_sync_data_lba_base = part_lba + rsvd + (uint64_t)fats * fat_sz;

    uint32_t c_sz = get_clus_buf_size();
    uint8_t *c_buf = (uint8_t *)kmalloc(c_sz);
    if (c_buf) {
        uint32_t dir_queue[64];
        int q_head = 0, q_tail = 0;
        dir_queue[q_tail++] = root_clus;

        while (q_head < q_tail && q_head < 64) {
            uint32_t cur_dir_c = dir_queue[q_head++];
            out_res->checked_dirs++;

            uint32_t walk_c = cur_dir_c;
            uint32_t dir_guard = 0;
            FatLfnCollector col;

            while (walk_c >= 2 && walk_c < 0x0FFFFFF8 && dir_guard++ < 256) {
                if (read_cluster(walk_c, c_buf) != 0) {
                    out_res->error_flags |= FSCK_ERR_CHAIN;
                    break;
                }
                lfn_collector_init(&col);
                FatDirEntry *entries = (FatDirEntry *)c_buf;
                uint32_t num_e = c_sz / sizeof(FatDirEntry);

                for (uint32_t i = 0; i < num_e; i++) {
                    if ((uint8_t)entries[i].name[0] == 0x00) break;
                    if ((uint8_t)entries[i].name[0] == 0xE5) {
                        col.valid = 0;
                        continue;
                    }
                    if (entries[i].attr == 0x0F) {
                        lfn_collector_feed(&col, (const FatLfnEntry *)&entries[i], i);
                        continue;
                    }
                    if (entries[i].attr & 0x08) {
                        col.valid = 0;
                        continue;
                    }
                    if (memcmp(entries[i].name, ".          ", 11) == 0 ||
                        memcmp(entries[i].name, "..         ", 11) == 0) {
                        col.valid = 0;
                        continue;
                    }

                    if (col.valid && fat_lfn_checksum(entries[i].name) != col.checksum) {
                        out_res->error_flags |= FSCK_ERR_DIR;
                    }
                    col.valid = 0;

                    uint32_t start_c = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;

                    if (entries[i].attr & 0x10) {
                        if (start_c >= 2 && start_c < (total_clusters + 2) && q_tail < 64) {
                            dir_queue[q_tail++] = start_c;
                        }
                    } else {
                        out_res->checked_files++;
                        if (start_c >= 2 && start_c < (total_clusters + 2)) {
                            uint32_t fc = start_c;
                            uint32_t fc_guard = 0;
                            uint32_t last_valid = fc;
                            int chain_broken = 0;
                            while (fc >= 2 && fc < 0x0FFFFFF8 && fc_guard++ < 8192) {
                                last_valid = fc;
                                fc = get_fat_entry(fc);
                                if (fc == 0 || (fc > 0x0FFFFFF8 && fc != 0x0FFFFFFF)) {
                                    chain_broken = 1;
                                    break;
                                }
                            }
                            if (chain_broken || fc_guard >= 8192) {
                                out_res->error_flags |= FSCK_ERR_CHAIN;
                                if (repair) {
                                    set_fat_entry(last_valid, 0x0FFFFFFF);
                                    out_res->fixed_chains++;
                                }
                            }
                        }
                    }
                }
                walk_c = get_fat_entry(walk_c);
            }
        }
        kfree(c_buf);
    }

    if (repair) {
        flush_fat_cache();
        if (dev->flush) dev->flush(dev);
    }

    g_sync_dev = prev_d;
    g_sync_part_lba = prev_lba;
    g_sync_spc = prev_spc;
    g_sync_bps = prev_bps;
    g_sync_cluster_size = prev_csz;
    g_sync_reserved_sectors = prev_rsv;
    g_sync_num_fats = prev_fats;
    g_sync_fat_size_sectors = prev_fsz;
    g_sync_root_cluster = prev_root;
    g_sync_data_lba_base = prev_data;

    return (out_res->error_flags != 0) ? 1 : 0;
}
