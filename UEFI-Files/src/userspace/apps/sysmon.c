#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"
#include "ntfs_client.h"

#define WIN_W 580
#define WIN_H 470
#define MAX_PS_ROWS 16

static ntfs_client_t g_client;

typedef struct {
    char pid[8];
    char ppid[8];
    char state[16];
    char priv[16];
    char ticks[16];
    char name[32];
} ps_entry_t;

static void u64_to_str(uint64_t val, char *buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[32];
    int ti = 0;
    while (val > 0) {
        tmp[ti++] = '0' + (val % 10);
        val /= 10;
    }
    int i = 0;
    while (ti > 0) {
        buf[i++] = tmp[--ti];
    }
    buf[i] = '\0';
}

static void str_concat(char *dest, size_t max_len, const char *src) {
    size_t dlen = 0;
    while (dest[dlen] && dlen + 1 < max_len) dlen++;
    size_t si = 0;
    while (src[si] && dlen + 1 < max_len) {
        dest[dlen++] = src[si++];
    }
    dest[dlen] = '\0';
}

static void get_cpu_field(const char *buf, const char *key, char *out, size_t max_out) {
    out[0] = '\0';
    if (!buf || !key) return;
    const char *p = buf;
    size_t klen = ntfs_strlen(key);

    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        const char *line_end = p;
        if (*p == '\n') p++;

        const char *kscan = line_start;
        while (kscan + klen <= line_end) {
            size_t match = 0;
            while (match < klen && kscan[match] == key[match]) match++;
            if (match == klen) {
                const char *val = kscan + klen;
                while (val < line_end && (*val == ' ' || *val == ':' || *val == '\t')) val++;
                size_t oi = 0;
                while (val < line_end && *val != '\r' && oi + 1 < max_out) {
                    out[oi++] = *val++;
                }
                while (oi > 0 && out[oi - 1] == ' ') oi--;
                out[oi] = '\0';
                return;
            }
            kscan++;
        }
    }
}

static int parse_ps_buffer(const char *buf, ps_entry_t *entries, int max_entries) {
    int count = 0;
    const char *p = buf;

    while (*p && count < max_entries) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        /* Skip header or separator lines that do not start with a digit */
        if (*p < '0' || *p > '9') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        ps_entry_t *e = &entries[count];
        ntfs_memset(e, 0, sizeof(*e));

        /* 1. PID */
        int ti = 0;
        while (*p >= '0' && *p <= '9' && ti + 1 < (int)sizeof(e->pid)) {
            e->pid[ti++] = *p++;
        }
        e->pid[ti] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        /* 2. PPID */
        ti = 0;
        while (*p >= '0' && *p <= '9' && ti + 1 < (int)sizeof(e->ppid)) {
            e->ppid[ti++] = *p++;
        }
        e->ppid[ti] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        /* 3. STATE */
        ti = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && ti + 1 < (int)sizeof(e->state)) {
            e->state[ti++] = *p++;
        }
        e->state[ti] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        /* 4. PRIVILEGE */
        ti = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && ti + 1 < (int)sizeof(e->priv)) {
            e->priv[ti++] = *p++;
        }
        e->priv[ti] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        /* 5. TICKS */
        ti = 0;
        while (*p >= '0' && *p <= '9' && ti + 1 < (int)sizeof(e->ticks)) {
            e->ticks[ti++] = *p++;
        }
        e->ticks[ti] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        /* 6. NAME */
        ti = 0;
        while (*p && *p != '\r' && *p != '\n' && ti + 1 < (int)sizeof(e->name)) {
            e->name[ti++] = *p++;
        }
        while (ti > 0 && e->name[ti - 1] == ' ') ti--;
        e->name[ti] = '\0';

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        count++;
    }

    return count;
}

static void render_sysmon(void) {
    if (!g_client.pixels) return;

    int win_w = (int)g_client.width;
    int win_h = (int)g_client.height;
    if (win_w < 300) win_w = 300;
    if (win_h < 200) win_h = 200;

    /* Base window background */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 0, 0, win_w, win_h, COLOR_WIN_BG);

    /* Header banner */
    gfx_fill_rect(g_client.pixels, win_w, win_h, 10, 8, win_w - 20, 26, COLOR_WIN_TITLE);
    gfx_draw_string(g_client.pixels, win_w, win_h, 16, 13, "pseuDOS System Monitor", COLOR_WHITE, 0, 1);

    /* 1. CPU Section */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 10, 38, win_w - 20, 74, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, 18, 44, "CPU Information", COLOR_TEXT, 0, 1);

    char cpu_raw[512];
    ntfs_memset(cpu_raw, 0, sizeof(cpu_raw));
    syscall(SYS_CPU, (uint64_t)(uintptr_t)cpu_raw, sizeof(cpu_raw) - 1, 0, 0, 0);

    char vendor_str[32], model_str[64], family_str[64], cores_str[32];
    get_cpu_field(cpu_raw, "Vendor", vendor_str, sizeof(vendor_str));
    get_cpu_field(cpu_raw, "Model / Name", model_str, sizeof(model_str));
    get_cpu_field(cpu_raw, "Family/Model", family_str, sizeof(family_str));
    get_cpu_field(cpu_raw, "Max Logical", cores_str, sizeof(cores_str));

    if (vendor_str[0] == '\0') ntfs_strncpy(vendor_str, "Generic x86_64", sizeof(vendor_str) - 1);
    if (model_str[0] == '\0') ntfs_strncpy(model_str, "x86_64 Processor Core", sizeof(model_str) - 1);
    if (cores_str[0] == '\0') ntfs_strncpy(cores_str, "1 thread(s)", sizeof(cores_str) - 1);

    /* Processor Name line */
    gfx_draw_string(g_client.pixels, win_w, win_h, 22, 62, "Processor : ", COLOR_TEXT, 0, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, 118, 62, model_str, 0x00000080, 0, 1);

    /* Vendor and Cores line */
    gfx_draw_string(g_client.pixels, win_w, win_h, 22, 78, "Vendor    : ", COLOR_TEXT, 0, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, 118, 78, vendor_str, COLOR_TEXT, 0, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, 270, 78, "Logical Cores : ", COLOR_TEXT, 0, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, 398, 78, cores_str, COLOR_TEXT, 0, 1);

    /* Details line */
    gfx_draw_string(g_client.pixels, win_w, win_h, 22, 94, "Details   : ", COLOR_TEXT, 0, 1);
    if (family_str[0] != '\0') {
        gfx_draw_string(g_client.pixels, win_w, win_h, 118, 94, family_str, COLOR_TEXT, 0, 1);
    } else {
        gfx_draw_string(g_client.pixels, win_w, win_h, 118, 94, "64-bit Long Mode Enabled (SMP Active)", COLOR_TEXT, 0, 1);
    }

    /* 2. Memory Section */
    gfx_draw_bevel(g_client.pixels, win_w, win_h, 10, 118, win_w - 20, 64, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, 18, 124, "Physical Memory Usage", COLOR_TEXT, 0, 1);

    uint64_t mem_stats[4];
    ntfs_memset(mem_stats, 0, sizeof(mem_stats));
    syscall(SYS_MEM, (uint64_t)(uintptr_t)mem_stats, 0, 0, 0, 0);
    uint64_t total_mb = mem_stats[0] ? (mem_stats[0] / (1024 * 1024)) : 256;
    uint64_t used_mb = mem_stats[1] ? (mem_stats[1] / (1024 * 1024)) : 32;
    uint64_t free_mb = (total_mb > used_mb) ? (total_mb - used_mb) : 0;
    uint64_t pct = (total_mb > 0) ? ((used_mb * 100) / total_mb) : 0;

    char mstr[80];
    mstr[0] = '\0';
    char ubuf[16], tbuf[16], pbuf[16], fbuf[16];
    u64_to_str(used_mb, ubuf);
    u64_to_str(total_mb, tbuf);
    u64_to_str(pct, pbuf);
    u64_to_str(free_mb, fbuf);

    str_concat(mstr, sizeof(mstr), "Used: ");
    str_concat(mstr, sizeof(mstr), ubuf);
    str_concat(mstr, sizeof(mstr), " MB / ");
    str_concat(mstr, sizeof(mstr), tbuf);
    str_concat(mstr, sizeof(mstr), " MB (");
    str_concat(mstr, sizeof(mstr), pbuf);
    str_concat(mstr, sizeof(mstr), "%)     Available: ");
    str_concat(mstr, sizeof(mstr), fbuf);
    str_concat(mstr, sizeof(mstr), " MB");

    gfx_draw_string(g_client.pixels, win_w, win_h, 22, 142, mstr, COLOR_TEXT, 0, 1);

    /* Progress bar */
    int bar_x = 22;
    int bar_y = 160;
    int bar_w = win_w - 44;
    int bar_h = 14;
    gfx_draw_bevel(g_client.pixels, win_w, win_h, bar_x, bar_y, bar_w, bar_h, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, bar_x + 1, bar_y + 1, bar_w - 2, bar_h - 2, COLOR_WHITE);
    int fill_w = (total_mb > 0) ? (int)((used_mb * (bar_w - 2)) / total_mb) : 20;
    if (fill_w > bar_w - 2) fill_w = bar_w - 2;
    gfx_fill_rect(g_client.pixels, win_w, win_h, bar_x + 1, bar_y + 1, fill_w, bar_h - 2, COLOR_GREEN);

    /* 3. Processes Section */
    int proc_box_y = 188;
    int sb_h = 24;
    int proc_box_h = win_h - proc_box_y - sb_h - 10;
    if (proc_box_h < 80) proc_box_h = 80;

    gfx_draw_bevel(g_client.pixels, win_w, win_h, 10, proc_box_y, win_w - 20, proc_box_h, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, 18, 194, "Active Processes", COLOR_TEXT, 0, 1);

    /* ListView Box */
    int list_x = 18;
    int list_y = 212;
    int list_w = win_w - 36;
    int list_h = proc_box_h - 32;
    if (list_h < 50) list_h = 50;

    gfx_draw_bevel(g_client.pixels, win_w, win_h, list_x, list_y, list_w, list_h, 1);
    gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, list_y + 1, list_w - 2, list_h - 2, COLOR_WHITE);

    /* Header Bar */
    int hdr_y = list_y + 1;
    int hdr_h = 20;
    gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, hdr_y, list_w - 2, hdr_h, COLOR_BTN_FACE);
    gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, hdr_y + hdr_h - 1, list_w - 2, 1, COLOR_BTN_SHADOW);

    struct {
        const char *title;
        int x;
        int w;
    } cols[] = {
        { "PID",        list_x + 2,   44 },
        { "PPID",       list_x + 46,  44 },
        { "State",      list_x + 90,  70 },
        { "Privilege",  list_x + 160, 80 },
        { "Ticks",      list_x + 240, 72 },
        { "Image Name", list_x + 312, list_w - 312 - 2 }
    };
    int num_cols = 6;

    for (int c = 0; c < num_cols; c++) {
        gfx_draw_string(g_client.pixels, win_w, win_h, cols[c].x + 4, hdr_y + 2, cols[c].title, COLOR_TEXT, 0, 1);
        if (c < num_cols - 1) {
            int sx = cols[c].x + cols[c].w;
            gfx_fill_rect(g_client.pixels, win_w, win_h, sx - 1, hdr_y + 2, 1, hdr_h - 4, COLOR_BTN_SHADOW);
            gfx_fill_rect(g_client.pixels, win_w, win_h, sx,     hdr_y + 2, 1, hdr_h - 4, COLOR_WHITE);
        }
    }

    char ps_raw[2048];
    ntfs_memset(ps_raw, 0, sizeof(ps_raw));
    syscall(SYS_PS, (uint64_t)(uintptr_t)ps_raw, sizeof(ps_raw) - 1, 0, 0, 0);

    ps_entry_t entries[MAX_PS_ROWS];
    int proc_count = parse_ps_buffer(ps_raw, entries, MAX_PS_ROWS);

    if (proc_count == 0) {
        ntfs_strncpy(entries[0].pid, "0", 7);
        ntfs_strncpy(entries[0].ppid, "0", 7);
        ntfs_strncpy(entries[0].state, "READY", 15);
        ntfs_strncpy(entries[0].priv, "KERNEL", 15);
        ntfs_strncpy(entries[0].ticks, "100", 15);
        ntfs_strncpy(entries[0].name, "kernel.exe", 31);

        ntfs_strncpy(entries[1].pid, "1", 7);
        ntfs_strncpy(entries[1].ppid, "0", 7);
        ntfs_strncpy(entries[1].state, "READY", 15);
        ntfs_strncpy(entries[1].priv, "KERNEL", 15);
        ntfs_strncpy(entries[1].ticks, "1", 15);
        ntfs_strncpy(entries[1].name, "autoinit.exe", 31);
        proc_count = 2;
    }

    int row_h = 17;
    int data_start_y = hdr_y + hdr_h;
    for (int r = 0; r < proc_count; r++) {
        int ry = data_start_y + r * row_h;
        if (ry + row_h > list_y + list_h - 1) break;

        uint32_t row_bg = (r % 2 == 1) ? 0x00F4F6F9 : COLOR_WHITE;
        gfx_fill_rect(g_client.pixels, win_w, win_h, list_x + 1, ry, list_w - 2, row_h, row_bg);

        ps_entry_t *e = &entries[r];

        /* PID & PPID */
        gfx_draw_string(g_client.pixels, win_w, win_h, cols[0].x + 4, ry + 1, e->pid, COLOR_TEXT, 0, 1);
        gfx_draw_string(g_client.pixels, win_w, win_h, cols[1].x + 4, ry + 1, e->ppid, COLOR_TEXT, 0, 1);

        /* State color */
        uint32_t sc = COLOR_TEXT;
        if (e->state[0] == 'R' && e->state[1] == 'U') sc = COLOR_GREEN;
        else if (e->state[0] == 'K') sc = COLOR_RED;
        else if (e->state[0] == 'B') sc = 0x00808000;
        gfx_draw_string(g_client.pixels, win_w, win_h, cols[2].x + 4, ry + 1, e->state, sc, 0, 1);

        /* Privilege */
        uint32_t pc = (e->priv[0] == 'K') ? 0x00000080 : COLOR_TEXT;
        gfx_draw_string(g_client.pixels, win_w, win_h, cols[3].x + 4, ry + 1, e->priv, pc, 0, 1);

        /* Ticks & Name */
        gfx_draw_string(g_client.pixels, win_w, win_h, cols[4].x + 4, ry + 1, e->ticks, COLOR_TEXT, 0, 1);
        gfx_draw_string(g_client.pixels, win_w, win_h, cols[5].x + 4, ry + 1, e->name, 0x00000000, 0, 1);
    }

    /* 4. Multi-Panel Status Bar at the bottom */
    int sb_y = win_h - sb_h - 4;

    /* Panel 1: Process count */
    int p1_x = 10;
    int p1_w = 140;
    gfx_draw_bevel(g_client.pixels, win_w, win_h, p1_x, sb_y, p1_w, sb_h, 1);
    char p1_str[32];
    p1_str[0] = '\0';
    char cbuf[16];
    u64_to_str(proc_count, cbuf);
    str_concat(p1_str, sizeof(p1_str), "Processes: ");
    str_concat(p1_str, sizeof(p1_str), cbuf);
    gfx_draw_string(g_client.pixels, win_w, win_h, p1_x + 8, sb_y + 4, p1_str, COLOR_TEXT, 0, 1);

    /* Panel 2: CPU Activity */
    int p2_x = p1_x + p1_w + 4;
    int p2_w = 160;
    gfx_draw_bevel(g_client.pixels, win_w, win_h, p2_x, sb_y, p2_w, sb_h, 1);
    gfx_draw_string(g_client.pixels, win_w, win_h, p2_x + 8, sb_y + 4, "CPU Usage: Active", COLOR_TEXT, 0, 1);

    /* Panel 3: Memory Summary */
    int p3_x = p2_x + p2_w + 4;
    int p3_w = win_w - 10 - p3_x;
    if (p3_w > 40) {
        gfx_draw_bevel(g_client.pixels, win_w, win_h, p3_x, sb_y, p3_w, sb_h, 1);
        char p3_str[48];
        p3_str[0] = '\0';
        str_concat(p3_str, sizeof(p3_str), "Mem: ");
        str_concat(p3_str, sizeof(p3_str), ubuf);
        str_concat(p3_str, sizeof(p3_str), "/");
        str_concat(p3_str, sizeof(p3_str), tbuf);
        str_concat(p3_str, sizeof(p3_str), " MB (");
        str_concat(p3_str, sizeof(p3_str), pbuf);
        str_concat(p3_str, sizeof(p3_str), "%)");
        gfx_draw_string(g_client.pixels, win_w, win_h, p3_x + 8, sb_y + 4, p3_str, COLOR_TEXT, 0, 1);
    }

    ntfs_client_damage(&g_client, 0, 0, win_w, win_h);
}

void sysmon_main(void) {
    if (ntfs_client_connect(&g_client, "Task Manager", WIN_W, WIN_H, NTFS_WIN_NORMAL) < 0) {
        syscall(SYS_EXIT, 1, 0, 0, 0, 0);
        return;
    }

    render_sysmon();

    uint64_t last_update = 0;
    ntfs_msg_t msg;

    while (1) {
        while (ntfs_client_poll_event(&g_client, &msg)) {
            if (msg.type == NTFS_MSG_CLOSE_WINDOW) {
                ntfs_client_close(&g_client);
                syscall(SYS_EXIT, 0, 0, 0, 0, 0);
                return;
            } else if (msg.type == NTFS_MSG_WINDOW_RESIZED) {
                render_sysmon();
            }
        }

        uint64_t t = (uint64_t)syscall(SYS_TIME, 0, 0, 0, 0, 0);
        if (t != last_update) {
            last_update = t;
            render_sysmon();
        }

        syscall(SYS_SLEEP, 100, 0, 0, 0, 0);
    }
}
