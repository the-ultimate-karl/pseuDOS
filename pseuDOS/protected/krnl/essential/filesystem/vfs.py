import os
from klog import klog, KLOG_DBG, KLOG_INFO, KLOG_WARN, KLOG_ERR

vfs_state = {
    "mounts": {},
    "open_files": {},
    "next_fd": 3,
    "cwd": "/"
}

def vfs_mount(path, fs_driver):
    """Mount a filesystem driver at the given path"""
    vfs_state["mounts"][path] = fs_driver
    klog(KLOG_DBG, 'vfs', f'mounted driver at {path}')
    return 0

def is_protected(vfs_path):
    """Checks if a path belongs to the core system folders.
    This is the single canonical implementation - all other modules should import this."""
    if not vfs_path:
        return False

    path = vfs_path.lower().replace('\\', '/')
    if not path.startswith('/'):
        path = '/' + path
    if len(path) > 1:
        path = path.rstrip('/')

    protected_zones = ['/protected', '/coreutils', '/commands']

    for zone in protected_zones:
        if path == zone or path.startswith(zone + '/'):
            return True
    return False


def vfs_normalize_path(pathname):
    if not pathname: return '/'
    current_cwd = vfs_state.get('cwd', '/')
    if not pathname.startswith('/'):
        full_path = current_cwd.rstrip('/') + '/' + pathname
    else:
        full_path = pathname
        
    import os
    clean_path = os.path.normpath(full_path).replace("\\", "/")
    if not clean_path.startswith('/'): clean_path = '/' + clean_path
    if len(clean_path) > 1: clean_path = clean_path.rstrip('/')
    return clean_path

def vfs_open(pathname, flags, mode=0o777):
    pathname = vfs_normalize_path(pathname)
    """Open a file through VFS"""
    klog(KLOG_DBG, 'vfs', f'open: {pathname} flags={flags} mode={oct(mode)}')
    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)

    for mount_point, driver in mounts:
        if pathname.startswith(mount_point):
            inode = driver.open(pathname, flags, mode)

            if isinstance(inode, int) and inode == -1:
                klog(KLOG_DBG, 'vfs', f'open: driver returned -1 for {pathname}')
                return -1

            fd = vfs_state["next_fd"]
            vfs_state['next_fd'] += 1
            vfs_state["open_files"][fd] = {
                'inode': inode,
                'driver': driver,
                'position': 0,
                'path': pathname
            }
            klog(KLOG_DBG, 'vfs', f'open: {pathname} -> fd={fd}')
            return fd

    klog(KLOG_WARN, 'vfs', f'open: no mount matched {pathname}')
    return -1

def vfs_read(fd, count):
    """Read from an open file descriptor"""
    if fd not in vfs_state["open_files"]:
        klog(KLOG_DBG, 'vfs', f'read: invalid fd={fd}')
        return None

    file_info = vfs_state["open_files"][fd]
    driver = file_info["driver"]
    inode = file_info["inode"]
    position = file_info["position"]

    data = driver.read(inode, position, count)

    if data is None:
        return None

    file_info["position"] += len(data)
    klog(KLOG_DBG, 'vfs', f'read: fd={fd} pos={position} got {len(data)} bytes')
    return data

def vfs_write(fd, data):
    """Write to an open file descriptor"""
    if fd not in vfs_state["open_files"]:
        klog(KLOG_DBG, 'vfs', f'write: invalid fd={fd}')
        return -1

    file_info = vfs_state["open_files"][fd]

    # Check if the file is in a protected zone
    if is_protected(file_info.get('path', '')):
        klog(KLOG_WARN, 'vfs', f'write: denied - protected path: {file_info["path"]}')
        return -1

    driver = file_info["driver"]
    inode = file_info["inode"]
    position = file_info["position"]

    result = driver.write(inode, position, data)
    if result > 0:
        file_info["position"] += result
    klog(KLOG_DBG, 'vfs', f'write: fd={fd} wrote {result} bytes')
    return result

def vfs_close(fd):
    """Close an open file descriptor"""
    if fd in vfs_state["open_files"]:
        file_info = vfs_state["open_files"][fd]
        driver = file_info["driver"]
        inode = file_info["inode"]

        driver.close(inode)

        path = file_info.get('path', '?')
        del vfs_state["open_files"][fd]
        klog(KLOG_DBG, 'vfs', f'close: fd={fd} ({path})')
        return 0
    klog(KLOG_DBG, 'vfs', f'close: invalid fd={fd}')
    return -1

def vfs_mkdir(pathname, mode):
    pathname = vfs_normalize_path(pathname)
    klog(KLOG_DBG, 'vfs', f'mkdir: {pathname} mode={oct(mode)}')

    if is_protected(pathname):
        klog(KLOG_WARN, 'vfs', f'mkdir: denied - protected path: {pathname}')
        return -1

    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)

    for mount_point, driver in mounts:
        if pathname.startswith(mount_point):
            klog(KLOG_DBG, 'vfs', f'mkdir: matched mount {mount_point}')
            if hasattr(driver, 'mkdir'):
                return driver.mkdir(pathname, mode)
            else:
                klog(KLOG_ERR, 'vfs', f'mkdir: driver has no mkdir method')
                return -1

    klog(KLOG_WARN, 'vfs', f'mkdir: no mount matched {pathname}')
    return -1

def vfs_unlink(pathname):
    pathname = vfs_normalize_path(pathname)
    if is_protected(pathname):
        klog(KLOG_WARN, 'vfs', f'unlink: denied - protected path: {pathname}')
        return -1

    klog(KLOG_DBG, 'vfs', f'unlink: {pathname}')
    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)
    for mount_point, driver in mounts:
        if pathname.startswith(mount_point):
            if hasattr(driver, 'unlink'):
                return driver.unlink(pathname)
            else:
                return -1
    return -1

def vfs_rmdir(pathname):
    pathname = vfs_normalize_path(pathname)
    if is_protected(pathname):
        klog(KLOG_WARN, 'vfs', f'rmdir: denied - protected path: {pathname}')
        return -1

    klog(KLOG_DBG, 'vfs', f'rmdir: {pathname}')
    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)
    for mount_point, driver in mounts:
        if pathname.startswith(mount_point):
            if hasattr(driver, 'rmdir'):
                return driver.rmdir(pathname)
    return -1

def vfs_listdir(pathname="/"):
    pathname = vfs_normalize_path(pathname)
    klog(KLOG_DBG, 'vfs', f'listdir: {pathname}')
    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)

    for mount_point, driver in mounts:
        if pathname.startswith(mount_point):
            if hasattr(driver, 'listdir'):
                result = driver.listdir(pathname)
                klog(KLOG_DBG, 'vfs', f'listdir: {pathname} -> {len(result) if result else 0} entries')
                return result
            else:
                return []

    return []

def vfs_getcwd():
    return vfs_state['cwd']

def vfs_chdir(pathname):
    clean_path = vfs_normalize_path(pathname)
    klog(KLOG_DBG, 'vfs', f'chdir: {pathname} -> normalized: {clean_path}')

    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)

    for mount_point, driver in mounts:
        if clean_path.startswith(mount_point):
            if hasattr(driver, 'listdir'):
                res = driver.listdir(clean_path)

                if isinstance(res, list):
                    vfs_state['cwd'] = clean_path
                    klog(KLOG_DBG, 'vfs', f'chdir: cwd set to {clean_path}')
                    return 0

    klog(KLOG_WARN, 'vfs', f'chdir: failed for {clean_path}')
    return -1

def vfs_create(pathname, mode=0o644):
    pathname = vfs_normalize_path(pathname)
    klog(KLOG_DBG, 'vfs', f'create: {pathname} mode={oct(mode)}')

    if is_protected(pathname):
        klog(KLOG_WARN, 'vfs', f'create: denied - protected path: {pathname}')
        return -1

    # Sort mounts to ensure longest match (like /commands) is checked before /
    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)

    for mount_point, driver in mounts:
        if pathname.startswith(mount_point):
            klog(KLOG_DBG, 'vfs', f'create: matched mount {mount_point}')

            # Check for BOTH possible names to be safe
            if hasattr(driver, 'realdir_create'):
                return driver.realdir_create(pathname, mode)
            elif hasattr(driver, 'create'):
                return driver.create(pathname, mode)
            else:
                klog(KLOG_ERR, 'vfs', f'create: driver has no create method')
                return -1

    klog(KLOG_WARN, 'vfs', f'create: no mount matched {pathname}')
    return -1

def vfs_stat(pathname):
    pathname = vfs_normalize_path(pathname)
    klog(KLOG_DBG, 'vfs', f'stat: {pathname}')
    mounts = sorted(vfs_state['mounts'].items(), key=lambda x: len(x[0]), reverse=True)
    for mount_point, driver in mounts:
        if pathname.startswith(mount_point):
            if hasattr(driver, 'stat'):
                return driver.stat(pathname)
    return None

def vfs_lseek(fd, offset, whence):
    if fd not in vfs_state["open_files"]:
        return -1
    file_info = vfs_state["open_files"][fd]
    
    if whence == 0:
        file_info["position"] = offset
    elif whence == 1:
        file_info["position"] += offset
    elif whence == 2:
        path = file_info.get("path")
        if not path: return -1
        st = vfs_stat(path)
        if st:
            file_info["position"] = st['size'] + offset
        else:
            return -1
    else:
        return -1
        
    if file_info["position"] < 0:
        file_info["position"] = 0
        
    klog(KLOG_DBG, 'vfs', f'lseek: fd={fd} pos={file_info["position"]}')
    return file_info["position"]

def vfs_panic(message):
    """VFS panic - called on critical VFS errors"""
    klog(KLOG_ERR, 'vfs', f'PANIC: {message}')
    klog(KLOG_ERR, 'vfs', f'  Mounts: {list(vfs_state["mounts"].keys())}')
    klog(KLOG_ERR, 'vfs', f'  Open files: {len(vfs_state["open_files"])}')
    klog(KLOG_ERR, 'vfs', f'  Next FD: {vfs_state["next_fd"]}')
    raise RuntimeError(f"VFS panic: {message}")
