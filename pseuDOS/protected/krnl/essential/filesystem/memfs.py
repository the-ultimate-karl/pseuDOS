import os
from klog import klog, KLOG_DBG, KLOG_INFO, KLOG_WARN, KLOG_ERR

memfs_state = None  # Start as None, initialize in memfs_init()

def memfs_init():
    global memfs_state

    protected_path = os.path.abspath(
        os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', '..')
    )

    if not os.path.exists(protected_path):
        klog(KLOG_ERR, 'memfs', f'cannot find {protected_path}, memfs init failed')
        # DIRECT PANIC - panic module not loaded yet
        print("\n\033[1;31mKERNEL PANIC\033[0m")
        print("Not syncing: VFS: unable to mount root filesystem")
        print("\033[1;31mSystem halted.\033[0m")
        import sys
        import builtins
        sys.exit(1)
        return None  # Never reached

    klog(KLOG_DBG, 'memfs', f'initialized with protected path: {protected_path}')

    memfs_state = {
        'type': 'real_directory',
        'path': protected_path,
        'exists': True,
        'real_fs': True,
        'inodes': {},
        'directory': {},
        'next_inode': 1000,
    }

    # mount protected path at root inode 1
    memfs_state['directory']['/'] = 1
    memfs_state['inodes'][1] = {
        'type': 'directory',
        'real_path': protected_path,
        'is_dir': True,
        'size': 0,
        'mtime': os.path.getmtime(protected_path),
        'contents': {
            '.': 1,
            '..': 1,
        },
    }

    # create standard subdirectories (/home, /tmp)
    home_inode = memfs_state['next_inode']
    memfs_state['next_inode'] += 1
    tmp_inode = memfs_state['next_inode']
    memfs_state['next_inode'] += 1

    memfs_state['directory']['/home'] = home_inode
    memfs_state['directory']['/tmp'] = tmp_inode

    memfs_state['inodes'][home_inode] = {
        'type': 'directory',
        'contents': {'.': home_inode, '..': 1},
        'is_dir': True,
        'size': 0,
    }

    memfs_state['inodes'][tmp_inode] = {
        'type': 'directory',
        'contents': {'.': tmp_inode, '..': 1},
        'is_dir': True,
        'size': 0,
    }

    # add a sample regular file in /home for smoke tests
    file_inode = memfs_state['next_inode']
    memfs_state['next_inode'] += 1
    memfs_state['directory']['/home/hello.txt'] = file_inode
    memfs_state['inodes'][file_inode] = {
        'type': 'regular',
        'content': b'Hello from memfs!\n',
        'size': len(b'Hello from memfs!\n')
    }

    klog(KLOG_INFO, 'memfs', f'mounted at / with root inode 1')
    klog(KLOG_DBG, 'memfs', f'path: {protected_path}')

    return memfs_state

def memfs_open(pathname, flags, mode):
    if memfs_state is None:
        return -1

    if not pathname.startswith("/"):
        pathname = '/' + pathname
    directory = memfs_state.get('directory', {})
    klog(KLOG_DBG, 'memfs', f'open: {pathname}')
    return directory.get(pathname, -1)

def memfs_read(inode, position, count):
    if memfs_state is None or inode not in memfs_state.get("inodes", {}):
        return None

    data = memfs_state["inodes"][inode]
    t = data.get('type')
    if t == 'regular':
        content = data.get('content', b'')
        if position >= len(content):
            return b''
        klog(KLOG_DBG, 'memfs', f'read: inode={inode} pos={position} count={count}')
        return content[position:position+count]

    # If this inode references a real file on disk, try to read it
    real_path = data.get('real_path')
    if real_path and os.path.isfile(real_path):
        import builtins
        try:
            with builtins.open(real_path, 'rb') as f:
                f.seek(position)
                return f.read(count)
        except Exception:
            return None

    return None

def memfs_write(inode, position, data):
    if memfs_state is None or inode not in memfs_state.get("inodes", {}):
        return -1

    file_data = memfs_state["inodes"][inode]

    if file_data.get('type') == 'regular':
        current = file_data.get('content', b'')
        if isinstance(data, str):
            data = data.encode()
        needed_length = position + len(data)
        if len(current) < needed_length:
            current += b'\x00' * (needed_length - len(current))

        new_content = current[:position] + data + current[position + len(data):]
        file_data['content'] = new_content
        file_data['size'] = len(new_content)
        klog(KLOG_DBG, 'memfs', f'write: inode={inode} wrote {len(data)} bytes')
        return len(data)

    # Do not allow writes to real mounted directories/files by default
    if file_data.get('real_path'):
        return -1

    return -1

def memfs_close(inode):
    return 0

def memfs_mkdir(pathname, mode=0o755):
    if memfs_state is None:
        return -1

    from filesystem.vfs import is_protected
    if is_protected(pathname):
        klog(KLOG_WARN, 'memfs', f'mkdir: denied - protected: {pathname}')
        return -1

    if not pathname.startswith("/"):
        pathname = '/' + pathname

    if pathname in memfs_state['directory']:
        return -1  # Already exists

    # Create inode
    inode = memfs_state['next_inode']
    memfs_state['next_inode'] += 1

    memfs_state['directory'][pathname] = inode
    memfs_state['inodes'][inode] = {
        'type': 'directory',
        'contents': {'.': inode, '..': 1},
        'is_dir': True,
        'size': 0,
    }

    klog(KLOG_DBG, 'memfs', f'mkdir: created {pathname} inode={inode}')
    return 0

def memfs_unlink(pathname):
    if memfs_state is None:
        return -1

    from filesystem.vfs import is_protected
    if is_protected(pathname):
        klog(KLOG_WARN, 'memfs', f'unlink: denied - protected: {pathname}')
        return -1  # BUG FIX: was missing this return!

    if not pathname.startswith("/"):
        pathname = '/' + pathname

    if pathname not in memfs_state['directory']:
        return -1

    inode = memfs_state['directory'][pathname]
    inode_data = memfs_state['inodes'].get(inode)

    # Don't delete directories or protected files
    if inode_data and (inode_data.get('type') == 'directory' or inode_data.get('protected')):
        return -1

    del memfs_state['directory'][pathname]
    if inode in memfs_state['inodes']:
        del memfs_state['inodes'][inode]

    klog(KLOG_DBG, 'memfs', f'unlink: removed {pathname}')
    return 0

def memfs_create(pathname, mode=0o644):
    if memfs_state is None:
        return -1

    if not pathname.startswith("/"):
        pathname = '/' + pathname

    if pathname in memfs_state['directory']:
        return -1  # Already exists

    inode = memfs_state['next_inode']
    memfs_state['next_inode'] += 1

    memfs_state['directory'][pathname] = inode
    memfs_state['inodes'][inode] = {
        'type': 'regular',
        'content': b'',
        'size': 0,
    }

    klog(KLOG_DBG, 'memfs', f'create: {pathname} inode={inode}')
    return inode

def memfs_listdir(pathname="/"):
    if memfs_state is None: return []
    if not pathname.startswith("/"): pathname = '/' + pathname

    klog(KLOG_DBG, 'memfs', f'listdir: {pathname}')

    # 1. Find the Inode for this path
    dir_inode = memfs_state.get('directory', {}).get(pathname)
    if dir_inode is None: return []

    dir_data = memfs_state.get('inodes', {}).get(dir_inode, {})
    entries = set() # Use a set to prevent duplicates

    # 2. Add Physical Files (from Disk)
    real_path = dir_data.get('real_path')
    if not real_path:
        root_real = memfs_state['inodes'][1].get('real_path')
        if root_real:
            real_path = os.path.join(root_real, pathname.lstrip('/'))

    if real_path and os.path.isdir(real_path):
        try:
            for name in os.listdir(real_path):
                full = os.path.join(real_path, name)
                etype = 'directory' if os.path.isdir(full) else 'file'
                entries.add((name, etype))
        except: pass

    # 3. Add Virtual Files (from Memory)
    search_path = pathname if pathname.endswith('/') else pathname + '/'
    for full_path, inode in memfs_state.get('directory', {}).items():
        if full_path.startswith(search_path):
            remaining = full_path[len(search_path):]
            if remaining and '/' not in remaining:
                inode_data = memfs_state['inodes'].get(inode, {})
                etype = 'directory' if inode_data.get('type') == 'directory' else 'file'
                entries.add((remaining, etype))

    klog(KLOG_DBG, 'memfs', f'listdir: {len(entries)} entries')
    return list(entries)

# Adapter functions expected by vfs (open/read/write/close)
def open(pathname, flags, mode=0o777):
    return memfs_open(pathname, flags, mode)

def read(inode, position, count):
    return memfs_read(inode, position, count)

def write(inode, position, data):
    return memfs_write(inode, position, data)

def close(inode):
    return memfs_close(inode)

def listdir(pathname="/"):
    return memfs_listdir(pathname)

def create(pathname, mode=0o644):
    return memfs_create(pathname, mode)

def mkdir(pathname, mode=0o755):
    return memfs_mkdir(pathname, mode)

def unlink(pathname):
    return memfs_unlink(pathname)

def memfs_stat(pathname):
    if memfs_state is None: return None
    if not pathname.startswith("/"): pathname = '/' + pathname
    inode = memfs_state.get('directory', {}).get(pathname)
    if inode is None: return None
    data = memfs_state.get('inodes', {}).get(inode)
    if not data: return None
    
    return {
        'size': data.get('size', 0),
        'is_dir': data.get('type') == 'directory',
        'mtime': data.get('mtime', 0)
    }

def stat(pathname):
    return memfs_stat(pathname)
