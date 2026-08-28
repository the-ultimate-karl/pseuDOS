__description__ = "create a new file interactively"
__example__ = "mkfile new_file.txt"

def main():
    from crit.pathutils import resolve

    args = globals().get('args', [])

    if len(args) < 3 or args[1].lower() != "as":
        kernel.syscall(1, 1, "usage: mkfile <name> as <ext>\n")
        return
    
    name = args[0]
    ext = args[2]
    if not ext.startswith("."): 
        ext = "." + ext

    filename = name + ext

    full_vfs_path = resolve(kernel, filename)

    res = kernel.syscall(82, full_vfs_path)

    if res == 0:
        kernel.syscall(1, 1, f"new file '{full_vfs_path}' has been made\n")
    else:
        kernel.syscall(1, 1, f"failed to create '{full_vfs_path}'\n")