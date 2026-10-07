#include <stdint.h>
#include <stddef.h>
#include "syscall.h"

static size_t strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void puts(const char *str) {
    if (!str) return;
    syscall(SYS_WRITE, 1, (uint64_t)(uintptr_t)str, strlen(str), 0, 0);
}

void superglue_main(void) {
    puts("[superglue] session orchestrator initializing pseuDOS desktop environment...\n");

    /* 1. Spawn ntfs.exe (Display Server) */
    puts("[superglue] launching /protected/gui/ntfs.exe...\n");
    int64_t ntfs_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/ntfs.exe", 0, 0, 0, 0);
    if (ntfs_pid <= 0) {
        puts("[superglue] FATAL: failed to spawn ntfs.exe\n");
        syscall(SYS_PANIC, (uint64_t)(uintptr_t)"unable to start display server", 0, 0, 0, 0);
        while (1) syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
    }
    syscall(SYS_SLEEP, 100, 0, 0, 0, 0);

    /* 2. Spawn lack.exe (Compositor) */
    puts("[superglue] launching /protected/gui/lack.exe...\n");
    int64_t lack_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/lack.exe", 0, 0, 0, 0);
    if (lack_pid <= 0) {
        puts("[superglue] WARNING: failed to spawn lack.exe; continuing in uncomposited mode...\n");
    }
    syscall(SYS_SLEEP, 100, 0, 0, 0, 0);

    /* 3. Spawn ninds.exe (Window Manager) */
    puts("[superglue] launching /protected/gui/ninds.exe...\n");
    int64_t ninds_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/ninds.exe", 0, 0, 0, 0);
    if (ninds_pid <= 0) {
        puts("[superglue] FATAL: failed to spawn ninds.exe\n");
        syscall(SYS_PANIC, (uint64_t)(uintptr_t)"unable to start window manager", 0, 0, 0, 0);
        while (1) syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
    }
    syscall(SYS_SLEEP, 100, 0, 0, 0, 0);

    /* 4. Spawn splash.exe (Loading Screen) */
    puts("[superglue] launching /protected/gui/splash.exe...\n");
    int64_t splash_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/splash.exe", 0, 0, 0, 0);
    if (splash_pid <= 0) {
        puts("[superglue] WARNING: failed to spawn splash.exe\n");
    }
    /* Allow loading screen to connect and present initial frame */
    syscall(SYS_SLEEP, 1200, 0, 0, 0, 0);

    /* 5. Spawn gshss.exe (Graphical Shell Subsystem) */
    puts("[superglue] launching /protected/gui/gshss.exe...\n");
    int64_t gshss_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/gshss.exe", 0, 0, 0, 0);
    if (gshss_pid <= 0) {
        puts("[superglue] WARNING: failed to spawn gshss.exe, falling back to xshss.exe\n");
        int64_t xshss_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/crit/xshss.exe", 0, 0, 0, 0);
        if (xshss_pid <= 0) {
            puts("[superglue] CRITICAL: unable to start userland processes\n");
            syscall(SYS_PANIC, (uint64_t)(uintptr_t)"unable to start userland processes", 0, 0, 0, 0);
            while (1) syscall(SYS_SLEEP, 1000, 0, 0, 0, 0);
        }
        gshss_pid = xshss_pid;
    }

    puts("[superglue] pseuDOS desktop session successfully orchestrated\n");

    /* 6. Launch default Terminal application on desktop */
    puts("[superglue] launching /protected/apps/shell.exe...\n");
    int64_t shell_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/shell.exe", 0, 0, 0, 0);

    /* 7. Launch background text console shell on TTY3 */
    puts("[superglue] launching /protected/crit/xshss.exe on tty3...\n");
    int64_t xshss_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/crit/xshss.exe", 0, 3, 0, 0);

    /* 8. Supervise session processes */
    while (1) {
        /* Check if ntfs.exe died */
        if (ntfs_pid > 0 && syscall(SYS_WAITPID, (uint64_t)ntfs_pid, 0, 0, 0, 0) != 0) {
            puts("[superglue] ntfs.exe (display server) terminated; restarting GUI session...\n");
            if (lack_pid > 0) syscall(SYS_KILL, (uint64_t)lack_pid, 0, 0, 0, 0);
            if (ninds_pid > 0) syscall(SYS_KILL, (uint64_t)ninds_pid, 0, 0, 0, 0);
            if (gshss_pid > 0) syscall(SYS_KILL, (uint64_t)gshss_pid, 0, 0, 0, 0);
            if (shell_pid > 0) syscall(SYS_KILL, (uint64_t)shell_pid, 0, 0, 0, 0);

            ntfs_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/ntfs.exe", 0, 0, 0, 0);
            syscall(SYS_SLEEP, 100, 0, 0, 0, 0);

            lack_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/lack.exe", 0, 0, 0, 0);
            syscall(SYS_SLEEP, 100, 0, 0, 0, 0);

            ninds_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/ninds.exe", 0, 0, 0, 0);
            syscall(SYS_SLEEP, 100, 0, 0, 0, 0);

            gshss_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/gshss.exe", 0, 0, 0, 0);
            syscall(SYS_SLEEP, 100, 0, 0, 0, 0);

            shell_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/apps/shell.exe", 0, 0, 0, 0);
        } else {
            /* Check if lack.exe died */
            if (lack_pid > 0 && syscall(SYS_WAITPID, (uint64_t)lack_pid, 0, 0, 0, 0) != 0) {
                puts("[superglue] lack.exe (compositor) terminated; respawning...\n");
                lack_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/lack.exe", 0, 0, 0, 0);
            }

            /* Check if ninds.exe died */
            if (ninds_pid > 0 && syscall(SYS_WAITPID, (uint64_t)ninds_pid, 0, 0, 0, 0) != 0) {
                puts("[superglue] ninds.exe (window manager) terminated; respawning...\n");
                ninds_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/ninds.exe", 0, 0, 0, 0);
            }

            /* Check if gshss.exe died */
            if (gshss_pid > 0 && syscall(SYS_WAITPID, (uint64_t)gshss_pid, 0, 0, 0, 0) != 0) {
                puts("[superglue] gshss.exe terminated; respawning...\n");
                gshss_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/gshss.exe", 0, 0, 0, 0);
            }

            /* Check if xshss.exe on tty3 died */
            if (xshss_pid > 0 && syscall(SYS_WAITPID, (uint64_t)xshss_pid, 0, 0, 0, 0) != 0) {
                xshss_pid = syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/crit/xshss.exe", 0, 3, 0, 0);
            }
        }
        syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
    }
}
