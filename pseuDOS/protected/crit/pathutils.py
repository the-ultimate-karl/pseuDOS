def resolve(kernel, path):
    if path.startswith('/'):
        full_path = path
    else:
        cwd = kernel.syscall(79) or "/"
        import os
        full_path = os.path.join(cwd, path).replace("\\", "/")
    
    import os
    return os.path.normpath(full_path).replace("\\", "/")