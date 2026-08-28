import sys
import os
import time
if 'kernel' in globals():
    boot = kernel
else:
    try:
        from krnl.essential import kernel as boot
    except ImportError as e:
        print(f"shell: cannot import critical shell modules")
        sys.exit(1)

def kwrite(text):
    boot.syscall(1, 1, text)

def kread():
    data = boot.syscall(0, 0, 256)
    return data if data != -1 else ""

def find_command(cmd_name):
    return f"/commands/{cmd_name}.py"

def load_command(cmd_name, args):
    filepath = find_command(cmd_name)
    if not filepath:
        boot.syscall(1, 1, f"{RED}{cmd_name}: command not found{RESET}\n")
        return
        
    # execute the command in a new process thread
    # 59 is sys_execve
    pid = boot.syscall(59, filepath, args)
    if pid == -1:
        boot.syscall(1, 1, f"{RED}shell: failed to execute {cmd_name}{RESET}\n")
        return
        
    # wait for the command to finish
    while True:
        r = boot.syscall(61, -1, 0, 0)
        if r == pid:
            break
            
        time.sleep(0.05)

def shell_loop():
    # Get initial info
    pid = boot.kdata['get_current_pid']() if hasattr(boot, 'kdata') else 0
    kwrite(f"{BOLD}{GREEN}successfully started pseuDOS shell v2{RESET}\n")
    kwrite(f"{CYAN}PID: {pid} | Kernel: {len(boot.kdata.get('syscall_table', {}))} syscalls{RESET}\n")
    kwrite(f"{YELLOW}type exit to quit{RESET}\n\n")

    while True:
        cwd = boot.syscall(79) or "/"
        
        kwrite(f"{BOLD}{GREEN}{cwd}{RESET} {BLUE}${RESET} ")

        line = kread().strip()
        if not line:
            if not sys.stdin.isatty():
                # EOF reached on piped input
                boot.syscall(60, 0)
                break
            continue

        parts = line.split()
        cmd = parts[0]
        args = parts[1:] if len(parts) > 1 else []

        if cmd == "exit":
            kwrite("exiting\n")
            boot.syscall(60, 0)
            break

        load_command(cmd, args)
def main():
    try:
        shell_loop()
    except KeyboardInterrupt:
        kwrite("\n^C\n")
        boot.syscall(60, 130)
    except Exception as e:
        boot.syscall(1, 1, f"Kernel panic: Not syncing: {e}")
        boot.syscall(60, 1)
if __name__ == "__main__":
    main()