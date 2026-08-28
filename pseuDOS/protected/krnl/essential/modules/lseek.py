syscall_number = 9

def sys_lseek(kdata, fd, offset, whence):
    from filesystem.vfs import vfs_lseek
    
    return vfs_lseek(fd, offset, whence)

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'lseek module initialized')
