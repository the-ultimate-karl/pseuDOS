__description__ = "test syscall 474"
__example__ = "test474"

def main():
    args = globals().get('args', [])
    
    kernel.syscall(1, 1, "=== Testing syscall 474 ===\n")
    
    # Test listing root
    kernel.syscall(1, 1, "Testing listdir('/'):\n")
    result = kernel.syscall(474, "/")
    kernel.syscall(1, 1, f"Result type: {type(result)}\n")
    kernel.syscall(1, 1, f"Result value: {result}\n")
    
    # Test listing /home
    kernel.syscall(1, 1, "\nTesting listdir('/home'):\n")
    result = kernel.syscall(474, "/home")
    kernel.syscall(1, 1, f"Result type: {type(result)}\n")
    kernel.syscall(1, 1, f"Result value: {result}\n")
    
    # Test listing non-existent
    kernel.syscall(1, 1, "\nTesting listdir('/nonexistent'):\n")
    result = kernel.syscall(474, "/nonexistent")
    kernel.syscall(1, 1, f"Result type: {type(result)}\n")
    kernel.syscall(1, 1, f"Result value: {result}\n")