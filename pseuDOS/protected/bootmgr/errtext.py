import sys
def error_boot(exception=None):
    sys.stdout.write("pseuDOS Boot Loader failed to start. A recent change in the structure of\n")
    sys.stdout.write("pseuDOS might be the culprit.\n\n")
    
    if exception:
        sys.stdout.write("")

    if isinstance(exception, ImportError):
        sys.stdout.write("error: /protected/krnl/essential/kernel.py: cannot mount kernel image\n\n")
    
    elif isinstance(exception, ModuleNotFoundError):
        sys.stdout.write("error: cannot find /protected/krnl/essential/kernel.py: No such file or directory\n\n")
    
    elif isinstance(exception, TypeError) or isinstance(exception, NameError) or isinstance(exception, SyntaxError):
        sys.stdout.write("error: cannot verify integrity of the kernel. might be corrupted\n\n")

    sys.stdout.write("Please re-check your project folder and confirm there are no\n")
    sys.stdout.write("import typos in the bootmgfw.py code.\n")
    sys.stdout.write("System halted!\n\n")
    sys.stdout.write("====================================\n\n")