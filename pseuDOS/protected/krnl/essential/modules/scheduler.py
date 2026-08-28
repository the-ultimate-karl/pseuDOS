syscall_number = 24  # sched_yield - was missing, so scheduler was never in syscall table

def sys_scheduler(kdata):
    from klog import klog, KLOG_DBG

    ready_procs = [p for p in kdata['processes'].values()
                   if p['state'] == 'ready']

    if ready_procs:
        current_pid = kdata.get('current_pid', 0)

        if current_pid > 0 and current_pid in kdata['processes']:
            kdata['processes'][current_pid]['state'] = 'ready'

        proc = ready_procs[0]
        proc['state'] = 'running'

        kdata['current_pid'] = proc['pid']

        klog(KLOG_DBG, 'sched', f'context switch: {current_pid} -> {proc["pid"]}')
        return proc['pid']

    kdata['current_pid'] = 0
    return 0

def kernel_init(kdata):
    if 'current_pid' not in kdata:
        kdata['current_pid'] = 0

    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'scheduler module initialized')
