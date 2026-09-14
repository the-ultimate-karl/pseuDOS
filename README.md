# pseuDOS

**A 64-Bit Preemptive Multitasking Operating System for Modern UEFI Hardware**

[![Architecture: x86_64](https://img.shields.io/badge/Architecture-x86__64-blue.svg)](#)
[![Firmware: UEFI 64-bit](https://img.shields.io/badge/Firmware-UEFI%20Class%203-brightgreen.svg)](#)
[![Version: v0.6.0-scheduling](https://img.shields.io/badge/Version-v0.6.0--scheduling-orange.svg)](#)
[![License: MIT](https://img.shields.io/badge/License-MIT-purple.svg)](#)

pseuDOS is a freestanding, 64-bit operating system engineered from scratch for modern x86_64 personal computers. It boots natively from UEFI firmware, transitions to long mode without legacy BIOS dependencies, and provides preemptive round-robin multitasking, fast x86_64 syscalls, a modular userspace boot chain (`autoinit` -> `xshss`), custom bare-metal drivers for NVMe PCIe SSDs, AHCI SATA drives, xHCI/EHCI USB storage, ACPI hardware power management, and dynamic FAT32 filesystem synchronization.

---

## The Vision & Origin

> *"THE SOLE GOAL OF THIS PROJECT IS TO PORT THE COPY OF pseuDOS ON /ROOT/pseuDOS TO 64 BIT BOOTABLE UEFI. INSTEAD OF DOING EVERYTHING ALL AT ONCE I NEED TO DO IT SEQUENTIALLY: FIRST, MAKE A BOOTLOADER. SECOND, MAKE A KERNEL. AND SO ON UNTIL I HAVE PORTED OVER 95% OF CODE FROM pseuDOS."*
> 
> — Original Project Manifesto (August 28, 2026)

pseuDOS started as a Python-simulated environment. It has been completely re-engineered into native C and x86_64 assembly, designed to run directly on physical silicon without any underlying host OS.

---

## Key Features & Architecture

### 1. Modular Boot Chain & PE32+ Relocatable Loader
- **Boot Chain**: `BOOTX64.EFI` (UEFI boot manager) → `kernel.bin` (PID 0 kernel idle) → `autoinit.bin` (PID 1 supervisor) → `xshss.bin` (PID 2 experimental shell subsystem).
- **Standalone PE32+ Loader**: Relocatable PE32+ executable engine (`pe_loader.c`) that dynamically allocates memory, maps `.text`, `.data`, and `.bss` sections, and applies `DIR64` base relocations.
- **Dynamic VM vs. Bare-Metal GOP Resolution**:
  - Uses `cpuid` leaf `1` (ECX bit 31 hypervisor detection) to inspect the execution environment.
  - Automatically targets **1280x720 (720p)** in virtualized environments (QEMU, KVM, VMware, VirtualBox, Hyper-V).
  - Automatically targets **1920x1080 (1080p True Color)** on bare-metal hardware.
- **Clean Firmware Handoff**: Discovers ACPI RSDP table pointers, captures physical RAM descriptors, and exits UEFI Boot Services seamlessly.

### 2. Preemptive Multitasking & Fast Syscall Interface
- **Preemptive Round-Robin Scheduler**: Driven by 8254 Programmable Interval Timer (PIT) IRQ 0 at 100 Hz (10ms tick rate) with time-slice quantum slicing (`DEFAULT_TIME_SLICE = 5` ticks = 50ms).
- **Process Management**: 64-slot process table (`process.c`) tracking process state, isolated 64KB execution stacks, CPU ticks, and privilege levels.
- **Fast x86_64 Syscalls**: Implemented via `STAR`, `LSTAR`, and `SFMASK` Model-Specific Registers (MSRs) with assembly context save/restore stubs (`syscall_entry.s`).
- **Policy-Based Privilege Separation**: Kernel-enforced privilege tokens (`PRIV_USER` and `PRIV_KERNEL`) with `sudo`, `kernel`/`su`, `drop`, and `exit` controls. Unprivileged processes are barred from modifying protected paths (`/protected/`), killing other processes, or flashing disks.

### 3. Memory Architecture & Hardware Protection
- **64-bit GDT & TSS**: Custom segment descriptors and Task State Segment with `rsp0` interrupt stack for ring transition safety (`gdt.c`).
- **Physical Memory Manager (PMM)**: 4KB bitmap page allocator managing physical memory blocks discovered via the UEFI memory map (`pmm.c`).
- **Virtual Memory Manager (VMM)**: 4-level paging (PML4, PDPT, PD, PT) with identity-mapped physical memory and kernel higher-half mappings (`vmm.c`).
- **Kernel Panic Screen (BSOD)**: Blue screen exception handler detailing register state (`RIP`, `RSP`, `CR3`), CPU brand, uptime, and dashed address formatting (`0x0000-0000-0000-0000`).

### 4. Storage Drivers & Direct DMA
- **NVMe PCIe SSD Driver**: High-performance NVM Express driver supporting PCIe DMA with 4KB page-aligned PRPs. Features volatile cache flushing (`NVME_CMD_FLUSH 0x00`) and graceful ACPI shutdown notifications (`CC.SHN = 01b`, `CSTS.SHST == 10b`) to protect on-disk flash.
- **AHCI SATA Controller Driver**: Full SATA 1.5/3.0/6.0 Gbps support utilizing 1024-byte command lists (CLB), 256-byte received FIS buffers (FB), 128-byte command tables (CTBA), and BIOS/OS handoff (`BOHC`).
- **USB Mass Storage**: xHCI (USB 3.0/3.1) and EHCI (USB 2.0) device enumeration with strict interface class verification (`0x08`) protecting USB HID keyboards from accidental reset.

### 5. Filesystem, Self-Installer (`flash`) & GRUB 2 Integration
- **Dynamic FAT32 Synchronization**: Physical write-through synchronization for file creation, writes, appends, and recursive deletion (`del -rf`), backed by multi-cluster recursive directory loading.
- **Disk Self-Installer (`flash`)**: Automatically scans for attached internal SATA/NVMe or external USB storage, partitions the target with a Protective MBR and GUID Partition Table (GPT), formats the EFI System Partition (ESP) with dynamic FAT32 cluster geometry, installs `BOOTX64.EFI`, `kernel.bin`, and writes an automated `startup.nsh` boot hook.
- **GRUB 2 Compatibility**: Out-of-the-box dual-boot support deploying `/EFI/pseuDOS/` payloads and a built-in `grub` helper command displaying chainloader configuration snippets.

### 6. Shell Subsystem (`xshss`) & Coreutils
- **Interactive History**: 16-entry ring buffer with Up/Down arrow recall in `readline()` and `history` command.
- **I/O Redirection**: Standard output redirection (`>` overwrite and `>>` append) with multi-chunk buffer flushing.
- **Wildcards (`*`)**: Single-star glob pattern expansion across directory nodes.
- **Shell Environment**: Variable storage (`USER`, `HOSTNAME`, `PWD`, `HOME`, `SHELL`), dynamic `PWD` tracking, and `$VAR` parameter expansion.
- **Linux-style Unprivileged Shutdown**: 1-minute default timer with cancellation (`shutdown -c`) or immediate poweroff (`shutdown now`).

---

## Repository Layout

```text
pseuDOS/
├── UEFI-Files/                  # 64-bit UEFI OS Source Tree
│   ├── bin/                     # Compiled EFI binaries and PE32+ payloads
│   ├── build/                   # Output bootable ISOs and disk images
│   ├── include/                 # Kernel and driver header files
│   │   ├── bootinfo.h           # Bootloader-to-kernel handoff structure
│   │   ├── drivers.h            # IDT, keyboard, framebuffer, and console API
│   │   ├── efi.h                # UEFI specification structures & GUIDs
│   │   ├── errtext.h            # Bootloader recovery dialog declarations
│   │   ├── fs.h                 # Virtual Filesystem & FAT32 sync declarations
│   │   ├── io.h                 # Port I/O (inb, outb, inl, outl) inline helpers
│   │   ├── lib.h                # String manipulation and heap memory headers
│   │   └── storage.h            # Unified mass-storage controller interface
│   ├── scripts/
│   │   └── make_iso.py          # Standalone pure-Python El Torito ISO builder
│   └── src/
│       ├── boot/                # UEFI bootloader and centered recovery dialog
│       ├── drivers/             # ACPI, AHCI, NVMe, USB, Console, IDT, PS/2
│       ├── fs/                  # GPT partitioning, FAT32 sync, and VFS tree
│       ├── kernel/              # Kernel entry, command shell, and payloads
│       └── lib/                 # Memory heap, GUID matching, string routines
├── build.ps1                    # Native Windows 11 PowerShell build orchestrator
├── build.sh                     # Linux / macOS Bash build script
├── run_normal.ps1               # Standard QEMU virtual machine runner
├── run_debug.ps1                # QEMU runner with timestamped bootlogging
├── run_realistic.ps1            # Strict bare-metal simulation (Q35, IOMMU, NUMA)
├── AGENTS.md                    # Engineering protocols for autonomous maintainers
└── BUGSPAWN.LOG                 # Comprehensive regression and bug tracking log
```

---

## Toolchain & Build Prerequisites

### Windows (Recommended)
1. **MinGW-w64 GCC**: `gcc` and `ld` on your system `PATH` (e.g., via WinLibs or MSYS2: `C:\mingw64\bin`).
2. **Python 3.8+**: Used for building the bootable El Torito ISO and embedding PE payloads.
3. **QEMU for Windows**: `qemu-system-x86_64` (installed at `C:\Program Files\qemu` or on `PATH`).
4. **EDK2 / OVMF UEFI Firmware**: Standard OVMF code binary (`edk2-x86_64-code.fd` or `OVMF.fd`).

### Linux / WSL
Install standard development packages:
```bash
sudo apt update
sudo apt install build-essential gcc-mingw-w64-x86-64 qemu-system-x86 ovmf python3
```

---

## Compiling pseuDOS

To build the entire operating system, compile the UEFI bootloader, map the kernel payload, and generate `UEFI-Files/build/pseuDOS.iso`:

### Windows PowerShell:
```powershell
.\build.ps1
```

### Linux / WSL:
```bash
./build.sh
```

---

## Running & Testing with QEMU

pseuDOS provides three test runners to simulate different hardware configurations:

### 1. Normal Mode (`run_normal.ps1`)
Runs the system in standard virtualized mode:
```powershell
# Run with graphical window and an attached NVMe SSD:
.\run_normal.ps1 -Gui -Nvme

# Run with an AHCI SATA drive in the terminal:
.\run_normal.ps1 -Terminal -Sata

# Run with all storage controllers attached (SATA, NVMe, USB 3.0):
.\run_normal.ps1 -Gui -All
```

### 2. Strict Bare-Metal Simulation (`run_realistic.ps1`)
Simulates physical hardware with an Intel Q35 chipset, System Management Mode (SMM), Intel VT-d IOMMU address translation, multi-node NUMA memory layouts, and PCIe Root Ports:
```powershell
# Run strict bare-metal hardware simulation:
.\run_realistic.ps1 -Gui -Nvme

# Verbose hardware topology logs:
.\run_realistic.ps1 -Gui -Nvme -Detail
```

### 3. Standalone Disk Boot (Booting Installed Disks)
Once you have installed pseuDOS to a virtual drive using `flash`, boot directly from the persistent disk without mounting the live ISO:
```powershell
# Boot directly from the installed NVMe drive:
.\run_realistic.ps1 -Gui --boot-from nvme

# Boot directly from the installed SATA drive:
.\run_realistic.ps1 -Gui --boot-from sata
```

---

## Installing to Physical Bare-Metal Hardware

### Step 1: Create Installation Media
1. Obtain the compiled ISO from `UEFI-Files/build/pseuDOS.iso`.
2. Flash the ISO onto a USB flash drive (minimum 1 GB) using [Rufus](https://rufus.ie/) with the following settings:
   - **Partition Scheme**: `GPT`
   - **Target System**: `UEFI (non CSM)`
   - **File System**: `FAT32`

### Step 2: Configure Physical PC BIOS / UEFI Settings
- Disable **Secure Boot** (pseuDOS is unsigned bare-metal code).
- Set storage controller mode to **AHCI** or **NVMe** (disable Intel RST / RAID).
- Boot from the USB flash drive in pure UEFI mode.

### Step 3: Self-Installation to Hard Drive
1. When pseuDOS boots to the shell, type:
   ```text
   flash
   ```
2. Select the target drive number from the list (e.g. your internal NVMe SSD or SATA hard disk).
3. Confirm drive formatting.
4. When the installation completes, remove the USB drive and press `ENTER`. pseuDOS will perform an ACPI reboot directly into your newly installed system.

---

## Command Reference

| Command | Arguments | Description |
| :--- | :--- | :--- |
| `help` | — | Displays all available kernel shell commands. |
| `echo` | `<text>` | Prints text directly to console or redirected target. |
| `date` / `time` | — | Displays the current hardware RTC date and time (`YYYY-MM-DD HH:MM:SS`). |
| `uptime` | — | Displays system uptime and 100 Hz PIT timer tick count. |
| `uname` | `[-a\|-r\|-m\|-s]` | Displays system identification, kernel release, and machine architecture. |
| `env` | — | Lists all active shell environment variables (`NAME=VALUE`). |
| `set` / `export` | `[name=value]` | Sets or inspects shell environment variables (supports `$VAR` expansion). |
| `history` | — | Displays recent command history with numbered recall entries. |
| `ls` / `dir` | `[-l] [-a] [path]` | Lists directory entries (supports `-l` detailed format and wildcards `*`). |
| `cd` | `<path>` | Changes current working directory and updates `$PWD`. |
| `pwd` | — | Prints the absolute working directory path. |
| `cat` / `type` | `<file>` | Displays the contents of a text file (supports wildcard filenames). |
| `more` / `less` | `<file>` | Paginated plaintext viewer (Space/Enter/Q controls). |
| `data` | `<file>` | Displays creation/access/modification timestamps and metadata. |
| `touch` | `<file>` | Creates a new, empty file on the active filesystem. |
| `write` | `[-a] <file> <text>` | Writes or appends plaintext data to a file. |
| `cp` | `<src> <dst>` | Copies files with directory resolution and write-through sync. |
| `mv` | `<src> <dst>` | Moves or renames files with failure preservation guards. |
| `mkdir` | `<path>` | Creates a new directory on the active filesystem. |
| `del` / `rm` | `[-rf] <path>` | Removes files or directories (`-rf` enables recursive deletion). |
| `ps` | — | Lists active processes, states, CPU ticks, and privilege levels. |
| `kill` | `<pid>` | Terminates a process (requires `sudo` for non-owned processes). |
| `proctest` | — | Tests preemptive multitasking with concurrent background tasks. |
| `syscalltest` | — | Validates the fast x86_64 syscall interface from userspace. |
| `kernel` / `su` | — | Escalates privilege level to `PRIV_KERNEL` (root mode). |
| `exit` / `drop` | — | Drops elevated privileges back to `PRIV_USER` mode, or exits shell. |
| `sudo` | `<command>` | Executes a single command with elevated `PRIV_KERNEL` privileges. |
| `dmesg` | — | Dumps the in-memory kernel message buffer ring. |
| `grub` | — | Displays GRUB 2 chainloader configuration snippets and setup steps. |
| `flash` | — | Interactive GPT partitioner, FAT32 ESP formatter, and OS installer. |
| `fs` | `[drive_no\|--drives]` | Displays active VFS statistics, storage devpath, and partition health. |
| `attached-drives`| `[--all]` | Lists all detected storage drives, connection buses, and capacities. |
| `switch-target` | `<--int\|--ext>` | Toggles default mass-storage target views. |
| `screenres` | `[width height]` | Queries or dynamically switches GOP graphical display resolutions. |
| `cpu` | — | Displays CPU vendor, brand string, topology, and architectural feature flags. |
| `mem` | — | Displays physical RAM regions, memory types, and kernel heap consumption. |
| `pci` | — | Scans and lists connected PCI and PCI Express bus devices and class codes. |
| `devpath` | `[--mode\|--info]` | Displays and toggles boot media device path string representation. |
| `panic` | `[reason]` | Triggers a Blue Screen of Death (BSOD) kernel panic for diagnostic testing. |
| `clear` / `cls` | — | Clears the screen and resets console cursor position. |
| `reboot` | — | Commits storage write caches and resets the CPU. |
| `shutdown` | `[now\|-c]` | Schedules shutdown in 1 minute, or 'now' to power off immediately via ACPI. |
| `halt` | — | Suspends CPU execution via `hlt` (requires sudo). |

---

## Testing & Quality Assurance

All modifications are tested against real hardware and virtualization suites according to the guidelines in [AGENTS.md](AGENTS.md). All discovered bugs, regression causes, and hardware-specific edge cases are strictly documented in [BUGSPAWN.LOG](BUGSPAWN.LOG).

---

## License

This project is open source and available under the terms of the **MIT License**.
