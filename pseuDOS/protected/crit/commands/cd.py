__description__ = "change directory"
__example__ = "cd /path/to/dir"

def main():
    args = globals().get('args', [])

    if not args:
        target = "/"
    else:
        target = args[0]

    if not target.startswith('/'):
        current_cwd = kernel.syscall(79)
        if current_cwd == -1: current_cwd = "/"

        if current_cwd == "/":
            target = "/" + target
        else:
            target = current_cwd.rstrip('/') + "/" + target
    result = kernel.syscall(80, target)
    
    if result != 0:
        kernel.syscall(1, 1, f"cd: {target}: No such directory\n")