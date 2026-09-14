# WORKING.md — pseuDOS v0.6.0 Planning

## Current State (2026-09-11)

### What Exists (v0.5.4-baremetal)
- **Branch**: `feature/scheduling` (branched off `feature/self-installation` at commit `afa6638`)
- **Boot chain**: `BOOTX64.EFI` → `kernel_main()` → `shell_run()` (shell is hardcoded inside kernel)
- **Architecture**: Flat ring 0, no GDT, no custom paging (uses whatever UEFI identity-maps), no CR3 manipulation, no syscall/sysret MSRs
- **Build**: Two PE32+ binaries — `BOOTX64.EFI` (bootloader) and `kernel.bin` (kernel + shell monolith)
- **Kernel entry**: `kernel_main()` in `src/kernel/kernel.c` initializes 8 subsystems then calls `shell_run()`
- **Shell**: `shell_run()` in `src/kernel/shell.c` (730 lines, 24 hardcoded commands)
- **Storage**: AHCI SATA, NVMe PCIe, USB mass storage drivers with `StorageDevice` function pointers
- **Filesystem**: In-memory VFS tree with FAT32 write-through sync on persistent boot media
- **Installer**: `gpt_fat32_format_and_install()` creates GPT + FAT32 ESP, deploys `BOOTX64.EFI` + `kernel.bin` with embedded payloads
- **IDT**: 256-vector dispatch table, 8259 PIC remapped to vectors 32-47, keyboard on IRQ 1
- **No RTC, no process table, no userspace, no privilege separation**

### Key Files
| File | Purpose |
|------|---------|
| `UEFI-Files/src/boot/bootloader.c` (672 lines) | UEFI bootloader, PE32+ loader, ExitBootServices, jump to kernel |
| `UEFI-Files/src/kernel/kernel.c` (53 lines) | `kernel_main()` — initializes subsystems, calls `shell_run()` |
| `UEFI-Files/src/kernel/shell.c` (730 lines) | All shell commands hardcoded, `shell_run()` main loop |
| `UEFI-Files/src/kernel/payloads.c` (28 lines) | Embedded BOOTX64.EFI + kernel.bin for self-installer |
| `UEFI-Files/include/bootinfo.h` (59 lines) | BootInfo handoff struct (framebuffer, memory map, ACPI, etc.) |
| `UEFI-Files/include/kernel.h` (11 lines) | kernel_main, shell_init, shell_run declarations |
| `UEFI-Files/Makefile` (131 lines) | Builds BOOTX64.EFI + kernel.bin, generates ISO |
| `UEFI-Files/src/drivers/idt.c` (237 lines) | IDT setup, PIC remap, ISR dispatch |
| `UEFI-Files/src/fs/gpt_fat32.c` (474 lines) | GPT + FAT32 formatter and OS installer |
| `UEFI-Files/src/fs/vfs.c` | In-memory VFS with FAT32 disk sync |
| `UEFI-Files/src/fs/fat32_sync.c` | Write-through FAT32 synchronization |
| `pseuDOS/protected/krnl/essential/kernel.py` | Python kernel (reference for process table, syscalls, module loading) |
| `pseuDOS/protected/krnl/init.py` | Python init process (reference for autoinit design) |
| `pseuDOS/protected/krnl/essential/klog.py` | Python klog (reference for dmesg-style logging) |

---

## v0.6.0 User Requirements (Verbatim Intent)

### 1. New Boot Chain
**Current**: `BOOTX64.EFI → kernel.bin → integrated shell inside kernel` (user said "BIG NO")
**New**: `BOOTX64.EFI >>> kernel.bin >>> autoinit.bin >>> xshss.bin`

- Bootloader does bare minimum to load kernel
- Bootloader uses boot configuration data to locate kernel
- Kernel does kernel stuff, then calls `autoinit.bin`
- `autoinit.bin` starts up `xshss.bin`
- `xshss.bin` = eXperimental Shell SubSystem (temporary name)

### 2. Privilege Model
- Shell runs in userspace (ring 3): `shell@pseuDOS [/] >`
- Forward slashes (not backslashes) in prompt
- `kernel` command escalates to kernel mode: `kernel@pseuDOS [/] >` — "congrats, you're root"
- `su` aliases the `kernel` command
- `sudo command-here` runs a single command with kernel privileges from ring 3
- Users can't `rm -rf /efi` without `sudo`

### 3. CMOS RTC
- "with this we can finally know when a file was made"
- Enables file timestamps (creation date, last access date)

### 4. `data` Command (File Metadata)
```
shell@pseuDOS [/] > data hi.txt
File metadata:
    Date created:    2026-09-11
    Date last accessed: 2026-09-11
    On directory: /
    etc etc
```

### 5. Essential File Utilities
- `cp` (copy), `mv` (move/rename)
- `more` / `less` — paged text viewer (user confirmed: scroll through long output page-by-page)

### 6. Process Management
- Major architectural rewrite following Python pseuDOS's process model
- Process table with PIDs
- Process states (running, killed, etc.)
- Based on Python's `kdata['processes']` dict pattern

### 7. Syscall Interface
- x86_64 `syscall`/`sysret` via MSRs (STAR, LSTAR, SFMASK)
- Based on Python's `kdata['syscall_table']` pattern

### 8. Memory Protection
- Higher-half paging
- Kernel/userspace separation
- "especially now that we have processes"

### 9. GRUB Compatibility
- "also lets add grub compatibility because yes"
- (No further elaboration from user)

---

## v0.6.0 Implementation Plan (Draft)

### Phase 0: Infrastructure Prerequisites
These must exist before anything else can work.

#### 0A. Custom GDT
Currently relying on UEFI's GDT. Need our own with:
- Segment 0x00: Null
- Segment 0x08: Kernel Code (ring 0, long mode)
- Segment 0x10: Kernel Data (ring 0)
- Segment 0x18: User Code (ring 3, long mode)
- Segment 0x20: User Data (ring 3)
- Segment 0x28: TSS (for RSP0 kernel stack on ring transitions)

#### 0B. Higher-Half Paging
Set up 4-level page tables (PML4 → PDPT → PD → PT):
- Map kernel to upper half (e.g., `0xFFFF800000000000` or `0xFFFFFFFF80000000`)
- Identity-map lower memory for UEFI leftovers (framebuffer, ACPI, MMIO)
- Mark kernel pages as supervisor-only (U/S bit = 0)
- Mark userspace pages with U/S bit = 1
- Physical memory manager (bitmap or buddy allocator) for page allocation

#### 0C. TSS (Task State Segment)
- Required for ring 3 → ring 0 transitions
- `RSP0` field holds kernel stack pointer
- Load via `ltr` instruction

#### 0D. Syscall/Sysret MSRs
- `STAR` MSR (0xC0000081): segment selectors for syscall/sysret
- `LSTAR` MSR (0xC0000082): syscall entry point address
- `SFMASK` MSR (0xC0000084): RFLAGS mask (clear IF during syscall)
- Syscall handler saves context, dispatches by number, sysret returns

---

## Confirmed Design Decisions (2026-09-11 16:30)

### D1: GRUB Compatibility
**Problem**: User has GRUB on his machine but GRUB can't detect pseuDOS at all.
**Root cause**: GRUB's `os-prober` scans for known OS signatures (Windows BCD, Linux `/etc/os-release`, etc.). pseuDOS has none of these. Also, pseuDOS installs to the ESP under `/EFI/BOOT/BOOTX64.EFI` which is the fallback bootloader path — GRUB won't list it as a separate OS because it looks like the default UEFI boot entry.
**Implementation**:
- Add an OS identifier file that `os-prober` or manual GRUB config can pick up
- The standard approach for UEFI OSes that GRUB should detect: create a GRUB menuentry that chainloads pseuDOS's EFI bootloader. This needs pseuDOS to install under its own EFI path (`/EFI/pseuDOS/BOOTX64.EFI` or similar) rather than overwriting the fallback path
- Add a `grub.cfg` snippet or documentation for manual GRUB config: `menuentry "pseuDOS" { chainloader /EFI/pseuDOS/BOOTX64.EFI }`
- Consider registering pseuDOS as a UEFI boot entry via `efibootmgr` (on Linux) or equivalent — this makes UEFI firmware boot manager and GRUB both see it
- The installer (`gpt_fat32_format_and_install`) should place `BOOTX64.EFI` at BOTH `/EFI/BOOT/BOOTX64.EFI` (fallback) AND `/EFI/pseuDOS/BOOTX64.EFI` (named entry for GRUB)

### D2: Executable Format — PE32+
**Decision**: All binaries (`BOOTX64.EFI`, `kernel.bin`, `autoinit.bin`, `xshss.bin`) use PE32+.
**Implementation**:
- Makefile gets new targets: `bin/autoinit.bin` and `bin/xshss.bin` with their own `LDFLAGS` and entry points
- The kernel's PE32+ loader (currently in `bootloader.c` lines 426-568) gets factored into a shared `pe_loader` module usable by the kernel at runtime to load `autoinit.bin` and `xshss.bin` from the VFS/FAT32
- Each binary gets its own linker entry point: `autoinit_main()`, `xshss_main()`
- The PE loader allocates userspace pages, maps sections, applies relocations, creates a process entry, and jumps to the entry point via `sysret`

### D3: Preemptive Scheduling
**Decision**: Preemptive scheduling from day one for robustness.
**Implementation**:
- PIT (Programmable Interval Timer) channel 0 on IRQ 0 (vector 32) — already masked in current PIC config, just needs unmasking and a handler
- PIT frequency: ~100 Hz (reload value 11932 for 1193182 / 100 ≈ 11932). Gives 10ms time slices
- Timer ISR: saves current process context (all GPRs + RIP + RSP + RFLAGS + CR3), picks next READY process from the run queue, loads its context, `iretq` to resume
- Round-robin scheduling across READY processes
- `cli`/`sti` around critical kernel sections to prevent preemption during page table / process table modifications
- Process states: RUNNING (currently on CPU), READY (in run queue), BLOCKED (waiting for I/O or `waitpid`), KILLED (zombie awaiting cleanup)
- Idle loop: PID 0 kernel idle process executes `hlt` in a loop when no other process is READY

### D4: Privilege Escalation — Ring 3 with Kernel Flag
**Decision**: `kernel` command keeps the shell in ring 3 but sets a per-process privilege flag that the kernel checks on every syscall.
**Implementation**:
- `struct Process` gets a `uint8_t privilege_level` field: `PRIV_USER = 0`, `PRIV_KERNEL = 1`
- Syscalls that touch protected paths (e.g., `/efi/`, `/protected/`) or perform dangerous operations (reboot, shutdown, raw disk I/O, process kill) check `current_process->privilege_level`
- If `PRIV_USER` and the operation is restricted → return `-EPERM`
- `kernel` command: issues `sys_elevate()` syscall → kernel sets `privilege_level = PRIV_KERNEL` on the calling process → prompt changes to `kernel@pseuDOS [/] >`
- `exit` from kernel mode: issues `sys_drop_privileges()` syscall → resets to `PRIV_USER` → prompt reverts to `shell@pseuDOS [/] >`
- `su`: alias for `kernel` — same syscall
- `sudo <cmd>`: issues `sys_sudo(cmd_string)` syscall → kernel temporarily sets `PRIV_KERNEL`, dispatches the command via the shell's command handler, then resets to `PRIV_USER` after the command completes. The command runs within the same process, just with elevated privileges for that one syscall chain
- No actual ring transition — the CPU stays in ring 3. The "privilege" is entirely a kernel-enforced policy check

### D5: Per-Process Memory Isolation — Separate CR3
**Decision**: Each process gets its own page table hierarchy (separate CR3) for full address space isolation.
**Implementation**:
- Physical memory manager: bitmap allocator tracking all usable physical pages from the UEFI memory map
- Each process gets its own PML4 table. Kernel pages (upper half) are shared across all PML4s by copying the PML4 entries for the kernel range. Userspace pages (lower half) are per-process
- `process_create()`: allocates a new PML4, clones kernel half, maps the PE32+ sections into userspace addresses (e.g., starting at `0x400000`), sets up a user stack (e.g., at `0x7FFFFFFFE000` growing down)
- Context switch: `mov cr3, <new_process_cr3>` — automatically flushes TLB for the old process's userspace
- Page fault handler (vector 14): needed to catch invalid userspace accesses and kill the offending process rather than triple-faulting
- Kernel memory (framebuffer, heap, drivers, ACPI tables, MMIO regions) stays identity-mapped in the upper half, shared across all processes, with U/S=0 (supervisor only)

### D6: Boot Configuration Data — Full Parsing with Path Extraction
**Decision**: Bootloader fully parses `boot.cfg` and passes paths to kernel. Kernel passes autoinit path at exec. Autoinit reads xshss path via its own config or a kernel-provided argument.
**Implementation**:
- `boot.cfg` format (INI-style, already partially exists):
  ```ini
  # pseuDOS Boot Configuration
  kernel=\protected\krnl\kernel.bin
  autoinit=\protected\krnl\autoinit.bin
  shell=\protected\crit\xshss.bin
  cmdline=quiet devpath=hardware
  default_resolution=1280x720
  ```
- Bootloader (`bootloader.c`): after opening `boot.cfg`, reads it into a buffer, parses key=value lines. Extracts `kernel` path and uses it to locate `kernel.bin` (replacing the hardcoded path search). Extracts `autoinit` and `shell` paths into the `BootInfo` struct
- `BootInfo` struct gets new fields:
  ```c
  char autoinit_path[128];   /* path to autoinit binary */
  char shell_path[128];      /* path to shell binary */
  char cmdline[256];         /* kernel command line */
  ```
- Kernel receives `BootInfo`, uses `autoinit_path` to load and exec `autoinit.bin`
- `autoinit.bin` receives the `shell_path` from the kernel (either via a syscall that reads BootInfo, or passed as an argument to `autoinit_main()`)
- `autoinit.bin` then loads and execs `xshss.bin` from that path

### D7: Incremental Milestone Strategy
**Decision**: Incremental phases, each bootable and testable.

**Phase A: GDT + Paging + TSS [COMPLETED + VERIFIED 2026-09-11 16:55]**
- Custom 64-bit GDT with Ring 0 code/data, Ring 3 code/data, and 16-byte TSS descriptor (`src/kernel/gdt.c`, `include/gdt.h`)
- Dedicated 16KB interrupt stack allocated for Ring 3 -> Ring 0 transitions in TSS (`rsp0`)
- Segment reloading and task register loading via `gdt_flush` (`src/kernel/gdt_flush.s`)
- Physical Memory Manager (PMM) bitmap allocator initialized from UEFI memory map descriptors (`src/kernel/pmm.c`, `include/pmm.h`)
- Virtual Memory Manager (VMM) 4-level paging initialized with Master Kernel PML4 (`src/kernel/vmm.c`, `include/vmm.h`):
  - Lower-half identity mapping (`0x0000000000000000+`) using 2MB pages for existing kernel code, hardware MMIO, and framebuffer
  - Higher-half canonical kernel mapping (`0xFFFF800000000000+`) using 2MB pages with `PTE_GLOBAL`
  - Dynamic page table allocation and splitting (`vmm_map_page`)
  - User address space cloning mechanism (`vmm_create_user_address_space`)
- Verified in QEMU: clean boot, memory reporting (`mem`), CPU detection (`cpu`), filesystem inspection (`fs`), and interactive shell with no regressions.
- **Files touched**: `include/gdt.h`, `src/kernel/gdt.c`, `src/kernel/gdt_flush.s`, `include/pmm.h`, `src/kernel/pmm.c`, `include/vmm.h`, `src/kernel/vmm.c`, `src/kernel/kernel.c`, `UEFI-Files/Makefile`.

**Phase B: RTC + klog + New Commands [COMPLETED + VERIFIED 2026-09-11 17:03, BUGFIX 17:15]**
- CMOS Real-Time Clock (RTC) driver reading hardware time/date registers via ports 0x70/0x71 with BCD decoding, 24h conversion, and UIP guard (`src/drivers/rtc.c`, `include/rtc.h`)
- VFS metadata timestamps (`date_created`, `date_accessed`, `date_modified`) added to `vfs_node_t` and tracked across create, write, and read operations (`src/fs/vfs.c`, `include/fs.h`)
- Kernel Logging Subsystem (`klog`) with ring buffer, uptime timestamps, log levels (ERR, WARN, INFO, DBG, KINF), and `boot_log` integration (`src/kernel/klog.c`, `include/klog.h`)
- Implemented file utility commands in `src/kernel/shell.c`:
  - `data <file>`: displays creation date, access date, modification date, parent directory, file size, node type, and protection status
  - `cp <src> <dst>`: copies files via VFS write-through
  - `mv <src> <dst>`: moves/renames files via VFS copy and source unlinking
  - `more` / `less`: paginated text viewer (22 lines per screen with Space/Enter/Q controls)
  - `dmesg`: dumps kernel log ring buffer to console
- Bugfix (2026-09-11 17:15): `cp` and `mv` returned "write error" when destination was an existing directory or ended in `/`. Implemented directory target path resolution (`resolve_copy_dest`), binary-safe writing (`vfs_write_file_bytes`), quotation handling, and same-file collision checks.
- Verified in QEMU: clean boot logging, `dmesg` buffer dump, file creation with live RTC timestamps, file copying (`cp a.txt b.txt`, `cp a.txt /home`, `cp a.txt /home/`), file moving (`mv foo.txt /tmp/bar.txt`), and metadata inspection via `data`.
- **Files touched**: `include/rtc.h`, `src/drivers/rtc.c`, `include/klog.h`, `src/kernel/klog.c`, `include/fs.h`, `src/fs/vfs.c`, `src/kernel/shell.c`, `src/kernel/kernel.c`, `UEFI-Files/Makefile`.

**Phase C: Syscall Interface + Process Table + Preemptive Scheduler [COMPLETED + VERIFIED 2026-09-11 17:48]**
- Programmable Interval Timer (PIT) 8254 Channel 0 driver configured for 100 Hz (10ms tick rate) (`src/drivers/pit.c`, `include/pit.h`)
- Process Table & Process Control Block (`process_t`) supporting up to 64 concurrent tasks (`src/kernel/process.c`, `include/process.h`):
  - State machine: `PROCESS_STATE_UNUSED`, `READY`, `RUNNING`, `BLOCKED`, `KILLED`
  - Privilege levels: `PRIV_USER` (0) and `PRIV_KERNEL` (1)
  - Dedicated 16KB kernel stacks allocated via PMM/heap for every process
  - Process creation (`process_create`), termination (`process_exit`, `process_kill`), and dump (`process_dump_list`)
  - Kernel idle/shell thread initialized as PID 0 with initial `PRIV_KERNEL` privilege
- Preemptive Round-Robin Scheduler driven by PIT Timer IRQ 0 (`src/kernel/scheduler.c`, `include/scheduler.h`):
  - Interrupt context preservation across all 16 general-purpose registers and CPU interrupt frame
  - Round-robin queue traversal with time slice enforcement (`DEFAULT_TIME_SLICE = 5` ticks = 50ms)
  - TSS `rsp0` and syscall stack synchronization on every context switch
  - Fast-path optimization when only one runnable task exists
  - Software yield via `int $48` (`scheduler_yield`)
- x86_64 Fast Syscall Interface via `syscall` and `sysretq` instructions (`src/kernel/syscall.c`, `src/kernel/syscall_entry.s`, `include/syscall.h`):
  - Model Specific Registers configured: `IA32_STAR` (0xC0000081), `IA32_LSTAR` (0xC0000082), `IA32_FMASK` (0xC0000084), `IA32_EFER.SCE` (bit 0)
  - Syscall assembly stub (`syscall_entry.s`) preserving caller-saved registers and handling both Ring 0 and Ring 3 callers
  - Syscalls implemented: `SYS_READ`, `SYS_WRITE`, `SYS_OPEN`, `SYS_CLOSE`, `SYS_READDIR`, `SYS_MKDIR`, `SYS_UNLINK`, `SYS_CHDIR`, `SYS_GETCWD`, `SYS_GETPID`, `SYS_TIME`, `SYS_SLEEP`, `SYS_YIELD`, `SYS_ELEVATE`, `SYS_DROP_PRIVILEGES`, `SYS_GET_PRIVILEGE`
- New Diagnostic Shell Commands in `src/kernel/shell.c`:
  - `ps`: inspects process table with PID, PPID, State, Privilege, Accumulated Ticks, and Name
  - `kill <pid>`: terminates a process by PID
  - `syscalltest`: executes live syscalls (`SYS_GETPID`, `SYS_TIME`, `SYS_WRITE`, `SYS_GET_PRIVILEGE`, `SYS_ELEVATE`, `SYS_DROP_PRIVILEGES`)
  - `proctest`: spawns background worker threads `worker_a` and `worker_b` to demonstrate preemptive round-robin concurrency and sleep yields
- Verified in QEMU:
  - `ps` correctly reports PID 0 running with tick accumulation
  - `syscalltest` successfully executes all 6 test syscalls with proper return values, live hardware timestamps, and privilege state transitions
  - `proctest` concurrently runs `worker_a` (PID 1) and `worker_b` (PID 2) across alternating iterations under timer preemption, exiting cleanly with exit code 0
  - `ps` after `proctest` confirms PID 1 and 2 completed and transitioned to KILLED with 10+ ticks
  - `dmesg` verifies all scheduling and syscall events recorded in ring buffer
- **Files touched**: `include/pit.h`, `src/drivers/pit.c`, `include/process.h`, `src/kernel/process.c`, `include/scheduler.h`, `src/kernel/scheduler.c`, `include/syscall.h`, `src/kernel/syscall.c`, `src/kernel/syscall_entry.s`, `src/drivers/idt.c`, `src/kernel/shell.c`, `src/kernel/kernel.c`, `UEFI-Files/Makefile`.

**Phase D: Boot Config Parsing + PE Loader + Boot Chain [COMPLETED + VERIFIED 2026-09-13 12:48]**
- `BootInfo` extended with `autoinit_path[128]`, `shell_path[128]`, and `cmdline[256]` in `include/bootinfo.h`.
- Dynamic boot configuration parser implemented in `src/boot/bootloader.c` reading `/protected/bootmgr/boot.cfg` for `kernel=`, `autoinit=`, `shell=`, and `cmdline=`.
- In-kernel PE32+ loader implemented in `src/kernel/pe_loader.c` and `include/pe_loader.h` (`pe_load_binary` and `pe_spawn_process`), supporting MZ/PE validation, section mapping to virtual addresses, and DIR64/HIGHLOW base relocations.
- Syscall interface extended in `include/syscall.h` and `src/kernel/syscall.c` with `SYS_EXEC`, `SYS_WAITPID`, `SYS_READDIR`, `SYS_REBOOT`, `SYS_SHUTDOWN`, `SYS_STAT`, `SYS_READFILE`, `SYS_WRITEFILE`, and `SYS_GET_BOOTINFO`.
- Userland Init process implemented in `src/userspace/autoinit.c`, compiled to `bin/autoinit.bin`: queries bootinfo via `SYS_GET_BOOTINFO`, spawns shell subsystem via `SYS_EXEC`, and supervises process termination via `SYS_WAITPID`.
- Experimental Shell Subsystem implemented in `src/userspace/xshss.c`, compiled to `bin/xshss.bin`: standalone userspace shell executing commands exclusively via syscalls (`help`, `ls`, `cd`, `pwd`, `cat`, `more`/`less`, `data`, `cp`, `mv`, `mkdir`, `touch`, `write`, `del`, `ps`, `kill`, `dmesg`, `syscalltest`, `kernel`, `su`, `sudo`, `reboot`, `shutdown`, `exit`).
- Autoinit and xshss binaries embedded into kernel image as assembly payloads in `src/kernel/payloads.c` and written to `/protected/krnl/autoinit.bin` and `/protected/crit/xshss.bin` on boot in `src/fs/vfs.c`.
- Kernel handoff updated in `src/kernel/kernel.c`: launches PID 1 (`autoinit.bin`), while PID 0 enters kernel idle loop.
- Bugfix: Resolved system hang on `SYS_SLEEP` where `pit_sleep_ms` executed `hlt` with `IF=0` (masked by `SFMASK` on syscall entry); updated `pit_sleep_ms` and `keyboard_getchar` to yield to the preemptive scheduler (`scheduler_yield`) when enabled.
- Verified in QEMU:
  - Bootloader dynamically parses `boot.cfg` and loads kernel.
  - Kernel initializes GDT, PMM, VMM, RTC, VFS, PIT, and preemptive scheduler.
  - Kernel spawns PID 1 (`autoinit`), which runs and executes `SYS_EXEC` to spawn PID 2 (`xshss.bin`).
  - Interactive shell prompt `kernel@pseuDOS [/] > ` displayed and fully responsive over serial and console.
  - Commands verified: `help`, `ps` (verifying PID 0 kernel, PID 1 autoinit, PID 2 xshss), `pwd`, `ls`, `syscalltest`, and `data /protected/bootmgr/boot.cfg`.
- **Files touched**: `include/bootinfo.h`, `include/syscall.h`, `include/pe_loader.h`, `src/boot/bootloader.c`, `src/kernel/pe_loader.c`, `src/kernel/syscall.c`, `src/userspace/autoinit.c`, `src/userspace/xshss.c`, `src/kernel/payloads.c`, `src/fs/vfs.c`, `src/kernel/kernel.c`, `src/drivers/pit.c`, `src/drivers/keyboard.c`, `UEFI-Files/Makefile`, `BUGSPAWN.LOG`.

**Phase E: Privilege Model [COMPLETED + VERIFIED 2026-09-13 15:10]**
- Per-process privilege separation policy enforced across kernel and userland (`PRIV_USER = 0`, `PRIV_KERNEL = 1`).
- `xshss.bin` initialized and spawned with `PRIV_USER` (0) by default via `SYS_EXEC(shell_path, PRIV_USER)`.
- Default userland prompt rendered dynamically: `shell@pseuDOS [/] > ` with forward slashes.
- Privilege escalation implemented via `kernel` and `su` commands issuing `SYS_ELEVATE`: sets `curr->privilege_level = PRIV_KERNEL` and updates prompt dynamically to `kernel@pseuDOS [/] > ` ("congrats, you're root").
- Single-command privilege elevation implemented via `sudo <command>`: executes `SYS_ELEVATE`, runs command with kernel privileges, and automatically reverts to previous privilege level via `SYS_DROP_PRIVILEGES`.
- Privilege de-escalation implemented via `exit` and `drop` commands: if running elevated (`PRIV_KERNEL`), drops privileges back to `PRIV_USER` and reverts prompt to `shell@pseuDOS [/] > `; if already unprivileged, exits shell (autoinit respawns).
- Syscall security checks enforced in `src/kernel/syscall.c`:
  - `SYS_UNLINK`: deleting files in protected paths (`/efi/`, `/protected/`, or nodes with `is_protected`) requires `PRIV_KERNEL`, returning `-EPERM` (1) otherwise.
  - `SYS_MKDIR`: creating directories in protected paths requires `PRIV_KERNEL`, returning `-EPERM`.
  - `SYS_WRITEFILE`: writing or appending to files in protected paths requires `PRIV_KERNEL`, returning `-EPERM`.
  - `SYS_KILL`: terminating another process (target PID != caller PID) requires `PRIV_KERNEL`, returning `-EPERM`.
  - `SYS_REBOOT` and `SYS_SHUTDOWN`: power management syscalls require `PRIV_KERNEL`, returning `-EPERM`.
- Bugfix: Userland syscall return `#PF (0x15)` resolved in `src/kernel/syscall_entry.s` and `src/kernel/process.c`; per Design Decision D4, privilege levels are verified as in-kernel process policy checks rather than arbitrary segment/ring switches into supervisor-only heap pages.
- Verified in QEMU:
  - Default boot prompt is `shell@pseuDOS [/] > `.
  - `syscalltest` verifies user privilege: `sys_get_privilege() = USER`.
  - `del /protected/bootmgr/boot.cfg` denied: `del: permission denied: protected system path requires 'sudo' or KERNEL mode`.
  - `mkdir /protected/bad`, `touch /protected/bad.txt`, `write /protected/... evil`, `kill 1`, `reboot`, `shutdown` all denied with permission denied messages.
  - Unprivileged user operations in `/home/user` (`mkdir`, `touch`, `write`, `cat`, `del`) all succeed cleanly.
  - `sudo touch /protected/sudotest.txt` and `sudo del /protected/sudotest.txt` successfully elevate and complete, prompt remains `shell@pseuDOS [/] > `.
  - `kernel` and `su` elevate process: prompt updates to `kernel@pseuDOS [/] > `, allowing direct modification of protected files.
  - `drop` and `exit` de-escalate process: prompt reverts to `shell@pseuDOS [/] > `, re-engaging path protections.
- **Files touched**: `src/kernel/syscall.c`, `src/kernel/syscall_entry.s`, `src/kernel/process.c`, `src/userspace/xshss.c`, `BUGSPAWN.LOG`.

**Phase F: GRUB Compatibility [COMPLETED + VERIFIED 2026-09-13 18:13]**
- Partition format and ISO ESP deploy named EFI entry `/EFI/pseuDOS/BOOTX64.EFI` and alias `/EFI/pseuDOS/pseudos.efi` alongside fallback `/EFI/BOOT/BOOTX64.EFI` in `src/fs/gpt_fat32.c` and `UEFI-Files/scripts/make_iso.py`.
- Dynamic VFS initramfs populates `/EFI/pseuDOS/BOOTX64.EFI`, `/EFI/pseuDOS/pseudos.efi`, `/EFI/pseuDOS/grub.cfg`, and `/EFI/pseuDOS/os-release` in `src/fs/vfs.c`.
- Created standardized `/EFI/pseuDOS/grub.cfg` chainloader menuentry snippet:
  ```text
  menuentry "pseuDOS x86_64" {
      insmod fat
      insmod chain
      search --no-floppy --set=root --file /EFI/pseuDOS/BOOTX64.EFI
      chainloader /EFI/pseuDOS/BOOTX64.EFI
  }
  ```
- Created standardized `/EFI/pseuDOS/os-release` for Linux / `os-prober` recognition (`NAME="pseuDOS"`, `ID=pseudos`, `VERSION="0.6.0"`, `PRETTY_NAME="pseuDOS v0.6.0 (x86_64 UEFI)"`).
- Implemented `grub` shell command in `src/userspace/xshss.c` and `src/kernel/shell.c` displaying EFI boot targets, GRUB 2 chainloader menuentry, and Linux setup instructions (`/etc/grub.d/40_custom` and `update-grub`).
- Fixed FAT 8.3 entry bug in `UEFI-Files/scripts/make_iso.py` and `src/fs/gpt_fat32.c` where 10-char name `"PSEUDOSEFI"` caused bytearray buffer shrinkage; corrected to 11-char `"PSEUDOS EFI"`.
- Verified in QEMU:
  - Clean boot directly into userspace shell `shell@pseuDOS [/] > `.
  - `help` lists `grub : display GRUB 2 chainloader config & setup`.
  - `grub` displays complete chainloader documentation and configuration block.
  - `ls /EFI/pseuDOS` shows `BOOTX64.EFI`, `pseudos.efi`, `grub.cfg`, `os-release`, and `kernel.bin`.
  - `cat /EFI/pseuDOS/grub.cfg` confirms exact chainloader menuentry syntax.
  - `cat /EFI/pseuDOS/os-release` verifies OS identification fields.
  - `data /EFI/pseuDOS/grub.cfg` verifies metadata and live timestamps.
  - `del /EFI/pseuDOS/grub.cfg` properly blocked by privilege system (requires `sudo` or KERNEL mode).
  - `sudo del /EFI/pseuDOS/grub.cfg` elevates and removes file; prompt remains unprivileged.
  - `sudo shutdown` shuts down system cleanly via ACPI.
- **Files touched**: `UEFI-Files/scripts/make_iso.py`, `src/fs/gpt_fat32.c`, `src/fs/vfs.c`, `src/userspace/xshss.c`, `src/kernel/shell.c`, `BUGSPAWN.LOG`.

**Phase G: Installer Update & End-to-End Self-Installation [COMPLETED + VERIFIED 2026-09-13 20:25]**
- Embedded all 4 binaries (`BOOTX64.EFI`, `kernel.bin`, `autoinit.bin`, `xshss.bin`) dynamically queryable in `src/kernel/payloads.c` and `src/fs/gpt_fat32.c`.
- Fully upgraded installer `gpt_fat32_format_and_install()` in `src/fs/gpt_fat32.c`:
  - Dynamically calculates cluster geometry, sectors per FAT, and cluster chains.
  - Constructs directory hierarchy: `\EFI\BOOT`, `\EFI\pseuDOS`, `\protected\krnl`, `\protected\bootmgr`, and `\protected\crit`.
  - Deploys all 4 binaries:
    - `\EFI\BOOT\BOOTX64.EFI` (fallback bootloader)
    - `\EFI\pseuDOS\BOOTX64.EFI` & `\EFI\pseuDOS\pseudos.efi` (GRUB chainloader targets)
    - `\protected\krnl\kernel.bin` (bare-metal kernel)
    - `\protected\krnl\autoinit.bin` (PID 1 init subsystem)
    - `\protected\crit\xshss.bin` (PID 2 shell subsystem)
  - Installs system configuration:
    - `\protected\bootmgr\boot.cfg` (kernel, autoinit, shell paths, cmdline, default resolution)
    - `\EFI\pseuDOS\grub.cfg` (GRUB 2 menuentry snippet)
    - `\EFI\pseuDOS\os-release` (OS identification)
    - `\startup.nsh` (UEFI Shell fallback autorun)
  - Synchronizes disk cache via `dev->flush(dev)`.
- Upgraded `load_fat32_dir_recursive` in `src/fs/fat32_sync.c`:
  - Traverses multi-cluster FAT chains using `get_fat_entry()` up to EOF (`0x0FFFFFF8`), fully loading multi-cluster executables (`autoinit.bin` 18.5KB, `xshss.bin` 32KB) into VFS memory.
  - Added explicit 8.3 name aliasing in `to_dos_name` and `from_dos_name` for `protected` <-> `PROTECT    ` and `os-release` <-> `OS-RELEA   `.
- Exposed installer to userland:
  - Added `#define SYS_FLASH 34` in `include/syscall.h`.
  - Implemented `SYS_FLASH` dispatch in `src/kernel/syscall.c` enforcing `curr->privilege_level == PRIV_KERNEL`.
  - Added `flash` command in `src/userspace/xshss.c`: requires `sudo flash` or `kernel` privilege, otherwise prints `permission denied`.
- Full end-to-end verification in QEMU via `scratch/test_phase_g.py`:
  - **Step 1 (Install)**: Live ISO booted with fresh 500 MB raw SATA disk attached. `flash` checked for unprivileged denial. `sudo flash` executed, drive 1 selected, confirmation given, all 14 installation steps completed with `[ok]`.
  - **Step 2 (Standalone Boot)**: Booted persistent SATA disk directly without ISO (`order=c,menu=off`, no CD-ROM). Bootloader parsed `\protected\bootmgr\boot.cfg`, found kernel, loaded kernel. Kernel mounted persistent root filesystem, launched PID 1 `autoinit.bin`. `autoinit` spawned PID 2 `xshss.bin`. Exact user-specified startup banner displayed. Shell prompt reached cleanly.
  - **Step 3 (Interactive Commands)**: Executed and verified `pwd` (`/`), `ps` (kernel, autoinit, xshss.bin), `ls /protected/krnl` (`KERNEL.BIN`, `AUTOINIT.BIN`), `ls /protected/crit` (`XSHSS.BIN`), `cat /protected/bootmgr/boot.cfg` (`bootmgr_version=1.1.0`), `grub` (GRUB 2 Menuentry Snippet), `data /protected/krnl/autoinit.bin` (live timestamps), `sudo touch /test_installed.txt`, `cat /test_installed.txt`, and `sudo shutdown` (clean ACPI power off).
- **Files touched**: `include/syscall.h`, `src/kernel/syscall.c`, `include/kernel.h`, `src/kernel/shell.c`, `src/userspace/xshss.c`, `src/userspace/autoinit.c`, `src/fs/gpt_fat32.c`, `src/fs/fat32_sync.c`, `scratch/test_phase_g.py`, `BUGSPAWN.LOG`.

---

## Status
- **Phase A**: Completed and verified (Custom 64-bit GDT, TSS with RSP0, PMM bitmap allocator, VMM 4-level paging)
- **Phase B**: Completed and verified (CMOS RTC, VFS timestamps, klog ring buffer, `data`, `cp`, `mv`, `more`/`less`, `dmesg`, cp/mv directory fix verified)
- **Phase C**: Completed and verified (PIT 100 Hz timer, process table, preemptive round-robin scheduler, x86_64 fast syscalls via STAR/LSTAR/SFMASK, `ps`, `kill`, `proctest`, `syscalltest`)
- **Phase D**: Completed and verified (Modular boot chain `BOOTX64.EFI` → `kernel.bin` → `autoinit.bin` → `xshss.bin`, `boot.cfg` parser, PE32+ loader, `SYS_EXEC`/`SYS_WAITPID`/`SYS_STAT`, userland init & shell subsystems, interactive execution verified)
- **Phase E**: Completed and verified (Privilege Separation Model: `PRIV_USER` default on shell `shell@pseuDOS [/] > `, kernel-enforced path & syscall security, `sudo` single-command elevation, `kernel`/`su` persistent elevation `kernel@pseuDOS [/] > `, `exit`/`drop` de-escalation)
- **Phase F**: Completed and verified (GRUB Compatibility: `/EFI/pseuDOS/BOOTX64.EFI` and `pseudos.efi` deployment, `/EFI/pseuDOS/grub.cfg`, `/EFI/pseuDOS/os-release`, and `grub` shell command)
- **Phase G**: Completed and verified (Installer Update & End-to-End Self-Installation: dynamic GPT/FAT32 installer deploying all 4 binaries + configs, multi-cluster FAT32 loader fix, 8.3 aliasing fix, `SYS_FLASH` privilege check, standalone SATA disk boot verified without ISO, full suite of interactive commands verified)
- **UI Customization (2026-09-13 20:11)**: Replaced verbose boot box banner with user-requested clean startup banner:
  ```text
  [autoinit] sucessfully spawned experimental shell subsystem, transitioning...
  [  xhss  ] started
  shell started
  type 'help' for a list of commands.

  shell@pseuDOS [/] > 
  ```
  Verified 100% in QEMU test runner.
- **Kernel Panic Subsystem (2026-09-13 20:01)**: Implemented fatal exception BSOD handler and `SYS_PANIC` following user's exact specification. Verified with `panic` command and hardware register / memory dump.
- **Command Restoration & Classic `ls` (2026-09-13 20:44)**:
  - Restored classic `ls` format via `vfs_listdir` in `SYS_READDIR`: explicitly displays directories with `<dir>` (including `.` and `..`), files with exact byte sizes (`%8u B`), and total item counts.
  - Restored `fs` command via `SYS_FS`: live root filesystem health, medium inspection, and partition analyzer.
  - Restored all system and hardware inspection commands in `xshss.c`: `cpu` (SYS_CPU), `mem` (SYS_MEM), `pci` (SYS_PCI), `devpath` (SYS_DEVPATH), `attached-drives` (SYS_ATTACHED_DRIVES), `switch-target` (SYS_SWITCH_TARGET), `screenres` (SYS_SCREENRES), `proctest` (SYS_PROCTEST), and `halt` (SYS_HALT, requiring sudo).
  - Verified 100% in QEMU test suites (`test_fs_and_ls.py` and `test_phase_g.py`).
- **Filesystem `#GP` Bugfix (2026-09-13 21:20)**:
  - **Root Cause**: `fs` without elevation crashed with `#GP (0x0D)` in `heap_get_used` / `kmalloc`. The 16KB `PROCESS_STACK_SIZE` allocated on the heap was exhausted by nested 5KB `StorageDriveInfo` structures, sector read buffers (`mbr`, `gpt_hdr`, `sec_buf`, `fsinfo`), and formatted printing in `cmd_fs` and `storage_inspect_fs`. The stack pointer grew downward past `kernel_stack_base` by over 3KB, smashing the adjacent `heap_block_t` header with FAT32 FSInfo sector signatures (`0x41615252`), leading to a non-canonical pointer dereference (`0x52000000...`) and `#GP(0)`.
  - **Fix Implemented**:
    - Expanded `PROCESS_STACK_SIZE` from 16KB (`16384`) to 64KB (`65536`) in `include/process.h`.
    - Dynamically allocated `StorageDriveInfo *drive_info` via `kmalloc`/`kfree` in `storage_inspect_fs()` (`src/drivers/storage.c`) and `cmd_fs()` (`src/kernel/shell.c`), eliminating 10KB+ of heavy stack frames.
    - Added defensive boundary validation in `heap_get_used()` and `kmalloc()` (`src/lib/heap.c`) to safely abort traversal if heap block pointers wander outside the allocated heap window.
  - **Verification**:
    - Standalone boot from SATA disk: `fs` executed cleanly without elevation (`shell@pseuDOS [/] > fs`), reporting partition health and memory stats.
    - Full end-to-end regression testing (`test_phase_g.py` and `test_fs_and_ls.py`) passed 100% with zero faults.
- **Screen Clear Subsystem (`clear` / `cls`) (2026-09-13 21:24)**:
  - Replaced legacy newline-printing hack (`for (int k = 0; k < 30; k++) x_puts("\n");`) in `xshss.c` with dedicated hardware screen clear syscall (`SYS_CLEAR` = 45).
  - In `src/kernel/syscall.c`, mapped `SYS_CLEAR` to `console_clear()`, which:
    1. Wipes the GOP graphical framebuffer clean with `fb_clear(g_bg_color)`.
    2. Resets text matrix cursor positions (`g_cursor_x = 0`, `g_cursor_y = 0`).
    3. Clears both in-memory text character and render cache buffers.
    4. Emits ANSI escape sequence `\033[2J\033[H` over UART serial to clear terminal emulators.
  - Verified with `scratch/test_clear.py` in QEMU: screen clears cleanly on both `clear` and `cls`, resetting prompt to top-left.
- **Codebase De-Sloppification & Function Renaming (2026-09-13 21:42)**:
  - **Function Renaming**:
    - Renamed `pseu_syscall(...)` to `syscall(...)` in `include/syscall.h`, `src/userspace/autoinit.c`, `src/userspace/xshss.c`, preserving an inline backward-compatibility alias.
    - Renamed all `x_*` helper functions in `xshss.c` to standard, clean names (`strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strchr`, `strrchr`, `trim`, `atoi`, `puts`, `putc`, `getchar`, `readline`).
    - Cleaned up `autoinit.c` (`u_strlen` -> `strlen`, `u_print` -> `puts`).
  - **File I/O Safety & Large File Streaming**:
    - Supported `offset` in `SYS_READFILE` via `vfs_read_file_offset(path, buf, max_len, offset)` in `src/fs/vfs.c` and `src/kernel/syscall.c`.
    - Eliminated 4KB cap in `cmd_cat`, `cmd_more_less`, and `cmd_cp`: files of any size stream chunk by chunk from offset 0 to EOF.
    - Implemented safe `cmd_mv`: verifies `do_cp()` returns `0` (success) before unlinking source file; if copying fails (e.g. permission denied or read error), preserves the source file completely.
    - Updated `cmd_write`: overwrites by default (`append = 0`), or appends with `-a` flag (`write -a <file> <text>`).
  - **UI & VFS Refinements**:
    - Fixed backspacing in `readline()`: replaced `puts("\b \b")` with single `putc('\b')` since kernel `console_putc('\b')` already erases the character on screen and handles serial echo, eliminating redundant 3x redraw flicker.
    - Replaced hardcoded date fallback (`"2026-09-11"`) in `vfs.c` with clean `"0000-00-00 00:00:00"` uninitialized timestamp.
    - Expanded path normalization segment capacity from 32 to 64 segments.
    - Added default `/home/user` directory creation in `vfs_init_initramfs()`.
  - **Verification**:
    - Verified all refactored operations via `scratch/test_cleanups.py` in QEMU (syscall test, write overwrite, write -a append, safe mv failure preservation, successful mv, screen clear, shutdown).
    - Passed all assertions in `test_fs_and_ls.py` and `test_phase_g.py` (full self-install & persistent standalone SATA boot) with zero regressions.
- **Dynamic Devpath Prompt Resolution Fix (2026-09-13 21:47)**:
  - **Root Cause**: `xshss.c` formatted the prompt using only `SYS_GETCWD`, which always returned the internal Unix-style root directory `/`. The kernel's `cmd_devpath()` toggled `g_boot_location.mode`, but userland had no mechanism to query `fs_get_prompt_path()`. Furthermore, running `devpath` without arguments printed status and returned early without toggling.
  - **Fix Implemented**:
    - Added `#define SYS_GET_PROMPT_PATH 46` in `include/syscall.h`.
    - Handled `SYS_GET_PROMPT_PATH` in `src/kernel/syscall.c` calling `fs_get_prompt_path(buf, size)`.
    - Updated `xshss_main` in `src/userspace/xshss.c` to query `SYS_GET_PROMPT_PATH` into `cwd_buf` with expanded 384-byte buffer (supporting long hardware devpaths).
    - Updated `src/fs/devpath.c` default mode to `DEVPATH_MODE_SOFTWARE` so prompt boots with clean `shell@pseuDOS [/] > `.
    - Updated `cmd_devpath` in `src/kernel/shell.c` so running `devpath` without args cycles through modes (`SOFTWARE` -> `HARDWARE` -> `FIRMWARE` -> `SOFTWARE`), while `devpath --info` displays current status without toggling, and `--absolute-hardware`, `--absolute-firmware`, `--absolute-software` flags set the mode directly.
  - **Verification**:
    - Verified 100% via `scratch/test_devpath.py` in QEMU: prompt correctly transitions between `[/] > `, `[PciRoot(0x0)...\] > `, and `[\] > `.
- **SATA Cache Flush & Init Kill Panic Fix (2026-09-13 22:40)**:
  - **SATA Hardware Cache Flushing**:
    - Root Cause: `ahci.c` left `dev.flush` and `dev.shutdown` unassigned (`NULL`), so disk synchronization (`fat32_sync_delete_node`, `gpt_fat32_format_and_install`) and power-off (`storage_flush_all`, `storage_shutdown_all`) never issued ATA cache flush commands to SATA drives. Uncommitted dirty writes in volatile drive DRAM/SRAM were dropped on sudden reboot/power cut, causing deleted files (`autoinit.bin`) to respawn.
    - Implemented `ahci_flush_cache_impl()` in `src/drivers/ahci.c` using non-data H2D Register FIS (`0x27`) with `ATA_CMD_FLUSH_CACHE_EXT` (`0xEA`) for 48-bit LBA drives and fallback to `ATA_CMD_FLUSH_CACHE` (`0xE7`) for 28-bit LBA drives.
    - Implemented `ahci_shutdown_impl()` in `src/drivers/ahci.c` issuing `ahci_flush_cache_impl()` and ATA `ATA_CMD_STANDBY_IMMED` (`0xE0`) to park heads / spin down drives before ACPI power-off.
    - Wired `dev.flush` and `dev.shutdown` into `probe_sata_port()` in `src/drivers/ahci.c`.
    - Added immediate hardware write cache flush calls (`g_sync_dev->flush`) in `fat32_sync_create_file()`, `fat32_sync_write_file()`, and `fat32_sync_mkdir()` in `src/fs/fat32_sync.c`.
  - **Init (PID 1) Kill Panic Protection**:
    - Root Cause: `process_kill(pid)` only checked `pid == 0`, leaving PID 1 (`autoinit`) unprotected. Terminating PID 1 silently orphaned the shell (`xshss.bin` PID 2), which upon exiting hung the system in an unrecoverable idle loop (`hlt`).
    - Added explicit PID 1 protection in `src/kernel/process.c`:
      - `process_kill`: if `pid == 1` or `p->pid == 1`, triggers `kernel_panic("Attempted to kill init! (PID 1)", NULL);`.
      - `process_exit`: if `p->pid == 1`, triggers `kernel_panic("init (PID 1) exited with code %d", NULL);`.
      - Unprivileged users attempting `kill 1` are denied with `permission denied`. Elevated users executing `sudo kill 1` trigger the classic fatal kernel panic.
  - **Verification**:
    - `test_init_panic.py` in QEMU: unprivileged `kill 1` rejected with `permission denied`; `sudo kill 1` triggered kernel panic BSOD with exact reason `Attempted to kill init! (PID 1)`.
    - `test_phase_g.py` in QEMU: end-to-end self-install to persistent SATA disk and standalone boot verified 100% with SATA flush active.
    - `test_cleanups.py` in QEMU: all file creation, overwrite, append, safe mv, screen clear, and ACPI shutdown verified 100%.
- **Dashed Address Formatting & Linux-Style Unprivileged Shutdown (2026-09-14 17:08)**:
  - **Dashed 64-Bit Address Formatting**:
    - Implemented `format_addr_dashed()` in `src/kernel/panic.c` formatting all 64-bit addresses into `0x%04X-%04X-%04X-%04X` blocks (e.g. `0x0000-0000-DEAD-BEEF`).
    - Applied to `last accessed memory`, `rip`, `rsp`, and `cr3` on the kernel panic BSOD screen.
  - **Linux-Style Shutdown & Unprivileged Power Management**:
    - Removed `PRIV_KERNEL` restriction on `SYS_REBOOT` and `SYS_SHUTDOWN` in `src/kernel/syscall.c`, allowing any user to reboot or shutdown.
    - Added scheduled shutdown support in `src/kernel/syscall.c`:
      - `shutdown` without arguments schedules shutdown 1 minute (60s) into the future, calculating scheduled target ticks and displaying target timestamp (`Shutdown scheduled for HH:MM:SS (in 1 minute), use 'shutdown -c' to cancel`).
      - `shutdown -c` cancels the active scheduled shutdown (`Shutdown cancelled`).
      - `shutdown now` executes immediate ACPI power-off.
    - Added `syscall_check_scheduled_shutdown()` invoked by `scheduler_tick()` in `src/kernel/scheduler.c` on every 10ms PIT IRQ 0 tick to trigger `acpi_shutdown()` when target tick expires.
  - **Verification**:
- **Lowercase Text Standardization (2026-09-14 18:14)**:
  - **Audit & Standardization Scope**:
    - Standardized prose and feedback messages to all-lowercase across kernel and userland subsystems.
    - Explicit exceptions strictly preserved:
      1. Hardware and system data descriptors (`Reserved MMIO`, `Paging`, `Conventional Memory`, `CR2`, `CR3`, `RIP`, `RSP`, `SATA`, `NVMe`, `USB`, `PCI`, `PCIe`, `KERNEL`, `USER`).
      2. Complete GRUB 2 chainloader and helper output in `cmd_grub()`.
      3. Autoinit warnings (`[autoinit] CRITICAL: unable to launch shell. triggering kernel panic!\n` with lowercase 't' in `triggering`).
      4. All mentions of `PID` (`PID %u`, `PID 1`, `PID 2`, etc.).
  - **Subsystems Updated**:
    - `src/userspace/xshss.c`:
      - `cmd_more_less()`: `-- more (press space/enter to continue, 'q' to quit) --`.
      - `cmd_data()`: `file metadata for:`, `node type:`, `directory` / `regular file`, `file size:`, `date created:`, `date last accessed:`, `date modified:`, `protection status:`, `protected system node` / `standard node`.
      - `cmd_syscalltest()`: `--- testing syscall interface from userspace ---`, `3. testing sys_write()... [ok]`, `--- all syscall tests completed! ---` (kept `PID` uppercase).
      - `execute_command()`: `privilege elevated to KERNEL (root)...`, `privilege reverted to USER mode.`, `rebooting system...`, `powering off system immediately...`, `dropped privileges to USER mode.`, `exiting shell...`.
    - `src/kernel/panic.c`: `fatal exception: kernel panic!`, `cpu: ... x86_64 processor` (kept `PID %u` and dashed addresses uppercase).
    - `src/kernel/syscall.c`: `shutdown cancelled.`, `shutdown scheduled for %02u:%02u:%02u (in %llu minute), use 'shutdown -c' to cancel.`, `shutdown scheduled for in %llu seconds...`.
    - `src/userspace/autoinit.c`: `[autoinit] CRITICAL: unable to launch shell. triggering kernel panic!\n`.
  - **Verification**:
    - `test_cleanups.py`: Passed 100% (syscall test with lowercase banners, write overwrite, write -a append, safe mv, screen clear, ACPI shutdown).
    - `test_shutdown_and_dashed_addr.py`: Passed 100% (lowercase fatal exception kernel panic, dashed addresses, scheduled shutdown, shutdown cancel, immediate shutdown).
    - `test_init_panic.py`: Passed 100% (unprivileged kill 1 denied, sudo kill 1 triggered fatal exception kernel panic with PID 1 reason).
    - `test_devpath.py`: Passed 100% (devpath mode cycling and prompt synchronization).
    - `test_fs_and_ls.py`: Passed 100% (classic ls with <dir>, fs health, cpu, mem, pci, devpath --info, screenres, shutdown now).
    - `test_phase_g.py`: Passed 100% (end-to-end SATA disk installation, standalone boot without ISO, full interactive verification suite).

---

## Phase H: Shell Polish & Coreutils (Planned)

### H1. Command History (Up/Down Arrow Recall)

**Where**: `src/userspace/xshss.c` — `readline()` function.

**Approach**:
- Add a static ring buffer `char history[HISTORY_MAX][256]` and `history_count` / `history_index` inside `xshss.c`.
- After each successful command entry (non-empty line), copy `line_buf` into `history[history_count % HISTORY_MAX]` and increment `history_count`.
- In `readline()`, detect ANSI escape sequences for arrow keys: the keyboard driver sends `\x1b[A` (Up) and `\x1b[B` (Down) over serial. When `getchar()` returns `\x1b`, peek the next two chars; if `[A`, navigate backward through history; if `[B`, navigate forward.
- On Up/Down: clear the current line on screen (emit `\r` + spaces + `\r` + re-emit prompt), copy the history entry into `buf`, set `idx` to its length, and echo the recalled line.
- `HISTORY_MAX` = 16 is sufficient for a minimal shell.

**Kernel changes**: None. This is entirely userland in `xshss.c`. The keyboard driver already sends raw scancodes via `SYS_READ`, and ANSI escape sequences are forwarded over serial.

---

### H2. `echo` Command

**Where**: `src/userspace/xshss.c` — new `cmd_echo()` + dispatch entry.

**Approach**:
- `cmd_echo(const char *arg)`: if `arg` is NULL or empty, print a blank newline. Otherwise, print `arg` followed by `\n`.
- Handle `$VAR` expansion if environment variables (H8) are implemented first; otherwise just print the raw text.
- Register in `execute_command()`: `else if (strcmp(cmd, "echo") == 0) { cmd_echo(arg); }`.
- Add help entry: `echo <text>               : print text to console`.

---

### H3. `date` / `time` Command

**Where**: `src/userspace/xshss.c` — new `cmd_date()` + dispatch entry.

**Approach**:
- Issue `syscall(SYS_TIME, (uint64_t)(uintptr_t)&dt, ...)` to get `rtc_datetime_t` (already defined in `include/rtc.h` and handled by `SYS_TIME` in `syscall.c`).
- Format and print: `2026-09-14 18:49:53` using manual digit formatting (no printf in userland — use `putc` and `print_num` or a small `print_padded(uint8_t val)` helper that zero-pads to 2 digits).
- Register as both `date` and `time` in `execute_command()`.
- Help entry: `date / time                : display current date and time`.

---

### H4. `uptime` Command

**Where**: `src/userspace/xshss.c` — new `cmd_uptime()` + dispatch entry. May need a new syscall or extend `SYS_TIME`.

**Approach**:
- Option A (simple): Add `#define SYS_UPTIME <next_num>` in `include/syscall.h`. In `syscall.c`, handle it by returning `pit_get_ticks()`. In userland, divide by 100 to get seconds, then format as `up X minutes, Y seconds`.
- Option B (no new syscall): Stash the boot-time RTC value at shell startup, then on `uptime` read current RTC and compute the delta. Less precise but avoids a new syscall.
- Option A is preferred since PIT ticks are monotonic and already tracked.
- Format output: `uptime: 12 minutes, 34 seconds (75400 ticks)`.

---

### H5. `uname` Command

**Where**: `src/userspace/xshss.c` — new `cmd_uname()` + dispatch entry.

**Approach**:
- Hardcode the system identification string directly in userland: `puts("pseuDOS 0.6.0-scheduling x86_64 UEFI\n")`.
- Alternatively, query `SYS_GET_BOOTINFO` for dynamic version info if `boot.cfg` ever gains a version field. For now, a hardcoded string is sufficient.
- Help entry: `uname                      : display system identification`.

---

### H6. I/O Redirection (`>` and `>>`)

**Where**: `src/userspace/xshss.c` — `execute_command()` pre-processing.

**Approach**:
- Before dispatching a command, scan the command line for `>>` (append) or `>` (overwrite) followed by a filename.
- If found:
  1. Truncate the command string at the `>` position (so the command itself doesn't see the redirect).
  2. Set a global/static redirect state: `static char redirect_path[256]; static int redirect_append;`.
  3. Temporarily replace `puts()` and `putc()` with versions that buffer output into a `redirect_buf[]` instead of calling `SYS_WRITE(fd=1, ...)`.
  4. After the command completes, write the buffer to the redirect path via `SYS_WRITEFILE` with the appropriate append flag, then reset redirect state.
- Alternatively (simpler): capture output by replacing `SYS_WRITE` fd=1 calls. Since all userland output goes through the local `puts()`/`putc()` wrappers, swapping them is straightforward.
- Simpler approach: add a `static int redirect_fd` flag. When set, `puts()` and `putc()` accumulate into a static buffer. After command dispatch, flush to file.
- Handle edge cases: spaces around `>`, quoted filenames are not necessary for v0.6.0.

---

### H7. Basic Wildcards (`*` Globbing)

**Where**: `src/userspace/xshss.c` — argument expansion before command dispatch.

**Approach**:
- Before passing `arg` to a command function, check if `arg` contains `*`.
- If it does:
  1. Split `arg` into directory prefix and pattern suffix (e.g., `/home/user/*.txt` → dir=`/home/user`, pattern=`*.txt`).
  2. Use `SYS_READDIR` to enumerate the directory (need a variant that returns names into a buffer rather than printing — may require a new syscall `SYS_READDIR_BUF` or `SYS_LISTDIR` that writes `\0`-separated names into a userland buffer).
  3. Match each entry against the pattern using a simple glob matcher: `*` matches any sequence of characters, character-by-character otherwise.
  4. Concatenate matching entries as space-separated arguments and re-dispatch.
- **New syscall needed**: `SYS_LISTDIR` — takes `(path, buf, buf_size)`, writes null-separated directory entry names into `buf`, returns total bytes written. In `syscall.c`, iterate `vfs_node->children` and copy names.
- Start with single-`*` support only (no `?`, no `**` recursion). Sufficient for `cat *.txt`, `ls /protected/*`, `del /tmp/*`.

---

### H8. Shell Environment Variables

**Where**: `src/userspace/xshss.c` — new environment table + `$VAR` expansion + `env` / `set` / `export` commands.

**Approach**:
- Static table: `struct { char name[32]; char value[128]; } env_vars[ENV_MAX];` with `ENV_MAX = 32`.
- Pre-populate on shell startup:
  - `USER=shell` (changes to `kernel` on `SYS_ELEVATE`, back to `shell` on `SYS_DROP_PRIVILEGES`).
  - `HOSTNAME=pseuDOS`.
  - `PWD=/` (updated on every `cd`).
  - `HOME=/home/user`.
  - `SHELL=/protected/crit/xshss.bin`.
- `env` command: iterate and print all `NAME=VALUE` pairs.
- `set NAME=VALUE` or `export NAME=VALUE`: set/update an environment variable.
- `$VAR` expansion: before dispatching a command, scan the command line for `$` followed by alphanumeric chars, look up in the env table, and substitute in-place (or into a scratch buffer).
- `cmd_cd()` should update `PWD` after a successful `SYS_CHDIR`.
- `kernel`/`su`/`drop`/`exit` should update `USER` accordingly.

---

### H9. `ls` Flags (`-l`, `-a`)

**Where**: `src/userspace/xshss.c` — `cmd_ls()` + kernel-side `SYS_READDIR` or new `SYS_LISTDIR_DETAILED`.

**Approach**:
- Parse flags in `cmd_ls()`: check if `arg` starts with `-`. Extract flags (`l`, `a`) and the remaining path.
- **`-a`** (show all): Currently `ls` already shows `.` and `..`. If hidden files (dotfiles) are ever skipped by default, `-a` would include them. For now, `-a` is a no-op since everything is shown, but implement the flag parsing for forward compatibility.
- **`-l`** (long listing): For each entry, query `SYS_STAT` to get size, type, timestamps, and protection status. Print in columnar format:
  ```
  drwx  protected   2026-09-14 11:14          0 B   .
  drwx  protected   2026-09-14 11:14          0 B   ..
  -rw-  standard    2026-09-14 11:14     303658 B   kernel.bin
  -rw-  standard    2026-09-14 11:14      18509 B   autoinit.bin
  ```
  Where:
  - `d` = directory, `-` = file.
  - `rw-` = readable/writable (always, since there's no per-file permission bitmap yet), or `r--` for protected nodes.
  - `protected` / `standard` from `is_protected`.
  - Date from `date_modified`.
  - Size in bytes.
  - Name.
- **New syscall consideration**: The current `SYS_READDIR` prints directly to console via `vfs_listdir()`. For `ls -l`, the shell needs per-entry metadata. Two options:
  1. Add `SYS_LISTDIR` (from H7) that returns names, then stat each one individually via `SYS_STAT`. Simple but O(n) syscalls.
  2. Add `SYS_READDIR_DETAILED` that returns a packed array of `{name, type, size, date_modified, is_protected}` structs. More efficient but more complex.
  Option 1 is simpler and sufficient for v0.6.0 directory sizes.

### Status: COMPLETE & VERIFIED (2026-09-14 19:02)

All 9 Phase H features have been implemented and verified end-to-end:
1. **H1: Command History**: 16-entry ring buffer, `history` command, and Up/Down arrow recall in `readline()` with escape sequences `\x1b[A` and `\x1b[B`.
2. **H2: `echo`**: Outputs text to console or redirected target; supports empty lines and variable-expanded strings.
3. **H3: `date` / `time`**: Queries `SYS_TIME` RTC registers and formats timestamps (`YYYY-MM-DD HH:MM:SS`).
4. **H4: `uptime`**: Uses `SYS_UPTIME` (syscall 47) returning 100 Hz PIT timer ticks and formats human-readable uptime.
5. **H5: `uname`**: Supports flags `-a`, `-r`, `-m`, `-s`, and default `pseuDOS`.
6. **H6: I/O Redirection**: Supports overwrite `>` and append `>>` with buffering, chunk flushing, and permission validation.
7. **H7: Basic Wildcards**: Single-`*` glob matcher with directory query via `SYS_LISTDIR` (syscall 48).
8. **H8: Environment Variables**: Static table for 32 variables, `env`, `set`/`export`, dynamic `PWD` tracking on `cd`, `USER` updates on privilege elevation/drop, and `$VAR` expansion.
9. **H9: `ls` Flags**: Long format `ls -l` with permissions (`drwx`/`-rw-`), protection flag, timestamp, byte size, and name. Classic `ls` preserved 100%.

### Verification Suite
- `test_phase_h.py`: Passed 100% (all 12 steps: shell boot, echo, date, uptime, uname, env & $VAR, `>` & `>>` redirection, `ls -l`, wildcards `*`, history list, Up-arrow recall, and clean ACPI shutdown).
- Regression testing:
  - `test_cleanups.py`: Passed 100% (syscall tests, write overwrite/append, mv protection & preservation, clear, shutdown).
  - `test_fs_and_ls.py`: Passed 100% (classic ls, fs, cpu, mem, pci, attached-drives, devpath --info, screenres).
  - `test_devpath.py`: Passed 100% (firmware/hardware/software prompt cycling and synchronization).
  - `test_init_panic.py`: Passed 100% (kill 1 protection and fatal exception BSOD).
  - `test_shutdown_and_dashed_addr.py`: Passed 100% (dashed addresses, scheduled shutdown, shutdown cancel, immediate shutdown).

---

## Post-Phase H Bug Fix: `ls <dir>` Stack Corruption & Directory Permissions (2026-09-15 06:20)

### Issue
Running `cd protected` followed by `ls bootmgr` resulted in `ls: cannot access '/protected//protected/...//p': no such file or directory`. In addition, directory entries in `ls -l` and `cmd_data` were displayed as regular files (`-rw-`) rather than directories (`drwx`).

### Root Cause
1. In `cmd_ls()` (`UEFI-Files/src/userspace/xshss.c`), `char temp[256]` was declared inside an inner `if (arg && arg[0] != '\0')` block. `target_path = p;` saved a pointer to this inner stack buffer. Once the `if` block exited, `temp` went out of scope and its stack space was reused by `char path[256]`.
2. When `resolve_path(target_path, path, sizeof(path))` copied the current working directory (`/protected/`) into `path`, `target_path` pointed into the exact destination buffer, creating a self-referential loop that repeatedly concatenated `/protected/` until buffer truncation.
3. In `cmd_ls`, `cmd_data`, and `do_cp`, node types were compared against `st.type == 2` (POSIX `S_IFDIR`), but pseuDOS `vfs_node_type_t` defines `VFS_NODE_DIRECTORY = 1` and `VFS_NODE_FILE = 0`.

### Resolution
1. Defined `VFS_TYPE_FILE` (0) and `VFS_TYPE_DIR` (1) in `UEFI-Files/include/syscall.h`.
2. Allocated a dedicated `target_path[256]` in `cmd_ls()` so path arguments persist across the entire command lifetime.
3. Added intermediate buffer in `resolve_path()` to guard against buffer aliasing and handled `"."` by returning CWD directly.
4. Updated `cmd_ls`, `cmd_data`, and `do_cp` to use `VFS_TYPE_DIR`.
5. Verified with `test_repro_ls.py` and `test_phase_h.py` (100% passed in QEMU).

---

## Toolchain & Environment Automation (2026-09-15 08:58)
- Added `check_env.ps1` and `check_env.sh` (along with `setup_env.ps1` and `setup_env.sh` forwarders).
- Validates host toolchain requirements: C compiler (`gcc`/`x86_64-w64-mingw32-gcc`), linker (`ld`/`x86_64-w64-mingw32-ld`), build utility (`make`), Python 3, QEMU emulator (`qemu-system-x86_64`), and EDK2/OVMF UEFI firmware.
- If dependencies are missing, automatically detects host package manager (`winget`, `choco`, `apt-get`, `dnf`, `pacman`, `apk`, `zypper`, `brew`) and installs missing packages.
- Verified execution on Windows PowerShell and Git Bash.

---

## Boot from ISO Support in Runner Scripts (2026-09-15 14:15)
- Added `-Iso` switch (`-cdrom`) and `--boot-from iso` / `--boot-from cdrom` options across all 6 runner scripts:
  - `run_normal.ps1`, `run_normal.sh`
  - `run_debug.ps1`, `run_debug.sh`
  - `run_realistic.ps1`, `run_realistic.sh`
- Allows forcing live CD-ROM boot even when persistent disk images (SATA, NVMe, USB) are attached. Attached disks remain accessible for installation (`sudo flash`) or inspection without stealing `bootindex=1`.

---

## Codebase Audit & Architectural Debugging (2026-09-15 14:20)
Comprehensive audit performed per user request without modifying source code:
1. **Bootloader & Early UART Hang**: Unbounded `while ((inb(0x3F8 + 5) & 0x20) == 0);` in `bootloader.c` and `errtext.c` locks hardware without COM1 before kernel initializes.
2. **Process Manager & Memory Leaks**:
   - `pe_loader.c`: `image_buffer` allocated via `kmalloc` is never stored or freed on process death.
   - `process.c`: `kernel_stack_base` (64KB) is never freed when a process exits or is killed.
   - `syscall.c` (`SYS_WAITPID`): Does not transition reaped processes from `PROCESS_STATE_KILLED` to `PROCESS_STATE_UNUSED`, exhausting the 64-process table after 64 process spawns.
3. **Virtual Memory & Address Space Sharing**:
   - `vmm.c`: `new_pml4[0] = g_kernel_pml4[0]` shallow-copies the first 512GB PDPT, causing userspace page mappings to alter the kernel's page table.
   - `process.c`: `process_create()` assigns `p->cr3 = vmm_get_kernel_pml4()`, meaning all processes currently execute with the shared kernel PML4 rather than isolated address spaces.
4. **Scheduler Quantum & Yield Behavior**:
   - `scheduler_schedule()`: Voluntarily yielded tasks (`scheduler_yield` / `int $48`) do not get their `time_slice` reset to `DEFAULT_TIME_SLICE`, causing them to run for partial ticks upon next dispatch.
5. **Storage & Boot Latency**:
   - `storage.c`: `ahci_init()`, `nvme_init()`, and `usb_storage_init()` each execute redundant full 65,536-function PCI bus sweeps (196,608 total I/O cycles), adding ~1-2 seconds to boot time.
   - `fat32_sync.c`: `get_fat_entry()` performs an uncached single-sector disk read for every cluster traversed.
## Phase 1 Fix: Process Lifecycle & Memory Leak Elimination (2026-09-15 14:26)
- **Issue**:
  1. `SYS_WAITPID` never marked reaped processes as `PROCESS_STATE_UNUSED`, causing dead processes to occupy slots indefinitely. After 64 processes were launched and terminated, the process table starved out and no further processes could be spawned.
  2. Each process launch permanently leaked its 64 KB `kernel_stack_base` and its PE executable buffer (`image_buffer`) from the 16 MB kernel heap upon termination.
- **Resolution**:
  1. Extended `process_t` in `include/process.h` with `void *image_base` and `size_t image_size`.
  2. Stored `image_base` and `image_size` upon process launch in `src/kernel/pe_loader.c`.
  3. Implemented `process_free_resources(process_t *p)` in `src/kernel/process.c`, safely freeing `kernel_stack_base` and `image_base` via `kfree` and setting `state = PROCESS_STATE_UNUSED` while strictly protecting PID 0 (`kernel`) and PID 1 (`autoinit`).
  4. Updated `SYS_WAITPID` in `src/kernel/syscall.c` to invoke `process_free_resources` when reaping `PROCESS_STATE_KILLED` processes.
  5. Updated `process_create()` in `src/kernel/process.c` to prioritize `PROCESS_STATE_UNUSED` slots and safely reclaim un-reaped `PROCESS_STATE_KILLED` zombies if the table ever fills up.
- **Verification**:
  - `test_proc_lifecycle.py`: Passed 100% (verified process creation, `proctest` background worker execution, multiple consecutive shell exits and `autoinit` waitpid reaping/respawn cycles, and clean ACPI shutdown).
  - `test_cleanups.py`: Passed 100% (file operations, write overwrite/append, mv safety, clear, shutdown).
  - `test_phase_h.py`: Passed 100% (echo, date, uptime, uname, env & $VAR, I/O redirection, ls -l, wildcards, history, arrow recall).
  - `test_fs_and_ls.py`: Passed 100% (classic ls, fs, cpu, mem, pci, devpath, screenres).
  - `test_repro_ls.py`: Passed 100% (`cd protected` -> `ls bootmgr` directory navigation).

---

## Phase 2 Fix: COM1 UART Infinite Busy Loop in Bootloader (2026-09-15 15:00)
- **Issue**:
  - In `UEFI-Files/src/boot/bootloader.c` and `UEFI-Files/src/boot/errtext.c`, `uart_putc` executed an unbounded `while ((inb(0x3F8 + 5) & 0x20) == 0);`.
  - On bare-metal PCs or UEFI systems without COM1 enabled, port 0x3F8 + 5 reads floating 0x00, causing the bootloader to lock up permanently in an infinite busy loop on the very first boot message before kernel loading.
- **Resolution**:
  - Implemented scratch register loopback tests (`outb(0x3F8 + 7, sig[i])`) across `bootloader.c`, `errtext.c`, and `console.c` verifying the `"hi lol"` string sequence character-by-character to detect UART presence reliably before transmitting.
  - Initialized UART baud rate and framing only when hardware is verified present.
  - Bounded TX ready wait polling in `uart_putc` with a 50,000-cycle timeout, preventing any hang even if a disconnected or faulty serial port is present.
- **Verification**:
  - Full system build succeeded (`build.ps1` -> `BOOTX64.EFI`, `kernel.bin`, `pseuDOS.iso`).
  - Tested in QEMU via `test_proc_lifecycle.py`: bootloader booted smoothly, GOP initialized, handoff completed, shell reached, commands executed, and clean shutdown passed 100%.

---

## Interactive Panic Recovery Menu & Register Dump (2026-09-17 17:25)
- **Feature**:
  - Added an interactive recovery menu to the kernel panic BSOD screen.
  - Users can press `(r)` to restart, `(d)` to dump 64-bit CPU register states, or `(s)` to trigger a clean ACPI shutdown.
- **Implementation**:
  - `include/panic.h`: Extended `panic_context_t` with complete GPRs (`rax`..`rdx`, `rsi`, `rdi`, `rbp`, `r8`..`r15`), `rflags`, `cs`, and `ss`.
  - `src/drivers/idt.c`: IDT exception handler populates all GPRs and frame registers into `panic_context_t` before invoking `kernel_panic`.
  - `src/kernel/panic.c`:
    - `dump_registers()`: Formats and displays 64-bit dashed registers (`RAX`..`R15`, `RIP`, `RFLAGS`, `CS`, `SS`, `CR0`, `CR2`, `CR3`, `CR4`).
    - Direct hardware keyboard polling via `keyboard_getchar()`: When interrupts are disabled by `cli`, `keyboard_getchar()` automatically bypasses the IRQ queue and directly polls PS/2 ports `0x60`/`0x64` and COM1 UART `0x3F8`.
    - Key dispatch loop:
      - `(r)` / `(R)`: Restarts machine via `acpi_reboot()`.
      - `(d)` / `(D)`: Displays full register dump and reprompts.
      - `(s)` / `(S)`: Powers off machine via `acpi_shutdown()`.
- **Verification**:
  - `test_panic_recovery.py`: Triggered panic from shell (`panic interactive recovery test`), verified BSOD rendering, pressed `d` to dump all registers (`cr0`, `cr3`, `rax`, `rip`), then pressed `s` to verify clean ACPI shutdown and QEMU process exit.
  - `test_panic_reboot.py`: Triggered panic from shell, pressed `r` to verify ACPI reboot back into firmware/bootloader (`bootmgfw`).
  - `test_cleanups.py`: Regression test suite passed 100%.




