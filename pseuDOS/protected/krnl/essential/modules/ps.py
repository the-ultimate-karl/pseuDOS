syscall_number = 117

def sys_ps(kdata):
    procs = []
    for pid, proc in kdata['processes'].items():
        procs.append({
            'pid': pid,
            'state': proc.get('state', 'unknown'),
            'name': proc.get('name', 'unknown')
        })
    return procs

def kernel_init(kdata):
    from klog import boot_log
    boot_log('OK', 'ps module initialized')
