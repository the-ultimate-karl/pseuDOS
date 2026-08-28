__description__ = "modify file content"
__example__ = "modfile file.txt"

def main():
    from crit.pathutils import resolve
    import builtins
    
    args = globals().get('args', [])

    if not args:
        kernel.syscall(1, 1, "usage: modfile <filename>\n")
        return
    
    raw_filename = args[0]
    full_path = resolve(kernel, raw_filename)

    # --- READ ---
    fd_read = kernel.syscall(2, full_path, 0)
    if fd_read != -1:
        data = kernel.syscall(7, fd_read, 4096)
        # Use simple addition instead of f-strings
        old_content = data.decode() if isinstance(data, bytes) else str(data)
        kernel.syscall(1, 1, "old contents: " + old_content + "\n")
        kernel.syscall(3, fd_read)

    # --- INPUT ---
    new_content = builtins.input("new contents: ")
    
    # --- WRITE ---
    fd_write = kernel.syscall(2, full_path, 1)
    
    if fd_write != -1:
        res = kernel.syscall(8, fd_write, new_content)
        kernel.syscall(3, fd_write)
        
        if res >= 0:
            kernel.syscall(1, 1, "Saved successfully\n")
        else:
            kernel.syscall(1, 1, "Write failed\n")
    else:
        kernel.syscall(1, 1, "Open for write failed\n")