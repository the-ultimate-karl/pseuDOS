import os

kernel_path = r'h:\repositories\backup repository\pseuDOS-backup\rewrite-05\pseuDOS\protected\krnl\essential\kernel.py'
with open(kernel_path, 'r', encoding='utf-8') as f:
    content = f.read()

# Add boot_log import
content = content.replace(
    "from klog import klog, KLOG_DBG, KLOG_INFO, KLOG_WARN, KLOG_ERR",
    "from klog import klog, boot_log, KLOG_DBG, KLOG_INFO, KLOG_WARN, KLOG_ERR"
)

# Update realdir mount
content = content.replace(
    "klog(KLOG_INFO, 'boot', f'real filesystem mounted at / from {pseudos_root}')",
    "boot_log('OK', f'Real filesystem mounted at / from {pseudos_root}')"
)

# Replace commands mount prints
content = content.replace(
    "klog(KLOG_INFO, 'boot', f'mounting /commands from {commands_path}')",
    ""
)
content = content.replace(
    "klog(KLOG_INFO, 'boot', '/commands mounted via VFS')",
    "boot_log('OK', '/commands mounted via VFS')"
)

content = content.replace(
    "klog(KLOG_WARN, 'boot', 'commands directory not found: /protected/crit/commands')",
    "boot_log('WARN', 'commands directory not found: /protected/crit/commands')"
)

# Replace memfs mount prints
content = content.replace(
    "klog(KLOG_INFO, 'boot', 'memfs mounted at /tmp')",
    "boot_log('OK', 'memfs mounted at /tmp')"
)

# Replace fail prints
content = content.replace(
    "klog(KLOG_ERR, 'boot', f'failed to mount filesystems: {e}')",
    "boot_log('FAIL', f'Failed to mount filesystems: {e}')"
)

# Add /services mount right after /commands mount block
services_mount = """
        # ========== MOUNT SERVICES DIRECTORY ==========
        services_path = os.path.join(pseudos_root, 'protected', 'crit', 'services')
        if os.path.exists(services_path):
            services_fs = realdir.create_mount(
                root_path=services_path,
                mount_point='/services',
                readonly=True,
                handle_start=3000
            )
            vfs.vfs_mount('/services/', services_fs)
            boot_log('OK', '/services mounted via VFS')
        else:
            boot_log('WARN', 'services directory not found: /protected/crit/services')
        # ========== END SERVICES MOUNT ==========
"""
content = content.replace("# ========== END COMMANDS MOUNT ==========", "# ========== END COMMANDS MOUNT ==========\n" + services_mount)

# Update syscall loading
content = content.replace(
    "klog(KLOG_INFO, 'kernel', f'{func_name} loaded (syscall {module.syscall_number})')",
    "boot_log('OK', f'{func_name} loaded (syscall {module.syscall_number})')"
)

content = content.replace(
    "klog(KLOG_ERR, 'kernel', f'{module_name} failed to load: {e}')",
    "boot_log('FAIL', f'{module_name} failed to load: {e}')"
)

# Update init start
content = content.replace(
    "klog(KLOG_INFO, 'kernel', 'spawning init process')",
    "boot_log('OK', 'spawning init process')"
)
content = content.replace(
    "klog(KLOG_INFO, 'kernel', 'initializing pseuDOS kernel')",
    "boot_log('OK', 'initializing pseuDOS kernel')"
)

with open(kernel_path, 'w', encoding='utf-8') as f:
    f.write(content)

print("kernel.py updated")
