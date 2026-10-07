#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "bootinfo.h"

static size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void puts(const char *str) {
    if (!str) return;
    syscall(SYS_WRITE, 1, (uint64_t)(uintptr_t)str, strlen(str), 0, 0);
}

void autoinit_main(void) {
    /* 0. Enforce single-instance invariant: autoinit must be PID 1 */
    int64_t my_pid = syscall(SYS_GETPID, 0, 0, 0, 0, 0);
    if (my_pid != 1) {
        puts("[autoinit] CRITICAL: Unexpected second instance of init process detected!\n");
        syscall(SYS_PANIC, (uint64_t)(uintptr_t)"unexpected second instance of init process detected!", 0, 0, 0, 0);
        while (1) {
            syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
        }
    }

    puts("[autoinit] init supervisor starting up...\n");

    /* 1. Query Boot Information to discover configured shell/session path */
    BootInfo bi;
    char target_path[128] = "/protected/gui/superglue.exe";

    int64_t ret = syscall(SYS_GET_BOOTINFO, (uint64_t)(uintptr_t)&bi, 0, 0, 0, 0);
    if (ret == 0 && bi.shell_path[0] != '\0') {
        size_t i = 0, j = 0;
        while (bi.shell_path[i] != '\0' && j < sizeof(target_path) - 1) {
            target_path[j++] = (bi.shell_path[i] == '\\') ? '/' : bi.shell_path[i];
            i++;
        }
        target_path[j] = '\0';
    }

    /* 2. Supervise session process */
    while (1) {
        int64_t child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)target_path, 1, 0, 0, 0);
        if (child_pid <= 0) {
            /* Try default superglue.exe if target_path was different */
            child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/superglue.exe", 1, 0, 0, 0);
            if (child_pid <= 0) {
                puts("[autoinit] WARNING: failed to spawn superglue.exe, falling back to xshss.exe\n");
                child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/crit/xshss.exe", 1, 0, 0, 0);
                if (child_pid <= 0) {
                    puts("[autoinit] CRITICAL: unable to spawn userland processes\n");
                    syscall(SYS_PANIC, (uint64_t)(uintptr_t)"unable to spawn userland processes", 0, 0, 0, 0);
                    while (1) {
                        syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
                    }
                }
            }
        }

        puts("[autoinit] successfully spawned userland session, supervising...\n");

        /* 3. Wait for child process termination */
        while (1) {
            int64_t waited = syscall(SYS_WAITPID, (uint64_t)child_pid, 0, 0, 0, 0);
            if (waited == child_pid || waited < 0) {
                break;
            }
            syscall(SYS_SLEEP, 100, 0, 0, 0, 0);
        }

        puts("\n[autoinit] session process exited; respawning in 1 second...\n");
        syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
    }
}
