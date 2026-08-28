import re

with open(r'h:\repositories\backup repository\pseuDOS-backup\rewrite-05\pseuDOS\protected\krnl\essential\filesystem\vfs.py', 'r', encoding='utf-8') as f:
    content = f.read()

helper = """
def vfs_normalize_path(pathname):
    if not pathname: return '/'
    current_cwd = vfs_state.get('cwd', '/')
    if not pathname.startswith('/'):
        full_path = current_cwd.rstrip('/') + '/' + pathname
    else:
        full_path = pathname
        
    import os
    clean_path = os.path.normpath(full_path).replace("\\\\", "/")
    if not clean_path.startswith('/'): clean_path = '/' + clean_path
    if len(clean_path) > 1: clean_path = clean_path.rstrip('/')
    return clean_path
"""

# Insert helper after is_protected
content = content.replace("def vfs_open(pathname, flags, mode=0o777):", helper + "\ndef vfs_open(pathname, flags, mode=0o777):")

funcs = [
    "def vfs_open(pathname, flags, mode=0o777):",
    "def vfs_mkdir(pathname, mode):",
    "def vfs_unlink(pathname):",
    "def vfs_rmdir(pathname):",
    "def vfs_listdir(pathname=\"/\"):",
    "def vfs_create(pathname, mode=0o644):",
    "def vfs_stat(pathname):"
]

for func in funcs:
    replacement = func + "\n    pathname = vfs_normalize_path(pathname)"
    content = content.replace(func, replacement)

# For vfs_chdir, replace its manual normalization
chdir_old = """def vfs_chdir(pathname):
    current_cwd = vfs_state.get('cwd', '/')

    if not pathname.startswith('/'):
        full_path = os.path.join(current_cwd, pathname)
    else:
        full_path = pathname

    clean_path = os.path.normpath(full_path).replace("\\\\", "/")

    klog(KLOG_DBG, 'vfs', f'chdir: {pathname} -> normalized: {clean_path}')

    if not clean_path.startswith('/'): clean_path = '/' + clean_path
    if len(clean_path) > 1: clean_path = clean_path.rstrip('/')"""
    
chdir_new = """def vfs_chdir(pathname):
    clean_path = vfs_normalize_path(pathname)
    klog(KLOG_DBG, 'vfs', f'chdir: {pathname} -> normalized: {clean_path}')"""

content = content.replace(chdir_old, chdir_new)

with open(r'h:\repositories\backup repository\pseuDOS-backup\rewrite-05\pseuDOS\protected\krnl\essential\filesystem\vfs.py', 'w', encoding='utf-8') as f:
    f.write(content)

print("vfs.py updated")
