__description__ = "print or control the kernel ring buffer"
__example__ = "dmesg [export|clear|tail <n>|count|level]"

def main():
    args = globals().get('args', [])
    kernel = globals().get('kernel')

    if not args:
        # print all
        logs = kernel.syscall(116, 2)
        if logs:
            for log in logs:
                kernel.syscall(1, 1, log + "\n")
        return

    arg = args[0].lower()

    if arg == "export":
        # Create /home/logs directory if it doesn't exist
        kernel.syscall(81, "/home/logs", 0o755)
        
        logs = kernel.syscall(116, 2)
        if logs:
            log_str = "\n".join(logs) + "\n"
            
            kernel.syscall(82, "/home/logs/kernel_dmesg.log", 0o644)
            fd = kernel.syscall(2, "/home/logs/kernel_dmesg.log", 1) # open for write
            if fd != -1:
                kernel.syscall(8, fd, log_str)
                kernel.syscall(3, fd)
                kernel.syscall(1, 1, "dmesg exported to /home/logs/kernel_dmesg.log\n")
            else:
                kernel.syscall(1, 1, "failed to open /home/logs/kernel_dmesg.log for writing\n")
        else:
            kernel.syscall(1, 1, "dmesg buffer is empty\n")

    elif arg == "clear":
        kernel.syscall(116, 3)
        kernel.syscall(1, 1, "dmesg buffer cleared\n")

    elif arg == "tail":
        n = 10
        if len(args) > 1:
            try:
                n = int(args[1])
            except ValueError:
                kernel.syscall(1, 1, "invalid number for tail\n")
                return
        logs = kernel.syscall(116, 2, n)
        if logs:
            for log in logs:
                kernel.syscall(1, 1, log + "\n")

    elif arg == "count":
        logs = kernel.syscall(116, 2)
        kernel.syscall(1, 1, f"Total log entries: {len(logs) if logs else 0}\n")

    elif arg == "level":
        lvl = kernel.syscall(116, 0)
        kernel.syscall(1, 1, f"Current log level threshold: {lvl}\n")
    
    else:
        kernel.syscall(1, 1, "Unknown argument. Available: export, clear, tail [n], count, level\n")
