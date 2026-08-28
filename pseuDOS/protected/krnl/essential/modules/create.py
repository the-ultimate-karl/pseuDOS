syscall_number = 82

def sys_create(kdata, pathname, mode=0o644):
    from klog import klog, KLOG_DBG, KLOG_ERR
    klog(KLOG_DBG, 'create', f'creating {pathname}')
    try:
        from filesystem import vfs
        klog(KLOG_DBG, 'create', f'active mounts: {list(vfs.vfs_state["mounts"].keys())}')
        result = vfs.vfs_create(pathname, mode)
        return result
    except Exception as e:
        klog(KLOG_ERR, 'create', f'failed: {e}')
        return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'create module initialized')
