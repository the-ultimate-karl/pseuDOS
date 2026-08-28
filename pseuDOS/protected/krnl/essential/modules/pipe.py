syscall_number = 22

import threading

class PipeDriver:
    def __init__(self):
        self.buffer = b""
        self.read_closed = False
        self.write_closed = False
        self.cond = threading.Condition()
        
    def read(self, inode, position, count):
        with self.cond:
            while not self.buffer and not self.write_closed:
                self.cond.wait(timeout=0.1) # timeout to allow checking for signals/kill
                
            if not self.buffer and self.write_closed:
                return b""
                
            data = self.buffer[:count]
            self.buffer = self.buffer[count:]
            return data
            
    def write(self, inode, position, data):
        with self.cond:
            if self.read_closed:
                return -1 # broken pipe
                
            if isinstance(data, str):
                data = data.encode()
                
            self.buffer += data
            self.cond.notify_all()
            return len(data)
            
    def close(self, inode):
        with self.cond:
            if inode == 0: # read end
                self.read_closed = True
            elif inode == 1: # write end
                self.write_closed = True
            self.cond.notify_all()
        return 0

def sys_pipe(kdata):
    from klog import klog, KLOG_DBG
    try:
        from filesystem.vfs import vfs_state
    except ImportError:
        return -1, -1
        
    driver = PipeDriver()
    
    # create read end
    read_fd = vfs_state["next_fd"]
    vfs_state["next_fd"] += 1
    vfs_state["open_files"][read_fd] = {
        'inode': 0, # 0 means read end
        'driver': driver,
        'position': 0,
        'path': 'pipe:read'
    }
    
    # create write end
    write_fd = vfs_state["next_fd"]
    vfs_state["next_fd"] += 1
    vfs_state["open_files"][write_fd] = {
        'inode': 1, # 1 means write end
        'driver': driver,
        'position': 0,
        'path': 'pipe:write'
    }
    
    klog(KLOG_DBG, 'pipe', f'created pipe r_fd={read_fd} w_fd={write_fd}')
    return read_fd, write_fd

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'pipe module initialized')
