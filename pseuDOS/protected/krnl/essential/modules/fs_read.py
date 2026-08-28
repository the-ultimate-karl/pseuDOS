syscall_number = 7

def sys_fs_read(kdata, fd, count):
    from klog import klog, KLOG_DBG
    klog(KLOG_DBG, 'fs_read', f'fd={fd} count={count}')
    try:
        from filesystem.vfs import vfs_read
    except ImportError as e:
        return -1

    data = vfs_read(fd, count)

    if data is None:
        return -1
    klog(KLOG_DBG, 'fs_read', f'got {len(data)} bytes')
    return data

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'fs_read module initialized')
