#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

/* VFS Node Types */
typedef enum {
    VFS_NODE_FILE = 0,
    VFS_NODE_DIRECTORY = 1
} vfs_node_type_t;

typedef struct vfs_node {
    char name[64];
    vfs_node_type_t type;
    size_t size;
    char *content; /* Dynamic buffer for file contents */
    size_t capacity;
    int is_protected; /* 1 if protected system node requiring -f to delete */

    struct vfs_node *parent;
    struct vfs_node *first_child;
    struct vfs_node *next_sibling;
} vfs_node_t;

/* Devpath Mode for Shell Prompt */
typedef enum {
    DEVPATH_MODE_FIRMWARE = 0, /* DOS-style \path starting from ESP partition root */
    DEVPATH_MODE_HARDWARE = 1, /* PciRoot(0x0)/...\path */
    DEVPATH_MODE_SOFTWARE = 2  /* Unix-style /path starting from VFS root */
} devpath_mode_t;

typedef struct {
    char base_hardware_path[384];
    char partition_boot_file[128];
    devpath_mode_t mode;
} BootLocationInfo;

extern BootLocationInfo g_boot_location;

/* VFS Core Operations */
int vfs_init_initramfs(void);
int vfs_mount_boot_media(const BootInfo *boot_info);
int fat32_is_mounted(void);
int fat32_sync_write_file(const char *path, const char *text, int append);
int fat32_sync_create_file(const char *path);
int fat32_sync_mkdir(const char *path);
int fat32_sync_delete_node(const char *path, int is_dir);

vfs_node_t *vfs_find_node(const char *path);
vfs_node_t *vfs_mkdir(const char *path);
vfs_node_t *vfs_create_file(const char *path);
int vfs_write_file(const char *path, const char *text, int append);
int vfs_read_file(const char *path, char *buffer, size_t max_len);
int vfs_remove_node_ex(const char *path, int recursive, int force);
int vfs_remove_node(const char *path);
int vfs_chdir(const char *path);
const char *vfs_getcwd(void);
void vfs_listdir(const char *path);
void vfs_get_stats(uint32_t *out_nodes, uint32_t *out_dirs, uint32_t *out_files, uint64_t *out_bytes);

/* Boot Device & Prompt Path Resolution */
void fs_init_devpath(const BootInfo *boot_info);
void fs_get_prompt_path(char *out_buf, size_t max_len);
void devpath_set_mode(devpath_mode_t mode);
devpath_mode_t devpath_get_mode(void);

#endif /* FS_H */
