syscall_number = 8

def sys_fs_write(kdata, fd, data):
    from klog import klog, KLOG_DBG
    klog(KLOG_DBG, 'fs_write', f'fd={fd} len={len(data)}')
    from filesystem.vfs import vfs_write
    written = vfs_write(fd, data)
    return written

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'fs_write module initialized')
