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
    /* 1. Query Boot Information to discover configured shell path */
    BootInfo bi;
    char shell_path[128] = "/protected/crit/xshss.bin";

    int64_t ret = syscall(SYS_GET_BOOTINFO, (uint64_t)(uintptr_t)&bi, 0, 0, 0, 0);
    if (ret == 0 && bi.shell_path[0] != '\0') {
        size_t i = 0, j = 0;
        while (bi.shell_path[i] != '\0' && j < sizeof(shell_path) - 1) {
            shell_path[j++] = (bi.shell_path[i] == '\\') ? '/' : bi.shell_path[i];
            i++;
        }
        shell_path[j] = '\0';
    }

    /* 2. Supervise Interactive Shell Subsystem */
    while (1) {
        int64_t child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)shell_path, 0, 0, 0, 0);
        if (child_pid <= 0) {
            child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/crit/xshss.bin", 0, 0, 0, 0);
            if (child_pid <= 0) {
                puts("[autoinit] CRITICAL: unable to launch shell. triggering kernel panic!\n");
                syscall(SYS_PANIC, (uint64_t)(uintptr_t)"autoinit: unable to execute shell subsystem", 0, 0, 0, 0);
                while (1) {
                    syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
                }
            }
        }

        puts("[autoinit] sucessfully spawned experimental shell subsystem, transitioning...\n");

        /* 3. Wait for child process termination */
        while (1) {
            int64_t waited = syscall(SYS_WAITPID, (uint64_t)child_pid, 0, 0, 0, 0);
            if (waited == child_pid || waited < 0) {
                break;
            }
            syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
        }

        puts("\n[autoinit] shell subsystem exited; respawning in 1 second...\n");
        syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
    }
}
