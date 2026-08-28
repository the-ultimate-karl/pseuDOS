syscall_number = 81

def sys_sysmkdir(kdata, pathname, mode=0o755):
    from klog import klog, KLOG_DBG, KLOG_ERR
    klog(KLOG_DBG, 'mkdir', f'creating directory {pathname}')
    try:
        from filesystem.vfs import vfs_mkdir
        return vfs_mkdir(pathname, mode)
    except Exception as e:
        klog(KLOG_ERR, 'mkdir', f'failed: {e}')
        return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'mkdir module initialized')
