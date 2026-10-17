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

static char *strstr_local(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && (*h == *n)) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
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
    char target_path[128] = "/protected/gui/gman.exe";
    int is_cli_mode = 0;

    int64_t ret = syscall(SYS_GET_BOOTINFO, (uint64_t)(uintptr_t)&bi, 0, 0, 0, 0);
    if (ret == 0) {
        if (strstr_local(bi.cmdline, "ui=cli") != NULL ||
            strstr_local(bi.cmdline, "cli") != NULL ||
            strstr_local(bi.cmdline, "nogui") != NULL ||
            strstr_local(bi.cmdline, "ui=text") != NULL) {
            is_cli_mode = 1;
        }

        if (is_cli_mode) {
            target_path[0] = '\0';
            const char *cli_path = "/protected/crit/xshss.exe";
            size_t k = 0;
            while (cli_path[k] && k < sizeof(target_path) - 1) {
                target_path[k] = cli_path[k];
                k++;
            }
            target_path[k] = '\0';
            puts("[autoinit] launch parameter 'ui=cli' active: booting into console shell (bypassing GUI)\n");
        } else if (bi.shell_path[0] != '\0') {
            size_t i = 0, j = 0;
            while (bi.shell_path[i] != '\0' && j < sizeof(target_path) - 1) {
                target_path[j++] = (bi.shell_path[i] == '\\') ? '/' : bi.shell_path[i];
                i++;
            }
            target_path[j] = '\0';
        }
    }

    /* 2. Supervise session process */
    while (1) {
        int64_t child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)target_path, 1, 0, 0, 0);
        if (child_pid <= 0) {
            if (is_cli_mode) {
                puts("[autoinit] WARNING: failed to spawn xshss.exe, trying /protected/apps/shell.exe\n");
                child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/shell.exe", 1, 0, 0, 0);
            } else {
                /* Try default gman.exe if target_path was different */
                child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/gman.exe", 1, 0, 0, 0);
                if (child_pid <= 0) {
                    child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/superglue.exe", 1, 0, 0, 0);
                }
                if (child_pid <= 0) {
                    puts("[autoinit] WARNING: failed to spawn gman.exe, falling back to xshss.exe\n");
                    child_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/crit/xshss.exe", 1, 0, 0, 0);
                }
            }
            if (child_pid <= 0) {
                puts("[autoinit] CRITICAL: unable to spawn userland processes\n");
                syscall(SYS_PANIC, (uint64_t)(uintptr_t)"unable to spawn userland processes", 0, 0, 0, 0);
                while (1) {
                    syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
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
