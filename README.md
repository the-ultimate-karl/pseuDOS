# pseuDOS

**A 64-Bit Bare-Metal Operating System for Modern UEFI Hardware**

[![Architecture: x86_64](https://img.shields.io/badge/Architecture-x86__64-blue.svg)](#)
[![Firmware: UEFI 64-bit](https://img.shields.io/badge/Firmware-UEFI%20Class%203-brightgreen.svg)](#)
[![Version: v0.5.4-baremetal](https://img.shields.io/badge/Version-v0.5.4--baremetal-orange.svg)](#)
[![License: MIT](https://img.shields.io/badge/License-MIT-purple.svg)](#)

pseuDOS is a freestanding, 64-bit operating system engineered from scratch for modern x86_64 personal computers. It boots natively from UEFI firmware, transitions to long mode without legacy BIOS dependencies, and provides custom bare-metal drivers for NVMe PCIe SSDs, AHCI SATA drives, xHCI/EHCI USB storage, ACPI hardware power management, and dynamic FAT32 filesystem synchronization.

---

## The Vision & Origin

> *"THE SOLE GOAL OF THIS PROJECT IS TO PORT THE COPY OF pseuDOS ON /ROOT/pseuDOS TO 64 BIT BOOTABLE UEFI. INSTEAD OF DOING EVERYTHING ALL AT ONCE I NEED TO DO IT SEQUENTIALLY: FIRST, MAKE A BOOTLOADER. SECOND, MAKE A KERNEL. AND SO ON UNTIL I HAVE PORTED OVER 95% OF CODE FROM pseuDOS."*
> 
> — Original Project Manifesto (August 28, 2026)

pseuDOS started as a Python-simulated environment. It has been completely re-engineered into native C and x86_64 assembly, designed to run directly on physical silicon without any underlying host OS.

---

## Key Features & Architecture

### 1. UEFI Boot Manager (`BOOTX64.EFI` / `[bootmgfw]`)
- **PE32+ Executable Engine**: Parses Windows-format PE32+ kernel binaries, maps sections into physical execution pages, cleanly zeroes uninitialized `.bss` memory, and applies dynamic `.reloc` base relocations (`DIR64` and `HIGHLOW`).
- **Dynamic VM vs. Bare-Metal GOP Resolution**:
  - Uses `cpuid` leaf `1` (ECX bit 31 hypervisor detection) to inspect the execution environment.
  - Automatically targets **1280x720 (720p)** in virtualized environments (QEMU, KVM, VMware, VirtualBox, Hyper-V).
  - Automatically targets **1920x1080 (1080p True Color)** on bare-metal hardware.
- **Auto-Centered Diagnostic Recovery (`errtext`)**: In the event of missing or corrupted kernel payloads, presents an auto-centered, formatted recovery dialog detailing diagnostics and recovery instructions.
- **Payload Preservation**: Retains the pristine, unrelocated raw kernel binary in memory so the self-installer (`flash`) can format and deploy to disks on the fly.
- **Clean Firmware Handoff**: Discovers ACPI RSDP table pointers, captures physical RAM descriptors, and exits UEFI Boot Services seamlessly.

### 2. Kernel Core & Hardware Drivers
- **Fast Framebuffer Console**: Hardware double-buffered text renderer using an embedded 8x16 bitmap font. Framebuffer scrolling updates dirty rows in system RAM and uses 64-bit Write-Combining memory copy routines to prevent PCIe MMIO read stalls.
- **Full 256-Vector IDT & Exception Normalizer**: Dedicated assembly interrupt service routines (`isr_stubs.s`) with uniform stack frame normalization, error code synthesis, and 8259 PIC spurious IRQ 7 / IRQ 15 suppression.
- **Dual Keyboard Subsystem**: Hardware i8042 PS/2 controller driver configured via command byte `0x60` for IRQ 1 interrupts, backed by a non-racing hardware polling fallback and scratch-register-validated COM1 UART serial loopback.
- **Physical Memory Management**: Dynamic 16 MB kernel heap allocator (`kmalloc`, `kcalloc`, `kfree`) with arithmetic overflow guards and pointer integrity checks.

### 3. Storage Drivers & Direct DMA
- **NVMe PCIe SSD Driver**: High-performance NVM Express driver supporting PCIe DMA with 4KB page-aligned PRPs. Features volatile cache flushing (`NVME_CMD_FLUSH 0x00`) and graceful ACPI shutdown notifications (`CC.SHN = 01b`, `CSTS.SHST == 10b`) to protect on-disk flash.
- **AHCI SATA Controller Driver**: Full SATA 1.5/3.0/6.0 Gbps support utilizing 1024-byte command lists (CLB), 256-byte received FIS buffers (FB), 128-byte command tables (CTBA), and BIOS/OS handoff (`BOHC`).
- **USB Mass Storage**: xHCI (USB 3.0/3.1) and EHCI (USB 2.0) device enumeration with strict interface class verification (`0x08`) protecting USB HID keyboards from accidental reset.

### 4. Filesystem & Self-Installer (`flash`)
- **Dynamic FAT32 Synchronization**: Physical write-through synchronization for file creation, writes, appends, and recursive deletion (`del -rf`).
- **Disk Self-Installer (`flash`)**: Automatically scans for attached internal SATA/NVMe or external USB storage, partitions the target with a Protective MBR and GUID Partition Table (GPT), formats the EFI System Partition (ESP) with dynamic FAT32 cluster geometry, installs `BOOTX64.EFI`, `kernel.bin`, and writes an automated `startup.nsh` boot hook.

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
| `ls` / `dir` | `[path]` | Lists files and directories in the current or specified path. |
| `cd` | `<path>` | Changes the current working directory. |
| `pwd` | — | Prints the absolute working directory path. |
| `cat` / `type` | `<file>` | Displays the contents of a text file. |
| `touch` | `<file>` | Creates a new, empty file on the active filesystem. |
| `write` | `<file> <text>` | Writes or appends plaintext data to a file. |
| `mkdir` | `<path>` | Creates a new directory on the active filesystem. |
| `del` / `rm` | `[-rf] <path>` | Removes files or directories (`-rf` enables recursive deletion). |
| `fs` | `[drive_no]` | Displays active VFS statistics, boot hardware devpath, and partition health. |
| `attached-drives`| `[--internal\|--external\|--all]` | Lists all detected storage drives, connection buses, and capacities. |
| `switch-target` | `<--internal\|--external>` | Toggles default storage target views. |
| `flash` | — | Interactive GPT & FAT32 partitioner and bare-metal OS self-installer. |
| `screenres` | `[width height]` | Queries or dynamically switches GOP graphical display resolutions. |
| `cpu` | — | Displays CPU vendor, brand string, topology, and architectural feature flags. |
| `mem` | — | Displays physical RAM regions, memory types, and kernel heap consumption. |
| `pci` | — | Scans and lists connected PCI and PCI Express bus devices and class codes. |
| `devpath` | `[--show\|--toggle]` | Displays the active UEFI boot media device path string. |
| `clear` / `cls` | — | Clears the graphical screen and resets console cursor position. |
| `reboot` | — | Commits storage write caches and resets the CPU via ACPI or 8042 reset. |
| `shutdown` | — | Issues NVMe flush and shutdown notifications, then powers off the PC via ACPI S5. |
| `halt` | — | Disables CPU interrupts (`cli`) and suspends execution (`hlt`). |

---

## Testing & Quality Assurance

All modifications are tested against real hardware and virtualization suites according to the guidelines in [AGENTS.md](AGENTS.md). All discovered bugs, regression causes, and hardware-specific edge cases are strictly documented in [BUGSPAWN.LOG](BUGSPAWN.LOG).

---

## License

This project is open source and available under the terms of the **MIT License**.
