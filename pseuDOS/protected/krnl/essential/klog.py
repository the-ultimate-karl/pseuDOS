"""
pseuDOS Kernel Logging Subsystem (klog)

Provides leveled logging for all kernel subsystems.
Usage: from klog import klog, KLOG_DBG, KLOG_INFO, KLOG_WARN, KLOG_ERR
"""

import sys
import os

_coreutils_path = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..', '..', 'coreutils'))
if _coreutils_path not in sys.path:
    sys.path.insert(0, _coreutils_path)

try:
    from thebucket import RED, YELLOW, GREEN, CYAN, RESET, BOLD, WHITE
except ImportError:
    RED = YELLOW = GREEN = CYAN = RESET = BOLD = WHITE = ""

KLOG_ERR  = 0
KLOG_WARN = 1
KLOG_INFO = 2
KLOG_DBG  = 3
KLOG_KINF = 4

_level_names = {
    KLOG_ERR: f'{BOLD}{RED}ERR {RESET}', 
    KLOG_WARN: f'{BOLD}{YELLOW}WARN{RESET}', 
    KLOG_INFO: f'{BOLD}{GREEN}INFO{RESET}', 
    KLOG_DBG: f'{BOLD}{CYAN}DBG {RESET}',
    KLOG_KINF: f'{BOLD}{WHITE}KINF{RESET}'
}

_state = {
    'level': KLOG_INFO,   # Default: show ERR, WARN, INFO. Hide DBG.
    'buffer': [],          # Ring buffer for dmesg-style retrieval
    'max_buffer': 80000,
    'total_logs': 0,
}

import time
BOOT_TIME = time.time()

def klog(level, subsystem, message):
    """Log a kernel message. Only prints if level <= current threshold."""
    tag = _level_names.get(level, '????')
    uptime = time.time() - BOOT_TIME
    timestamp = f"{uptime:07.3f}"
    
    # Silence syslogd debug spam by elevating it to KINF
    if level == KLOG_DBG:
        try:
            import sys
            _k = sys.modules.get('protected.krnl.essential.kernel') or sys.modules.get('kernel')
            if _k and hasattr(_k, 'kdata'):
                pid = _k.kdata['get_current_pid']()
                if pid in _k.kdata['processes']:
                    if _k.kdata['processes'][pid]['name'] == 'syslogd':
                        level = KLOG_KINF
                        tag = _level_names.get(level, '????')
        except Exception:
            pass

    entry = f"[{timestamp}][{tag}][{subsystem}] {message}"

    # Always buffer for dmesg (up to DBG, ignore KINF to prevent loop)
    if level <= KLOG_DBG:
        _state['buffer'].append(entry)
        if len(_state['buffer']) > _state['max_buffer']:
            _state['buffer'].pop(0)

    if level <= _state['level']:
        print(entry, flush=True)

def boot_log(status, message):
    """Log a stylized boot message."""
    if status == 'OK':
        tag = f'{BOLD}{GREEN} OK {RESET}'
    elif status == 'WARN':
        tag = f'{BOLD}{YELLOW}WARN{RESET}'
    elif status == 'DPND':
        tag = f'{BOLD}{CYAN}DPND{RESET}'
    elif status == 'FAIL':
        tag = f'{BOLD}{RED}FAIL{RESET}'
    else:
        tag = f'{status:4}'
        
    uptime = time.time() - BOOT_TIME
    timestamp = f"{uptime:07.3f}"
    entry = f"[{timestamp}][{tag}] {message}"
    
    _state['buffer'].append(entry)
    if len(_state['buffer']) > _state['max_buffer']:
        _state['buffer'].pop(0)
        
    print(entry, flush=True)

def set_level(level):
    """Set the current log level threshold (0-3)."""
    old = _state['level']
    _state['level'] = max(KLOG_ERR, min(KLOG_DBG, level))
    return old

def get_level():
    """Get the current log level threshold."""
    return _state['level']

def get_level_name(level=None):
    """Get human-readable name for a log level."""
    if level is None:
        level = _state['level']
    return _level_names.get(level, '????').strip()

def get_buffer(count=None):
    """Get recent log entries (like dmesg). Pass count to limit."""
    if count is None:
        return list(_state['buffer'])
    return list(_state['buffer'][-count:])

def get_buffer_since(cursor):
    total = _state.get('total_logs', 0)
    if cursor >= total:
        return [], total
        
    start_idx = total - len(_state['buffer'])
    if cursor < start_idx:
        cursor = start_idx
        
    slice_idx = cursor - start_idx
    return list(_state['buffer'][slice_idx:]), total

def clear_buffer():
    """Clear the log buffer."""
    _state['buffer'].clear()
