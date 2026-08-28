def is_protected(vfs_path):
    """Delegate to VFS's canonical is_protected implementation."""
    try:
        from filesystem.vfs import is_protected as _vfs_is_protected
        return _vfs_is_protected(vfs_path)
    except ImportError:
        # Fallback if VFS isn't loaded yet
        path = vfs_path.lower().replace('\\', '/').rstrip('/')
        if not path.startswith('/'):
            path = '/' + path
        forbidden = ['/protected', '/coreutils', '/commands']
        for zone in forbidden:
            if path == zone or path.startswith(zone + '/'):
                return True
        return False