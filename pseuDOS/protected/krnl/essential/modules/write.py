syscall_number = 1
import sys as _sys

def sys_write(kdata, fd, data):
    from klog import klog, KLOG_DBG

    if fd == 1:  # stdout
        text = data.decode() if isinstance(data, bytes) else data
        print(text, end='', flush=True)
        return len(data)

    elif fd == 2:  # stderr
        text = data.decode() if isinstance(data, bytes) else data
        _sys.stderr.write(text)
        _sys.stderr.flush()
        return len(data)

    else:  # File descriptor -> route to VFS
        klog(KLOG_DBG, 'write', f'fd={fd} routing to VFS ({len(data)} bytes)')
        from filesystem.vfs import vfs_write
        return vfs_write(fd, data)
