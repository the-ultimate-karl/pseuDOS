#include "fs.h"
#include "lib.h"
#include "drivers.h"

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

    char segments[32][64];
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
                    if (seg_count < 32) {
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
            if (seg_count < 32) {
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

    return create_node(dir_name, VFS_NODE_DIRECTORY, parent);
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

    return create_node(file_name, VFS_NODE_FILE, parent);
}

int vfs_write_file(const char *path, const char *text, int append) {
    if (!path || !text) return -1;

    vfs_node_t *node = vfs_create_file(path);
    if (!node || node->type != VFS_NODE_FILE) return -1;

    size_t text_len = strlen(text);

    if (append && node->content) {
        size_t new_size = node->size + text_len;
        if (new_size + 1 > node->capacity) {
            size_t new_cap = (new_size + 1 + 255) & ~255;
            char *new_buf = (char *)krealloc(node->content, new_cap);
            if (!new_buf) return -1;
            node->content = new_buf;
            node->capacity = new_cap;
        }
        memcpy(node->content + node->size, text, text_len);
        node->size = new_size;
        node->content[node->size] = '\0';
    } else {
        if (!node->content || (text_len + 1) > node->capacity) {
            if (node->content) kfree(node->content);
            node->capacity = (text_len + 1 + 255) & ~255;
            node->content = (char *)kmalloc(node->capacity);
            if (!node->content) return -1;
        }
        memcpy(node->content, text, text_len);
        node->size = text_len;
        node->content[node->size] = '\0';
    }

    return (int)node->size;
}

int vfs_read_file(const char *path, char *buffer, size_t max_len) {
    if (!path || !buffer || max_len == 0) return -1;

    vfs_node_t *node = vfs_find_node(path);
    if (!node || node->type != VFS_NODE_FILE) return -1;

    size_t to_read = node->size < max_len - 1 ? node->size : max_len - 1;
    if (to_read > 0 && node->content) {
        memcpy(buffer, node->content, to_read);
    }
    buffer[to_read] = '\0';
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

    vfs_node_t *parent = node->parent;
    if (!parent) return -3;

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
        target = g_vfs_cwd;
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

int vfs_init_initramfs(void) {
    /* 1. Create root directory node */
    g_vfs_root = create_node("/", VFS_NODE_DIRECTORY, NULL);
    if (!g_vfs_root) return -1;

    g_vfs_cwd = g_vfs_root;
    strcpy(g_cwd_path, "/");

    /* 2. Create EFI system tree */
    vfs_node_t *efi = vfs_mkdir("/EFI");
    if (efi) efi->is_protected = 1;
    vfs_mkdir("/EFI/BOOT");
    vfs_mkdir("/EFI/pseuDOS");

    vfs_node_t *boot_efi = vfs_create_file("/EFI/BOOT/BOOTX64.EFI");
    if (boot_efi) boot_efi->size = 31554;

    vfs_node_t *krnl_bin = vfs_create_file("/EFI/pseuDOS/kernel.bin");
    if (krnl_bin) krnl_bin->size = 54204;

    /* 3. Create Home & Configuration directories */
    vfs_mkdir("/home");
    vfs_mkdir("/etc");

    /* 4. Create Protected system trees */
    vfs_node_t *prot = vfs_mkdir("/protected");
    if (prot) prot->is_protected = 1;
    vfs_mkdir("/protected/bootmgr");
    vfs_mkdir("/protected/crit");
    vfs_mkdir("/protected/krnl");
    vfs_mkdir("/protected/krnl/essential");

    /* 5. Create default system configuration and documents */
    vfs_write_file("/home/readme.txt", "welcome to pseuDOS bare-metal kernel filesystem!\ntype 'help' to view available commands.\n", 0);
    vfs_write_file("/etc/hostname", "pseuDOS\n", 0);
    vfs_write_file("/etc/os-release", "NAME=pseuDOS\nVERSION=0.4.1-baremetal\nARCH=x86_64\nEDITION=bare-metal\n", 0);
    vfs_write_file("/protected/bootmgr/config.sys", "boot_default=pseuDOS\ntimeout=5\ndebug=0\n", 0);

    return 0;
}
