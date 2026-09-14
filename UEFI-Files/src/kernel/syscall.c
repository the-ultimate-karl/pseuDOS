#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "pe_loader.h"
#include "bootinfo.h"
#include "pit.h"
#include "rtc.h"
#include "klog.h"
#include "fs.h"
#include "drivers.h"
#include "lib.h"
#include "panic.h"
#include "kernel.h"

extern const BootInfo *g_boot_info_global;

/* MSR Register Constants */
#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_SFMASK  0xC0000084

#define EFER_SCE    (1ULL << 0)  /* System Call Extensions enable */

/* Global Stack Pointers used by syscall_entry.s */
uint64_t g_current_kernel_rsp = 0;
uint64_t g_syscall_user_rsp = 0;
uint64_t g_current_process_privilege = 1;
static volatile uint64_t g_shutdown_target_tick = 0;

void syscall_check_scheduled_shutdown(void) {
    if (g_shutdown_target_tick > 0 && pit_get_ticks() >= g_shutdown_target_tick) {
        g_shutdown_target_tick = 0;
        console_puts("\n[system] scheduled shutdown reached, powering off via ACPI...\n");
        acpi_shutdown();
    }
}

extern void syscall_entry(void);

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static int is_protected_path(const char *path) {
    if (!path) return 0;
    while (*path == ' ' || *path == '/' || *path == '\\') path++;
    if (strncasecmp(path, "efi", 3) == 0 && (path[3] == '\0' || path[3] == '/' || path[3] == '\\')) return 1;
    if (strncasecmp(path, "protected", 9) == 0 && (path[9] == '\0' || path[9] == '/' || path[9] == '\\')) return 1;
    return 0;
}

int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    process_t *curr = process_get_current();
    if (!curr) {
        return -ESRCH;
    }

    switch (num) {
        case SYS_GETPID:
            return (int64_t)curr->pid;

        case SYS_EXIT:
            process_exit((int)a1);
            return 0;

        case SYS_EXEC: {
            const char *path = (const char *)a1;
            if (!path) return -EFAULT;
            process_privilege_t priv = (curr->privilege_level == PRIV_KERNEL && a2 == 1) ? PRIV_KERNEL : PRIV_USER;
            process_t *proc = pe_spawn_process(NULL, path, priv);
            if (!proc) return -ENOENT;
            return (int64_t)proc->pid;
        }

        case SYS_WAITPID: {
            uint32_t target_pid = (uint32_t)a1;
            process_t *target = process_get_by_pid(target_pid);
            if (!target) return -ESRCH;
            if (target->state == PROCESS_STATE_KILLED || target->state == PROCESS_STATE_UNUSED) {
                return (int64_t)target_pid;
            }
            return 0;
        }

        case SYS_KILL: {
            uint32_t target_pid = (uint32_t)a1;
            if (target_pid != curr->pid && curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            return (int64_t)process_kill(target_pid);
        }

        case SYS_YIELD:
            scheduler_yield();
            return 0;

        case SYS_SLEEP:
            pit_sleep_ms((uint32_t)a1);
            return 0;

        case SYS_ELEVATE:
            curr->privilege_level = PRIV_KERNEL;
            g_current_process_privilege = 1;
            klog_info("Process %u '%s' elevated privileges to KERNEL", curr->pid, curr->name);
            return 0;

        case SYS_DROP_PRIVILEGES:
            curr->privilege_level = PRIV_USER;
            g_current_process_privilege = 0;
            klog_info("Process %u '%s' dropped privileges to USER", curr->pid, curr->name);
            return 0;

        case SYS_GET_PRIVILEGE:
            return (int64_t)curr->privilege_level;

        case SYS_WRITE: {
            int fd = (int)a1;
            const char *buf = (const char *)a2;
            size_t count = (size_t)a3;
            if (!buf) return -EFAULT;

            if (fd == 1 || fd == 2) {
                for (size_t i = 0; i < count; i++) {
                    console_putc(buf[i]);
                }
                return (int64_t)count;
            }
            return -EBADF;
        }

        case SYS_READ: {
            int fd = (int)a1;
            char *buf = (char *)a2;
            size_t count = (size_t)a3;
            if (!buf) return -EFAULT;

            if (fd == 0) {
                size_t read_bytes = 0;
                while (read_bytes < count) {
                    char c = keyboard_getchar();
                    buf[read_bytes++] = c;
                    if (c == '\n' || c == '\r') break;
                }
                return (int64_t)read_bytes;
            }
            return -EBADF;
        }

        case SYS_UNLINK: {
            const char *path = (const char *)a1;
            if (!path) return -EFAULT;
            vfs_node_t *node = vfs_find_node(path);
            if ((is_protected_path(path) || (node && node->is_protected)) && curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            int res = vfs_remove_node(path);
            return (res == 0) ? 0 : -ENOENT;
        }

        case SYS_MKDIR: {
            const char *path = (const char *)a1;
            if (!path) return -EFAULT;
            if (is_protected_path(path) && curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            vfs_node_t *node = vfs_mkdir(path);
            return node ? 0 : -EEXIST;
        }

        case SYS_CHDIR: {
            const char *path = (const char *)a1;
            if (!path) return -EFAULT;
            int res = vfs_chdir(path);
            if (res == 0) {
                strncpy(curr->cwd, vfs_getcwd(), sizeof(curr->cwd) - 1);
                return 0;
            }
            return -ENOENT;
        }

        case SYS_GETCWD: {
            char *buf = (char *)a1;
            size_t size = (size_t)a2;
            if (!buf || size == 0) return -EINVAL;
            strncpy(buf, vfs_getcwd(), size - 1);
            buf[size - 1] = '\0';
            return 0;
        }

        case SYS_TIME: {
            rtc_datetime_t *out = (rtc_datetime_t *)a1;
            if (!out) return -EFAULT;
            return (int64_t)rtc_get_datetime(out);
        }

        case SYS_DMESG:
            klog_dmesg();
            return 0;

        case SYS_PS:
            process_dump_list();
            return 0;

        case SYS_READDIR: {
            const char *path = (const char *)a1;
            vfs_listdir(path);
            return 0;
        }

        case SYS_REBOOT:
            acpi_reboot();
            return 0;

        case SYS_SHUTDOWN: {
            uint64_t delay_sec = (uint64_t)a1;
            if (delay_sec == (uint64_t)-1) {
                if (g_shutdown_target_tick == 0) {
                    console_puts("shutdown: no shutdown is currently scheduled\n");
                    return -1;
                }
                g_shutdown_target_tick = 0;
                console_puts("shutdown cancelled.\n");
                return 0;
            }

            if (delay_sec == 0) {
                g_shutdown_target_tick = 0;
                acpi_shutdown();
                return 0;
            }

            g_shutdown_target_tick = pit_get_ticks() + (delay_sec * 100);
            rtc_datetime_t dt;
            if (rtc_get_datetime(&dt) == 0) {
                uint8_t min = dt.minutes + (uint8_t)(delay_sec / 60);
                uint8_t hr = dt.hours;
                if (min >= 60) {
                    hr = (hr + (min / 60)) % 24;
                    min %= 60;
                }
                console_printf("shutdown scheduled for %02u:%02u:%02u (in %llu minute), use 'shutdown -c' to cancel.\n",
                               hr, min, dt.seconds, (unsigned long long)(delay_sec / 60));
            } else {
                console_printf("shutdown scheduled for in %llu seconds, use 'shutdown -c' to cancel.\n",
                               (unsigned long long)delay_sec);
            }
            return 0;
        }

        case SYS_STAT: {
            const char *path = (const char *)a1;
            vfs_stat_t *st = (vfs_stat_t *)a2;
            if (!path || !st) return -EFAULT;
            vfs_node_t *node = vfs_find_node(path);
            if (!node) return -ENOENT;
            st->size = (uint32_t)node->size;
            st->type = (uint32_t)node->type;
            st->is_protected = node->is_protected;
            strncpy(st->date_created, node->date_created, sizeof(st->date_created) - 1);
            strncpy(st->date_accessed, node->date_accessed, sizeof(st->date_accessed) - 1);
            strncpy(st->date_modified, node->date_modified, sizeof(st->date_modified) - 1);
            return 0;
        }

        case SYS_READFILE: {
            const char *path = (const char *)a1;
            char *buf = (char *)a2;
            size_t max_len = (size_t)a3;
            size_t offset = (size_t)a4;
            if (!path || !buf || max_len == 0) return -EFAULT;
            int res = vfs_read_file_offset(path, buf, max_len, offset);
            return (int64_t)res;
        }

        case SYS_WRITEFILE: {
            const char *path = (const char *)a1;
            const void *buf = (const void *)a2;
            size_t size = (size_t)a3;
            int append = (int)a4;
            if (!path || !buf) return -EFAULT;
            vfs_node_t *node = vfs_find_node(path);
            if ((is_protected_path(path) || (node && node->is_protected)) && curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            int res = vfs_write_file_bytes(path, buf, size, append);
            return (int64_t)res;
        }

        case SYS_GET_BOOTINFO: {
            BootInfo *out = (BootInfo *)a1;
            if (!out) return -EFAULT;
            if (g_boot_info_global) {
                memcpy(out, g_boot_info_global, sizeof(BootInfo));
                return 0;
            }
            return -ENODEV;
        }

        case SYS_FLASH: {
            if (curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            cmd_flash("");
            return 0;
        }

        case SYS_FS:
            cmd_fs((const char *)a1);
            return 0;

        case SYS_CPU:
            cpu_print_info();
            return 0;

        case SYS_MEM:
            cmd_mem();
            return 0;

        case SYS_PCI:
            pci_scan_bus();
            return 0;

        case SYS_DEVPATH:
            cmd_devpath((const char *)a1);
            return 0;

        case SYS_ATTACHED_DRIVES:
            cmd_attached_drives((const char *)a1);
            return 0;

        case SYS_SWITCH_TARGET:
            cmd_switch_target((const char *)a1);
            return 0;

        case SYS_SCREENRES:
            cmd_screenres((const char *)a1);
            return 0;

        case SYS_PROCTEST:
            cmd_proctest();
            return 0;

        case SYS_HALT:
            if (curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            cmd_halt();
            return 0;

        case SYS_CLEAR:
            console_clear();
            return 0;

        case SYS_GET_PROMPT_PATH: {
            char *buf = (char *)a1;
            size_t size = (size_t)a2;
            if (!buf || size == 0) return -EINVAL;
            fs_get_prompt_path(buf, size);
            return 0;
        }

        case SYS_UPTIME:
            return (int64_t)pit_get_ticks();

        case SYS_LISTDIR: {
            const char *path = (const char *)a1;
            char *buf = (char *)a2;
            size_t size = (size_t)a3;
            return (int64_t)vfs_listdir_names(path, buf, size);
        }

        case SYS_PANIC: {
            const char *reason = (const char *)a1;
            panic_context_t pctx;
            memset(&pctx, 0, sizeof(pctx));
            pctx.rsp = g_syscall_user_rsp;
            kernel_panic(reason ? reason : "userland requested kernel panic", &pctx);
            return 0;
        }

        default:
            klog_warn("Unknown syscall %llu requested by PID %u", (unsigned long long)num, curr->pid);
            return -ENOSYS;
    }
}

void syscall_init(void) {
    /* 1. Enable SCE (System Call Extensions) in IA32_EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    /* 2. Configure IA32_STAR:
     *    Bits [47:32] = Kernel CS selector (0x08), Kernel SS will be CS+8 (0x10)
     *    Bits [63:48] = User CS/SS base (0x10) so sysret loads SS=0x18|3 and CS=0x20|3
     */
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);

    /* 3. Configure IA32_LSTAR: Target RIP for syscall */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* 4. Configure IA32_SFMASK: Mask RFLAGS on syscall entry
     *    Clear IF (bit 9), TF (bit 8), DF (bit 10), AC (bit 18)
     */
    uint64_t sfmask = (1ULL << 9) | (1ULL << 8) | (1ULL << 10) | (1ULL << 18);
    wrmsr(MSR_SFMASK, sfmask);

    klog_info("Syscall MSRs configured (STAR, LSTAR=0x%016llX, SFMASK)", (unsigned long long)(uintptr_t)syscall_entry);
}
