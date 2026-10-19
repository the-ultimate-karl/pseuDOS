#!/usr/bin/env python3
import struct
import os
import sys
import subprocess
import tempfile
import argparse
import shutil

def make_fat12_esp(output_path, efi_binary_path, kernel_binary_path=None, initramfs_binary_path=None, recovery_binary_path=None, size_kb=8192):
    with open(efi_binary_path, "rb") as f:
        efi_data = f.read()

    kernel_data = None
    if kernel_binary_path and os.path.exists(kernel_binary_path):
        with open(kernel_binary_path, "rb") as f:
            kernel_data = f.read()

    initramfs_data = None
    if initramfs_binary_path and os.path.exists(initramfs_binary_path):
        with open(initramfs_binary_path, "rb") as f:
            initramfs_data = f.read()

    recovery_data = None
    if recovery_binary_path and os.path.exists(recovery_binary_path):
        with open(recovery_binary_path, "rb") as f:
            recovery_data = f.read()

    needed_bytes = len(efi_data) + (len(kernel_data) if kernel_data else 0) + (len(initramfs_data) if initramfs_data else 0) + (len(recovery_data) if recovery_data else 0) + 1024 * 1024
    needed_kb = (needed_bytes + 1023) // 1024
    if size_kb < needed_kb:
        size_kb = ((needed_kb + 2047) // 2048) * 2048

    sector_size = 512
    total_sectors = (size_kb * 1024) // sector_size
    sec_per_clus = 8
    rsvd_sec = 1
    num_fats = 2
    root_entries = 224
    root_sectors = (root_entries * 32) // sector_size
    
    total_clusters = (total_sectors - rsvd_sec - root_sectors) // sec_per_clus
    fat_bytes = (total_clusters * 3 + 1) // 2
    sec_per_fat = (fat_bytes + sector_size - 1) // sector_size + 1
    
    image = bytearray(total_sectors * sector_size)
    
    # BPB
    struct.pack_into("<3s8sHBHBHHBHHHII", image, 0,
        b"\xeb\x3c\x90", b"MSDOS5.0",
        sector_size, sec_per_clus, rsvd_sec, num_fats,
        root_entries, total_sectors, 0xF0, sec_per_fat,
        36, 2, 0, 0
    )
    # Extended BPB
    struct.pack_into("<BBBI11s8s", image, 36,
        0x00, 0, 0x29, 0x12345678, b"EFI BOOT   ", b"FAT12   "
    )
    image[510] = 0x55
    image[511] = 0xAA
    
    fat = bytearray(sec_per_fat * sector_size)
    fat[0] = 0xF0
    fat[1] = 0xFF
    fat[2] = 0xFF
    
    def set_fat12(entry, val):
        offset = (entry * 3) // 2
        if offset + 1 >= len(fat): return
        if entry % 2 == 0:
            fat[offset] = val & 0xFF
            fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((val >> 8) & 0x0F)
        else:
            fat[offset] = (fat[offset] & 0x0F) | ((val << 4) & 0xF0)
            fat[offset + 1] = (val >> 4) & 0xFF

    cur_cluster = 2
    def alloc(n):
        nonlocal cur_cluster
        if n == 0: return 0
        s = cur_cluster
        for i in range(n):
            c = s + i
            nxt = c + 1 if i < n - 1 else 0x0FFF
            set_fat12(c, nxt)
        cur_cluster += n
        return s

    data_start_sec = rsvd_sec + num_fats * sec_per_fat + root_sectors
    def write_clus(c, d):
        off = (data_start_sec + (c - 2) * sec_per_clus) * sector_size
        image[off:off + len(d)] = d
        
    def mkentry(name8_3, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name8_3.encode("ascii")
        e[11] = attr
        struct.pack_into("<H", e, 26, cluster)
        struct.pack_into("<I", e, 28, size)
        return e

    def lfn_checksum(name8_3):
        s = 0
        for b in name8_3.encode("ascii")[:11]:
            s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
        return s

    def mklfn(long_name, name8_3, attr, cluster, size):
        if long_name in (".", ".."):
            return mkentry(name8_3, attr, cluster, size)
        chk = lfn_checksum(name8_3)
        chunks = [long_name[i:i+13] for i in range(0, len(long_name), 13)]
        out = bytearray()
        total = len(chunks)
        for seq in range(total, 0, -1):
            chunk = chunks[seq - 1]
            e = bytearray(32)
            e[0] = seq | (0x40 if seq == total else 0x00)
            chars = [ord(c) for c in chunk]
            if len(chars) < 13:
                chars.append(0x0000)
                while len(chars) < 13:
                    chars.append(0xFFFF)
            for i in range(5):
                struct.pack_into("<H", e, 1 + i * 2, chars[i])
            e[11] = 0x0F
            e[12] = 0x00
            e[13] = chk
            for i in range(6):
                struct.pack_into("<H", e, 14 + i * 2, chars[5 + i])
            struct.pack_into("<H", e, 26, 0x0000)
            for i in range(2):
                struct.pack_into("<H", e, 28 + i * 2, chars[11 + i])
            out.extend(e)
        out.extend(mkentry(name8_3, attr, cluster, size))
        return out

    def append_dir(buf, off, data):
        buf[off:off+len(data)] = data
        return off + len(data)

    efi_clus = alloc(1)
    boot_clus = alloc(1)
    pseudos_clus = alloc(1) if kernel_data else 0
    protect_clus = alloc(1) if kernel_data else 0
    krnl_clus = alloc(1) if kernel_data else 0
    bootmgr_clus = alloc(1) if kernel_data else 0
    bootcfg_clus = alloc(1) if kernel_data else 0
    grubcfg_clus = alloc(1) if kernel_data else 0
    osrelease_clus = alloc(1) if kernel_data else 0
    
    root_off = (rsvd_sec + num_fats * sec_per_fat) * sector_size
    roff = root_off
    roff = append_dir(image, roff, mklfn("EFI", "EFI        ", 0x10, efi_clus, 0))
    if protect_clus:
        roff = append_dir(image, roff, mklfn("protected", "PROTEC~1   ", 0x10, protect_clus, 0))
    
    # EFI dir contains BOOT and pseuDOS subdirectories
    d = bytearray(sec_per_clus * sector_size)
    off = 0
    off = append_dir(d, off, mkentry(".          ", 0x10, efi_clus, 0))
    off = append_dir(d, off, mkentry("..         ", 0x10, 0, 0))
    off = append_dir(d, off, mklfn("BOOT", "BOOT       ", 0x10, boot_clus, 0))
    if kernel_data:
        off = append_dir(d, off, mklfn("pseuDOS", "PSEUDOS    ", 0x10, pseudos_clus, 0))
    write_clus(efi_clus, d)
    
    # BOOT dir with BOOTX64.EFI and recovery.efi
    c_cnt = (len(efi_data) + sec_per_clus * sector_size - 1) // (sec_per_clus * sector_size)
    c_start = alloc(c_cnt)
    write_clus(c_start, efi_data)
    
    rec_start = 0
    if recovery_data:
        rec_cnt = (len(recovery_data) + sec_per_clus * sector_size - 1) // (sec_per_clus * sector_size)
        rec_start = alloc(rec_cnt)
        write_clus(rec_start, recovery_data)

    d2 = bytearray(sec_per_clus * sector_size)
    off = 0
    off = append_dir(d2, off, mkentry(".          ", 0x10, boot_clus, 0))
    off = append_dir(d2, off, mkentry("..         ", 0x10, efi_clus, 0))
    off = append_dir(d2, off, mklfn("BOOTX64.EFI", "BOOTX64 EFI", 0x20, c_start, len(efi_data)))
    if recovery_data and rec_start:
        off = append_dir(d2, off, mklfn("recovery.efi", "RECOVERYEFI", 0x20, rec_start, len(recovery_data)))
    write_clus(boot_clus, d2)

    # pseuDOS dir
    if kernel_data:
        k_cnt = (len(kernel_data) + sec_per_clus * sector_size - 1) // (sec_per_clus * sector_size)
        k_start = alloc(k_cnt)
        write_clus(k_start, kernel_data)

        d3 = bytearray(sec_per_clus * sector_size)
        off = 0
        off = append_dir(d3, off, mkentry(".          ", 0x10, pseudos_clus, 0))
        off = append_dir(d3, off, mkentry("..         ", 0x10, efi_clus, 0))
        off = append_dir(d3, off, mklfn("vpkernel", "VPKERNEL   ", 0x20, k_start, len(kernel_data)))
        off = append_dir(d3, off, mklfn("BOOTX64.EFI", "BOOTX64 EFI", 0x20, c_start, len(efi_data)))
        off = append_dir(d3, off, mklfn("silo.efi", "SILO    EFI", 0x20, c_start, len(efi_data)))

        # GRUB config snippet and OS release info
        grub_bytes = b"# GRUB 2 configuration snippet for pseuDOS\r\nmenuentry \"pseuDOS x86_64\" {\r\n    insmod fat\r\n    insmod chain\r\n    search --no-floppy --set=root --file /EFI/pseuDOS/BOOTX64.EFI\r\n    chainloader /EFI/pseuDOS/BOOTX64.EFI\r\n}\r\n"
        write_clus(grubcfg_clus, grub_bytes)
        off = append_dir(d3, off, mklfn("grub.cfg", "GRUB    CFG", 0x20, grubcfg_clus, len(grub_bytes)))

        osrelease_bytes = b"NAME=\"pseuDOS\"\r\nID=pseudos\r\nVERSION=\"0.7.0\"\r\nPRETTY_NAME=\"pseuDOS v0.7.0\"\r\nHOME_URL=\"https://github.com/the-ultimate-karl/pseuDOS\"\r\n"
        write_clus(osrelease_clus, osrelease_bytes)
        off = append_dir(d3, off, mklfn("os-release", "OS-REL~1   ", 0x20, osrelease_clus, len(osrelease_bytes)))

        write_clus(pseudos_clus, d3)

        # protected dir contains krnl and bootmgr
        dp = bytearray(sec_per_clus * sector_size)
        off = 0
        off = append_dir(dp, off, mkentry(".          ", 0x10, protect_clus, 0))
        off = append_dir(dp, off, mkentry("..         ", 0x10, 0, 0))
        off = append_dir(dp, off, mklfn("krnl", "KRNL       ", 0x10, krnl_clus, 0))
        off = append_dir(dp, off, mklfn("bootmgr", "BOOTMGR    ", 0x10, bootmgr_clus, 0))
        write_clus(protect_clus, dp)

        # krnl dir with primary vpkernel and initramfs.img
        dk = bytearray(sec_per_clus * sector_size)
        off = 0
        off = append_dir(dk, off, mkentry(".          ", 0x10, krnl_clus, 0))
        off = append_dir(dk, off, mkentry("..         ", 0x10, protect_clus, 0))
        off = append_dir(dk, off, mklfn("vpkernel", "VPKERNEL   ", 0x20, k_start, len(kernel_data)))
        if initramfs_data:
            init_cnt = (len(initramfs_data) + sec_per_clus * sector_size - 1) // (sec_per_clus * sector_size)
            init_start = alloc(init_cnt)
            write_clus(init_start, initramfs_data)
            off = append_dir(dk, off, mklfn("initramfs.img", "INITRAMFIMG", 0x20, init_start, len(initramfs_data)))
        write_clus(krnl_clus, dk)

        # bootmgr dir with boot.cfg
        cfg_bytes = b"# pseuDOS Boot Configuration\r\nkernel=\\protected\\krnl\\vpkernel\r\ninitramfs=\\protected\\krnl\\initramfs.img\r\nautoinit=\\protected\\krnl\\autoinit.exe\r\nshell=\\protected\\gui\\gman.exe\r\ncmdline=quiet devpath=hardware boot=live\r\ndefault_resolution=auto\r\n"
        write_clus(bootcfg_clus, cfg_bytes)

        db = bytearray(sec_per_clus * sector_size)
        off = 0
        off = append_dir(db, off, mkentry(".          ", 0x10, bootmgr_clus, 0))
        off = append_dir(db, off, mkentry("..         ", 0x10, protect_clus, 0))
        off = append_dir(db, off, mklfn("boot.cfg", "BOOT    CFG", 0x20, bootcfg_clus, len(cfg_bytes)))
        write_clus(bootmgr_clus, db)
    
    fat1_off = rsvd_sec * sector_size
    fat2_off = (rsvd_sec + sec_per_fat) * sector_size
    image[fat1_off:fat1_off+len(fat)] = fat
    image[fat2_off:fat2_off+len(fat)] = fat
    
    with open(output_path, "wb") as f:
        f.write(image)

def make_eltorito_iso(efiboot_img_path, output_iso_path):
    SECTOR_SIZE = 2048
    with open(efiboot_img_path, "rb") as f:
        efi_img_bytes = f.read()

    efi_size = len(efi_img_bytes)
    efi_sectors = (efi_size + SECTOR_SIZE - 1) // SECTOR_SIZE

    # ISO layout:
    # LBA 0..15: 32KB zeroes
    # LBA 16: PVD
    # LBA 17: BRVD (El Torito)
    # LBA 18: VDST
    # LBA 19: Root Directory
    # LBA 20: Boot Catalog
    # LBA 21..21+efi_sectors-1: efiboot.img
    # LBA end..end+15: 16 post-gap sectors (standard for CD-ROM read-ahead)

    pvd_lba = 16
    brvd_lba = 17
    vdst_lba = 18
    root_dir_lba = 19
    boot_cat_lba = 20
    efiboot_lba = 21

    total_sectors = efiboot_lba + efi_sectors + 16
    iso = bytearray(total_sectors * SECTOR_SIZE)

    # --- LBA 16: Primary Volume Descriptor ---
    pvd_off = pvd_lba * SECTOR_SIZE
    iso[pvd_off] = 1
    iso[pvd_off+1:pvd_off+6] = b"CD001"
    iso[pvd_off+6] = 1
    iso[pvd_off+8:pvd_off+40] = b"EL TORITO SPECIFICATION".ljust(32, b" ")
    iso[pvd_off+40:pvd_off+72] = b"PSEUDOS_BOOT".ljust(32, b" ")

    struct.pack_into("<I", iso, pvd_off + 80, total_sectors)
    struct.pack_into(">I", iso, pvd_off + 84, total_sectors)

    struct.pack_into("<H", iso, pvd_off + 120, 1)
    struct.pack_into(">H", iso, pvd_off + 122, 1)

    struct.pack_into("<H", iso, pvd_off + 124, 1)
    struct.pack_into(">H", iso, pvd_off + 126, 1)

    struct.pack_into("<H", iso, pvd_off + 128, 2048)
    struct.pack_into(">H", iso, pvd_off + 130, 2048)

    # Root directory record in PVD (offset 156)
    rrec = bytearray(34)
    rrec[0] = 34
    struct.pack_into("<I", rrec, 2, root_dir_lba)
    struct.pack_into(">I", rrec, 6, root_dir_lba)
    struct.pack_into("<I", rrec, 10, SECTOR_SIZE)
    struct.pack_into(">I", rrec, 14, SECTOR_SIZE)
    rrec[18:25] = b"\x7e\x09\x08\x00\x00\x00\x00"
    rrec[25] = 0x02 # Directory
    struct.pack_into("<H", rrec, 28, 1)
    struct.pack_into(">H", rrec, 30, 1)
    rrec[32] = 1
    rrec[33] = 0 # root
    iso[pvd_off+156:pvd_off+190] = rrec

    iso[pvd_off+190:pvd_off+318] = b"".ljust(128, b" ")
    iso[pvd_off+318:pvd_off+446] = b"".ljust(128, b" ")
    iso[pvd_off+446:pvd_off+574] = b"".ljust(128, b" ")
    iso[pvd_off+574:pvd_off+702] = b"".ljust(128, b" ")
    iso[pvd_off+702:pvd_off+739] = b"".ljust(37, b" ")
    iso[pvd_off+739:pvd_off+776] = b"".ljust(37, b" ")
    iso[pvd_off+776:pvd_off+813] = b"".ljust(37, b" ")
    now_str = b"2026090800000000\x00"
    iso[pvd_off+813:pvd_off+830] = now_str
    iso[pvd_off+830:pvd_off+847] = now_str
    iso[pvd_off+847:pvd_off+864] = b"0000000000000000\x00"
    iso[pvd_off+864:pvd_off+881] = now_str
    iso[pvd_off+881] = 1

    # --- LBA 17: Boot Record Volume Descriptor (El Torito) ---
    brvd_off = brvd_lba * SECTOR_SIZE
    iso[brvd_off] = 0 # Boot Record
    iso[brvd_off+1:brvd_off+6] = b"CD001"
    iso[brvd_off+6] = 1
    iso[brvd_off+7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
    struct.pack_into("<I", iso, brvd_off + 71, boot_cat_lba)

    # --- LBA 18: Volume Descriptor Set Terminator ---
    vdst_off = vdst_lba * SECTOR_SIZE
    iso[vdst_off] = 255
    iso[vdst_off+1:vdst_off+6] = b"CD001"
    iso[vdst_off+6] = 1

    # --- LBA 19: Root Directory Sector ---
    rd_off = root_dir_lba * SECTOR_SIZE
    dot = bytearray(34)
    dot[0] = 34
    struct.pack_into("<I", dot, 2, root_dir_lba)
    struct.pack_into(">I", dot, 6, root_dir_lba)
    struct.pack_into("<I", dot, 10, SECTOR_SIZE)
    struct.pack_into(">I", dot, 14, SECTOR_SIZE)
    dot[18:25] = b"\x7e\x09\x08\x00\x00\x00\x00"
    dot[25] = 0x02
    struct.pack_into("<H", dot, 28, 1)
    struct.pack_into(">H", dot, 30, 1)
    dot[32] = 1
    dot[33] = 0
    iso[rd_off:rd_off+34] = dot

    dotdot = bytearray(34)
    dotdot[0] = 34
    struct.pack_into("<I", dotdot, 2, root_dir_lba)
    struct.pack_into(">I", dotdot, 6, root_dir_lba)
    struct.pack_into("<I", dotdot, 10, SECTOR_SIZE)
    struct.pack_into(">I", dotdot, 14, SECTOR_SIZE)
    dotdot[18:25] = b"\x7e\x09\x08\x00\x00\x00\x00"
    dotdot[25] = 0x02
    struct.pack_into("<H", dotdot, 28, 1)
    struct.pack_into(">H", dotdot, 30, 1)
    dotdot[32] = 1
    dotdot[33] = 1
    iso[rd_off+34:rd_off+68] = dotdot

    name = b"EFIBOOT.IMG;1"
    rec_len = 33 + len(name)
    if rec_len % 2 != 0:
        rec_len += 1
    efirec = bytearray(rec_len)
    efirec[0] = rec_len
    struct.pack_into("<I", efirec, 2, efiboot_lba)
    struct.pack_into(">I", efirec, 6, efiboot_lba)
    struct.pack_into("<I", efirec, 10, efi_size)
    struct.pack_into(">I", efirec, 14, efi_size)
    efirec[18:25] = b"\x7e\x09\x08\x00\x00\x00\x00"
    efirec[25] = 0x00
    struct.pack_into("<H", efirec, 28, 1)
    struct.pack_into(">H", efirec, 30, 1)
    efirec[32] = len(name)
    efirec[33:33+len(name)] = name
    iso[rd_off+68:rd_off+68+rec_len] = efirec

    # --- LBA 20: Boot Catalog ---
    cat_off = boot_cat_lba * SECTOR_SIZE
    val = bytearray(32)
    val[0] = 0x01
    val[1] = 0xEF # EFI platform ID
    val[4:4+7] = b"pseuDOS"
    val[30] = 0x55
    val[31] = 0xAA
    w_sum = sum(struct.unpack("<16H", val))
    chk = (-w_sum) & 0xFFFF
    struct.pack_into("<H", val, 28, chk)
    iso[cat_off:cat_off+32] = val

    initial = bytearray(32)
    initial[0] = 0x88 # Bootable
    initial[1] = 0x00 # No emulation
    initial[4] = 0xEF # System type: EFI
    sec_count_512 = min((efi_size + 511) // 512, 0xFFFF)
    struct.pack_into("<H", initial, 6, sec_count_512)
    struct.pack_into("<I", initial, 8, efiboot_lba)
    iso[cat_off+32:cat_off+64] = initial

    # --- LBA 21: efiboot.img data ---
    data_off = efiboot_lba * SECTOR_SIZE
    iso[data_off:data_off+efi_size] = efi_img_bytes
    try:
        with open(output_iso_path, "wb") as f:
            f.write(iso)
    except OSError as e:
        sys.stderr.write(
            f"\n[make_iso] ERROR: Failed to write '{output_iso_path}': {e}\n"
            f"[make_iso] The ISO file is currently locked by another process (most likely a running QEMU instance).\n"
            f"[make_iso] Please close or kill QEMU, then retry the build.\n"
        )
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description="Create UEFI El Torito Bootable ISO")
    parser.add_argument("--efi", required=True, help="Path to BOOTX64.EFI")
    parser.add_argument("--recovery", required=False, help="Path to recovery.efi")
    parser.add_argument("--kernel", required=False, help="Path to kernel.bin")
    parser.add_argument("--initramfs", required=False, help="Path to initramfs.img")
    parser.add_argument("--iso", required=True, help="Output ISO path")
    args = parser.parse_args()

    iso_dir = os.path.dirname(os.path.abspath(args.iso))
    os.makedirs(iso_dir, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        iso_root = os.path.join(td, "iso_root")
        os.makedirs(iso_root, exist_ok=True)
        efiboot_img = os.path.join(iso_root, "efiboot.img")

        make_fat12_esp(efiboot_img, args.efi, args.kernel, args.initramfs, args.recovery)

        # Also populate ISO 9660 directory tree so Rufus "ISO Mode" extraction works on real USB sticks
        efi_boot_dir = os.path.join(iso_root, "EFI", "BOOT")
        efi_pseudos_dir = os.path.join(iso_root, "EFI", "pseuDOS")
        prot_krnl_dir = os.path.join(iso_root, "protected", "krnl")
        prot_bmgr_dir = os.path.join(iso_root, "protected", "bootmgr")
        os.makedirs(efi_boot_dir, exist_ok=True)
        os.makedirs(efi_pseudos_dir, exist_ok=True)
        os.makedirs(prot_krnl_dir, exist_ok=True)
        os.makedirs(prot_bmgr_dir, exist_ok=True)

        shutil.copy2(args.efi, os.path.join(efi_boot_dir, "BOOTX64.EFI"))
        shutil.copy2(args.efi, os.path.join(efi_pseudos_dir, "BOOTX64.EFI"))
        if args.recovery and os.path.exists(args.recovery):
            shutil.copy2(args.recovery, os.path.join(efi_boot_dir, "recovery.efi"))
            shutil.copy2(args.recovery, os.path.join(efi_pseudos_dir, "recovery.efi"))
        if args.kernel and os.path.exists(args.kernel):
            shutil.copy2(args.kernel, os.path.join(efi_pseudos_dir, "vpkernel"))
            shutil.copy2(args.kernel, os.path.join(prot_krnl_dir, "vpkernel"))
        if args.initramfs and os.path.exists(args.initramfs):
            shutil.copy2(args.initramfs, os.path.join(prot_krnl_dir, "initramfs.img"))
        with open(os.path.join(prot_bmgr_dir, "boot.cfg"), "wb") as bf:
            bf.write(b"# pseuDOS Boot Configuration\r\nkernel=\\protected\\krnl\\vpkernel\r\ninitramfs=\\protected\\krnl\\initramfs.img\r\nautoinit=\\protected\\krnl\\autoinit.exe\r\nshell=\\protected\\gui\\gman.exe\r\ncmdline=quiet devpath=hardware boot=live\r\ndefault_resolution=auto\r\n")

        genisoimage_bin = shutil.which("genisoimage")
        if genisoimage_bin:
            cmd = [
                genisoimage_bin,
                "-input-charset", "utf-8",
                "-rational-rock",
                "-volid", "PSEUDOS_BOOT",
                "-e", "efiboot.img",
                "-no-emul-boot",
                "-o", args.iso,
                iso_root
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if res.returncode != 0:
                print(f"Error creating ISO with genisoimage: {res.stderr}, falling back to built-in generator...", file=sys.stderr)
                make_eltorito_iso(efiboot_img, args.iso)
        else:
            make_eltorito_iso(efiboot_img, args.iso)

    print(f"Created bootable UEFI El Torito ISO: {args.iso}")

if __name__ == "__main__":
    main()
