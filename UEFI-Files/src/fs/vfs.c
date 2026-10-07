#include "fs.h"
#include "storage.h"
#include "rtc.h"
#include "lib.h"
#include "drivers.h"
#include "default_media.h"
#include "wallpapers_payload.h"
#include "ipc.h"

static vfs_node_t *g_vfs_root = NULL;
static vfs_node_t *g_vfs_cwd = NULL;
static char g_cwd_path[256] = "/";

static vfs_node_t *create_node(const char *name, vfs_node_type_t type, vfs_node_t *parent) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;

    memset(node, 0, sizeof(vfs_node_t));
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->type = type;
    node->parent = parent;

    /* Initialize Creation, Access, and Modification timestamps from CMOS RTC */
    rtc_datetime_t dt;
    if (rtc_get_datetime(&dt) == 0) {
        rtc_format_datetime(node->date_created, sizeof(node->date_created), &dt);
        strncpy(node->date_accessed, node->date_created, sizeof(node->date_accessed) - 1);
        strncpy(node->date_modified, node->date_created, sizeof(node->date_modified) - 1);
    } else {
        strcpy(node->date_created, "0000-00-00 00:00:00");
        strcpy(node->date_accessed, "0000-00-00 00:00:00");
        strcpy(node->date_modified, "0000-00-00 00:00:00");
    }

    if (parent) {
        if (!parent->first_child) {
            parent->first_child = node;
        } else {
            vfs_node_t *sibling = parent->first_child;
            while (sibling->next_sibling) {
                sibling = sibling->next_sibling;
            }
            sibling->next_sibling = node;
        }
    }

    return node;
}

static void normalize_path(const char *path, char *out_buf, size_t max_len) {
    if (!path || path[0] == '\0') {
        strncpy(out_buf, g_cwd_path, max_len - 1);
        out_buf[max_len - 1] = '\0';
        return;
    }

    char full[512];
    if (path[0] == '/' || path[0] == '\\') {
        strncpy(full, path, sizeof(full) - 1);
    } else {
        if (strcmp(g_cwd_path, "/") == 0) {
            snprintf(full, sizeof(full), "/%s", path);
        } else {
            snprintf(full, sizeof(full), "%s/%s", g_cwd_path, path);
        }
    }
    full[sizeof(full) - 1] = '\0';

    char segments[64][64];
    int seg_count = 0;
    char token[64];
    int token_len = 0;

    for (size_t i = 0; full[i] != '\0'; i++) {
        if (full[i] == '/' || full[i] == '\\') {
            if (token_len > 0) {
                token[token_len] = '\0';
                if (strcmp(token, ".") == 0) {
                    /* Ignore current dir */
                } else if (strcmp(token, "..") == 0) {
                    if (seg_count > 0) seg_count--;
                } else {
                    if (seg_count < 64) {
                        strncpy(segments[seg_count++], token, 63);
                    }
                }
                token_len = 0;
            }
        } else {
            if (token_len < 63) {
                token[token_len++] = full[i];
            }
        }
    }

    if (token_len > 0) {
        token[token_len] = '\0';
        if (strcmp(token, ".") == 0) {
            /* Ignore */
        } else if (strcmp(token, "..") == 0) {
            if (seg_count > 0) seg_count--;
        } else {
            if (seg_count < 64) {
                strncpy(segments[seg_count++], token, 63);
            }
        }
    }

    if (seg_count == 0) {
        strncpy(out_buf, "/", max_len - 1);
    } else {
        out_buf[0] = '\0';
        for (int i = 0; i < seg_count; i++) {
            strcat(out_buf, "/");
            strcat(out_buf, segments[i]);
        }
    }
    out_buf[max_len - 1] = '\0';
}

vfs_node_t *vfs_find_node(const char *path) {
    if (!path || !g_vfs_root) return NULL;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));

    if (strcmp(norm_path, "/") == 0) {
        return g_vfs_root;
    }

    vfs_node_t *curr = g_vfs_root;
    char token[64];
    int token_len = 0;

    for (size_t i = 1; norm_path[i] != '\0'; i++) {
        if (norm_path[i] == '/') {
            if (token_len > 0) {
                token[token_len] = '\0';
                vfs_node_t *child = curr->first_child;
                vfs_node_t *found = NULL;
                while (child) {
                    if (strcasecmp(child->name, token) == 0) {
                        found = child;
                        break;
                    }
                    child = child->next_sibling;
                }
                if (!found || found->type != VFS_NODE_DIRECTORY) {
                    return NULL;
                }
                curr = found;
                token_len = 0;
            }
        } else {
            if (token_len < 63) {
                token[token_len++] = norm_path[i];
            }
        }
    }

    if (token_len > 0) {
        token[token_len] = '\0';
        vfs_node_t *child = curr->first_child;
        while (child) {
            if (strcasecmp(child->name, token) == 0) {
                if (strcmp(norm_path, "/dev/fb0") == 0 ||
                    strcmp(norm_path, "/dev/urandom") == 0 ||
                    strcmp(norm_path, "/dev/zero") == 0) {
                    child->size = (size_t)fb_get_buffer_size();
                }
                return child;
            }
            child = child->next_sibling;
        }
        return NULL;
    }

    return curr;
}

vfs_node_t *vfs_mkdir(const char *path) {
    if (!path) return NULL;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));

    vfs_node_t *existing = vfs_find_node(norm_path);
    if (existing) {
        return (existing->type == VFS_NODE_DIRECTORY) ? existing : NULL;
    }

    char parent_path[256];
    char dir_name[64];

    char *last_slash = strrchr(norm_path, '/');
    if (last_slash == norm_path) {
        strcpy(parent_path, "/");
        strcpy(dir_name, last_slash + 1);
    } else {
        size_t len = last_slash - norm_path;
        strncpy(parent_path, norm_path, len);
        parent_path[len] = '\0';
        strcpy(dir_name, last_slash + 1);
    }

    vfs_node_t *parent = vfs_find_node(parent_path);
    if (!parent || parent->type != VFS_NODE_DIRECTORY) {
        return NULL;
    }

    vfs_node_t *node = create_node(dir_name, VFS_NODE_DIRECTORY, parent);
    if (node && fat32_is_mounted()) {
        fat32_sync_mkdir(norm_path);
    }
    return node;
}

vfs_node_t *vfs_create_file(const char *path) {
    if (!path) return NULL;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));

    vfs_node_t *existing = vfs_find_node(norm_path);
    if (existing) {
        return (existing->type == VFS_NODE_FILE) ? existing : NULL;
    }

    char parent_path[256];
    char file_name[64];

    char *last_slash = strrchr(norm_path, '/');
    if (last_slash == norm_path) {
        strcpy(parent_path, "/");
        strcpy(file_name, last_slash + 1);
    } else {
        size_t len = last_slash - norm_path;
        strncpy(parent_path, norm_path, len);
        parent_path[len] = '\0';
        strcpy(file_name, last_slash + 1);
    }

    vfs_node_t *parent = vfs_find_node(parent_path);
    if (!parent || parent->type != VFS_NODE_DIRECTORY) {
        return NULL;
    }

    vfs_node_t *node = create_node(file_name, VFS_NODE_FILE, parent);
    if (node && fat32_is_mounted()) {
        fat32_sync_create_file(norm_path);
    }
    return node;
}

int vfs_write_file_bytes(const char *path, const void *data, size_t size, int append) {
    if (!path) return -1;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));

    /* Special Device: /dev/fb0 (Physical Video Framebuffer) */
    if (strcmp(norm_path, "/dev/fb0") == 0) {
        uint64_t fb_base = fb_get_physical_base();
        uint64_t fb_size = fb_get_buffer_size();
        if (fb_base == 0 || fb_size == 0) return -1;
        if (!data || size == 0) return 0;

        static size_t s_fb0_write_pos = 0;
        size_t offset = append ? s_fb0_write_pos : 0;
        if (offset >= fb_size) {
            offset = 0;
        }

        size_t to_write = size;
        if (to_write > (size_t)(fb_size - offset)) {
            to_write = (size_t)(fb_size - offset);
        }

        memcpy((void *)(uintptr_t)(fb_base + offset), data, to_write);
        s_fb0_write_pos = (offset + to_write) % fb_size;

        vfs_node_t *node = vfs_find_node("/dev/fb0");
        if (node) {
            node->size = (size_t)fb_size;
            rtc_datetime_t dt;
            if (rtc_get_datetime(&dt) == 0) {
                rtc_format_datetime(node->date_modified, sizeof(node->date_modified), &dt);
            }
        }
        return (int)to_write;
    }

    /* Special Devices: /dev/null and /dev/zero discard writes */
    if (strcmp(norm_path, "/dev/null") == 0 || strcmp(norm_path, "/dev/zero") == 0) {
        return (int)size;
    }

    const char *bytes = (const char *)data;

    vfs_node_t *node = vfs_create_file(path);
    if (!node || node->type != VFS_NODE_FILE) return -1;

    if (append && node->content) {
        size_t new_size = node->size + size;
        if (new_size + 1 > node->capacity) {
            size_t new_cap = (new_size + 1 + 255) & ~255;
            char *new_buf = (char *)krealloc(node->content, new_cap);
            if (!new_buf) return -1;
            node->content = new_buf;
            node->capacity = new_cap;
        }
        if (size > 0 && bytes) {
            memcpy(node->content + node->size, bytes, size);
        }
        node->size = new_size;
        node->content[node->size] = '\0';
    } else {
        if (!node->content || (size + 1) > node->capacity) {
            if (node->content) kfree(node->content);
            node->capacity = (size + 1 + 255) & ~255;
            node->content = (char *)kmalloc(node->capacity);
            if (!node->content) return -1;
        }
        if (size > 0 && bytes) {
            memcpy(node->content, bytes, size);
        }
        node->size = size;
        node->content[node->size] = '\0';
    }

    if (fat32_is_mounted() && bytes) {
        char norm_sync[256];
        normalize_path(path, norm_sync, sizeof(norm_sync));
        fat32_sync_write_file_bytes(norm_sync, bytes, size, append);
    }

    rtc_datetime_t dt;
    if (rtc_get_datetime(&dt) == 0) {
        rtc_format_datetime(node->date_modified, sizeof(node->date_modified), &dt);
        rtc_format_datetime(node->date_accessed, sizeof(node->date_accessed), &dt);
    }

    return (int)node->size;
}

int vfs_write_file(const char *path, const char *text, int append) {
    if (!path || !text) return -1;
    return vfs_write_file_bytes(path, text, strlen(text), append);
}

int vfs_read_file_offset(const char *path, char *buffer, size_t max_len, size_t offset) {
    if (!path || !buffer || max_len == 0) return -1;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));

    /* Special Device: /dev/fb0 (Direct Physical VRAM Read) */
    if (strcmp(norm_path, "/dev/fb0") == 0) {
        uint64_t fb_base = fb_get_physical_base();
        uint64_t fb_size = fb_get_buffer_size();
        if (fb_base == 0 || fb_size == 0) return -1;
        if (offset >= fb_size) {
            buffer[0] = '\0';
            return 0; /* EOF */
        }
        size_t avail = (size_t)(fb_size - offset);
        size_t to_read = (avail < max_len) ? avail : max_len;
        memcpy(buffer, (const void *)(uintptr_t)(fb_base + offset), to_read);
        if (to_read < max_len) {
            buffer[to_read] = '\0';
        }
        return (int)to_read;
    }

    /* Special Device: /dev/urandom (High-entropy pseudo-random stream) */
    if (strcmp(norm_path, "/dev/urandom") == 0) {
        uint64_t limit = fb_get_buffer_size();
        if (offset >= limit) {
            buffer[0] = '\0';
            return 0; /* EOF */
        }
        size_t avail = (size_t)(limit - offset);
        size_t to_read = (avail < max_len) ? avail : max_len;
        static uint64_t s_prng = 0x853c49e6748fea9bULL;
        uint8_t *ubuf = (uint8_t *)buffer;
        for (size_t i = 0; i < to_read; i++) {
            s_prng ^= (s_prng << 13);
            s_prng ^= (s_prng >> 7);
            s_prng ^= (s_prng << 17);
            ubuf[i] = (uint8_t)(s_prng & 0xFF);
        }
        if (to_read < max_len) {
            buffer[to_read] = '\0';
        }
        return (int)to_read;
    }

    /* Special Device: /dev/zero */
    if (strcmp(norm_path, "/dev/zero") == 0) {
        uint64_t limit = fb_get_buffer_size();
        if (offset >= limit) {
            buffer[0] = '\0';
            return 0; /* EOF */
        }
        size_t avail = (size_t)(limit - offset);
        size_t to_read = (avail < max_len) ? avail : max_len;
        memset(buffer, 0, to_read);
        if (to_read < max_len) {
            buffer[to_read] = '\0';
        }
        return (int)to_read;
    }

    /* Special Device: /dev/null */
    if (strcmp(norm_path, "/dev/null") == 0) {
        buffer[0] = '\0';
        return 0;
    }

    vfs_node_t *node = vfs_find_node(path);
    if (!node || node->type != VFS_NODE_FILE) return -1;

    if (offset >= node->size) {
        buffer[0] = '\0';
        return 0; /* EOF */
    }

    size_t avail = node->size - offset;
    size_t to_read = (avail < max_len) ? avail : max_len;
    if (to_read > 0 && node->content) {
        memcpy(buffer, node->content + offset, to_read);
    }
    if (to_read < max_len) {
        buffer[to_read] = '\0';
    }

    rtc_datetime_t dt;
    if (rtc_get_datetime(&dt) == 0) {
        rtc_format_datetime(node->date_accessed, sizeof(node->date_accessed), &dt);
    }

    return (int)to_read;
}

int vfs_read_file(const char *path, char *buffer, size_t max_len) {
    if (!path || !buffer || max_len == 0) return -1;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));
    if (strncmp(norm_path, "/dev/", 5) == 0) {
        return vfs_read_file_offset(path, buffer, max_len, 0);
    }

    vfs_node_t *node = vfs_find_node(path);
    if (!node || node->type != VFS_NODE_FILE) return -1;

    size_t to_read = node->size < max_len - 1 ? node->size : max_len - 1;
    if (to_read > 0 && node->content) {
        memcpy(buffer, node->content, to_read);
    }
    buffer[to_read] = '\0';

    rtc_datetime_t dt;
    if (rtc_get_datetime(&dt) == 0) {
        rtc_format_datetime(node->date_accessed, sizeof(node->date_accessed), &dt);
    }

    return (int)to_read;
}

static int check_node_protected_recursive(vfs_node_t *node) {
    if (!node) return 0;
    if (node->is_protected) return 1;

    vfs_node_t *child = node->first_child;
    while (child) {
        if (check_node_protected_recursive(child)) {
            return 1;
        }
        child = child->next_sibling;
    }
    return 0;
}

static int is_node_in_subtree(vfs_node_t *root, vfs_node_t *target) {
    vfs_node_t *curr = target;
    while (curr) {
        if (curr == root) return 1;
        curr = curr->parent;
    }
    return 0;
}

static void free_vfs_subtree(vfs_node_t *node) {
    if (!node) return;

    vfs_node_t *child = node->first_child;
    while (child) {
        vfs_node_t *next = child->next_sibling;
        free_vfs_subtree(child);
        child = next;
    }

    if (node->content) {
        kfree(node->content);
    }
    kfree(node);
}

int vfs_remove_node_ex(const char *path, int recursive, int force) {
    if (!path) return -1;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));

    if (strcmp(norm_path, "/") == 0) return -3; /* Cannot delete root */

    vfs_node_t *node = vfs_find_node(norm_path);
    if (!node) return -1;

    if (node->type == VFS_NODE_DIRECTORY && node->first_child != NULL && !recursive) {
        return -2; /* Directory not empty */
    }

    if (check_node_protected_recursive(node) && !force) {
        return -4; /* Protected directory! */
    }

    /* Unbind and sever any active IPC unix-domain sockets associated with this path */
    ipc_unbind_path(norm_path);

    int is_dir = (node->type == VFS_NODE_DIRECTORY);

    /* 1. Synchronize deletion with physical persistent disk FIRST */
    if (fat32_is_mounted()) {
        int sync_res = fat32_sync_delete_node(norm_path, is_dir);
        if (sync_res == -1) {
            return -5; /* Hardware disk I/O synchronization error */
        }
    }

    vfs_node_t *parent = node->parent;
    if (!parent) return -3;

    /* If current working directory is inside the subtree being deleted, reset cwd to safe parent */
    if (is_node_in_subtree(node, g_vfs_cwd)) {
        vfs_node_t *safe_dir = parent;
        while (safe_dir && is_node_in_subtree(node, safe_dir)) {
            safe_dir = safe_dir->parent;
        }
        if (!safe_dir) safe_dir = g_vfs_root;
        g_vfs_cwd = safe_dir;

        /* Rebuild g_cwd_path for the safe directory */
        char path_stack[16][64];
        int depth = 0;
        vfs_node_t *c = g_vfs_cwd;
        while (c && c != g_vfs_root && depth < 16) {
            strncpy(path_stack[depth++], c->name, 63);
            c = c->parent;
        }
        if (depth == 0) {
            strcpy(g_cwd_path, "/");
        } else {
            g_cwd_path[0] = '\0';
            for (int d = depth - 1; d >= 0; d--) {
                strcat(g_cwd_path, "/");
                strcat(g_cwd_path, path_stack[d]);
            }
        }
    }

    if (parent->first_child == node) {
        parent->first_child = node->next_sibling;
    } else {
        vfs_node_t *sibling = parent->first_child;
        while (sibling && sibling->next_sibling != node) {
            sibling = sibling->next_sibling;
        }
        if (sibling) {
            sibling->next_sibling = node->next_sibling;
        }
    }

    free_vfs_subtree(node);

    return 0;
}

int vfs_remove_node(const char *path) {
    return vfs_remove_node_ex(path, 0, 0);
}

int vfs_chdir(const char *path) {
    if (!path) return -1;

    char norm_path[256];
    normalize_path(path, norm_path, sizeof(norm_path));

    vfs_node_t *target = vfs_find_node(norm_path);
    if (!target || target->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    g_vfs_cwd = target;
    strncpy(g_cwd_path, norm_path, sizeof(g_cwd_path) - 1);
    g_cwd_path[sizeof(g_cwd_path) - 1] = '\0';
    return 0;
}

const char *vfs_getcwd(void) {
    return g_cwd_path;
}

void vfs_listdir(const char *path) {
    vfs_node_t *target = NULL;
    if (!path || path[0] == '\0') {
        target = g_vfs_cwd ? g_vfs_cwd : g_vfs_root;
    } else {
        char norm_path[256];
        normalize_path(path, norm_path, sizeof(norm_path));
        target = vfs_find_node(norm_path);
    }

    if (!target) {
        console_printf("ls: cannot access '%s': no such file or directory\n", path ? path : "");
        return;
    }

    if (target->type != VFS_NODE_DIRECTORY) {
        console_printf("  %8u B   %s\n", (unsigned int)target->size, target->name);
        return;
    }

    console_printf("directory of %s:\n", target == g_vfs_root ? "/" : target->name);
    console_printf("  <dir>          .\n");
    console_printf("  <dir>          ..\n");

    vfs_node_t *child = target->first_child;
    int count = 0;
    while (child) {
        if (child->type == VFS_NODE_DIRECTORY) {
            console_printf("  <dir>          %s\n", child->name);
        } else {
            console_printf("  %8u B   %s\n", (unsigned int)child->size, child->name);
        }
        count++;
        child = child->next_sibling;
    }
    console_printf("  total: %d item(s)\n", count);
}

int vfs_listdir_names(const char *path, char *buf, size_t buf_size) {
    vfs_node_t *target = NULL;
    if (!path || path[0] == '\0') {
        target = g_vfs_cwd ? g_vfs_cwd : g_vfs_root;
    } else {
        char norm_path[256];
        normalize_path(path, norm_path, sizeof(norm_path));
        target = vfs_find_node(norm_path);
    }

    if (!target) return -2; /* -ENOENT */
    if (target->type != VFS_NODE_DIRECTORY) return -20; /* -ENOTDIR */

    size_t total_bytes = 0;
    vfs_node_t *child = target->first_child;
    while (child) {
        size_t name_len = strlen(child->name) + 1; /* includes null terminator */
        if (buf && total_bytes + name_len <= buf_size) {
            memcpy(buf + total_bytes, child->name, name_len);
        }
        total_bytes += name_len;
        child = child->next_sibling;
    }
    return (int)total_bytes;
}

void vfs_ensure_system_payloads(void);

int vfs_init_initramfs(void) {
    /* 1. Create root directory node if not present */
    if (!g_vfs_root) {
        g_vfs_root = create_node("/", VFS_NODE_DIRECTORY, NULL);
        if (!g_vfs_root) return -1;
    }

    g_vfs_cwd = g_vfs_root;
    strcpy(g_cwd_path, "/");

    /* 2. Create EFI system tree */
    vfs_node_t *efi = vfs_mkdir("/EFI");
    if (efi) efi->is_protected = 1;
    vfs_mkdir("/EFI/BOOT");
    vfs_mkdir("/EFI/pseuDOS");

    size_t boot_size = 0;
    payload_get_bootloader(&boot_size);
    vfs_node_t *boot_efi = vfs_create_file("/EFI/BOOT/BOOTX64.EFI");
    if (boot_efi) boot_efi->size = boot_size;

    /* Deploy BOOTX64.EFI and pseudos.efi to /EFI/pseuDOS for GRUB / os-prober */
    vfs_node_t *pseudos_boot = vfs_create_file("/EFI/pseuDOS/BOOTX64.EFI");
    if (pseudos_boot) pseudos_boot->size = boot_size;

    vfs_node_t *pseudos_efi = vfs_create_file("/EFI/pseuDOS/pseudos.efi");
    if (pseudos_efi) pseudos_efi->size = boot_size;

    /* GRUB 2 configuration snippet */
    vfs_write_file("/EFI/pseuDOS/grub.cfg",
        "# GRUB 2 configuration snippet for pseuDOS\n"
        "# Add this to /etc/grub.d/40_custom or /boot/grub/grub.cfg\n"
        "\n"
        "menuentry \"pseuDOS x86_64\" {\n"
        "    insmod fat\n"
        "    insmod chain\n"
        "    search --no-floppy --set=root --file /EFI/pseuDOS/BOOTX64.EFI\n"
        "    chainloader /EFI/pseuDOS/BOOTX64.EFI\n"
        "}\n", 0);

    /* Standard OS identification file */
    vfs_write_file("/EFI/pseuDOS/os-release",
        "NAME=\"pseuDOS\"\n"
        "ID=pseudos\n"
        "VERSION=\"0.6.0\"\n"
        "PRETTY_NAME=\"pseuDOS v0.6.0 (x86_64 UEFI)\"\n"
        "HOME_URL=\"https://github.com/the-ultimate-karl/pseuDOS\"\n", 0);

    /* 3. Ensure all system directories and userland payloads exist */
    vfs_ensure_system_payloads();

    return 0;
}

void vfs_ensure_system_payloads(void) {
    /* 1. Create standard user, volatile, and device directories */
    vfs_mkdir("/home");
    vfs_mkdir("/home/user");
    vfs_mkdir("/tmp");
    vfs_mkdir("/services");

    vfs_node_t *dev_dir = vfs_mkdir("/dev");
    if (dev_dir) dev_dir->is_protected = 1;

    vfs_node_t *fb0 = vfs_find_node("/dev/fb0");
    if (!fb0) fb0 = vfs_create_file("/dev/fb0");
    if (fb0) fb0->size = (size_t)fb_get_buffer_size();

    vfs_node_t *dev_rnd = vfs_find_node("/dev/urandom");
    if (!dev_rnd) dev_rnd = vfs_create_file("/dev/urandom");
    if (dev_rnd) dev_rnd->size = (size_t)fb_get_buffer_size();

    vfs_node_t *dev_zr = vfs_find_node("/dev/zero");
    if (!dev_zr) dev_zr = vfs_create_file("/dev/zero");
    if (dev_zr) dev_zr->size = (size_t)fb_get_buffer_size();

    vfs_node_t *dev_nl = vfs_find_node("/dev/null");
    if (!dev_nl) dev_nl = vfs_create_file("/dev/null");
    if (dev_nl) dev_nl->size = 0;

    /* 2. Create Protected system zones matching vfs.py & memfs.py */
    vfs_node_t *prot = vfs_mkdir("/protected");
    if (prot) prot->is_protected = 1;
    vfs_mkdir("/protected/bootmgr");
    vfs_mkdir("/protected/crit");
    vfs_mkdir("/protected/krnl");
    vfs_mkdir("/protected/krnl/essential");
    vfs_mkdir("/protected/gui");
    vfs_mkdir("/protected/apps");

    vfs_node_t *cmds = vfs_mkdir("/commands");
    if (cmds) cmds->is_protected = 1;

    vfs_node_t *coreutils = vfs_mkdir("/coreutils");
    if (coreutils) coreutils->is_protected = 1;

    /* Ensure boot config */
    if (!vfs_find_node("/protected/bootmgr/boot.cfg")) {
        vfs_write_file("/protected/bootmgr/boot.cfg",
            "# pseuDOS Boot Configuration\n"
            "# Architecture: x86_64 UEFI\n"
            "kernel=\\protected\\krnl\\vpkernel\n"
            "autoinit=\\protected\\krnl\\autoinit.exe\n"
            "shell=\\protected\\gui\\superglue.exe\n"
            "cmdline=quiet devpath=hardware\n"
            "default_resolution=1280x720\n"
            "bootmgr_version=1.1.0\n", 0);
    }

    /* 3. Deploy kernel metadata */
    size_t krnl_size = 0;
    payload_get_kernel(&krnl_size);
    vfs_node_t *krnl_bin = vfs_find_node("/protected/krnl/vpkernel");
    if (!krnl_bin) {
        krnl_bin = vfs_create_file("/protected/krnl/vpkernel");
        if (krnl_bin) krnl_bin->size = krnl_size;
    }
    vfs_node_t *krnl_legacy = vfs_find_node("/EFI/pseuDOS/vpkernel");
    if (!krnl_legacy) {
        krnl_legacy = vfs_create_file("/EFI/pseuDOS/vpkernel");
        if (krnl_legacy) krnl_legacy->size = krnl_size;
    }

    /* 4. Deploy userland and GUI payloads */
    #define ENSURE_PAYLOAD(path, getter_func) do { \
        vfs_node_t *fn = vfs_find_node(path); \
        if (!fn || fn->size == 0) { \
            size_t psz = 0; \
            const uint8_t *pdt = getter_func(&psz); \
            if (pdt && psz > 0) { \
                vfs_write_file_bytes(path, pdt, psz, 0); \
            } \
        } \
    } while(0)

    ENSURE_PAYLOAD("/protected/krnl/autoinit.exe", payload_get_autoinit);
    ENSURE_PAYLOAD("/protected/crit/xshss.exe", payload_get_xshss);
    ENSURE_PAYLOAD("/protected/gui/superglue.exe", payload_get_superglue);
    ENSURE_PAYLOAD("/protected/gui/ntfs.exe", payload_get_ntfs);
    ENSURE_PAYLOAD("/protected/gui/lack.exe", payload_get_lack);
    ENSURE_PAYLOAD("/protected/gui/ninds.exe", payload_get_ninds);
    ENSURE_PAYLOAD("/protected/gui/splash.exe", payload_get_splash);
    ENSURE_PAYLOAD("/protected/gui/gshss.exe", payload_get_gshss);
    ENSURE_PAYLOAD("/protected/apps/shell.exe", payload_get_shell);
    ENSURE_PAYLOAD("/protected/apps/sysmon.exe", payload_get_sysmon);
    ENSURE_PAYLOAD("/protected/apps/calc.exe", payload_get_calc);
    ENSURE_PAYLOAD("/protected/apps/notepad.exe", payload_get_notepad);
    ENSURE_PAYLOAD("/protected/apps/paint.exe", payload_get_paint);
    ENSURE_PAYLOAD("/protected/apps/clock.exe", payload_get_clock);
    ENSURE_PAYLOAD("/protected/apps/filemgr.exe", payload_get_filemgr);

    #undef ENSURE_PAYLOAD

    /* 5. Deploy sample media files into /home/user */
    extern const uint8_t g_media_image2_png[];
    extern const uint64_t g_media_image2_png_size;

    if (!vfs_find_node("/home/user/image1.jpg")) {
        vfs_write_file_bytes("/home/user/image1.jpg", g_media_image1_jpg, g_media_image1_jpg_size, 0);
    }
    if (!vfs_find_node("/home/user/image2.png")) {
        vfs_write_file_bytes("/home/user/image2.png", g_media_image2_png, (size_t)g_media_image2_png_size, 0);
    }
    if (!vfs_find_node("/home/user/sample.png")) {
        vfs_write_file_bytes("/home/user/sample.png", g_media_sample_png, g_media_sample_png_size, 0);
    }
    if (!vfs_find_node("/home/user/sample.bmp")) {
        vfs_write_file_bytes("/home/user/sample.bmp", g_media_sample_bmp, g_media_sample_bmp_size, 0);
    }

    /* 6. Deploy bundled wallpapers into /home/user/Wallpapers/ */
    vfs_mkdir("/home/user/Wallpapers");
    vfs_mkdir("/protected/wallpapers");

    for (int w = 0; w < NUM_BUNDLED_WALLPAPERS; w++) {
        char wall_path[128];
        snprintf(wall_path, sizeof(wall_path), "/home/user/Wallpapers/%s", g_bundled_wallpapers[w].filename);
        uint64_t wsize = get_bundled_wallpaper_size(w);
        if (!vfs_find_node(wall_path) && wsize > 0) {
            vfs_write_file_bytes(wall_path, g_bundled_wallpapers[w].data, (size_t)wsize, 0);
        }
    }

    /* Indestructible master copy in protected system directory */
    if (!vfs_find_node("/protected/wallpapers/fallback.png")) {
        vfs_write_file_bytes("/protected/wallpapers/fallback.png", g_wall_fallback_png, (size_t)g_wall_fallback_png_size, 0);
        vfs_node_t *fnode = vfs_find_node("/protected/wallpapers/fallback.png");
        if (fnode) fnode->is_protected = 1;
    }

    /* Default wallpaper setting */
    if (!vfs_find_node("/home/user/wallpaper.cfg")) {
        const char *def_cfg = "/home/user/Wallpapers/Under Construction.png\n";
        vfs_write_file_bytes("/home/user/wallpaper.cfg", (const uint8_t *)def_cfg, strlen(def_cfg), 0);
    }
}

static void vfs_init_mounts(StorageDevice *boot_dev, int boot_drive_idx);

int vfs_mount_boot_media(const BootInfo *boot_info) {
    /* 1. Create root directory node */
    g_vfs_root = create_node("/", VFS_NODE_DIRECTORY, NULL);
    if (!g_vfs_root) return -1;

    g_vfs_cwd = g_vfs_root;
    strcpy(g_cwd_path, "/");

    /* 2. Check if boot device is a physical persistent block device */
    int boot_drive_idx = 0;
    StorageDevice *boot_dev = storage_get_boot_device(boot_info ? boot_info->hardware_devpath : NULL, &boot_drive_idx);

    if (boot_dev && (boot_dev->type == STORAGE_TYPE_INTERNAL_SATA ||
                     boot_dev->type == STORAGE_TYPE_INTERNAL_NVME ||
                     boot_dev->type == STORAGE_TYPE_EXTERNAL_USB)) {
        /* Mount physical FAT32 partition */
        extern int fat32_mount_disk(StorageDevice *dev);
        if (fat32_mount_disk(boot_dev) == 0) {
            /* Successfully mounted persistent disk - ensure all system payloads are available */
            vfs_ensure_system_payloads();
            vfs_init_mounts(boot_dev, boot_drive_idx);
            vfs_refresh_mounts_dir();
            return 0;
        }
    }

    /* Fallback to volatile in-memory ramfs (e.g. CD-ROM or unformatted drive) */
    int ret = vfs_init_initramfs();
    vfs_init_mounts(NULL, -1);
    vfs_refresh_mounts_dir();
    return ret;
}

static void vfs_crawl_stats(vfs_node_t *node, uint32_t *nodes, uint32_t *dirs, uint32_t *files, uint64_t *bytes) {
    if (!node) return;
    (*nodes)++;
    if (node->type == VFS_NODE_DIRECTORY) {
        (*dirs)++;
    } else {
        (*files)++;
        *bytes += node->size;
    }
    vfs_node_t *child = node->first_child;
    while (child) {
        vfs_crawl_stats(child, nodes, dirs, files, bytes);
        child = child->next_sibling;
    }
}

void vfs_get_stats(uint32_t *out_nodes, uint32_t *out_dirs, uint32_t *out_files, uint64_t *out_bytes) {
    uint32_t nodes = 0, dirs = 0, files = 0;
    uint64_t bytes = 0;
    if (g_vfs_root) {
        vfs_crawl_stats(g_vfs_root, &nodes, &dirs, &files, &bytes);
    }
    if (out_nodes) *out_nodes = nodes;
    if (out_dirs) *out_dirs = dirs;
    if (out_files) *out_files = files;
    if (out_bytes) *out_bytes = bytes;
}

/* ========================================================================= */
/* VFS Mount Subsystem Implementation                                        */
/* ========================================================================= */

typedef struct {
    int active;
    int is_root;
    int is_raw;
    int drive_index;
    StorageDeviceType dev_type;
    char dev_name[32];   /* e.g. "/dev/sda1" */
    char alt_name[32];   /* e.g. "sata0" */
    char mount_path[64]; /* e.g. "/", "/tmp", "/mounts/sata0", "/mnt/drive2" */
    char fs_type[16];    /* "fat32", "tmpfs", "raw" */
    char label[32];
    uint64_t total_bytes;
} vfs_mount_entry_t;

#define MAX_MOUNTS 16
static vfs_mount_entry_t g_mounts[MAX_MOUNTS];

static void vfs_init_mounts(StorageDevice *boot_dev, int boot_drive_idx) {
    memset(g_mounts, 0, sizeof(g_mounts));

    /* Mount 0: Root (/) */
    g_mounts[0].active = 1;
    g_mounts[0].is_root = 1;
    strcpy(g_mounts[0].mount_path, "/");

    if (boot_dev) {
        g_mounts[0].drive_index = boot_drive_idx;
        g_mounts[0].dev_type = boot_dev->type;
        strcpy(g_mounts[0].fs_type, "fat32");
        strcpy(g_mounts[0].dev_name, "/dev/sda1");
        if (boot_dev->type == STORAGE_TYPE_INTERNAL_SATA) strcpy(g_mounts[0].alt_name, "sata0");
        else if (boot_dev->type == STORAGE_TYPE_INTERNAL_NVME) strcpy(g_mounts[0].alt_name, "nvme0n1");
        else strcpy(g_mounts[0].alt_name, "usb0");

        StorageFsInfo info;
        if (storage_inspect_fs(boot_dev, &info) == 0 && info.has_filesystem) {
            strncpy(g_mounts[0].label, info.vol_label, sizeof(g_mounts[0].label) - 1);
            g_mounts[0].total_bytes = info.total_bytes;
        } else {
            strcpy(g_mounts[0].label, "PSEUDOS ESP");
        }
    } else {
        g_mounts[0].drive_index = -1;
        strcpy(g_mounts[0].fs_type, "ramfs");
        strcpy(g_mounts[0].dev_name, "ramfs");
        strcpy(g_mounts[0].alt_name, "none");
        strcpy(g_mounts[0].label, "RAMDISK");
    }

    /* Mount 1: /tmp (tmpfs) */
    g_mounts[1].active = 1;
    g_mounts[1].is_root = 0;
    g_mounts[1].drive_index = -1;
    strcpy(g_mounts[1].mount_path, "/tmp");
    strcpy(g_mounts[1].fs_type, "tmpfs");
    strcpy(g_mounts[1].dev_name, "none");
    strcpy(g_mounts[1].alt_name, "tmpfs");
    strcpy(g_mounts[1].label, "TEMP");
}

static StorageDevice *vfs_resolve_device(const char *source, int *out_drive_idx, char *out_canon_name, char *out_alt_name) {
    if (!source || source[0] == '\0') return NULL;
    const char *s = source;
    if (strncmp(s, "/dev/", 5) == 0) s += 5;

    uint32_t count = storage_get_device_count();
    if (count == 0) return NULL;

    int target_idx = -1;

    /* Check sda, sda1, sdb, sdc, etc. */
    if (s[0] == 's' && s[1] == 'd' && s[2] >= 'a' && s[2] <= 'z') {
        target_idx = s[2] - 'a';
    } else if (strncmp(s, "sata", 4) == 0) {
        int req = atoi(s + 4);
        int cur_sata = 0;
        for (uint32_t i = 0; i < count; i++) {
            StorageDevice *d = storage_get_device(i);
            if (d && d->type == STORAGE_TYPE_INTERNAL_SATA) {
                if (cur_sata == req) {
                    target_idx = (int)i;
                    break;
                }
                cur_sata++;
            }
        }
    } else if (strncmp(s, "nvme", 4) == 0) {
        int req = atoi(s + 4);
        int cur_nvme = 0;
        for (uint32_t i = 0; i < count; i++) {
            StorageDevice *d = storage_get_device(i);
            if (d && d->type == STORAGE_TYPE_INTERNAL_NVME) {
                if (cur_nvme == req) {
                    target_idx = (int)i;
                    break;
                }
                cur_nvme++;
            }
        }
    } else if (strncmp(s, "usb", 3) == 0) {
        int req = atoi(s + 3);
        int cur_usb = 0;
        for (uint32_t i = 0; i < count; i++) {
            StorageDevice *d = storage_get_device(i);
            if (d && d->type == STORAGE_TYPE_EXTERNAL_USB) {
                if (cur_usb == req) {
                    target_idx = (int)i;
                    break;
                }
                cur_usb++;
            }
        }
    } else if (strncmp(s, "drive", 5) == 0) {
        target_idx = atoi(s + 5) - 1;
    } else if (s[0] >= '1' && s[0] <= '9') {
        target_idx = atoi(s) - 1;
    }

    if (target_idx < 0 || (uint32_t)target_idx >= count) {
        /* Check by model / product name */
        for (uint32_t i = 0; i < count; i++) {
            StorageDevice *d = storage_get_device(i);
            if (d && strcasecmp(d->name, s) == 0) {
                target_idx = (int)i;
                break;
            }
        }
    }

    if (target_idx >= 0 && (uint32_t)target_idx < count) {
        StorageDevice *dev = storage_get_device((uint32_t)target_idx);
        if (!dev) return NULL;
        if (out_drive_idx) *out_drive_idx = target_idx;
        if (out_canon_name) {
            snprintf(out_canon_name, 32, "/dev/sd%c1", 'a' + target_idx);
        }
        if (out_alt_name) {
            if (dev->type == STORAGE_TYPE_INTERNAL_SATA) snprintf(out_alt_name, 32, "sata%d", target_idx);
            else if (dev->type == STORAGE_TYPE_INTERNAL_NVME) snprintf(out_alt_name, 32, "nvme%dn1", target_idx);
            else snprintf(out_alt_name, 32, "usb%d", target_idx);
        }
        return dev;
    }

    return NULL;
}

void vfs_refresh_mounts_dir(void) {
    vfs_mkdir("/mounts");
    uint32_t count = storage_get_device_count();
    int sata_idx = 0, nvme_idx = 0, usb_idx = 0;

    for (uint32_t i = 0; i < count; i++) {
        StorageDevice *dev = storage_get_device(i);
        if (!dev) continue;

        char dev_tag[32];
        if (dev->type == STORAGE_TYPE_INTERNAL_SATA) {
            snprintf(dev_tag, sizeof(dev_tag), "sata%d", sata_idx++);
        } else if (dev->type == STORAGE_TYPE_INTERNAL_NVME) {
            snprintf(dev_tag, sizeof(dev_tag), "nvme%dn1", nvme_idx++);
        } else {
            snprintf(dev_tag, sizeof(dev_tag), "usb%d", usb_idx++);
        }

        char mount_dir[64];
        snprintf(mount_dir, sizeof(mount_dir), "/mounts/%s", dev_tag);
        vfs_mkdir(mount_dir);

        StorageFsInfo info;
        int has_fs = (storage_inspect_fs(dev, &info) == 0 && info.has_filesystem);

        if (!has_fs) {
            /* Create marker file indicating RAW / Unformatted */
            char raw_marker[80];
            snprintf(raw_marker, sizeof(raw_marker), "%s/.raw", mount_dir);
            vfs_write_file(raw_marker, "RAW / Unformatted\n", 0);
        } else {
            /* Has a recognized filesystem (FAT32) - mount files into /mounts/<dev_tag> */
            extern int fat32_mount_to_path(StorageDevice *dev, const char *mount_path);
            fat32_mount_to_path(dev, mount_dir);

            /* Also record in g_mounts if not already present */
            int found = 0;
            for (int m = 0; m < MAX_MOUNTS; m++) {
                if (g_mounts[m].active && strcmp(g_mounts[m].mount_path, mount_dir) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                for (int m = 0; m < MAX_MOUNTS; m++) {
                    if (!g_mounts[m].active) {
                        g_mounts[m].active = 1;
                        g_mounts[m].is_root = 0;
                        g_mounts[m].is_raw = 0;
                        g_mounts[m].drive_index = (int)i;
                        g_mounts[m].dev_type = dev->type;
                        snprintf(g_mounts[m].dev_name, sizeof(g_mounts[m].dev_name), "/dev/sd%c1", 'a' + i);
                        strncpy(g_mounts[m].alt_name, dev_tag, sizeof(g_mounts[m].alt_name) - 1);
                        strncpy(g_mounts[m].mount_path, mount_dir, sizeof(g_mounts[m].mount_path) - 1);
                        strncpy(g_mounts[m].fs_type, info.fs_type, sizeof(g_mounts[m].fs_type) - 1);
                        strncpy(g_mounts[m].label, info.vol_label, sizeof(g_mounts[m].label) - 1);
                        g_mounts[m].total_bytes = info.total_bytes;
                        break;
                    }
                }
            }
        }
    }
}

int vfs_mount_device(const char *source, const char *target, const char *fstype, char *out_log, size_t out_cap) {
    if (!source || !target) {
        if (out_log && out_cap > 0) snprintf(out_log, out_cap, "mount: missing source or target argument\n");
        return -1;
    }

    int drive_idx = -1;
    char canon_name[32] = "";
    char alt_name[32] = "";
    StorageDevice *dev = vfs_resolve_device(source, &drive_idx, canon_name, alt_name);

    if (!dev) {
        if (out_log && out_cap > 0) snprintf(out_log, out_cap, "mount: %s: device not found\n", source);
        return -1;
    }

    StorageFsInfo info;
    if (storage_inspect_fs(dev, &info) != 0 || !info.has_filesystem) {
        if (out_log && out_cap > 0) {
            snprintf(out_log, out_cap, "mount: %s: cannot access filesystem because it is RAW / Unformatted.\n", source);
        }
        return -1;
    }

    /* Mount filesystem into target path */
    extern int fat32_mount_to_path(StorageDevice *dev, const char *mount_path);
    if (fat32_mount_to_path(dev, target) != 0) {
        if (out_log && out_cap > 0) snprintf(out_log, out_cap, "mount: %s: failed to mount to %s\n", source, target);
        return -1;
    }

    /* Add to g_mounts */
    int slot = -1;
    for (int m = 0; m < MAX_MOUNTS; m++) {
        if (g_mounts[m].active && strcmp(g_mounts[m].mount_path, target) == 0) {
            slot = m;
            break;
        }
    }
    if (slot < 0) {
        for (int m = 0; m < MAX_MOUNTS; m++) {
            if (!g_mounts[m].active) {
                slot = m;
                break;
            }
        }
    }
    if (slot >= 0) {
        g_mounts[slot].active = 1;
        g_mounts[slot].is_root = (strcmp(target, "/") == 0);
        g_mounts[slot].is_raw = 0;
        g_mounts[slot].drive_index = drive_idx;
        g_mounts[slot].dev_type = dev->type;
        strncpy(g_mounts[slot].dev_name, canon_name, sizeof(g_mounts[slot].dev_name) - 1);
        strncpy(g_mounts[slot].alt_name, alt_name, sizeof(g_mounts[slot].alt_name) - 1);
        strncpy(g_mounts[slot].mount_path, target, sizeof(g_mounts[slot].mount_path) - 1);
        strncpy(g_mounts[slot].fs_type, info.fs_type, sizeof(g_mounts[slot].fs_type) - 1);
        strncpy(g_mounts[slot].label, info.vol_label, sizeof(g_mounts[slot].label) - 1);
        g_mounts[slot].total_bytes = info.total_bytes;
    }

    if (out_log && out_cap > 0) {
        snprintf(out_log, out_cap, "mount: %s mounted on %s (%s) [%s]\n",
            source, target, info.fs_type, info.vol_label[0] ? info.vol_label : "NO_NAME");
    }
    return 0;
}

int vfs_umount_target(const char *target, char *out_log, size_t out_cap) {
    if (!target || target[0] == '\0') {
        if (out_log && out_cap > 0) snprintf(out_log, out_cap, "umount: missing target\n");
        return -1;
    }

    if (strcmp(target, "/") == 0) {
        if (out_log && out_cap > 0) snprintf(out_log, out_cap, "umount: /: root filesystem cannot be unmounted\n");
        return -1;
    }

    int found_idx = -1;
    for (int m = 0; m < MAX_MOUNTS; m++) {
        if (g_mounts[m].active) {
            if (strcmp(g_mounts[m].mount_path, target) == 0 ||
                strcmp(g_mounts[m].dev_name, target) == 0 ||
                strcmp(g_mounts[m].alt_name, target) == 0) {
                found_idx = m;
                break;
            }
        }
    }

    if (found_idx < 0) {
        if (out_log && out_cap > 0) snprintf(out_log, out_cap, "umount: %s: not currently mounted\n", target);
        return -1;
    }

    if (g_mounts[found_idx].is_root) {
        if (out_log && out_cap > 0) snprintf(out_log, out_cap, "umount: %s: root filesystem cannot be unmounted\n", target);
        return -1;
    }

    char unmounted_path[64];
    strncpy(unmounted_path, g_mounts[found_idx].mount_path, sizeof(unmounted_path) - 1);
    unmounted_path[sizeof(unmounted_path) - 1] = '\0';

    /* Remove files from VFS under that mount path */
    vfs_remove_node_ex(unmounted_path, 1, 1);
    g_mounts[found_idx].active = 0;

    if (out_log && out_cap > 0) {
        snprintf(out_log, out_cap, "umount: %s unmounted successfully\n", unmounted_path);
    }
    return 0;
}

int vfs_list_mounts(char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap == 0) return -1;
    out_buf[0] = '\0';
    size_t offset = 0;

    for (int m = 0; m < MAX_MOUNTS; m++) {
        if (g_mounts[m].active) {
            char line[128];
            snprintf(line, sizeof(line), "%s on %s type %s (rw) [%s]\n",
                g_mounts[m].dev_name, g_mounts[m].mount_path, g_mounts[m].fs_type,
                g_mounts[m].label[0] ? g_mounts[m].label : "NO_NAME");
            size_t llen = strlen(line);
            if (offset + llen < out_cap) {
                memcpy(out_buf + offset, line, llen);
                offset += llen;
                out_buf[offset] = '\0';
            }
        }
    }
    return (int)offset;
}

int vfs_auto_mount_all(char *out_log, size_t out_cap) {
    if (out_log && out_cap > 0) out_log[0] = '\0';
    uint32_t count = storage_get_device_count();
    size_t offset = 0;

    for (uint32_t i = 0; i < count; i++) {
        StorageDevice *dev = storage_get_device(i);
        if (!dev) continue;

        char canon[32], alt[32];
        snprintf(canon, sizeof(canon), "/dev/sd%c1", 'a' + i);
        if (dev->type == STORAGE_TYPE_INTERNAL_SATA) snprintf(alt, sizeof(alt), "sata%d", i);
        else if (dev->type == STORAGE_TYPE_INTERNAL_NVME) snprintf(alt, sizeof(alt), "nvme%dn1", i);
        else snprintf(alt, sizeof(alt), "usb%d", i);

        /* Check if already mounted */
        int already_mounted = 0;
        for (int m = 0; m < MAX_MOUNTS; m++) {
            if (g_mounts[m].active && g_mounts[m].drive_index == (int)i) {
                already_mounted = 1;
                break;
            }
        }
        if (already_mounted) continue;

        StorageFsInfo info;
        if (storage_inspect_fs(dev, &info) != 0 || !info.has_filesystem) {
            if (out_log && out_cap > 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "mount: skipping %s (%s): RAW / Unformatted\n", canon, alt);
                size_t mlen = strlen(msg);
                if (offset + mlen < out_cap) {
                    memcpy(out_log + offset, msg, mlen);
                    offset += mlen;
                    out_log[offset] = '\0';
                }
            }
            continue;
        }

        /* Mount to /mounts/<alt> */
        char target[64];
        snprintf(target, sizeof(target), "/mounts/%s", alt);
        int res = vfs_mount_device(canon, target, info.fs_type, NULL, 0);
        if (out_log && out_cap > 0) {
            char msg[128];
            if (res == 0) {
                snprintf(msg, sizeof(msg), "mount: %s mounted on %s (%s)\n", canon, target, info.fs_type);
            } else {
                snprintf(msg, sizeof(msg), "mount: failed to mount %s on %s\n", canon, target);
            }
            size_t mlen = strlen(msg);
            if (offset + mlen < out_cap) {
                memcpy(out_log + offset, msg, mlen);
                offset += mlen;
                out_log[offset] = '\0';
            }
        }
    }

    vfs_refresh_mounts_dir();
    return 0;
}


