syscall_number = 84

def sys_rmdir(kdata, pathname):
    from klog import klog, KLOG_DBG, KLOG_ERR
    klog(KLOG_DBG, 'rmdir', f'removing directory {pathname}')
    try:
        from filesystem.vfs import vfs_rmdir
        result = vfs_rmdir(pathname)
        return result
    except Exception as e:
        klog(KLOG_ERR, 'rmdir', f'failed: {e}')
        return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'rmdir module initialized')
