syscall_number = 3

def sys_fs_close(kdata, fd):
    from klog import klog, KLOG_DBG
    klog(KLOG_DBG, 'close', f'closing fd={fd}')
    from filesystem.vfs import vfs_close
    result = vfs_close(fd)
    return result

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'close module initialized')
