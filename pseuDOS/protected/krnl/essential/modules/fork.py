syscall_number = 57

def sys_fork(kdata):
    from klog import klog, KLOG_DBG, KLOG_INFO

    child_pid = kdata['next_pid']
    kdata['next_pid'] += 1

    # BUG FIX: use current_pid instead of hardcoded PID 0
    parent_pid = kdata.get('current_pid', 0)
    parent_proc = kdata['processes'].get(parent_pid, kdata['processes'][0])

    child_proc = {
        'pid': child_pid,
        'state': 'ready',
        'name': f'child_of_{parent_proc["pid"]}',
        'parent': parent_proc['pid'],
        'memory': [],
        'files': []
    }

    kdata['processes'][child_pid] = child_proc
    klog(KLOG_INFO, 'fork', f'parent={parent_proc["pid"]} -> child={child_pid}')

    return 0, child_pid

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'fork module initialized')
