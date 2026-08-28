__description__ = "shut down the system completely"
__example__ = "shutdown"

import os

def main():
    kernel = globals().get('kernel')
    kernel.syscall(1, 1, "System halted.\n")
    os._exit(0)
