__description__ = "print the system date and time"
__example__ = "date"

import time

def main():
    kernel = globals().get('kernel')
    current_time = time.ctime()
    kernel.syscall(1, 1, current_time + "\n")
