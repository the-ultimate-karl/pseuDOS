import protected.krnl.essential.kernel as kernel

print("running a kernel test")
print("=" * 30)

print("kernel state:")
print(f"  syscalls loaded: {len(kernel.kdata['syscall_table'])}")
print(f"  processes: {len(kernel.kdata['processes'])}")
print(f"  next PID: {kernel.kdata['next_pid']}")

print("\ninvoking fork syscall...")
child_pid = kernel.syscall(2)
print(f"fork syscall returned child PID: {child_pid}")

print("verifying process table")
print(f"   process {child_pid} exists: {child_pid in kernel.kdata['processes']}")
print(f"   process {child_pid} details:")
print(f"     - Name: {kernel.kdata['processes'][child_pid]['name']}")
print(f"     - State: {kernel.kdata['processes'][child_pid]['state']}")
print(f"     - Parent: {kernel.kdata['processes'][child_pid]['parent']}")