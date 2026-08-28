syscall_number = 60

def sys_exit(kdata, exit_code=0):
    from klog import klog, KLOG_INFO, KLOG_WARN
    current_pid = kdata['get_current_pid']()

    if current_pid in kdata['processes']:
        kdata['processes'][current_pid]['state'] = 'zombie'
        kdata['processes'][current_pid]['exit_code'] = exit_code
        klog(KLOG_INFO, 'exit', f'process {current_pid} exited with code {exit_code}')
        return exit_code
    klog(KLOG_WARN, 'exit', f'process {current_pid} not found')
    return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'exit module initialized')
