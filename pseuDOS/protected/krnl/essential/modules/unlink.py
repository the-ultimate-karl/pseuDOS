syscall_number = 10
import errno

def sys_unlink(kdata, pathname):
    from klog import klog, KLOG_DBG, KLOG_ERR
    klog(KLOG_DBG, 'unlink', f'unlinking {pathname}')
    try:
        from filesystem.vfs import vfs_unlink
        result = vfs_unlink(pathname)
        return result
    except Exception as e:
        klog(KLOG_ERR, 'unlink', f'failed: {e}')
        return -errno.EIO

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'unlink module initialized')
