syscall_number = 79

def sys_getcwd(kdata):
    try:
        from filesystem import vfs as vfs
        return vfs.vfs_getcwd()
    except:
        return "/"

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'getcwd module initialized')
