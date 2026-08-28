__description__ = "create an empty file"
__example__ = "touch file.txt"

def main():
    from crit.pathutils import resolve

    args = globals().get('args', [])
    if not args:
        kernel.syscall(1, 1, "usage: touch <filename>\n")
        return

    target = resolve(kernel, args[0])

    fd = kernel.syscall(2, target, 0, 0o644)

    if fd < 0:
        res = kernel.syscall(82, target)
        if res == 0:
            kernel.syscall(1, 1, f"touch: created file '{args[0]}'\n")
        else:
            kernel.syscall(1, 1, f"touch: failed to create '{args[0]}'\n")
    else:
        kernel.syscall(3, fd)
        kernel.syscall(1, 1, f"touch: updated timestamp for '{args[0]}'\n")