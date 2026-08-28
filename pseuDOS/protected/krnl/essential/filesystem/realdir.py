import os
import builtins
import errno
from klog import klog, KLOG_DBG, KLOG_INFO, KLOG_WARN, KLOG_ERR

# Windows error codes
ERROR_DIR_NOT_EMPTY = 145
ERROR_SHARING_VIOLATION = 32
ERROR_ACCESS_DENIED = 5
ERROR_BUSY = 170

realdir_state = {'root_path': '', 'open_files': {}}

def realdir_init(root_path):
    realdir_state['root_path'] = os.path.abspath(root_path)
    klog(KLOG_DBG, 'realdir', f'initialized with root: {realdir_state["root_path"]}')
    return True

def realdir_vfs_to_real(vfs_path):
    rel = vfs_path.lstrip('/')
    return os.path.normpath(os.path.join(realdir_state['root_path'], rel))

def mkdir(vfs_path, mode=0o755):
    klog(KLOG_DBG, 'realdir', f'mkdir: vfs={vfs_path}')
    real_path = realdir_vfs_to_real(vfs_path)
    klog(KLOG_DBG, 'realdir', f'mkdir: real={real_path}')

    try:
        os.makedirs(real_path, exist_ok=True)
        klog(KLOG_DBG, 'realdir', f'mkdir: success')
        return 0
    except Exception as e:
        klog(KLOG_ERR, 'realdir', f'mkdir: failed: {e}')
        return -1

def unlink(vfs_path):
    from filesystem.vfs import is_protected
    if is_protected(vfs_path):
        klog(KLOG_WARN, 'realdir', f'unlink: denied - protected: {vfs_path}')
        return -errno.EACCES

    real_path = realdir_vfs_to_real(vfs_path)
    klog(KLOG_DBG, 'realdir', f'unlink: {vfs_path} -> {real_path}')
    try:
        if os.path.isfile(real_path):
            os.remove(real_path)
            return 0
        elif os.path.isdir(real_path):
            os.rmdir(real_path)
            return 0
        return -errno.ENOENT
    except OSError as e:
        if hasattr(e, 'winerror'):
            if e.winerror == ERROR_SHARING_VIOLATION:
                klog(KLOG_WARN, 'realdir', f'unlink: file in use: {real_path}')
                return -errno.EBUSY
            elif e.winerror == ERROR_ACCESS_DENIED:
                klog(KLOG_WARN, 'realdir', f'unlink: access denied: {real_path}')
                return -errno.EACCES
            elif e.winerror == ERROR_DIR_NOT_EMPTY:
                klog(KLOG_WARN, 'realdir', f'unlink: dir not empty: {real_path}')
                return -errno.ENOTEMPTY
        klog(KLOG_ERR, 'realdir', f'unlink: error: {e}')
        return -1

def rmdir(vfs_path):
    real_path = realdir_vfs_to_real(vfs_path)
    klog(KLOG_DBG, 'realdir', f'rmdir: {vfs_path} -> {real_path}')
    try:
        os.rmdir(real_path)
        return 0
    except OSError as e:
        if hasattr(e, 'winerror'):
            if e.winerror == ERROR_DIR_NOT_EMPTY:
                klog(KLOG_WARN, 'realdir', f'rmdir: dir not empty: {real_path}')
                return -errno.ENOTEMPTY
            elif e.winerror == ERROR_SHARING_VIOLATION:
                klog(KLOG_WARN, 'realdir', f'rmdir: in use: {real_path}')
                return -errno.EBUSY
            elif e.winerror == ERROR_ACCESS_DENIED:
                klog(KLOG_WARN, 'realdir', f'rmdir: access denied: {real_path}')
                return -errno.EACCES
            elif e.winerror == ERROR_BUSY:
                klog(KLOG_WARN, 'realdir', f'rmdir: resource busy: {real_path}')
                return -errno.EBUSY
        klog(KLOG_ERR, 'realdir', f'rmdir: error: {e}')
        return -1

def realdir_create(vfs_path, mode=0o644):
    from filesystem.vfs import is_protected
    if is_protected(vfs_path):
        klog(KLOG_WARN, 'realdir', f'create: denied - protected: {vfs_path}')
        return -1
    klog(KLOG_DBG, 'realdir', f'create: {vfs_path}')
    real_path = realdir_vfs_to_real(vfs_path)
    klog(KLOG_DBG, 'realdir', f'create: real={real_path}')
    try:
        with builtins.open(real_path, 'w') as f:
            pass
        klog(KLOG_DBG, 'realdir', f'create: success')
        return 0
    except Exception as e:
        klog(KLOG_ERR, 'realdir', f'create: failed: {e}')
        return -1

def realdir_listdir(vfs_path):
    real_path = realdir_vfs_to_real(vfs_path)
    klog(KLOG_DBG, 'realdir', f'listdir: {vfs_path} -> {real_path}')

    if not os.path.isdir(real_path):
        klog(KLOG_DBG, 'realdir', f'listdir: not a directory: {real_path}')
        return None

    entries = []
    try:
        for name in os.listdir(real_path):
            full_path = os.path.join(real_path, name)
            etype = 'directory' if os.path.isdir(full_path) else 'file'
            entries.append((name, etype))
        klog(KLOG_DBG, 'realdir', f'listdir: {len(entries)} entries')
        return entries
    except Exception as e:
        klog(KLOG_ERR, 'realdir', f'listdir: error: {e}')
        return None

def realdir_open(vfs_path, flags, mode=0o777):
    real_path = realdir_vfs_to_real(vfs_path)
    klog(KLOG_DBG, 'realdir', f'open: {vfs_path} -> {real_path}')

    if not os.path.exists(real_path):
        klog(KLOG_DBG, 'realdir', f'open: path does not exist: {real_path}')
        return -1

    handle = real_path
    realdir_state['open_files'][handle] = {
        'real_path': real_path,
        'position': 0
    }
    klog(KLOG_DBG, 'realdir', f'open: handle registered')
    return handle

def realdir_read(handle, position, count):
    if handle not in realdir_state['open_files']:
        klog(KLOG_DBG, 'realdir', f'read: invalid handle')
        return None

    file_info = realdir_state['open_files'][handle]
    real_path = file_info['real_path']

    try:
        with builtins.open(real_path, 'rb') as f:
            f.seek(position)
            data = f.read(count)
            realdir_state['open_files'][handle]['position'] = position + len(data)
            klog(KLOG_DBG, 'realdir', f'read: {len(data)} bytes from pos {position}')
            return data
    except Exception as e:
        klog(KLOG_ERR, 'realdir', f'read: error: {e}')
        return None

def realdir_write(handle, position, data):
    klog(KLOG_DBG, 'realdir', f'write: handle={handle} pos={position} len={len(data)}')

    if handle not in realdir_state['open_files']:
        klog(KLOG_DBG, 'realdir', f'write: handle not found in open_files')
        klog(KLOG_DBG, 'realdir', f'write: active handles: {list(realdir_state["open_files"].keys())}')
        return -1

    file_info = realdir_state['open_files'][handle]
    real_path = file_info['real_path']

    try:
        with builtins.open(real_path, 'rb') as f:
            current = f.read()

        if isinstance(data, str):
            data = data.encode()

        needed = position + len(data)
        if len(current) < needed:
            current += b'\x00' * (needed - len(current))

        new_content = current[:position] + data + current[position + len(data):]

        with builtins.open(real_path, 'wb') as f:
            f.write(new_content)

        klog(KLOG_DBG, 'realdir', f'write: {len(data)} bytes to {real_path}')
        return len(data)
    except Exception as e:
        klog(KLOG_ERR, 'realdir', f'write: critical error: {e}')
        return -1

def realdir_close(handle):
    klog(KLOG_DBG, 'realdir', f'close: handle={handle}')
    if handle in realdir_state['open_files']:
        del realdir_state['open_files'][handle]
    return 0

# === Factory: create independent mounts backed by real directories ===

def create_mount(root_path, mount_point='/', readonly=False, handle_start=2000):
    """Create a new realdir-backed VFS driver with its own independent state.

    Args:
        root_path: Absolute path to the host directory to serve.
        mount_point: The VFS mount point (used to strip prefixes from paths).
        readonly: If True, write operations return -1.
        handle_start: Starting number for file handles (avoid collisions).

    Returns:
        An object with open/read/write/close/listdir methods suitable for vfs_mount().
    """
    mount_state = {
        'root_path': os.path.abspath(root_path),
        'open_files': {},
        'next_handle': handle_start,
    }

    clean_mount = mount_point.rstrip('/')

    class RealDirMount:
        def _to_real(self, vfs_path):
            """Convert a VFS path to a real host OS path."""
            # Strip mount point prefix
            if clean_mount and vfs_path.startswith(clean_mount + '/'):
                vfs_path = vfs_path[len(clean_mount):]
            elif clean_mount and vfs_path == clean_mount:
                vfs_path = '/'

            if vfs_path == '/' or vfs_path == '':
                return mount_state['root_path']

            rel = vfs_path.lstrip('/')
            return os.path.normpath(os.path.join(mount_state['root_path'], rel))

        def open(self, pathname, flags, mode=0o777):
            real_path = self._to_real(pathname)
            klog(KLOG_DBG, 'mount', f'open: {pathname} -> {real_path}')

            if not os.path.exists(real_path):
                klog(KLOG_DBG, 'mount', f'open: path does not exist')
                return -1

            handle = mount_state['next_handle']
            mount_state['next_handle'] += 1
            mount_state['open_files'][handle] = {
                'real_path': real_path,
                'position': 0,
            }
            klog(KLOG_DBG, 'mount', f'open: assigned handle {handle}')
            return handle

        def read(self, handle, position, count):
            if handle not in mount_state['open_files']:
                return None

            file_info = mount_state['open_files'][handle]
            try:
                with builtins.open(file_info['real_path'], 'rb') as f:
                    f.seek(position)
                    data = f.read(count)
                    klog(KLOG_DBG, 'mount', f'read: {len(data)} bytes from handle {handle}')
                    return data
            except Exception as e:
                klog(KLOG_ERR, 'mount', f'read: error: {e}')
                return None

        def write(self, handle, position, data):
            if readonly:
                klog(KLOG_DBG, 'mount', f'write: denied (readonly mount)')
                return -1
            if handle not in mount_state['open_files']:
                return -1

            file_info = mount_state['open_files'][handle]
            try:
                with builtins.open(file_info['real_path'], 'rb') as f:
                    current = f.read()
                if isinstance(data, str):
                    data = data.encode()
                needed = position + len(data)
                if len(current) < needed:
                    current += b'\x00' * (needed - len(current))
                new_content = current[:position] + data + current[position + len(data):]
                with builtins.open(file_info['real_path'], 'wb') as f:
                    f.write(new_content)
                return len(data)
            except Exception as e:
                klog(KLOG_ERR, 'mount', f'write: error: {e}')
                return -1

        def close(self, handle):
            if handle in mount_state['open_files']:
                del mount_state['open_files'][handle]
            return 0

        def listdir(self, pathname="/"):
            real_path = self._to_real(pathname)
            klog(KLOG_DBG, 'mount', f'listdir: {pathname} -> {real_path}')

            if not os.path.isdir(real_path):
                return []

            entries = []
            try:
                for name in os.listdir(real_path):
                    full = os.path.join(real_path, name)
                    etype = 'directory' if os.path.isdir(full) else 'file'
                    entries.append((name, etype))
            except Exception as e:
                klog(KLOG_ERR, 'mount', f'listdir: error: {e}')

            return entries

        def stat(self, pathname):
            real_path = self._to_real(pathname)
            try:
                st = os.stat(real_path)
                return {
                    'size': st.st_size,
                    'is_dir': os.path.isdir(real_path),
                    'mtime': st.st_mtime
                }
            except Exception:
                return None

    klog(KLOG_DBG, 'realdir', f'factory: created mount root={root_path} at={mount_point} ro={readonly}')
    return RealDirMount()

# === VFS adapter functions (used by the root "/" mount) ===

def open(pathname, flags, mode=0o777):
    return realdir_open(pathname, flags, mode)

def read(handle, position, count):
    return realdir_read(handle, position, count)

def write(handle, position, data):
    return realdir_write(handle, position, data)

def close(handle):
    return realdir_close(handle)

def listdir(pathname="/"):
    return realdir_listdir(pathname)

def create(pathname, mode=0o644):
    return realdir_create(pathname, mode)

def realdir_stat(vfs_path):
    real_path = realdir_vfs_to_real(vfs_path)
    try:
        st = os.stat(real_path)
        return {
            'size': st.st_size,
            'is_dir': os.path.isdir(real_path),
            'mtime': st.st_mtime
        }
    except Exception as e:
        klog(KLOG_DBG, 'realdir', f'stat failed for {real_path}: {e}')
        return None

def stat(pathname):
    return realdir_stat(pathname)
