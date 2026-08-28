syscall_number = 12

def sys_memory(kdata, addr):
    from klog import klog, KLOG_DBG
    pid = kdata['get_current_pid']()
    if 'brk' not in kdata:
        kdata['brk'] = {}
    if addr == 0:
        result = kdata['brk'].get(pid, 0x100000)
        klog(KLOG_DBG, 'memory', f'brk query pid={pid} -> {hex(result)}')
        return result
    else:
        kdata['brk'][pid] = addr
        klog(KLOG_DBG, 'memory', f'brk set pid={pid} -> {hex(addr)}')
        return addr

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'memory module initialized')
