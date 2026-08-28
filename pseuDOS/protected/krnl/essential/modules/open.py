syscall_number = 2

def sys_open(kdata, pathname, flags, mode=0o777):
    from klog import klog, KLOG_DBG, KLOG_ERR
    klog(KLOG_DBG, 'open', f'opening {pathname} flags={flags}')
    try:
        from filesystem.vfs import vfs_open
        fd = vfs_open(pathname, flags, mode)
        klog(KLOG_DBG, 'open', f'{pathname} -> fd={fd}')
        return fd
    except Exception as e:
        klog(KLOG_ERR, 'open', f'error: {e}')
        return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'open module initialized')
