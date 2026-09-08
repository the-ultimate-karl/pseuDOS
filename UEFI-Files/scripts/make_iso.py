#!/usr/bin/env python3
import struct
import os
import sys
import subprocess
import tempfile
import argparse
import shutil

def make_fat12_esp(output_path, efi_binary_path, kernel_binary_path=None, size_kb=2880):
    sector_size = 512
    total_sectors = (size_kb * 1024) // sector_size
    sec_per_clus = 2
    rsvd_sec = 1
    num_fats = 2
    root_entries = 224
    root_sectors = (root_entries * 32) // sector_size
    
    total_clusters = (total_sectors - rsvd_sec - root_sectors) // sec_per_clus
    fat_bytes = (total_clusters * 3 + 1) // 2
    sec_per_fat = (fat_bytes + sector_size - 1) // sector_size
    
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

    with open(efi_binary_path, "rb") as f:
        efi_data = f.read()

    kernel_data = None
    if kernel_binary_path and os.path.exists(kernel_binary_path):
        with open(kernel_binary_path, "rb") as f:
            kernel_data = f.read()

    efi_clus = alloc(1)
    boot_clus = alloc(1)
    pseudos_clus = alloc(1) if kernel_data else 0
    protect_clus = alloc(1) if kernel_data else 0
    krnl_clus = alloc(1) if kernel_data else 0
    bootmgr_clus = alloc(1) if kernel_data else 0
    bootcfg_clus = alloc(1) if kernel_data else 0
    
    root_off = (rsvd_sec + num_fats * sec_per_fat) * sector_size
    image[root_off:root_off+32] = mkentry("EFI        ", 0x10, efi_clus, 0)
    if protect_clus:
        image[root_off+32:root_off+64] = mkentry("PROTECT    ", 0x10, protect_clus, 0)
    
    # EFI dir contains BOOT and PSEUDOS subdirectories
    d = bytearray(sec_per_clus * sector_size)
    d[0:32] = mkentry(".          ", 0x10, efi_clus, 0)
    d[32:64] = mkentry("..         ", 0x10, 0, 0)
    d[64:96] = mkentry("BOOT       ", 0x10, boot_clus, 0)
    if kernel_data:
        d[96:128] = mkentry("PSEUDOS    ", 0x10, pseudos_clus, 0)
    write_clus(efi_clus, d)
    
    # BOOT dir with BOOTX64.EFI
    c_cnt = (len(efi_data) + sec_per_clus * sector_size - 1) // (sec_per_clus * sector_size)
    c_start = alloc(c_cnt)
    write_clus(c_start, efi_data)
    
    d2 = bytearray(sec_per_clus * sector_size)
    d2[0:32] = mkentry(".          ", 0x10, boot_clus, 0)
    d2[32:64] = mkentry("..         ", 0x10, efi_clus, 0)
    d2[64:96] = mkentry("BOOTX64 EFI", 0x20, c_start, len(efi_data))
    write_clus(boot_clus, d2)

    # PSEUDOS dir with fallback KERNEL.BIN
    if kernel_data:
        k_cnt = (len(kernel_data) + sec_per_clus * sector_size - 1) // (sec_per_clus * sector_size)
        k_start = alloc(k_cnt)
        write_clus(k_start, kernel_data)

        d3 = bytearray(sec_per_clus * sector_size)
        d3[0:32] = mkentry(".          ", 0x10, pseudos_clus, 0)
        d3[32:64] = mkentry("..         ", 0x10, efi_clus, 0)
        d3[64:96] = mkentry("KERNEL  BIN", 0x20, k_start, len(kernel_data))
        write_clus(pseudos_clus, d3)

        # PROTECT dir contains KRNL and BOOTMGR
        dp = bytearray(sec_per_clus * sector_size)
        dp[0:32] = mkentry(".          ", 0x10, protect_clus, 0)
        dp[32:64] = mkentry("..         ", 0x10, 0, 0)
        dp[64:96] = mkentry("KRNL       ", 0x10, krnl_clus, 0)
        dp[96:128] = mkentry("BOOTMGR    ", 0x10, bootmgr_clus, 0)
        write_clus(protect_clus, dp)

        # KRNL dir with primary KERNEL.BIN
        dk = bytearray(sec_per_clus * sector_size)
        dk[0:32] = mkentry(".          ", 0x10, krnl_clus, 0)
        dk[32:64] = mkentry("..         ", 0x10, protect_clus, 0)
        dk[64:96] = mkentry("KERNEL  BIN", 0x20, k_start, len(kernel_data))
        write_clus(krnl_clus, dk)

        # BOOTMGR dir with BOOT.CFG
        cfg_bytes = b"# pseuDOS Boot Configuration\r\nkernel=\\protected\\krnl\\kernel.bin\r\ncmdline=quiet devpath=hardware\r\ndefault_resolution=1280x720\r\n"
        write_clus(bootcfg_clus, cfg_bytes)

        db = bytearray(sec_per_clus * sector_size)
        db[0:32] = mkentry(".          ", 0x10, bootmgr_clus, 0)
        db[32:64] = mkentry("..         ", 0x10, protect_clus, 0)
        db[64:96] = mkentry("BOOT    CFG", 0x20, bootcfg_clus, len(cfg_bytes))
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

    with open(output_iso_path, "wb") as f:
        f.write(iso)

def main():
    parser = argparse.ArgumentParser(description="Create UEFI El Torito Bootable ISO")
    parser.add_argument("--efi", required=True, help="Path to BOOTX64.EFI")
    parser.add_argument("--kernel", required=False, help="Path to kernel.bin")
    parser.add_argument("--iso", required=True, help="Output ISO path")
    args = parser.parse_args()

    iso_dir = os.path.dirname(os.path.abspath(args.iso))
    os.makedirs(iso_dir, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        iso_root = os.path.join(td, "iso_root")
        os.makedirs(iso_root, exist_ok=True)
        efiboot_img = os.path.join(iso_root, "efiboot.img")

        make_fat12_esp(efiboot_img, args.efi, args.kernel)

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
