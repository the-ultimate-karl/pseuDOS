__description__ = "read and print file content"
__example__ = "cat file.txt"

# cat :3
# rawr :3

def main():
    from crit.pathutils import resolve
    
    args = globals().get('args', [])
    if not args:
        kernel.syscall(1, 1, "usage: cat <filename>\n")
        return
    
    for filename in args:
        full_path = resolve(kernel, filename)
        fd = kernel.syscall(2, full_path, 0)
        
        if fd == -1:
            kernel.syscall(1, 1, f"cat: {full_path}: No such file\n")
            continue
        
        last_chunk = b""
        
        while True:
            data = kernel.syscall(7, fd, 33554432) # 32 MB :3
            if not data or data == -1 or len(data) == 0:
                break
            
            last_chunk = data
            output = data.decode() if isinstance(data, bytes) else str(data)
            kernel.syscall(1, 1, output)
        
        if last_chunk:
            last_char = last_chunk[-1:] 
            if last_char != b'\n' and last_char != b'\r':
                kernel.syscall(1, 1, "\n")
        
        kernel.syscall(3, fd)
