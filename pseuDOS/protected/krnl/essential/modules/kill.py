syscall_number = 62

def sys_kill(kdata, pid, sig):
    from klog import klog, KLOG_DBG
    
    if pid not in kdata['processes']:
        return -1
        
    proc = kdata['processes'][pid]
    
    if sig == 9: # SIGKILL
        klog(KLOG_DBG, 'kill', f'sending SIGKILL to PID {pid}')
        proc['state'] = 'killed'
        return 0
        
    return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'kill module initialized')
