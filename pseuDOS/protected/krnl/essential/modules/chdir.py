syscall_number = 80

def sys_chdir(kdata, pathname):
    from klog import klog, KLOG_DBG
    klog(KLOG_DBG, 'chdir', f'changing to {pathname}')
    try:
        from filesystem.vfs import vfs_chdir
        result = vfs_chdir(pathname)
        klog(KLOG_DBG, 'chdir', f'result: {result}')
        return result
    except Exception as e:
        from klog import KLOG_ERR
        klog(KLOG_ERR, 'chdir', f'error: {e}')
        return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'chdir module initialized')
