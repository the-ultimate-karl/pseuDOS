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
#include "mice.h"
#include "ipc.h"
#include "shm.h"
#include "storage.h"
#include "pmm.h"

static char *g_syscall_flash_log = NULL;
static size_t g_syscall_flash_pos = 0;
static size_t g_syscall_flash_cap = 0;

static void flash_install_progress(const char *step_name, int is_ok) {
    if (console_get_fb_output()) {
        console_printf("flash: %s [%s]\n", step_name, is_ok ? "ok" : "failed");
    }
    if (g_syscall_flash_log && g_syscall_flash_pos + 128 < g_syscall_flash_cap) {
        const char *p = "flash: ";
        while (*p && g_syscall_flash_pos < g_syscall_flash_cap - 1) g_syscall_flash_log[g_syscall_flash_pos++] = *p++;
        p = step_name;
        while (*p && g_syscall_flash_pos < g_syscall_flash_cap - 1) g_syscall_flash_log[g_syscall_flash_pos++] = *p++;
        p = is_ok ? " [ok]\n" : " [failed]\n";
        while (*p && g_syscall_flash_pos < g_syscall_flash_cap - 1) g_syscall_flash_log[g_syscall_flash_pos++] = *p++;
        g_syscall_flash_log[g_syscall_flash_pos] = '\0';
    }
}

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
        console_puts("\n[system] scheduled shutdown reached, starting shutdown sequence...\n");
        system_shutdown_sequence(0);
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

static inline int validate_user_buffer(const void *ptr, size_t len, process_privilege_t priv) {
    if (priv == PRIV_KERNEL) return 1;
    if (len == 0) return 1;
    if (!ptr) return 0;
    uintptr_t addr = (uintptr_t)ptr;
    if (addr >= 0x0000800000000000ULL) return 0;
    if (addr + len < addr) return 0;
    if (addr + len > 0x0000800000000000ULL) return 0;
    return 1;
}

static inline int validate_user_str(const char *str, size_t max_len, process_privilege_t priv) {
    if (priv == PRIV_KERNEL) return 1;
    if (!str) return 0;
    uintptr_t addr = (uintptr_t)str;
    if (addr >= 0x0000800000000000ULL) return 0;
    for (size_t i = 0; i < max_len; i++) {
        if (addr + i >= 0x0000800000000000ULL) return 0;
        if (str[i] == '\0') return 1;
    }
    return 0;
}

static int64_t run_captured_void(void (*func)(void), char *user_buf, size_t cap, process_privilege_t priv) {
    if (user_buf && cap > 0) {
        if (!validate_user_buffer(user_buf, cap, priv)) return -EFAULT;
        console_set_capture_buffer(user_buf, cap);
        func();
        size_t len = console_get_capture_length();
        console_set_capture_buffer(NULL, 0);
        return (int64_t)len;
    }
    func();
    return 0;
}

static int64_t run_captured_str(void (*func)(const char *), const char *arg, char *user_buf, size_t cap, process_privilege_t priv) {
    if (user_buf && cap > 0) {
        if (!validate_user_buffer(user_buf, cap, priv)) return -EFAULT;
        console_set_capture_buffer(user_buf, cap);
        func(arg ? arg : "");
        size_t len = console_get_capture_length();
        console_set_capture_buffer(NULL, 0);
        return (int64_t)len;
    }
    func(arg ? arg : "");
    return 0;
}

int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a5;
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
            if (!validate_user_str(path, 256, curr->privilege_level)) return -EFAULT;
            if (strstr(path, "autoinit") != NULL) {
                if (process_has_active_init()) {
                    kernel_panic("unexpected second instance of init process detected!", NULL);
                }
            }
            process_privilege_t priv = (curr->privilege_level == PRIV_KERNEL && a2 == 1) ? PRIV_KERNEL : PRIV_USER;
            process_t *proc = pe_spawn_process(NULL, path, priv);
            if (!proc) {
                int err = pe_get_last_error();
                return (err < 0) ? (int64_t)err : -ENOENT;
            }
            if (a2 != 0 && a2 != 1) {
                const char *arg = (const char *)a2;
                if (validate_user_str(arg, 256, curr->privilege_level)) {
                    strncpy(proc->cmdline, arg, sizeof(proc->cmdline) - 1);
                    proc->cmdline[sizeof(proc->cmdline) - 1] = '\0';
                }
            }
            if (a3 != 0) {
                proc->tty = (int)a3;
            }
            return (int64_t)proc->pid;
        }

        case SYS_WAITPID: {
            uint32_t target_pid = (uint32_t)a1;
            process_t *target = process_get_by_pid(target_pid);
            if (!target) return -ESRCH;
            if (target->state == PROCESS_STATE_KILLED) {
                process_free_resources(target);
                return (int64_t)target_pid;
            }
            if (target->state == PROCESS_STATE_UNUSED) {
                return (int64_t)target_pid;
            }
            return 0;
        }

        case SYS_KILL: {
            uint32_t target_pid = (uint32_t)a1;
            if (target_pid < 3) {
                kernel_panic("Attempted to kill critical processes!", NULL);
            }
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
            if (!validate_user_buffer(buf, count, curr->privilege_level)) return -EFAULT;

            if (fd == 1 || fd == 2) {
                for (size_t i = 0; i < count; i++) {
                    console_putc(buf[i]);
                }
                return (int64_t)count;
            }
            if (fd >= SOCKET_FD_BASE) {
                return sys_send(fd, buf, count, 0);
            }
            return -EBADF;
        }

        case SYS_READ: {
            int fd = (int)a1;
            char *buf = (char *)a2;
            size_t count = (size_t)a3;
            int flags = (int)a4;
            if (!validate_user_buffer(buf, count, curr->privilege_level)) return -EFAULT;

            if (fd == 0) {
                if (count == 0) return 0;
                if (curr->tty != 0 && curr->tty != tty_get_active() && !console_get_fb_output()) {
                    if (flags & MSG_DONTWAIT) return -EAGAIN;
                    while (curr->tty != tty_get_active() && !console_get_fb_output()) {
                        pit_sleep_ms(20);
                    }
                }
                if (flags & MSG_DONTWAIT) {
                    char c = keyboard_getchar_nonblock();
                    if (c == 0) return -EAGAIN;
                    buf[0] = c;
                    return 1;
                }
                size_t read_bytes = 0;
                while (read_bytes < count) {
                    char c = keyboard_getchar();
                    buf[read_bytes++] = c;
                    if (c == '\n' || c == '\r') break;
                }
                return (int64_t)read_bytes;
            }
            if (fd >= SOCKET_FD_BASE) {
                return sys_recv(fd, buf, count, flags);
            }
            return -EBADF;
        }

        case SYS_CLOSE: {
            int fd = (int)a1;
            if (fd >= SOCKET_FD_BASE) {
                return (int64_t)sys_close_socket(fd);
            }
            return 0;
        }

        case SYS_UNLINK: {
            const char *path = (const char *)a1;
            uint64_t flags = a2;
            if (!validate_user_str(path, 256, curr->privilege_level)) return -EFAULT;
            vfs_node_t *node = vfs_find_node(path);
            if ((is_protected_path(path) || (node && node->is_protected)) && curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            int recursive = (flags & 1) ? 1 : 0;
            int force = (flags & 2) ? 1 : 0;
            int res = vfs_remove_node_ex(path, recursive, force);
            if (res == -1) return -ENOENT;
            if (res == -2) return -ENOTEMPTY;
            if (res == -3) return -EINVAL;
            if (res == -4) return -EPERM;
            if (res == -5) return -EIO;
            return (res == 0) ? 0 : -ENOENT;
        }

        case SYS_MKDIR: {
            const char *path = (const char *)a1;
            if (!validate_user_str(path, 256, curr->privilege_level)) return -EFAULT;
            if (is_protected_path(path) && curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            vfs_node_t *node = vfs_mkdir(path);
            return node ? 0 : -EEXIST;
        }

        case SYS_CHDIR: {
            const char *path = (const char *)a1;
            if (!validate_user_str(path, 256, curr->privilege_level)) return -EFAULT;
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
            if (!validate_user_buffer(buf, size, curr->privilege_level) || size == 0) return -EINVAL;
            strncpy(buf, vfs_getcwd(), size - 1);
            buf[size - 1] = '\0';
            return 0;
        }

        case SYS_TIME: {
            if (a1 == 0) {
                rtc_datetime_t dt;
                if (rtc_get_datetime(&dt) == 0) {
                    return (int64_t)((uint64_t)dt.hours * 3600 + (uint64_t)dt.minutes * 60 + (uint64_t)dt.seconds);
                }
                return 0;
            }
            rtc_datetime_t *out = (rtc_datetime_t *)a1;
            if (!validate_user_buffer(out, sizeof(rtc_datetime_t), curr->privilege_level)) return -EFAULT;
            return (int64_t)rtc_get_datetime(out);
        }

        case SYS_DMESG:
            return run_captured_void(klog_dmesg, (char *)a1, (size_t)a2, curr->privilege_level);

        case SYS_PS:
            return run_captured_void(process_dump_list, (char *)a1, (size_t)a2, curr->privilege_level);

        case SYS_READDIR: {
            const char *path = (const char *)a1;
            if (!validate_user_str(path, 256, curr->privilege_level)) return -EFAULT;
            vfs_listdir(path);
            return 0;
        }

        case SYS_REBOOT:
            system_shutdown_sequence(1);
            return 0;

        case SYS_SHUTDOWN: {
            uint64_t delay_sec = (uint64_t)a1;
            char *user_buf = (char *)a2;
            size_t cap = (size_t)a3;
            if (user_buf && cap > 0) {
                if (!validate_user_buffer(user_buf, cap, curr->privilege_level)) return -EFAULT;
                console_set_capture_buffer(user_buf, cap);
            }
            int64_t ret = 0;
            if (delay_sec == (uint64_t)-1) {
                if (g_shutdown_target_tick == 0) {
                    console_puts("shutdown: no shutdown is currently scheduled\n");
                    ret = -1;
                } else {
                    g_shutdown_target_tick = 0;
                    console_puts("shutdown cancelled.\n");
                    ret = 0;
                }
            } else if (delay_sec == 0) {
                g_shutdown_target_tick = 0;
                if (user_buf && cap > 0) {
                    console_set_capture_buffer(NULL, 0);
                }
                system_shutdown_sequence(0);
                return 0;
            } else {
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
                ret = 0;
            }
            if (user_buf && cap > 0) {
                console_set_capture_buffer(NULL, 0);
            }
            return ret;
        }

        case SYS_STAT: {
            const char *path = (const char *)a1;
            vfs_stat_t *st = (vfs_stat_t *)a2;
            if (!validate_user_str(path, 256, curr->privilege_level) ||
                !validate_user_buffer(st, sizeof(vfs_stat_t), curr->privilege_level)) return -EFAULT;
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
            if (!validate_user_str(path, 256, curr->privilege_level) ||
                !validate_user_buffer(buf, max_len, curr->privilege_level) || max_len == 0) return -EFAULT;
            int res = vfs_read_file_offset(path, buf, max_len, offset);
            return (int64_t)res;
        }

        case SYS_WRITEFILE: {
            const char *path = (const char *)a1;
            const void *buf = (const void *)a2;
            size_t size = (size_t)a3;
            int append = (int)a4;
            if (!validate_user_str(path, 256, curr->privilege_level) ||
                !validate_user_buffer(buf, size, curr->privilege_level)) return -EFAULT;
            vfs_node_t *node = vfs_find_node(path);
            if ((is_protected_path(path) || (node && node->is_protected)) && curr->privilege_level != PRIV_KERNEL) {
                return -EPERM;
            }
            int res = vfs_write_file_bytes(path, buf, size, append);
            return (int64_t)res;
        }

        case SYS_GET_BOOTINFO: {
            BootInfo *out = (BootInfo *)a1;
            if (!validate_user_buffer(out, sizeof(BootInfo), curr->privilege_level)) return -EFAULT;
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
            uint64_t op = a1;
            if (op == 0) {
                /* Legacy fallback: run interactive cmd_flash directly */
                int prev_tty = tty_get_active();
                if (prev_tty != 3) {
                    tty_switch(3);
                }
                __asm__ volatile ("sti");
                cmd_flash("");
                __asm__ volatile ("cli");
                if (prev_tty != 3) {
                    tty_switch(prev_tty);
                }
                return 0;
            }
            if (op == FLASH_OP_GET_COUNT) {
                return (int64_t)storage_get_device_count();
            }
            if (op == FLASH_OP_GET_DEVICE) {
                uint32_t idx = (uint32_t)a2;
                flash_dev_info_t *out = (flash_dev_info_t *)a3;
                if (!out) return -EINVAL;
                StorageDevice *dev = storage_get_device(idx);
                if (!dev) return -ENODEV;
                flash_dev_info_t kinfo;
                memset(&kinfo, 0, sizeof(kinfo));
                strncpy(kinfo.name, dev->name, sizeof(kinfo.name) - 1);
                strncpy(kinfo.type_str, dev->type_str, sizeof(kinfo.type_str) - 1);
                strncpy(kinfo.bus_speed, dev->bus_speed, sizeof(kinfo.bus_speed) - 1);
                strncpy(kinfo.size_str, dev->size_str, sizeof(kinfo.size_str) - 1);
                strncpy(kinfo.devpath, dev->devpath, sizeof(kinfo.devpath) - 1);
                kinfo.type = (uint32_t)dev->type;
                kinfo.usb_version = dev->usb_version;
                kinfo.raw_index = idx;
                kinfo.total_sectors = dev->total_sectors;
                memcpy(out, &kinfo, sizeof(flash_dev_info_t));
                return 0;
            }
            if (op == FLASH_OP_INSTALL) {
                uint32_t idx = (uint32_t)a2;
                StorageDevice *dev = storage_get_device(idx);
                if (!dev) return -ENODEV;
                char *log_buf = (char *)a3;
                size_t log_cap = (size_t)a4;
                g_syscall_flash_log = log_buf;
                g_syscall_flash_pos = 0;
                g_syscall_flash_cap = log_cap;
                if (g_syscall_flash_log && g_syscall_flash_cap > 0) {
                    g_syscall_flash_log[0] = '\0';
                }
                int res = gpt_fat32_format_and_install(dev, flash_install_progress);
                g_syscall_flash_log = NULL;
                return (res == 0) ? 0 : -EIO;
            }
            return -EINVAL;
        }

        case SYS_FS:
            return run_captured_str(cmd_fs, (const char *)a1, (char *)a2, (size_t)a3, curr->privilege_level);

        case SYS_CPU:
            return run_captured_void(cpu_print_info, (char *)a1, (size_t)a2, curr->privilege_level);

        case SYS_MEM: {
            if (a2 == 0) {
                uint64_t *out = (uint64_t *)a1;
                if (!validate_user_buffer(out, 4 * sizeof(uint64_t), curr->privilege_level)) return -EFAULT;
                uint64_t total_bytes = (uint64_t)pmm_get_total_pages() * 4096ULL;
                uint64_t free_bytes = (uint64_t)pmm_get_free_pages() * 4096ULL;
                uint64_t used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;
                out[0] = total_bytes;
                out[1] = used_bytes;
                out[2] = free_bytes;
                out[3] = (uint64_t)heap_get_used();
                return 0;
            }
            return run_captured_void(cmd_mem, (char *)a1, (size_t)a2, curr->privilege_level);
        }

        case SYS_PCI:
            return run_captured_void(pci_scan_bus, (char *)a1, (size_t)a2, curr->privilege_level);

        case SYS_DEVPATH:
            return run_captured_str(cmd_devpath, (const char *)a1, (char *)a2, (size_t)a3, curr->privilege_level);

        case SYS_ATTACHED_DRIVES:
            return run_captured_str(cmd_attached_drives, (const char *)a1, (char *)a2, (size_t)a3, curr->privilege_level);

        case SYS_SWITCH_TARGET:
            return run_captured_str(cmd_switch_target, (const char *)a1, (char *)a2, (size_t)a3, curr->privilege_level);

        case SYS_SCREENRES:
            return run_captured_str(cmd_screenres, (const char *)a1, (char *)a2, (size_t)a3, curr->privilege_level);

        case SYS_PROCTEST:
            return run_captured_void(cmd_proctest, (char *)a1, (size_t)a2, curr->privilege_level);

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
            if (!validate_user_buffer(buf, size, curr->privilege_level) || size == 0) return -EINVAL;
            fs_get_prompt_path(buf, size);
            return 0;
        }

        case SYS_UPTIME:
            return (int64_t)pit_get_ticks();

        case SYS_LISTDIR: {
            const char *path = (const char *)a1;
            char *buf = (char *)a2;
            size_t size = (size_t)a3;
            if (!validate_user_str(path, 256, curr->privilege_level) ||
                !validate_user_buffer(buf, size, curr->privilege_level)) return -EFAULT;
            return (int64_t)vfs_listdir_names(path, buf, size);
        }

        case SYS_GET_ARGS: {
            char *buf = (char *)a1;
            size_t size = (size_t)a2;
            if (!validate_user_buffer(buf, size, curr->privilege_level) || size == 0) return -EFAULT;
            size_t len = strlen(curr->cmdline);
            if (len >= size) len = size - 1;
            memcpy(buf, curr->cmdline, len);
            buf[len] = '\0';
            return (int64_t)len;
        }

        case SYS_GET_MOUSE_EVENT: {
            mouse_event_t *ev = (mouse_event_t *)a1;
            if (!validate_user_buffer(ev, sizeof(mouse_event_t), curr->privilege_level)) return -EFAULT;
            return (int64_t)mice_get_event(ev);
        }

        case SYS_GET_MOUSE_STATE: {
            mouse_state_t *st = (mouse_state_t *)a1;
            if (!validate_user_buffer(st, sizeof(mouse_state_t), curr->privilege_level)) return -EFAULT;
            mice_get_state(st);
            return 0;
        }

        case SYS_SOCKET:
            return (int64_t)sys_socket((int)a1, (int)a2, (int)a3);

        case SYS_BIND:
            if (!validate_user_buffer((const void *)a2, sizeof(sockaddr_un_t), curr->privilege_level)) return -EFAULT;
            return (int64_t)sys_bind((int)a1, (const sockaddr_un_t *)a2, (size_t)a3);

        case SYS_CONNECT:
            if (!validate_user_buffer((const void *)a2, sizeof(sockaddr_un_t), curr->privilege_level)) return -EFAULT;
            return (int64_t)sys_connect((int)a1, (const sockaddr_un_t *)a2, (size_t)a3);

        case SYS_LISTEN:
            return (int64_t)sys_listen((int)a1, (int)a2);

        case SYS_ACCEPT:
            if (a2 && !validate_user_buffer((void *)a2, sizeof(sockaddr_un_t), curr->privilege_level)) return -EFAULT;
            if (a3 && !validate_user_buffer((void *)a3, sizeof(size_t), curr->privilege_level)) return -EFAULT;
            return (int64_t)sys_accept((int)a1, (sockaddr_un_t *)a2, (size_t *)a3);

        case SYS_SEND:
            if (!validate_user_buffer((const void *)a2, (size_t)a3, curr->privilege_level)) return -EFAULT;
            return sys_send((int)a1, (const void *)a2, (size_t)a3, (int)a4);

        case SYS_RECV:
            if (!validate_user_buffer((void *)a2, (size_t)a3, curr->privilege_level)) return -EFAULT;
            return sys_recv((int)a1, (void *)a2, (size_t)a3, (int)a4);

        case SYS_POLL:
            if (a2 > 0 && !validate_user_buffer((void *)a1, (size_t)a2 * sizeof(pollfd_t), curr->privilege_level)) return -EFAULT;
            return (int64_t)sys_poll((pollfd_t *)a1, (size_t)a2, (int)a3);

        case SYS_SHM_CREATE:
            if (!validate_user_str((const char *)a1, SHM_NAME_MAX, curr->privilege_level)) return -EFAULT;
            return (int64_t)sys_shm_create((const char *)a1, (size_t)a2);

        case SYS_SHM_MAP:
            return (int64_t)(uintptr_t)sys_shm_map((int)a1, (void *)a2, (int)a3);

        case SYS_SHM_UNMAP:
            return (int64_t)sys_shm_unmap((void *)a1);

        case SYS_SHM_CLOSE:
            return (int64_t)sys_shm_close((int)a1);

        case SYS_TTY_GET:
            return (int64_t)tty_get_active();

        case SYS_TTY_SET:
            curr->tty = (int)a1;
            return 0;

        case SYS_MOUNT: {
            const char *src = (const char *)a1;
            const char *tgt = (const char *)a2;
            const char *fstype = (const char *)a3;
            char *log_buf = (char *)a4;
            size_t log_sz = (size_t)a5;
            if (src && !validate_user_str(src, 128, curr->privilege_level)) return -EFAULT;
            if (tgt && !validate_user_str(tgt, 128, curr->privilege_level)) return -EFAULT;
            if (fstype && !validate_user_str(fstype, 32, curr->privilege_level)) return -EFAULT;
            if (log_buf && !validate_user_buffer(log_buf, log_sz, curr->privilege_level)) return -EFAULT;
            if (!src && !tgt) {
                return (int64_t)vfs_list_mounts(log_buf, log_sz);
            }
            if (src && strcmp(src, "-a") == 0) {
                return (int64_t)vfs_auto_mount_all(log_buf, log_sz);
            }
            return (int64_t)vfs_mount_device(src, tgt, fstype, log_buf, log_sz);
        }

        case SYS_UMOUNT: {
            const char *tgt = (const char *)a1;
            char *log_buf = (char *)a2;
            size_t log_sz = (size_t)a3;
            if (!validate_user_str(tgt, 128, curr->privilege_level)) return -EFAULT;
            if (log_buf && !validate_user_buffer(log_buf, log_sz, curr->privilege_level)) return -EFAULT;
            return (int64_t)vfs_umount_target(tgt, log_buf, log_sz);
        }

        case SYS_PANIC: {
            const char *reason = (const char *)a1;
            int is_init_panic = (reason && validate_user_str(reason, 256, curr->privilege_level) &&
                                 strstr(reason, "unexpected second instance") != NULL);
            if (curr->privilege_level != PRIV_KERNEL && !is_init_panic) {
                return -EPERM;
            }
            if (!reason || !validate_user_str(reason, 256, curr->privilege_level)) {
                reason = "privileged requested kernel panic";
            }
            panic_context_t pctx;
            memset(&pctx, 0, sizeof(pctx));
            pctx.rsp = g_syscall_user_rsp;
            kernel_panic(reason ? reason : "privileged requested kernel panic", &pctx);
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
