syscall_number = 474

def sys_listdir(kdata, pathname):
    from klog import klog, KLOG_DBG, KLOG_ERR
    klog(KLOG_DBG, 'listdir', f'listing {pathname}')
    try:
        from filesystem.vfs import vfs_listdir
        entries = vfs_listdir(pathname)
        klog(KLOG_DBG, 'listdir', f'{len(entries) if entries else 0} entries')
        return entries
    except Exception as e:
        klog(KLOG_ERR, 'listdir', f'failed: {e}')

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'listdir module initialized')
