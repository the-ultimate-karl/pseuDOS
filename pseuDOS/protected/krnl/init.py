def main():
    from krnl.essential import kernel
    from krnl.essential.klog import boot_log
    import time

    boot_log('OK', "System initialization started")
    
    # 1. Test all loaded syscalls safely
    for num, handler in kernel.kdata['syscall_table'].items():
        name = handler.__name__
        try:
            kernel.syscall(num)
            boot_log('OK', f"Tested {name} (syscall {num})")
        except TypeError:
            # Expected because we are missing args
            boot_log('OK', f"Tested {name} (syscall {num})")
        except Exception as e:
            boot_log('WARN', f"{name} reported an error during testing, pseuDOS will continue with limited functionality")
    
    # 2. Start services
    boot_log('INFO', "Scanning for background services...")
    services = kernel.syscall(474, "/services")
    if services:
        for entry in services:
            name, etype = entry
            if etype == 'file' and name.endswith('.py'):
                boot_log('INFO', f"Starting service: {name}")
                pid = kernel.syscall(59, f"/services/{name}", [])
                if pid != -1:
                    boot_log('OK', f"Started {name} with PID {pid}")
                else:
                    boot_log('FAIL', f"Failed to start {name}")
    else:
        boot_log('WARN', "No services found in /services")

    # 3. Reduce log verbosity and start shell loop
    kernel.syscall(116, 1, 1) # Set log level to WARN (1)
    
    while True:
        boot_log('OK', "Spawning interactive shell")
        child_pid = kernel.syscall(59, "/protected/crit/shell.py", [])
        
        if child_pid == -1:
            kernel.syscall(99, "init: unable to execute /protected/crit/shell.py") # Panic
            break
            
        # Wait for any child process
        while True:
            r = kernel.syscall(61, -1, 0, 0)
            if r == child_pid:
                import sys
                if not sys.stdin.isatty():
                    boot_log('INFO', 'End of piped input reached. Halting system.')
                    sys.exit(0)
                boot_log('WARN', f"Shell (PID {child_pid}) exited, respawning...")
                time.sleep(1)
                break # breaks inner loop, outer loop respawns
            elif r > 0 and r != child_pid:
                # Some other background service exited
                boot_log('INFO', f"Background process {r} exited")
            time.sleep(1)
