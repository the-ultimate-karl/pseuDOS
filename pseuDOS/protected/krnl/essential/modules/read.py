syscall_number = 0
import sys

def sys_read(kdata, fd, count):
    from klog import klog, KLOG_DBG

    if fd == 0:  # stdin
        sys.stderr.write("")
        sys.stderr.flush()

        try:
            data = sys.stdin.readline()
            if data.endswith('\n'):
                data = data[:-1]

            if count > 0 and len(data) > count:
                data = data[:count]

            return data
        except EOFError:
            return ""

    else:  # File descriptor -> route to VFS
        klog(KLOG_DBG, 'read', f'fd={fd} routing to VFS (count={count})')
        from filesystem.vfs import vfs_read
        data = vfs_read(fd, count)
        return data if data is not None else -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'read module initialized')
