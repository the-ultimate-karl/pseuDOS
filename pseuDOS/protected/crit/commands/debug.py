__description__ = "kernel debug control"
__example__ = "debug 3"

def main():
    """Toggle kernel debug output level or view kernel log buffer."""

    level_names = {
        0: 'ERR  (errors only)',
        1: 'WARN (warnings + errors)',
        2: 'INFO (info + warnings + errors)',
        3: 'DBG  (everything - verbose debug output)'
    }

    if not args:
        # Show current level and usage
        level = kernel.syscall(116, 0)
        write(f"kernel log level: {level} - {level_names.get(level, 'unknown')}\n")
        write(f"\nusage:\n")
        write(f"  debug <level>   set log level (0=ERR, 1=WARN, 2=INFO, 3=DBG)\n")
        write(f"  debug on        shortcut for level 3 (show all debug output)\n")
        write(f"  debug off       shortcut for level 2 (hide debug output)\n")
        write(f"  debug dmesg     show kernel log buffer\n")
        write(f"  debug clear     clear kernel log buffer\n")
        return

    action = args[0].lower()

    if action == 'dmesg':
        count = int(args[1]) if len(args) > 1 else None
        if count:
            entries = kernel.syscall(116, 2, count)
        else:
            entries = kernel.syscall(116, 2)

        if isinstance(entries, list):
            if not entries:
                write("(kernel log buffer empty)\n")
            else:
                for entry in entries:
                    write(f"{entry}\n")
        else:
            write("error: could not read kernel log\n")

    elif action == 'clear':
        kernel.syscall(116, 3)
        write("kernel log buffer cleared\n")

    elif action == 'on':
        kernel.syscall(116, 1, 3)
        write("debug output enabled (level 3 - DBG)\n")

    elif action == 'off':
        kernel.syscall(116, 1, 2)
        write("debug output disabled (level 2 - INFO)\n")

    else:
        try:
            level = int(action)
            if 0 <= level <= 3:
                kernel.syscall(116, 1, level)
                write(f"log level set to {level} ({level_names.get(level, '?')})\n")
            else:
                write("error: level must be 0-3\n")
        except ValueError:
            write(f"error: unknown action '{action}'\n")
