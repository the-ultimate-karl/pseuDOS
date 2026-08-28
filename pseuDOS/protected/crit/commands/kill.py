__description__ = "send SIGKILL to a process"
__example__ = "kill <pid>"

def main():
    kernel = globals().get('kernel')
    args = globals().get('args', [])
    
    if not args:
        kernel.syscall(1, 1, "kill: missing operand\n")
        kernel.syscall(1, 1, f"Usage: {__example__}\n")
        return
        
    try:
        pid = int(args[0])
    except ValueError:
        kernel.syscall(1, 1, "kill: invalid PID format\n")
        return
        
    res = kernel.syscall(62, pid, 9) # sys_kill with SIGKILL
    if res == -1:
        kernel.syscall(1, 1, f"kill: failed to kill PID {pid} (no such process or permission denied)\n")
    else:
        kernel.syscall(1, 1, f"kill: sent SIGKILL to PID {pid}\n")
