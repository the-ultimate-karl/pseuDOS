#!/usr/bin/env python3
import struct
import os
import sys

# Format:
# Magic: b"PRAM" (4 bytes)
# File count: uint32 (4 bytes)
# Header entries:
#   Path: 128 bytes (null-padded UTF-8)
#   Size: uint64 (8 bytes)
# Followed by contiguous raw file bytes

def pack_initramfs(output_path, file_mappings):
    """
    file_mappings: list of tuples (archive_path, local_filesystem_path)
    """
    valid_entries = []
    for arch_path, local_path in file_mappings:
        if os.path.exists(local_path):
            with open(local_path, "rb") as f:
                content = f.read()
            valid_entries.append((arch_path, content))
        else:
            print(f"[make_initramfs] Warning: {local_path} not found, skipping.")

    header = bytearray()
    header += b"PRAM"
    header += struct.pack("<I", len(valid_entries))

    data_payload = bytearray()

    for arch_path, content in valid_entries:
        path_bytes = arch_path.encode("utf-8")[:127]
        path_padded = path_bytes.ljust(128, b"\x00")
        size_bytes = struct.pack("<Q", len(content))
        header += path_padded
        header += size_bytes
        data_payload += content

    with open(output_path, "wb") as f:
        f.write(header)
        f.write(data_payload)

    print(f"[make_initramfs] Packed {len(valid_entries)} files into {output_path} ({len(header) + len(data_payload)} bytes)")

if __name__ == "__main__":
    base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    bin_dir = os.path.join(base_dir, "bin")
    out_file = os.path.join(bin_dir, "initramfs.img")

    # Target system mapping
    files_to_pack = [
        # System Core & Init
        ("/EFI/BOOT/BOOTX64.EFI", os.path.join(bin_dir, "BOOTX64.EFI")),
        ("/EFI/BOOT/recovery.efi", os.path.join(bin_dir, "recovery.efi")),
        ("/EFI/pseuDOS/recovery.efi", os.path.join(bin_dir, "recovery.efi")),
        ("/protected/krnl/autoinit.exe", os.path.join(bin_dir, "autoinit.exe")),
        ("/protected/crit/bshss.exe", os.path.join(bin_dir, "bshss.exe")),
        
        # Display Server & Window Manager Stack
        ("/protected/gui/gman.exe", os.path.join(bin_dir, "gman.exe")),
        ("/protected/gui/superglue.exe", os.path.join(bin_dir, "superglue.exe")),
        ("/protected/gui/ntfs.exe", os.path.join(bin_dir, "ntfs.exe")),
        ("/protected/gui/lack.exe", os.path.join(bin_dir, "lack.exe")),
        ("/protected/gui/ninds.exe", os.path.join(bin_dir, "ninds.exe")),
        ("/protected/gui/splash.exe", os.path.join(bin_dir, "splash.exe")),
        ("/protected/gui/gshss.exe", os.path.join(bin_dir, "gshss.exe")),

        # Desktop Applications
        ("/protected/apps/shell.exe", os.path.join(bin_dir, "shell.exe")),
        ("/protected/apps/sysmon.exe", os.path.join(bin_dir, "sysmon.exe")),
        ("/protected/apps/calc.exe", os.path.join(bin_dir, "calc.exe")),
        ("/protected/apps/notepad.exe", os.path.join(bin_dir, "notepad.exe")),
        ("/protected/apps/paint.exe", os.path.join(bin_dir, "paint.exe")),
        ("/protected/apps/clock.exe", os.path.join(bin_dir, "clock.exe")),
        ("/protected/apps/filemgr.exe", os.path.join(bin_dir, "filemgr.exe")),
        ("/protected/apps/view.exe", os.path.join(bin_dir, "view.exe")),
    ]

    # Add test media images from Test-Media into /home/user
    test_media_dir = os.path.abspath(os.path.join(base_dir, "..", "Test-Media"))
    if os.path.exists(test_media_dir):
        for f in os.listdir(test_media_dir):
            local_path = os.path.join(test_media_dir, f)
            if os.path.isfile(local_path) and f.lower().endswith((".png", ".bmp", ".jpg", ".jpeg")):
                files_to_pack.append((f"/home/user/{f}", local_path))
                # Also provide convenient short aliases
                if f == "image1_testmedia.jpg":
                    files_to_pack.append(("/home/user/image1.jpg", local_path))
                elif f == "image1_testmedia.png":
                    files_to_pack.append(("/home/user/image1.png", local_path))
                elif f == "image1_testmedia.bmp":
                    files_to_pack.append(("/home/user/image1.bmp", local_path))
                elif f == "image2_testmedia.png":
                    files_to_pack.append(("/home/user/image2.png", local_path))

    # Add wallpapers from Test-Media/Wallpapers
    wall_dir = os.path.join(test_media_dir, "Wallpapers")
    if os.path.exists(wall_dir):
        for w in os.listdir(wall_dir):
            local_w = os.path.join(wall_dir, w)
            if os.path.isfile(local_w) and w.lower().endswith((".png", ".bmp", ".jpg", ".jpeg")):
                files_to_pack.append((f"/home/user/Wallpapers/{w}", local_w))

    pack_initramfs(out_file, files_to_pack)
