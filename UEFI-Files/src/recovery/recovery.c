#include "efi.h"
#include "lib.h"
#include "io.h"

/*
 * SiLo Recovery Console v0.0.1
 * Standalone 64-bit UEFI Application for system diagnostics and recovery.
 */

#define MAX_PATH 256
#define MAX_VOLUMES 16
#define LINE_BUF_SZ 512

static EFI_HANDLE g_image_handle = NULL;
static EFI_SYSTEM_TABLE *g_st = NULL;
static EFI_BOOT_SERVICES *g_bs = NULL;

/* Volume management */
typedef struct {
    char name[8];          /* "fs0:", "fs1:" */
    EFI_HANDLE handle;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_FILE_PROTOCOL *root;
} volume_info_t;

static volume_info_t g_volumes[MAX_VOLUMES];
static UINTN g_num_volumes = 0;
static UINTN g_cur_volume_idx = 0;
static char g_cwd[MAX_PATH] = "\\";

/* COM1 UART helpers */
static void uart_putc(char c) {
    int timeout = 50000;
    while ((inb(0x3F8 + 5) & 0x20) == 0 && --timeout > 0);
    if (timeout > 0) outb(0x3F8, (uint8_t)c);
}

static void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

/* Console output helpers */
static void con_puts(const char *s) {
    if (!s) return;
    uart_puts(s);
    if (g_st && g_st->ConOut) {
        CHAR16 buf[256];
        size_t i = 0;
        while (*s && i < 254) {
            if (*s == '\n') buf[i++] = (CHAR16)'\r';
            buf[i++] = (CHAR16)(*s++);
        }
        buf[i] = 0;
        g_st->ConOut->OutputString(g_st->ConOut, buf);
    }
}

static void con_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_puts(buf);
}

static void ascii_to_unicode(const char *src, CHAR16 *dst, size_t max_len) {
    size_t i = 0;
    if (!src || !dst || max_len == 0) return;
    while (src[i] != '\0' && i + 1 < max_len) {
        dst[i] = (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

static void unicode_to_ascii(const CHAR16 *src, char *dst, size_t max_len) {
    size_t i = 0;
    if (!src || !dst || max_len == 0) return;
    while (src[i] != 0 && i + 1 < max_len) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

static void get_screen_size(UINTN *cols_out, UINTN *rows_out) {
    *cols_out = 80;
    *rows_out = 25;
    if (g_st && g_st->ConOut && g_st->ConOut->QueryMode && g_st->ConOut->Mode) {
        (void)g_st->ConOut->QueryMode(g_st->ConOut, (UINTN)g_st->ConOut->Mode->Mode, cols_out, rows_out);
    }
}



static void render_header(void) {
    /* 1. Direct hardware GOP framebuffer wipe: zeroes out all pixels on the graphical display */
    if (g_bs && g_bs->LocateProtocol) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        EFI_STATUS st = g_bs->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
        if (!EFI_ERROR(st) && gop && gop->Mode) {
            if (gop->Mode->FrameBufferBase && gop->Mode->FrameBufferSize > 0) {
                memset((void *)gop->Mode->FrameBufferBase, 0, gop->Mode->FrameBufferSize);
            }
        }
    }

    /* 2. Switch ConOut to matching high-resolution text mode */
    if (g_st && g_st->ConOut && g_st->ConOut->QueryMode && g_st->ConOut->SetMode && g_st->ConOut->Mode) {
        INT32 best_text_mode = g_st->ConOut->Mode->Mode;
        UINTN max_text_cols = 0;
        for (INT32 tm = 0; tm < g_st->ConOut->Mode->MaxMode; tm++) {
            UINTN c = 0, r = 0;
            if (!EFI_ERROR(g_st->ConOut->QueryMode(g_st->ConOut, (UINTN)tm, &c, &r))) {
                if (c > max_text_cols) {
                    max_text_cols = c;
                    best_text_mode = tm;
                }
            }
        }
        if (max_text_cols > 0 && best_text_mode != g_st->ConOut->Mode->Mode) {
            g_st->ConOut->SetMode(g_st->ConOut, (UINTN)best_text_mode);
        }
    }

    /* 3. Reset UEFI text console state and clear text cells */
    if (g_st && g_st->ConOut) {
        if (g_st->ConOut->SetAttribute) {
            g_st->ConOut->SetAttribute(g_st->ConOut, 0x0F);
        }
        if (g_st->ConOut->ClearScreen) {
            g_st->ConOut->ClearScreen(g_st->ConOut);
        }
        if (g_st->ConOut->SetCursorPosition) {
            g_st->ConOut->SetCursorPosition(g_st->ConOut, 0, 0);
        }
        if (g_st->ConOut->EnableCursor) {
            g_st->ConOut->EnableCursor(g_st->ConOut, TRUE);
        }
    }

    /* Clear ANSI serial terminal */
    uart_puts("\033[2J\033[H");

    UINTN cols = 80, rows = 25;
    get_screen_size(&cols, &rows);

    /* 4. Position cursor at row 1 */
    if (g_st && g_st->ConOut && g_st->ConOut->SetCursorPosition) {
        g_st->ConOut->SetCursorPosition(g_st->ConOut, 0, 1);
    }

    const char *title = "SiLo Recovery Console v0.0.1";
    const char *warn = "Please be careful, as any changes here can potentially be destructive!";

    /* Print centered title */
    size_t tlen = strlen(title);
    size_t t_pad = (cols > tlen) ? ((cols - tlen) / 2) : 0;
    for (size_t i = 0; i < t_pad; i++) con_puts(" ");
    con_puts(title);
    con_puts("\n");

    /* Print centered warning */
    size_t wlen = strlen(warn);
    size_t w_pad = (cols > wlen) ? ((cols - wlen) / 2) : 0;
    for (size_t i = 0; i < w_pad; i++) con_puts(" ");
    con_puts(warn);
    con_puts("\n\n");
}

/* Enumerate filesystem volumes */
static void discover_volumes(void) {
    g_num_volumes = 0;
    g_cur_volume_idx = 0;

    EFI_HANDLE handles[MAX_VOLUMES];
    UINTN buf_size = sizeof(handles);

    EFI_STATUS st = g_bs->LocateHandle(
        ByProtocol,
        &gEfiSimpleFileSystemProtocolGuid,
        NULL,
        &buf_size,
        handles
    );

    if (EFI_ERROR(st)) return;
    UINTN count = buf_size / sizeof(EFI_HANDLE);
    if (count > MAX_VOLUMES) count = MAX_VOLUMES;

    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
        st = g_bs->HandleProtocol(handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&sfs);
        if (!EFI_ERROR(st) && sfs) {
            EFI_FILE_PROTOCOL *root = NULL;
            st = sfs->OpenVolume(sfs, &root);
            if (!EFI_ERROR(st) && root) {
                snprintf(g_volumes[g_num_volumes].name, 8, "fs%u:", (unsigned int)g_num_volumes);
                g_volumes[g_num_volumes].handle = handles[i];
                g_volumes[g_num_volumes].sfs = sfs;
                g_volumes[g_num_volumes].root = root;
                g_num_volumes++;
            }
        }
    }

    /* Auto-select volume containing pseuDOS boot files */
    for (UINTN v = 0; v < g_num_volumes; v++) {
        EFI_FILE_PROTOCOL *vr = g_volumes[v].root;
        if (!vr) continue;
        EFI_FILE_PROTOCOL *tf = NULL;
        CHAR16 test_p1[] = { 'p','r','o','t','e','c','t','e','d','\\','b','o','o','t','m','g','r','\\','b','o','o','t','.','c','f','g', 0 };
        CHAR16 test_p2[] = { '\\','p','r','o','t','e','c','t','e','d','\\','b','o','o','t','m','g','r','\\','b','o','o','t','.','c','f','g', 0 };
        if (!EFI_ERROR(vr->Open(vr, &tf, test_p1, EFI_FILE_MODE_READ, 0)) && tf) {
            tf->Close(tf);
            g_cur_volume_idx = v;
            break;
        }
        if (!EFI_ERROR(vr->Open(vr, &tf, test_p2, EFI_FILE_MODE_READ, 0)) && tf) {
            tf->Close(tf);
            g_cur_volume_idx = v;
            break;
        }
    }
}

static EFI_FILE_PROTOCOL *get_current_root(void) {
    if (g_num_volumes == 0 || g_cur_volume_idx >= g_num_volumes) return NULL;
    return g_volumes[g_cur_volume_idx].root;
}

/* Resolve path relative to CWD */
static void resolve_path(const char *rel, char *out, size_t out_sz) {
    if (!rel || rel[0] == '\0') {
        strncpy(out, g_cwd, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    if (rel[0] == '\\' || rel[0] == '/') {
        size_t j = 0;
        for (size_t i = 0; rel[i] && j + 1 < out_sz; i++) {
            out[j++] = (rel[i] == '/') ? '\\' : rel[i];
        }
        out[j] = '\0';
        return;
    }

    /* Relative to g_cwd */
    strncpy(out, g_cwd, out_sz - 1);
    out[out_sz - 1] = '\0';
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] != '\\') {
        if (len + 1 < out_sz) {
            out[len++] = '\\';
            out[len] = '\0';
        }
    }
    for (size_t i = 0; rel[i] && len + 1 < out_sz; i++) {
        out[len++] = (rel[i] == '/') ? '\\' : rel[i];
    }
    out[len] = '\0';
}

/* File exists check */
static int file_exists(const char *path, uint64_t *file_size_out) {
    EFI_FILE_PROTOCOL *root = get_current_root();
    if (!root) return 0;

    char norm[MAX_PATH];
    resolve_path(path, norm, sizeof(norm));

    const char *p = (norm[0] == '\\') ? (norm + 1) : norm;
    if (*p == '\0') {
        if (file_size_out) *file_size_out = 0;
        return 1;
    }

    CHAR16 u16[MAX_PATH];
    ascii_to_unicode(p, u16, sizeof(u16)/sizeof(CHAR16));

    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f) {
        ascii_to_unicode(norm, u16, sizeof(u16)/sizeof(CHAR16));
        st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(st) || !f) return 0;

    if (file_size_out) {
        UINT8 buf[sizeof(EFI_FILE_INFO) + 256];
        UINTN sz = sizeof(buf);
        st = f->GetInfo(f, &gEfiFileInfoGuid, &sz, buf);
        if (!EFI_ERROR(st)) {
            *file_size_out = ((EFI_FILE_INFO *)buf)->FileSize;
        } else {
            *file_size_out = 0;
        }
    }
    f->Close(f);
    return 1;
}

/* Commands */
static void cmd_help(void) {
    con_puts("Available commands:\n");
    con_puts("  bootcfg               - Display boot configuration health status\n");
    con_puts("  bootcfg --repair      - Inspect boot.cfg and repair only broken/missing directives\n");
    con_puts("  bootcfg --generate    - Generate a fresh, valid boot.cfg with system defaults\n");
    con_puts("  ls / dir [path]       - List files and directories in current or specified path\n");
    con_puts("  cd [path]             - Change current working directory\n");
    con_puts("  cat / type <file>     - Display contents of a text file\n");
    con_puts("  cp / copy <src> <dst> - Copy a file\n");
    con_puts("  mv / ren <src> <dst>  - Rename or move a file\n");
    con_puts("  rm / del <file>       - Delete a file\n");
    con_puts("  echo <text> [> file]  - Print text or write/append to file\n");
    con_puts("  fs0:, fs1:...         - Switch active filesystem volume\n");
    con_puts("  cls / clear           - Clear screen\n");
    con_puts("  boot                  - Switch back to bootloader and retry booting system\n");
    con_puts("  reboot                - Cold restart computer\n");
    con_puts("  shutdown              - Power off computer\n");
    con_puts("  exit / ESC            - Return to SiLo error screen\n");
}

static void cmd_ls(const char *arg) {
    EFI_FILE_PROTOCOL *root = get_current_root();
    if (!root) {
        con_puts("error: no active filesystem volume\n");
        return;
    }

    char target[MAX_PATH];
    resolve_path(arg, target, sizeof(target));

    EFI_FILE_PROTOCOL *dir = NULL;
    EFI_STATUS st;

    if (strcmp(target, "\\") == 0 || target[0] == '\0') {
        /* Open root directory directly from volume */
        st = g_volumes[g_cur_volume_idx].sfs->OpenVolume(g_volumes[g_cur_volume_idx].sfs, &dir);
    } else {
        const char *p = (target[0] == '\\') ? (target + 1) : target;
        CHAR16 u16[MAX_PATH];
        ascii_to_unicode(p, u16, sizeof(u16)/sizeof(CHAR16));
        st = root->Open(root, &dir, u16, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(st) || !dir) {
            ascii_to_unicode(target, u16, sizeof(u16)/sizeof(CHAR16));
            st = root->Open(root, &dir, u16, EFI_FILE_MODE_READ, 0);
        }
    }

    if (EFI_ERROR(st) || !dir) {
        con_printf("error: cannot open directory '%s'\n", target);
        return;
    }

    con_printf(" Directory of %s%s\n\n",
               g_num_volumes > g_cur_volume_idx ? (char *)g_volumes[g_cur_volume_idx].name : "",
               target);

    UINT8 info_buf[sizeof(EFI_FILE_INFO) + 512];
    UINTN count = 0;
    while (1) {
        UINTN buf_sz = sizeof(info_buf);
        st = dir->Read(dir, &buf_sz, info_buf);
        if (EFI_ERROR(st) || buf_sz == 0) break;

        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info_buf;
        char name[128];
        unicode_to_ascii(fi->FileName, name, sizeof(name));

        if (name[0] == '.' && name[1] == '\0') continue;
        if (name[0] == '.' && name[1] == '.' && name[2] == '\0') continue;

        int is_dir = (fi->Attribute & 0x10) ? 1 : 0;
        if (is_dir) {
            con_printf("  <DIR>          %-24s\n", name);
        } else {
            con_printf("  %10lu B   %-24s\n", (unsigned long)fi->FileSize, name);
        }
        count++;
    }
    dir->Close(dir);
    con_printf("\n  Total entries: %lu\n", (unsigned long)count);
}

static void cmd_cd(const char *path) {
    if (!path || path[0] == '\0') {
        con_printf("%s%s\n", (char *)g_volumes[g_cur_volume_idx].name, g_cwd);
        return;
    }

    if (strcmp(path, "\\") == 0 || strcmp(path, "/") == 0) {
        strcpy(g_cwd, "\\");
        return;
    }

    if (strcmp(path, "..") == 0) {
        /* Go up one directory */
        char *last = strrchr(g_cwd, '\\');
        if (last && last != g_cwd) {
            *last = '\0';
        } else {
            strcpy(g_cwd, "\\");
        }
        return;
    }

    char target[MAX_PATH];
    resolve_path(path, target, sizeof(target));

    if (strcmp(target, "\\") == 0) {
        strcpy(g_cwd, "\\");
        return;
    }

    EFI_FILE_PROTOCOL *root = get_current_root();
    if (!root) return;

    const char *p = (target[0] == '\\') ? (target + 1) : target;
    CHAR16 u16[MAX_PATH];
    ascii_to_unicode(p, u16, sizeof(u16)/sizeof(CHAR16));

    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f) {
        ascii_to_unicode(target, u16, sizeof(u16)/sizeof(CHAR16));
        st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    }
    if (!EFI_ERROR(st) && f) {
        UINT8 buf[sizeof(EFI_FILE_INFO) + 256];
        UINTN sz = sizeof(buf);
        if (!EFI_ERROR(f->GetInfo(f, &gEfiFileInfoGuid, &sz, buf))) {
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
            if (fi->Attribute & 0x10) {
                strncpy(g_cwd, target, sizeof(g_cwd) - 1);
                g_cwd[sizeof(g_cwd) - 1] = '\0';
            } else {
                con_printf("error: '%s' is not a directory\n", target);
            }
        }
        f->Close(f);
    } else {
        con_printf("error: directory '%s' not found\n", target);
    }
}

static void cmd_cat(const char *path) {
    if (!path || path[0] == '\0') {
        con_puts("usage: cat <file>\n");
        return;
    }

    char target[MAX_PATH];
    resolve_path(path, target, sizeof(target));

    EFI_FILE_PROTOCOL *root = get_current_root();
    if (!root) return;

    const char *p = (target[0] == '\\') ? (target + 1) : target;
    CHAR16 u16[MAX_PATH];
    ascii_to_unicode(p, u16, sizeof(u16)/sizeof(CHAR16));

    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f) {
        ascii_to_unicode(target, u16, sizeof(u16)/sizeof(CHAR16));
        st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(st) || !f) {
        con_printf("error: cannot open '%s'\n", target);
        return;
    }

    char buf[512];
    UINTN read_sz = sizeof(buf) - 1;
    while (!EFI_ERROR(f->Read(f, &read_sz, buf)) && read_sz > 0) {
        buf[read_sz] = '\0';
        con_puts(buf);
        read_sz = sizeof(buf) - 1;
    }
    con_puts("\n");
    f->Close(f);
}

static void cmd_rm(const char *path) {
    if (!path || path[0] == '\0') {
        con_puts("usage: rm <file>\n");
        return;
    }

    char target[MAX_PATH];
    resolve_path(path, target, sizeof(target));

    EFI_FILE_PROTOCOL *root = get_current_root();
    if (!root) return;

    const char *p = (target[0] == '\\') ? (target + 1) : target;
    CHAR16 u16[MAX_PATH];
    ascii_to_unicode(p, u16, sizeof(u16)/sizeof(CHAR16));

    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, u16, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(st) || !f) {
        if (st == (EFI_STATUS)0x8 || (st & 0xFF) == 0x8) {
            con_puts("error: volume is write-protected (booting from read-only CD-ROM/ISO media)\n");
        } else {
            con_printf("error: cannot open '%s' for deletion\n", target);
        }
        return;
    }

    st = f->Delete(f);
    if (!EFI_ERROR(st)) {
        con_printf("[ok] deleted '%s'\n", target);
    } else {
        if (st == (EFI_STATUS)0x8 || (st & 0xFF) == 0x8) {
            con_puts("error: volume is write-protected (booting from read-only CD-ROM/ISO media)\n");
        } else {
            con_printf("error: failed to delete '%s' (status 0x%lx)\n", target, (unsigned long)st);
        }
    }
}

static void cmd_cp(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path || src_path[0] == '\0' || dst_path[0] == '\0') {
        con_puts("usage: cp <src> <dst>\n");
        return;
    }

    EFI_FILE_PROTOCOL *root = get_current_root();
    if (!root) return;

    char src_n[MAX_PATH], dst_n[MAX_PATH];
    resolve_path(src_path, src_n, sizeof(src_n));
    resolve_path(dst_path, dst_n, sizeof(dst_n));

    const char *p_src = (src_n[0] == '\\') ? (src_n + 1) : src_n;
    const char *p_dst = (dst_n[0] == '\\') ? (dst_n + 1) : dst_n;

    CHAR16 u_src[MAX_PATH], u_dst[MAX_PATH];
    ascii_to_unicode(p_src, u_src, sizeof(u_src)/sizeof(CHAR16));
    ascii_to_unicode(p_dst, u_dst, sizeof(u_dst)/sizeof(CHAR16));

    EFI_FILE_PROTOCOL *f_src = NULL, *f_dst = NULL;
    EFI_STATUS st = root->Open(root, &f_src, u_src, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f_src) {
        ascii_to_unicode(src_n, u_src, sizeof(u_src)/sizeof(CHAR16));
        st = root->Open(root, &f_src, u_src, EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(st) || !f_src) {
        con_printf("error: cannot open source '%s'\n", src_n);
        return;
    }

    st = root->Open(root, &f_dst, u_dst, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(st) || !f_dst) {
        f_src->Close(f_src);
        if (st == (EFI_STATUS)0x8 || (st & 0xFF) == 0x8) {
            con_puts("error: volume is write-protected (booting from read-only CD-ROM/ISO media)\n");
        } else {
            con_printf("error: cannot create destination '%s'\n", dst_n);
        }
        return;
    }

    char buf[4096];
    UINTN rsz = sizeof(buf);
    while (!EFI_ERROR(f_src->Read(f_src, &rsz, buf)) && rsz > 0) {
        UINTN wsz = rsz;
        (void)f_dst->Write(f_dst, &wsz, buf);
        rsz = sizeof(buf);
    }
    f_dst->Flush(f_dst);
    f_dst->Close(f_dst);
    f_src->Close(f_src);

    con_printf("[ok] copied '%s' -> '%s'\n", src_n, dst_n);
}

/* Locate existing boot.cfg path (checks active volume first, then other volumes) */
static int find_bootcfg_path(char *out_path, size_t out_sz) {
    const char *candidates[] = {
        "\\protected\\bootmgr\\boot.cfg",
        "\\boot\\boot.cfg",
        "\\boot.cfg",
        NULL
    };

    /* First check current volume */
    for (int i = 0; candidates[i]; i++) {
        if (file_exists(candidates[i], NULL)) {
            strncpy(out_path, candidates[i], out_sz - 1);
            out_path[out_sz - 1] = '\0';
            return 1;
        }
    }

    /* If not found on current volume, search other discovered volumes */
    for (UINTN v = 0; v < g_num_volumes; v++) {
        if (v == g_cur_volume_idx) continue;
        EFI_FILE_PROTOCOL *vr = g_volumes[v].root;
        if (!vr) continue;
        for (int i = 0; candidates[i]; i++) {
            const char *cand = candidates[i];
            const char *p = (cand[0] == '\\') ? (cand + 1) : cand;
            CHAR16 u16[MAX_PATH];
            ascii_to_unicode(p, u16, sizeof(u16)/sizeof(CHAR16));
            EFI_FILE_PROTOCOL *f = NULL;
            if (!EFI_ERROR(vr->Open(vr, &f, u16, EFI_FILE_MODE_READ, 0)) && f) {
                f->Close(f);
                g_cur_volume_idx = v;
                strcpy(g_cwd, "\\");
                con_printf("(Switched active volume to %s)\n", (char *)g_volumes[v].name);
                strncpy(out_path, cand, out_sz - 1);
                out_path[out_sz - 1] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/*
 * bootcfg command implementation
 */
static void cmd_bootcfg(const char *subarg) {
    EFI_FILE_PROTOCOL *root = get_current_root();
    if (!root) {
        con_puts("error: no active filesystem volume\n");
        return;
    }

    int do_repair = (subarg && (strcmp(subarg, "--repair") == 0 || strcmp(subarg, "-r") == 0));
    int do_generate = (subarg && (strcmp(subarg, "--generate") == 0 || strcmp(subarg, "-g") == 0));

    char cfg_path[MAX_PATH];
    int found = find_bootcfg_path(cfg_path, sizeof(cfg_path));

    if (do_generate) {
        /* Generate fresh boot.cfg with safe defaults */
        const char *gen_path = "\\protected\\bootmgr\\boot.cfg";
        const char *p_gen = (gen_path[0] == '\\') ? (gen_path + 1) : gen_path;
        CHAR16 u16[MAX_PATH];
        ascii_to_unicode(p_gen, u16, sizeof(u16)/sizeof(CHAR16));

        /* Ensure parent directory exists: create protected, then protected\bootmgr */
        EFI_FILE_PROTOCOL *dir1 = NULL, *dir2 = NULL;
        CHAR16 u_p1[] = { 'p', 'r', 'o', 't', 'e', 'c', 't', 'e', 'd', 0 };
        CHAR16 u_p2[] = { 'p', 'r', 'o', 't', 'e', 'c', 't', 'e', 'd', '\\', 'b', 'o', 'o', 't', 'm', 'g', 'r', 0 };
        (void)root->Open(root, &dir1, u_p1, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0x10);
        if (dir1) dir1->Close(dir1);
        (void)root->Open(root, &dir2, u_p2, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0x10);
        if (dir2) dir2->Close(dir2);

        /* Detect valid kernel location on disk */
        const char *k_loc = "\\protected\\krnl\\vpkernel";
        if (!file_exists(k_loc, NULL)) {
            if (file_exists("\\protected\\bin\\vpkernel", NULL)) k_loc = "\\protected\\bin\\vpkernel";
            else if (file_exists("\\boot\\vpkernel", NULL)) k_loc = "\\boot\\vpkernel";
            else if (file_exists("\\vpkernel", NULL)) k_loc = "\\vpkernel";
        }

        /* Detect initramfs location on disk */
        const char *init_loc = "\\protected\\krnl\\initramfs.img";
        if (!file_exists(init_loc, NULL)) {
            if (file_exists("\\protected\\bootmgr\\initramfs.img", NULL)) init_loc = "\\protected\\bootmgr\\initramfs.img";
            else if (file_exists("\\boot\\initramfs.img", NULL)) init_loc = "\\boot\\initramfs.img";
            else if (file_exists("\\initramfs.img", NULL)) init_loc = "\\initramfs.img";
        }

        char out_buf[1024];
        snprintf(out_buf, sizeof(out_buf),
                 "# pseuDOS Boot Configuration (Generated by SiLo Recovery)\r\n"
                 "kernel=%s\r\n"
                 "initramfs=%s\r\n"
                 "autoinit=\\protected\\krnl\\autoinit.exe\r\n"
                 "shell=\\protected\\gui\\gman.exe\r\n"
                 "cmdline=quiet devpath=hardware boot=live\r\n"
                 "default_resolution=1280x720\r\n",
                 k_loc, init_loc);

        EFI_FILE_PROTOCOL *f = NULL;
        EFI_STATUS st = root->Open(root, &f, u16, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        if (EFI_ERROR(st) || !f) {
            ascii_to_unicode(gen_path, u16, sizeof(u16)/sizeof(CHAR16));
            st = root->Open(root, &f, u16, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        }
        if (!EFI_ERROR(st) && f) {
            UINTN wsz = strlen(out_buf);
            st = f->Write(f, &wsz, out_buf);
            f->Flush(f);
            f->Close(f);
            con_printf("[ok] successfully generated fresh '%s'\n", gen_path);
        } else {
            if (st == (EFI_STATUS)0x8 || (st & 0xFF) == 0x8) {
                con_puts("error: volume is write-protected (booting from read-only CD-ROM/ISO media)\n");
                con_puts("cannot generate configuration file on read-only media.\n");
            } else {
                con_printf("error: failed to write '%s' (status 0x%lx)\n", gen_path, (unsigned long)st);
            }
        }
        return;
    }

    if (!found) {
        con_puts("bootcfg is missing, please run bootcfg --generate\n");
        return;
    }

    /* Open existing boot.cfg in READ-ONLY mode */
    const char *p_cfg = (cfg_path[0] == '\\') ? (cfg_path + 1) : cfg_path;
    CHAR16 u16[MAX_PATH];
    ascii_to_unicode(p_cfg, u16, sizeof(u16)/sizeof(CHAR16));

    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !f) {
        ascii_to_unicode(cfg_path, u16, sizeof(u16)/sizeof(CHAR16));
        st = root->Open(root, &f, u16, EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(st) || !f) {
        con_printf("error: cannot open '%s' (status 0x%lx)\n", cfg_path, (unsigned long)st);
        return;
    }

    char cfg_buf[4096];
    UINTN rsz = sizeof(cfg_buf) - 1;
    st = f->Read(f, &rsz, cfg_buf);
    f->Close(f); /* Read complete, close handle immediately */

    if (EFI_ERROR(st) || rsz == 0) {
        con_puts("boot.cfg is empty or corrupted, please run bootcfg --generate\n");
        return;
    }
    cfg_buf[rsz] = '\0';

    /* Parse directives */
    char k_val[MAX_PATH] = "";
    char init_val[MAX_PATH] = "";
    char auto_val[MAX_PATH] = "";
    char sh_val[MAX_PATH] = "";
    char cmd_val[MAX_PATH] = "";

    char *line = cfg_buf;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
        if (*line == '\0') break;
        char *line_end = line;
        while (*line_end && *line_end != '\r' && *line_end != '\n') line_end++;
        char orig_end = *line_end;
        *line_end = '\0';

        if (*line != '#') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim(line);
                char *val = trim(eq + 1);
                if (strcmp(key, "kernel") == 0) strncpy(k_val, val, sizeof(k_val) - 1);
                else if (strcmp(key, "initramfs") == 0) strncpy(init_val, val, sizeof(init_val) - 1);
                else if (strcmp(key, "autoinit") == 0) strncpy(auto_val, val, sizeof(auto_val) - 1);
                else if (strcmp(key, "shell") == 0) strncpy(sh_val, val, sizeof(sh_val) - 1);
                else if (strcmp(key, "cmdline") == 0) strncpy(cmd_val, val, sizeof(cmd_val) - 1);
            }
        }
        if (orig_end == '\0') break;
        line = line_end + 1;
    }

    int k_ok = (k_val[0] && file_exists(k_val, NULL));
    int init_ok = (init_val[0] == '\0' || file_exists(init_val, NULL));
    int auto_ok = (auto_val[0] == '\0' || file_exists(auto_val, NULL));
    int sh_ok = (sh_val[0] == '\0' || file_exists(sh_val, NULL));

    if (!do_repair) {
        /*
         * Health Status Display:
         * Formatted lines aligning [OK] tags with the longest line:
         * kernel    = \protected\krnl\vpkernel          [OK]
         * initramfs = \protected\bootmgr\initramfs.img [OK]
         * autoinit  = \protected\krnl\autoinit.exe     [OK]
         * shell     = \protected\gui\gman.exe           [OK]
         * cmdline   = ...
         */
        con_printf("Configuration file: %s\n\n", cfg_path);

        /* Determine max length of "key       = val" prefix for dynamic alignment */
        char s_k[MAX_PATH + 32], s_init[MAX_PATH + 32], s_auto[MAX_PATH + 32], s_sh[MAX_PATH + 32];
        snprintf(s_k, sizeof(s_k), "kernel    = %s", k_val[0] ? k_val : "(not set)");
        snprintf(s_init, sizeof(s_init), "initramfs = %s", init_val[0] ? init_val : "(not set)");
        snprintf(s_auto, sizeof(s_auto), "autoinit  = %s", auto_val[0] ? auto_val : "(not set)");
        snprintf(s_sh, sizeof(s_sh), "shell     = %s", sh_val[0] ? sh_val : "(not set)");

        size_t max_w = strlen(s_k);
        if (strlen(s_init) > max_w) max_w = strlen(s_init);
        if (strlen(s_auto) > max_w) max_w = strlen(s_auto);
        if (strlen(s_sh) > max_w) max_w = strlen(s_sh);

        /* Print kernel */
        con_puts(s_k);
        for (size_t i = strlen(s_k); i < max_w + 1; i++) con_puts(" ");
        con_printf("[%s]\n", k_ok ? "OK" : "MISSING");

        /* Print initramfs */
        con_puts(s_init);
        for (size_t i = strlen(s_init); i < max_w + 1; i++) con_puts(" ");
        con_printf("[%s]\n", init_ok ? "OK" : "MISSING");

        /* Print autoinit */
        con_puts(s_auto);
        for (size_t i = strlen(s_auto); i < max_w + 1; i++) con_puts(" ");
        con_printf("[%s]\n", auto_ok ? "OK" : "MISSING");

        /* Print shell */
        con_puts(s_sh);
        for (size_t i = strlen(s_sh); i < max_w + 1; i++) con_puts(" ");
        con_printf("[%s]\n", sh_ok ? "OK" : "MISSING");

        /* Print cmdline */
        con_printf("cmdline   = %s\n", cmd_val[0] ? cmd_val : "(empty)");

        con_puts("\n");
        if (!k_ok || !init_ok || !auto_ok || !sh_ok) {
            con_puts("Status: ERRORS DETECTED! Run 'bootcfg --repair' to fix broken directives.\n");
        } else {
            con_puts("Status: All directives verified OK. System is ready to boot.\n");
        }
        return;
    }

    /* REPAIR MODE: find broken parts and fix ONLY those */
    con_printf("Inspecting '%s' for repair...\n", cfg_path);
    int repaired_any = 0;

    if (!k_ok) {
        /* Find genuine kernel on disk */
        const char *candidates[] = {
            "\\protected\\krnl\\vpkernel",
            "\\protected\\bin\\vpkernel",
            "\\boot\\vpkernel",
            "\\vpkernel",
            NULL
        };
        const char *found_k = NULL;
        for (int i = 0; candidates[i]; i++) {
            if (file_exists(candidates[i], NULL)) {
                found_k = candidates[i];
                break;
            }
        }
        if (found_k) {
            con_printf(" [fixed] kernel: '%s' -> '%s'\n", k_val, found_k);
            strncpy(k_val, found_k, sizeof(k_val) - 1);
            repaired_any = 1;
        } else {
            con_puts(" [failed] could not locate a valid 'vpkernel' executable on this volume!\n");
        }
    }

    if (!init_ok) {
        const char *candidates[] = {
            "\\protected\\krnl\\initramfs.img",
            "\\protected\\bootmgr\\initramfs.img",
            "\\boot\\initramfs.img",
            "\\initramfs.img",
            NULL
        };
        const char *found_init = NULL;
        for (int i = 0; candidates[i]; i++) {
            if (file_exists(candidates[i], NULL)) {
                found_init = candidates[i];
                break;
            }
        }
        if (found_init) {
            con_printf(" [fixed] initramfs: '%s' -> '%s'\n", init_val, found_init);
            strncpy(init_val, found_init, sizeof(init_val) - 1);
            repaired_any = 1;
        } else {
            con_printf(" [removed] stale initramfs path '%s' (no valid image found)\n", init_val);
            init_val[0] = '\0';
            repaired_any = 1;
        }
    }

    if (!auto_ok) {
        const char *candidates[] = {
            "\\protected\\krnl\\autoinit.exe",
            "\\protected\\crit\\autoinit.exe",
            "\\autoinit.exe",
            NULL
        };
        for (int i = 0; candidates[i]; i++) {
            if (file_exists(candidates[i], NULL)) {
                con_printf(" [fixed] autoinit: '%s' -> '%s'\n", auto_val, candidates[i]);
                strncpy(auto_val, candidates[i], sizeof(auto_val) - 1);
                repaired_any = 1;
                break;
            }
        }
    }

    if (!sh_ok) {
        const char *candidates[] = {
            "\\protected\\gui\\gman.exe",
            "\\protected\\crit\\bshss.exe",
            "\\protected\\apps\\shell.exe",
            NULL
        };
        for (int i = 0; candidates[i]; i++) {
            if (file_exists(candidates[i], NULL)) {
                con_printf(" [fixed] shell: '%s' -> '%s'\n", sh_val, candidates[i]);
                strncpy(sh_val, candidates[i], sizeof(sh_val) - 1);
                repaired_any = 1;
                break;
            }
        }
    }

    if (!repaired_any) {
        con_puts("Nothing broken found; boot.cfg is already healthy!\n");
        return;
    }

    /* Open file with WRITE permission to save repaired directives */
    EFI_FILE_PROTOCOL *fw = NULL;
    st = root->Open(root, &fw, u16, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(st) || !fw) {
        ascii_to_unicode(cfg_path, u16, sizeof(u16)/sizeof(CHAR16));
        st = root->Open(root, &fw, u16, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    }
    if (EFI_ERROR(st) || !fw) {
        if (st == (EFI_STATUS)0x8 || (st & 0xFF) == 0x8) {
            con_puts("error: volume is write-protected (booting from read-only CD-ROM/ISO media)\n");
            con_puts("repairs cannot be written to read-only media.\n");
        } else {
            con_printf("error: cannot open '%s' for writing (status 0x%lx)\n", cfg_path, (unsigned long)st);
        }
        return;
    }

    /* Write patched file */
    char out_buf[2048];
    snprintf(out_buf, sizeof(out_buf),
             "# pseuDOS Boot Configuration (Repaired by SiLo Recovery)\r\n"
             "kernel=%s\r\n"
             "%s%s%s"
             "autoinit=%s\r\n"
             "shell=%s\r\n"
             "cmdline=%s\r\n"
             "default_resolution=1280x720\r\n",
             k_val,
             init_val[0] ? "initramfs=" : "", init_val[0] ? init_val : "", init_val[0] ? "\r\n" : "",
             auto_val[0] ? auto_val : "\\protected\\krnl\\autoinit.exe",
             sh_val[0] ? sh_val : "\\protected\\gui\\gman.exe",
             cmd_val[0] ? cmd_val : "quiet devpath=hardware boot=live");

    (void)fw->SetPosition(fw, 0);
    UINTN wsz = strlen(out_buf);
    st = fw->Write(fw, &wsz, out_buf);
    fw->Flush(fw);
    fw->Close(fw);

    con_printf("[ok] successfully repaired '%s'\n", cfg_path);
}

/* Read interactive line */
static int read_command_line(char *buf, size_t max_len) {
    size_t idx = 0;
    buf[0] = '\0';

    while (1) {
        EFI_INPUT_KEY key;
        EFI_STATUS st = g_st->ConIn->ReadKeyStroke(g_st->ConIn, &key);
        if (!EFI_ERROR(st)) {
            /* Check ESC key -> returns 1 to signal exit to SiLo */
            if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B) {
                con_puts("\n");
                return 1;
            }
            if (key.UnicodeChar == (CHAR16)'\r' || key.UnicodeChar == (CHAR16)'\n') {
                con_puts("\n");
                buf[idx] = '\0';
                return 0;
            }
            if (key.UnicodeChar == (CHAR16)'\b') {
                if (idx > 0) {
                    idx--;
                    buf[idx] = '\0';
                    con_puts("\b \b");
                }
                continue;
            }
            if (key.UnicodeChar >= 32 && key.UnicodeChar < 127) {
                if (idx + 1 < max_len) {
                    char c = (char)key.UnicodeChar;
                    buf[idx++] = c;
                    buf[idx] = '\0';
                    char echo_s[2] = { c, '\0' };
                    con_puts(echo_s);
                }
            }
        }

        /* UART input fallback */
        if (inb(0x3F8 + 5) & 0x01) {
            uint8_t c = inb(0x3F8);
            if (c == 0x1B) {
                con_puts("\n");
                return 1;
            }
            if (c == '\r' || c == '\n') {
                con_puts("\n");
                buf[idx] = '\0';
                return 0;
            }
            if (c == 0x08 || c == 0x7F) {
                if (idx > 0) {
                    idx--;
                    buf[idx] = '\0';
                    con_puts("\b \b");
                }
                continue;
            }
            if (c >= 32 && c < 127) {
                if (idx + 1 < max_len) {
                    buf[idx++] = (char)c;
                    buf[idx] = '\0';
                    char echo_s[2] = { (char)c, '\0' };
                    con_puts(echo_s);
                }
            }
        }
    }
}

/* Main recovery shell loop */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    g_image_handle = ImageHandle;
    g_st = SystemTable;
    g_bs = SystemTable ? SystemTable->BootServices : NULL;

    if (!g_st || !g_bs) return EFI_UNSUPPORTED;

    discover_volumes();
    render_header();

    char line[LINE_BUF_SZ];
    while (1) {
        /* Print prompt: recovery@silo fs0:\ > */
        const char *vol_name = (g_num_volumes > g_cur_volume_idx) ? (char *)g_volumes[g_cur_volume_idx].name : "fs0:";
        con_printf("recovery@silo %s%s > ", vol_name, g_cwd);

        int cancelled = read_command_line(line, sizeof(line));
        if (cancelled) {
            /* User pressed ESC -> return to SiLo */
            return EFI_SUCCESS;
        }

        char *cmd = trim(line);
        if (cmd[0] == '\0') continue;

        /* Check volume switch: e.g. fs0:, fs1: */
        size_t clen = strlen(cmd);
        if (clen >= 4 && (cmd[0] == 'f' || cmd[0] == 'F') && (cmd[1] == 's' || cmd[1] == 'S') && cmd[clen - 1] == ':') {
            int v_num = atoi(cmd + 2);
            if (v_num >= 0 && (UINTN)v_num < g_num_volumes) {
                g_cur_volume_idx = (UINTN)v_num;
                strcpy(g_cwd, "\\");
                con_printf("Switched to volume %s\n", (char *)g_volumes[g_cur_volume_idx].name);
            } else {
                con_printf("error: unknown volume '%s' (available: 0..%lu)\n", cmd, (unsigned long)(g_num_volumes - 1));
            }
            continue;
        }

        /* Split command and arguments */
        char *arg = strchr(cmd, ' ');
        if (arg) {
            *arg = '\0';
            arg = trim(arg + 1);
        }

        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "cls") == 0 || strcmp(cmd, "clear") == 0) {
            render_header();
        } else if (strcmp(cmd, "bootcfg") == 0) {
            cmd_bootcfg(arg);
        } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
            cmd_ls(arg);
        } else if (strcmp(cmd, "cd") == 0) {
            cmd_cd(arg);
        } else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
            cmd_cat(arg);
        } else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "del") == 0) {
            cmd_rm(arg);
        } else if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "copy") == 0) {
            if (arg) {
                char *dst = strchr(arg, ' ');
                if (dst) {
                    *dst = '\0';
                    dst = trim(dst + 1);
                    cmd_cp(arg, dst);
                } else {
                    con_puts("usage: cp <src> <dst>\n");
                }
            } else {
                con_puts("usage: cp <src> <dst>\n");
            }
        } else if (strcmp(cmd, "mv") == 0 || strcmp(cmd, "ren") == 0) {
            if (arg) {
                char *dst = strchr(arg, ' ');
                if (dst) {
                    *dst = '\0';
                    dst = trim(dst + 1);
                    cmd_cp(arg, dst);
                    cmd_rm(arg);
                } else {
                    con_puts("usage: mv <src> <dst>\n");
                }
            } else {
                con_puts("usage: mv <src> <dst>\n");
            }
        } else if (strcmp(cmd, "echo") == 0) {
            if (arg) con_printf("%s\n", arg);
            else con_puts("\n");
        } else if (strcmp(cmd, "boot") == 0) {
            con_puts("Switching back to Simple Loader (SiLo) to boot system...\n");
            /* Return EFI_ALREADY_STARTED (or special code) to tell SiLo to retry booting */
            return EFI_ALREADY_STARTED;
        } else if (strcmp(cmd, "reboot") == 0) {
            con_puts("Rebooting system...\n");
            if (g_st->RuntimeServices && g_st->RuntimeServices->ResetSystem) {
                g_st->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
            }
        } else if (strcmp(cmd, "shutdown") == 0) {
            con_puts("Shutting down system...\n");
            if (g_st->RuntimeServices && g_st->RuntimeServices->ResetSystem) {
                g_st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
            }
        } else if (strcmp(cmd, "exit") == 0) {
            return EFI_SUCCESS;
        } else {
            con_printf("Unknown command '%s'. Type 'help' for available commands.\n", cmd);
        }
    }
}
