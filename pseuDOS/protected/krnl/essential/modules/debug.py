syscall_number = 116  # Modeled after Linux syslog

def sys_debug(kdata, action, *args):
    """Debug/logging control syscall.
    action=0: get current log level
    action=1: set log level (args[0] = level 0-3)
    action=2: get log buffer (dmesg), optional args[0] = count
    action=3: clear log buffer
    """
    from klog import (klog as _klog, set_level, get_level, get_buffer,
                      clear_buffer, get_level_name, KLOG_INFO)

    if action == 0:  # GET level
        return get_level()

    elif action == 1:  # SET level
        if args:
            old = set_level(args[0])
            _klog(KLOG_INFO, 'klog', f'log level changed: {get_level_name(old)} -> {get_level_name()}')
            return get_level()
        return -1

    elif action == 2:  # DMESG - get buffer
        count = args[0] if args else None
        return get_buffer(count)

    elif action == 3:  # CLEAR buffer
        clear_buffer()
        return 0

    elif action == 4:  # Get logs since cursor
        cursor = args[0] if args else 0
        from klog import get_buffer_since
        return get_buffer_since(cursor)

    elif action == 5:  # Log as KLOG_KINF
        if args:
            from klog import klog as _klog, KLOG_KINF
            _klog(KLOG_KINF, 'syslogd', args[0])
            return 0
        return -1

    return -1

def kernel_init(kdata):
    from klog import klog, KLOG_INFO
    klog(KLOG_INFO, 'kernel', 'debug/logging control syscall initialized')
