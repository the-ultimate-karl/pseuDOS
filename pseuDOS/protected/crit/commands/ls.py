__description__ = "list directory contents"
__example__ = "ls /path/to/dir"

def main():
    pass
    args = globals().get('args', [])
    path = args[0] if len(args) > 0 else (kernel.syscall(79) or '/')
    
    # Call Syscall 474
    entries = kernel.syscall(474, path)

    kernel.syscall(1, 1, f"directory: {path}\n")
    kernel.syscall(1, 1, "=" * 30 + "\n")

    if not entries:
        kernel.syscall(1, 1, f"{YELLOW}  (empty){RESET}\n")
        return

    try:
        entries.sort()
    except:
        pass

    for entry in entries:
        # Parse "name:type" string from syscall
        if isinstance(entry, str) and ":" in entry:
            name, etype = entry.split(":", 1)
        elif isinstance(entry, (list, tuple)):
            name, etype = entry[0], entry[1]
        else:
            name, etype = str(entry), "file"

        # Display folders with a slash and color
        if etype == "directory":
            kernel.syscall(1, 1, f"  {BOLD}{BLUE}{name}/{RESET}\n")
        else:
            kernel.syscall(1, 1, f"  {GREEN}{name}{RESET}\n")

if __name__ == "__main__":
    main()