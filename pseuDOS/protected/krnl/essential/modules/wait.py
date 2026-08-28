syscall_number = 61

def sys_wait(kdata, pid, status_ptr, options):
    from klog import klog, KLOG_DBG, KLOG_INFO
    current_pid = kdata['get_current_pid']()
    if pid == -1:
        children = [
            p for p in kdata['processes'].values()
            if p.get('parent') == current_pid and p.get('state') == 'zombie'
        ]
    elif pid > 0:
        if pid not in kdata['processes']:
            return -1
        child = kdata['processes'][pid]
        if child.get('parent') != current_pid:
            return -1
        if child.get('state') == 'zombie':
            if options & 1:
                return 0
            else:
                return -1
        children = [child]
    else:
        return -1
    if not children:
        return -1

    zombie = children[0]
    zombie_pid = zombie['pid']

    if zombie_pid in kdata['processes']:
        exit_code = kdata['processes'][zombie_pid].get('exit_code', 0)
        del kdata['processes'][zombie_pid]

    klog(KLOG_INFO, 'wait', f'cleaned up zombie PID {zombie_pid}')
    return zombie_pid

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'wait module initialized')

    for pid, proc in kdata['processes'].items():
        if 'parent' not in proc:
            proc['parent'] = 0
