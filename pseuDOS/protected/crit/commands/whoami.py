__description__ = "print current user name"
__example__ = "whoami"

def main():
    args = globals().get('args', [])
    kernel.syscall(1, 1, "eternal root lol\n")