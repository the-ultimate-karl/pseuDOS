__description__ = "show this help message"
__example__ = "help"

def main():
    boot = kernel
    cmd_dir = "/protected/crit/commands"
    cmd_files = boot.syscall(474, cmd_dir) # listdir
    
    internals = [("exit", "exit the shell")]
    externals = []
    
    if isinstance(cmd_files, list):
        for f in cmd_files:
            name = f[0] if isinstance(f, (tuple, list)) else f
            if name.endswith(".py") and not name.startswith("__"):
                cmd_name = name[:-3]
                path = f"{cmd_dir}/{name}"
                fd = boot.syscall(2, path, 0) # sys_open
                desc = "no description"
                example = "no example"
                if fd != -1:
                    content = boot.syscall(7, fd, 1024) # sys_fs_read
                    if content and isinstance(content, bytes):
                        content = content.decode('utf-8', errors='ignore')
                    elif content:
                        content = str(content)
                    boot.syscall(3, fd) # sys_fs_close
                    
                    if content:
                        for line in content.split('\n'):
                            if line.startswith('__description__'):
                                parts = line.split('=', 1)
                                if len(parts) == 2:
                                    desc = parts[1].strip().strip('\'"')
                            elif line.startswith('__example__'):
                                parts = line.split('=', 1)
                                if len(parts) == 2:
                                    example = parts[1].strip().strip('\'"')
                externals.append((cmd_name, desc, example))

    boot.syscall(1, 1, f"{BOLD}{CYAN}pseuDOS help system{RESET}\n")
    boot.syscall(1, 1, f"{CYAN}================================{RESET}\n")
    
    for cmd, desc in internals:
        boot.syscall(1, 1, f"- {GREEN}{cmd.ljust(15)}{RESET} : {desc}\n")
        
    for cmd, desc, example in sorted(externals):
        boot.syscall(1, 1, f"- {GREEN}{cmd.ljust(15)}{RESET} : {desc}\n")
        boot.syscall(1, 1, f"  {YELLOW}Example:{RESET} {example}\n")

    boot.syscall(1, 1, f"{CYAN}================================{RESET}\n")

if __name__ == "__main__":
    main()