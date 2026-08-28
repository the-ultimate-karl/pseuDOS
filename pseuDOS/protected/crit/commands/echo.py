__description__ = "print text"
__example__ = "echo hello world"

def main():
    args = globals().get('args', [])
    output = ' '.join(args) if args else ''
    kernel.syscall(1, 1, output + '\n')