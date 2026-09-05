#!/usr/bin/env python3
import struct
import os
import sys
import subprocess
import tempfile
import argparse

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

        cmd = [
            "genisoimage",
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
            print(f"Error creating ISO: {res.stderr}", file=sys.stderr)
            sys.exit(res.returncode)

    print(f"Created bootable UEFI El Torito ISO: {args.iso}")

if __name__ == "__main__":
    main()
