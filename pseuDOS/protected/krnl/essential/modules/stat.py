syscall_number = 4

def sys_stat(kdata, pathname):
    from filesystem.vfs import vfs_stat
    
    st = vfs_stat(pathname)
    if st is None:
        return -1
    
    # Returning a dictionary for ease of use in python emulator
    return st

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'stat module initialized')
