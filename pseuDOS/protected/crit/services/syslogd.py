import time

def main():
    kernel = globals().get('kernel')
    
    # Ensure /home/logs exists
    kernel.syscall(81, "/home/logs", 0o755)
    
    cursor = 0
    while True:
        res = kernel.syscall(116, 4, cursor)
        if isinstance(res, tuple) and len(res) == 2:
            logs, cursor = res
            if logs:
                log_str = "\n".join(logs) + "\n"
                
                # Create if doesn't exist
                if kernel.syscall(4, "/home/logs/syslog.log") == -1:
                    kernel.syscall(82, "/home/logs/syslog.log", 0o644)
                
                fd = kernel.syscall(2, "/home/logs/syslog.log", 1) # open for write
                if fd != -1:
                    kernel.syscall(9, fd, 0, 2) # seek to end
                    kernel.syscall(8, fd, log_str)
                    kernel.syscall(3, fd)
                
        time.sleep(3)
