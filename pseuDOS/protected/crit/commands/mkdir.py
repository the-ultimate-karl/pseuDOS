__description__ = "create a directory"
__example__ = "mkdir new_dir"

def main():
    from crit.pathutils import resolve

    args = globals().get('args', [])
    if not args:
        kernel.syscall(1, 1, "usage: mkdir <dirname>\n")
        return
    
    # Use new global logic
    target = resolve(kernel, args[0])
    
    res = kernel.syscall(81, target)
    if res == 0:
        kernel.syscall(1, 1, f"Created directory: {target}\n")
    else:
        kernel.syscall(1, 1, f"mkdir: failed to create '{target}'\n")