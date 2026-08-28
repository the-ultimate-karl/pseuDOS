syscall_number = 99
import sys, traceback

def sys_panic(kdata, message):
    from klog import klog, KLOG_ERR

    klog(KLOG_ERR, 'panic', '=' * 50)
    klog(KLOG_ERR, 'panic', 'Kernel panic: Not syncing')
    klog(KLOG_ERR, 'panic', '=' * 50)
    klog(KLOG_ERR, 'panic', f'message: {message}')
    klog(KLOG_ERR, 'panic', f'PID: {kdata["get_current_pid"]()}')

    klog(KLOG_ERR, 'panic', 'stack trace:')
    for frame in traceback.extract_stack()[:-2]:
        klog(KLOG_ERR, 'panic', f'  {frame.filename}:{frame.lineno} in {frame.name}')
        if frame.line:
            klog(KLOG_ERR, 'panic', f'    {frame.line.strip()}')

    klog(KLOG_ERR, 'panic', f'processes: {len(kdata.get("processes", {}))}')
    klog(KLOG_ERR, 'panic', f'syscalls: {len(kdata.get("syscall_table", {}))}')
    klog(KLOG_ERR, 'panic', 'System halted!')
    klog(KLOG_ERR, 'panic', '=' * 50)
    sys.exit(1)

def panic(message):
    import protected.krnl.essential.kernel as kernel
    sys_panic(kernel.kdata, message)

def kernel_init(kdata):
    kdata['panic'] = lambda msg: sys_panic(kdata, msg)
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'panic handler initialized')
