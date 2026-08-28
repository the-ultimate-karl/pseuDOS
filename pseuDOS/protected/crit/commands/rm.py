__description__ = "remove a file or directory"
__example__ = "rm file.txt"

import errno

def main():
    from crit.pathutils import resolve
    from crit.protection import is_protected 

    args = globals().get('args', [])
    if not args:
        kernel.syscall(1, 1, "usage: rm <path>\n")
        return
    
    target = resolve(kernel, args[0])

    # --- STEP 1: SAFETY CHECK ---
    if is_protected(target):
        kernel.syscall(1, 1, f"rm: permission denied: {target} is a core system file\n")
        return

    # --- STEP 2: ATTEMPT REMOVAL ---
    # Try unlink (syscall 10) first
    res = kernel.syscall(10, target)  # unlink
    if res != 0:
        res = kernel.syscall(84, target)  # rmdir

    if res == 0:
        kernel.syscall(1, 1, f"removed '{args[0]}'\n")
    elif res == -errno.ENOTEMPTY:  # You'd need to import errno or define constants
        kernel.syscall(1, 1, f"rm: cannot remove '{args[0]}': Directory not empty\n")
    elif res == -errno.EBUSY:
        kernel.syscall(1, 1, f"rm: cannot remove '{args[0]}': File in use\n")
    else:
        kernel.syscall(1, 1, f"rm: cannot remove '{args[0]}': No such file or directory\n")