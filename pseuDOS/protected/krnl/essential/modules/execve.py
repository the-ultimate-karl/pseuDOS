syscall_number = 59
import threading
import sys

def _run_process(kdata, pid, code, args, cmd_name):
    from klog import klog, KLOG_DBG, KLOG_ERR
    
    # Environment provided to the command
    def _write(text):
        handler = kdata['syscall_table'].get(1)
        if handler:
            handler(kdata, 1, text)
            
    def _read():
        handler = kdata['syscall_table'].get(0)
        if handler:
            res = handler(kdata, 0, 256)
            return res if res != -1 else ""
        return ""
        
    def _exit(code=0):
        handler = kdata['syscall_table'].get(60)
        if handler:
            handler(kdata, code)
        sys.exit(0) # kill the thread

    import os
    import sys
    _coreutils_path = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'coreutils'))
    if _coreutils_path not in sys.path:
        sys.path.insert(0, _coreutils_path)

    try:
        from thebucket import RED, YELLOW, GREEN, CYAN, RESET, BOLD, BLUE, WHITE
    except ImportError:
        RED = YELLOW = GREEN = CYAN = RESET = BOLD = BLUE = WHITE = ""

    env = {
        'write': _write,
        'read': _read,
        'kernel': sys.modules.get('kernel') or sys.modules.get('krnl.essential.kernel'),
        'args': args,
        'cmd': cmd_name,
        'exit': _exit,
        'RED': RED,
        'YELLOW': YELLOW,
        'GREEN': GREEN,
        'CYAN': CYAN,
        'RESET': RESET,
        'BOLD': BOLD,
        'BLUE': BLUE,
        'WHITE': WHITE
    }

    try:
        klog(KLOG_DBG, 'spawn', f'starting execution of PID {pid}')
        exec(code, env)
        if 'main' in env:
            env['main']()
    except SystemExit:
        pass
    except Exception as e:
        klog(KLOG_ERR, 'spawn', f'PID {pid} crashed: {e}')
    finally:
        # cleanup process state
        if pid in kdata['processes']:
            kdata['processes'][pid]['state'] = 'zombie'
            klog(KLOG_DBG, 'spawn', f'PID {pid} became zombie')

def sys_execve(kdata, filepath, args):
    from klog import klog, KLOG_DBG, KLOG_ERR, KLOG_INFO
    from filesystem.vfs import vfs_open, vfs_read, vfs_close
    
    fd = vfs_open(filepath, 0)
    if fd == -1:
        return -1
        
    full_code = b""
    while True:
        chunk = vfs_read(fd, 33554432)
        if not chunk or chunk == -1:
            break
        full_code += chunk
    vfs_close(fd)
    
    if not full_code:
        return -1
        
    code_str = full_code.decode('utf-8') if isinstance(full_code, bytes) else str(full_code)
    
    # create new process
    child_pid = kdata['next_pid']
    kdata['next_pid'] += 1
    parent_pid = kdata['get_current_pid']()
    
    cmd_name = filepath.split('/')[-1].replace('.py', '')
    
    child_proc = {
        'pid': child_pid,
        'state': 'running',
        'name': cmd_name,
        'parent': parent_pid,
        'memory': [],
        'files': []
    }
    
    kdata['processes'][child_pid] = child_proc
    
    # spawn thread
    klog(KLOG_INFO, 'exec', f'executing {cmd_name} with args {args}')
    t = threading.Thread(target=_run_process, args=(kdata, child_pid, code_str, args, cmd_name), daemon=True)
    t.start()
    kdata['thread_pids'][t.ident] = child_pid
    
    return child_pid

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'execve (spawn) module initialized')
