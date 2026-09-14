#include "klog.h"
#include "lib.h"
#include "drivers.h"

#define KLOG_BUFFER_SIZE 512
#define KLOG_LINE_SIZE   160

static char g_klog_buffer[KLOG_BUFFER_SIZE][KLOG_LINE_SIZE];
static size_t g_klog_head = 0;
static size_t g_klog_count = 0;

static int g_klog_level = KLOG_INFO;
static uint64_t g_boot_tsc = 0;
static uint64_t g_tsc_per_ms = 2000000ULL; /* ~2.0 GHz default */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void klog_init(void) {
    g_boot_tsc = rdtsc();
    g_klog_head = 0;
    g_klog_count = 0;
    g_klog_level = KLOG_INFO;
    memset(g_klog_buffer, 0, sizeof(g_klog_buffer));
}

static void get_timestamp_str(char *buf, size_t size) {
    uint64_t now = rdtsc();
    uint64_t elapsed = (now >= g_boot_tsc) ? (now - g_boot_tsc) : 0;
    uint64_t total_ms = elapsed / g_tsc_per_ms;
    uint32_t sec = (uint32_t)(total_ms / 1000);
    uint32_t ms  = (uint32_t)(total_ms % 1000);

    snprintf(buf, size, "[%4u.%03u]", sec, ms);
}

static void klog_buffer_append(const char *line) {
    strncpy(g_klog_buffer[g_klog_head], line, KLOG_LINE_SIZE - 1);
    g_klog_buffer[g_klog_head][KLOG_LINE_SIZE - 1] = '\0';

    g_klog_head = (g_klog_head + 1) % KLOG_BUFFER_SIZE;
    if (g_klog_count < KLOG_BUFFER_SIZE) {
        g_klog_count++;
    }
}

void klog(int level, const char *subsystem, const char *fmt, ...) {
    char time_str[32];
    get_timestamp_str(time_str, sizeof(time_str));

    const char *tag = "INFO";
    switch (level) {
        case KLOG_ERR:  tag = "ERR "; break;
        case KLOG_WARN: tag = "WARN"; break;
        case KLOG_INFO: tag = "INFO"; break;
        case KLOG_DBG:  tag = "DBG "; break;
        case KLOG_KINF: tag = "KINF"; break;
        default: break;
    }

    char msg_buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    char line_buf[KLOG_LINE_SIZE];
    snprintf(line_buf, sizeof(line_buf), "%s [%s] [%s] %s",
        time_str, tag, subsystem ? subsystem : "kernel", msg_buf);

    klog_buffer_append(line_buf);

    if (level <= g_klog_level) {
        console_puts(line_buf);
        console_putc('\n');
    }
}

void boot_log(const char *status, const char *fmt, ...) {
    char time_str[32];
    get_timestamp_str(time_str, sizeof(time_str));

    char msg_buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    char line_buf[KLOG_LINE_SIZE];
    snprintf(line_buf, sizeof(line_buf), "%s [%-4s] %s",
        time_str, status ? status : "OK", msg_buf);

    klog_buffer_append(line_buf);

    console_puts(line_buf);
    console_putc('\n');
}

void klog_dmesg(void) {
    if (g_klog_count == 0) {
        console_puts("dmesg: kernel log buffer is empty\n");
        return;
    }

    size_t start_idx = (g_klog_count < KLOG_BUFFER_SIZE) ? 0 : g_klog_head;

    for (size_t i = 0; i < g_klog_count; i++) {
        size_t idx = (start_idx + i) % KLOG_BUFFER_SIZE;
        console_puts(g_klog_buffer[idx]);
        console_putc('\n');
    }
}

void klog_set_level(int level) {
    if (level < KLOG_ERR) level = KLOG_ERR;
    if (level > KLOG_KINF) level = KLOG_KINF;
    g_klog_level = level;
}

int klog_get_level(void) {
    return g_klog_level;
}
