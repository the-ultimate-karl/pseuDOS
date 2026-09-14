#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>
#include <stddef.h>

#define KLOG_ERR  0
#define KLOG_WARN 1
#define KLOG_INFO 2
#define KLOG_DBG  3
#define KLOG_KINF 4

/* Kernel Logging API */
void klog_init(void);
void klog(int level, const char *subsystem, const char *fmt, ...);
void boot_log(const char *status, const char *fmt, ...);

void klog_dmesg(void);
void klog_set_level(int level);
int  klog_get_level(void);

#define klog_err(fmt, ...)   klog(KLOG_ERR, "kernel", fmt, ##__VA_ARGS__)
#define klog_warn(fmt, ...)  klog(KLOG_WARN, "kernel", fmt, ##__VA_ARGS__)
#define klog_info(fmt, ...)  klog(KLOG_INFO, "kernel", fmt, ##__VA_ARGS__)
#define klog_dbg(fmt, ...)   klog(KLOG_DBG, "kernel", fmt, ##__VA_ARGS__)

#endif /* KLOG_H */
