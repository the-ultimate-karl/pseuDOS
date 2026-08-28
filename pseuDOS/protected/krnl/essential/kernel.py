import os
import sys
import builtins

def init_kernel():
    # Import klog early - essential path is already the directory containing this file
    ess_path = os.path.dirname(__file__)
    if ess_path not in sys.path:
        sys.path.insert(0, ess_path)

    from klog import klog, boot_log, KLOG_DBG, KLOG_INFO, KLOG_WARN, KLOG_ERR

    kdata = {
        'processes': {0: {'pid': 0, 'state': 'running', 'name': 'kernel', 'thread_id': __import__('threading').get_ident()}},
        'next_pid': 1,
        'current_pid': 0,
        'syscall_table': {},
        'kernel_ready': False,
        'thread_pids': {__import__('threading').get_ident(): 0}
    }

    boot_log('OK', 'initializing pseuDOS kernel')

    modules_dir = os.path.join(os.path.dirname(__file__), 'modules')

    # ========== MOUNT REAL FILESYSTEM AT ROOT ==========
    try:
        import filesystem.realdir as realdir
        import filesystem.vfs as vfs

        # Mount real directory at root (/)
        pseudos_root = os.path.abspath(
            os.path.join(os.path.dirname(__file__), '..', '..', '..')
        )
        realdir.realdir_init(pseudos_root)
        vfs.vfs_mount('/', realdir)
        boot_log('OK', f'Real filesystem mounted at / from {pseudos_root}')

        # ========== MOUNT COMMANDS DIRECTORY ==========
        commands_path = os.path.join(pseudos_root, 'protected', 'crit', 'commands')

        if os.path.exists(commands_path):
            

            # Use the realdir factory instead of inline driver code
            commands_fs = realdir.create_mount(
                root_path=commands_path,
                mount_point='/commands',
                readonly=True,
                handle_start=2000
            )
            vfs.vfs_mount('/commands', commands_fs)
            boot_log('OK', '/commands mounted via VFS')

        else:
            boot_log('WARN', 'commands directory not found: /protected/crit/commands')
            klog(KLOG_WARN, 'boot', 'please create: /protected/crit/commands')
            klog(KLOG_WARN, 'boot', 'system stability not guaranteed')
        # ========== END COMMANDS MOUNT ==========

        # ========== MOUNT SERVICES DIRECTORY ==========
        services_path = os.path.join(pseudos_root, 'protected', 'crit', 'services')
        if os.path.exists(services_path):
            services_fs = realdir.create_mount(
                root_path=services_path,
                mount_point='/services',
                readonly=True,
                handle_start=3000
            )
            vfs.vfs_mount('/services', services_fs)
            boot_log('OK', '/services mounted via VFS')
        else:
            boot_log('WARN', 'services directory not found: /protected/crit/services')
        # ========== END SERVICES MOUNT ==========


        # ========== MOUNT MEMFS FOR /tmp ==========
        import filesystem.memfs as memfs
        vfs.vfs_mount('/tmp', memfs)
        boot_log('OK', 'memfs mounted at /tmp')

    except Exception as e:
        boot_log('FAIL', f'Failed to mount filesystems: {e}')
        klog(KLOG_ERR, 'boot', 'Kernel panic - Not syncing: unable to initialize VFS')
        print("System halted!")
        sys.exit(1)

    # Load kernel modules
    if os.path.exists(modules_dir):
        for filename in sorted(os.listdir(modules_dir)):
            if filename.endswith('.py') and not filename.startswith('_'):
                module_name = filename[:-3]
                try:
                    module = __import__(f'modules.{module_name}', fromlist=[''])

                    if hasattr(module, 'syscall_number'):
                        func_name = f'sys_{module_name}'
                        if hasattr(module, func_name):
                            kdata['syscall_table'][module.syscall_number] = getattr(module, func_name)
                            boot_log('OK', f'{func_name} loaded (syscall {module.syscall_number})')
                    if hasattr(module, 'kernel_init'):
                        module.kernel_init(kdata)
                except Exception as e:
                    boot_log('FAIL', f'{module_name} failed to load: {e}')
    else:
        os.makedirs(modules_dir, exist_ok=True)

    kdata['kernel_ready'] = True
    klog(KLOG_INFO, 'kernel', f'initialized with {len(kdata["syscall_table"])} syscalls')

    return kdata

if __name__ == "__main__":
    kdata = init_kernel()
    sys.modules[__name__].kdata = kdata

    import threading
    def get_current_pid():
        tid = threading.get_ident()
        return kdata['thread_pids'].get(tid, 0)
        
    kdata['get_current_pid'] = get_current_pid

    def syscall(number, *args):
        if not kdata['kernel_ready']:
            return -1
            
        pid = get_current_pid()
        if pid > 0 and pid in kdata['processes']:
            if kdata['processes'][pid]['state'] == 'killed':
                raise SystemExit("Process killed by SIGKILL")
                
        handler = kdata['syscall_table'].get(number)
        if not handler:
            return -1
        return handler(kdata, *args)

    sys.modules[__name__].syscall = syscall
else:
    kdata = init_kernel()

    import threading
    def get_current_pid():
        tid = threading.get_ident()
        return kdata['thread_pids'].get(tid, 0)

    # Export to kdata so modules can use it
    kdata['get_current_pid'] = get_current_pid

    def syscall(number, *args):
        if not kdata['kernel_ready']:
            return -1
            
        pid = get_current_pid()
        if pid > 0 and pid in kdata['processes']:
            if kdata['processes'][pid]['state'] == 'killed':
                raise SystemExit("Process killed by SIGKILL")
                
        handler = kdata['syscall_table'].get(number)
        if not handler:
            return -1
        return handler(kdata, *args)

    sys.modules[__name__].kdata = kdata
    sys.modules[__name__].syscall = syscall

    from klog import klog, boot_log, KLOG_INFO
    boot_log('OK', 'spawning init process')
    try:
        from krnl import init
        init.main()
    except:
        print("Kernel panic - Not syncing: failed spawning init")
        print("System halted!")
        sys.exit(-1)
