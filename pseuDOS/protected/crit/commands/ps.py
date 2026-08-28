__description__ = "list currently running processes"
__example__ = "ps"

def main():
    kernel = globals().get('kernel')
    procs = kernel.syscall(117)
    
    if procs == -1:
        kernel.syscall(1, 1, "ps: syscall 117 not available\n")
        return
        
    kernel.syscall(1, 1, "PID   | STATE        | NAME\n")
    kernel.syscall(1, 1, "-"*40 + "\n")
    
    for p in procs:
        pid_str = str(p['pid']).ljust(5)
        state_str = str(p['state']).ljust(12)
        name_str = str(p['name'])
        kernel.syscall(1, 1, f"{pid_str} | {state_str} | {name_str}\n")
