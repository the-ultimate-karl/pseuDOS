#!/usr/bin/env python3
"""
qemu_agent.py - High-Level QEMU Automation & Testing Protocol for AI Agents
===========================================================================

Provides both a reusable Python class (`QemuAgentSession`) and a CLI interface
so AI agents can boot pseuDOS in headless QEMU, send keyboard/mouse input,
inspect serial logs (`klog`), query the QEMU human monitor, and capture PNG
screenshots without writing boilerplate socket/PPM/keysym code from scratch.

Usage as a Python library:
--------------------------
    import sys
    sys.path.insert(0, "UEFI-Files/scripts")
    from qemu_agent import QemuAgentSession

    with QemuAgentSession(usb_hid=True) as vm:
        vm.wait_for_desktop()
        vm.type_text("sysmon\n")
        vm.sleep(2.0)
        vm.click(400, 300, button="left")
        vm.screenshot("sysmon_test.png")
        print(vm.read_serial())

Usage from the Command Line:
----------------------------
    python UEFI-Files/scripts/qemu_agent.py \\
        --boot-wait 10 \\
        --type "calc.exe --as-root\\n" \\
        --sleep 2 \\
        --screenshot calc_root.png \\
        --print-serial
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path
from typing import List, Optional, Tuple


# Map standard ASCII characters to QEMU Human Monitor `sendkey` names
ASCII_TO_QEMU_KEY = {
    " ": "spc",
    "\n": "ret",
    "\r": "ret",
    "\t": "tab",
    "\b": "backspace",
    "-": "minus",
    "_": "shift-minus",
    "=": "equal",
    "+": "shift-equal",
    "[": "bracket_left",
    "{": "shift-bracket_left",
    "]": "bracket_right",
    "}": "shift-bracket_right",
    ";": "semicolon",
    ":": "shift-semicolon",
    "'": "apostrophe",
    '"': "shift-apostrophe",
    "`": "grave_accent",
    "~": "shift-grave_accent",
    "\\": "backslash",
    "|": "shift-backslash",
    ",": "comma",
    "<": "shift-comma",
    ".": "dot",
    ">": "shift-dot",
    "/": "slash",
    "?": "shift-slash",
    "!": "shift-1",
    "@": "shift-2",
    "#": "shift-3",
    "$": "shift-4",
    "%": "shift-5",
    "^": "shift-6",
    "&": "shift-7",
    "*": "shift-8",
    "(": "shift-9",
    ")": "shift-0",
}

BUTTON_MASK = {
    "left": 1,
    "middle": 2,
    "right": 4,
}


def ppm_to_png_pure_python(ppm_path: str, png_path: str) -> None:
    """
    Convert a raw binary P6 PPM image from QEMU `screendump` to a standard PNG.
    Uses Pillow if available, or falls back to a built-in pure-Python zlib PNG
    writer so it has zero external dependencies.
    """
    try:
        from PIL import Image
        img = Image.open(ppm_path)
        img.save(png_path, "PNG")
        return
    except Exception:
        pass

    with open(ppm_path, "rb") as f:
        data = f.read()

    # Parse P6 header: P6 <width> <height> <maxval>\n<raw RGB bytes>
    idx = 0
    tokens: List[bytes] = []
    n = len(data)
    while len(tokens) < 4 and idx < n:
        while idx < n and data[idx] in b" \t\r\n":
            idx += 1
        if idx < n and data[idx] == ord("#"):
            while idx < n and data[idx] != ord("\n"):
                idx += 1
            continue
        start = idx
        while idx < n and data[idx] not in b" \t\r\n#":
            idx += 1
        if idx > start:
            tokens.append(data[start:idx])

    if len(tokens) < 4 or tokens[0] != b"P6":
        raise ValueError(f"Unsupported PPM header in {ppm_path}")

    width = int(tokens[1])
    height = int(tokens[2])
    idx += 1  # Skip single whitespace byte after maxval
    rgb = data[idx : idx + width * height * 3]

    # Build PNG scanlines (filter byte 0x00 per row)
    stride = width * 3
    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)
        row_start = y * stride
        raw_rows.extend(rgb[row_start : row_start + stride])

    def make_chunk(chunk_type: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(chunk_type + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + chunk_type + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    compressed = zlib.compress(bytes(raw_rows), level=6)

    with open(png_path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(make_chunk(b"IHDR", ihdr))
        out.write(make_chunk(b"IDAT", compressed))
        out.write(make_chunk(b"IEND", b""))


class QemuAgentSession:
    """
    Manages a headless QEMU instance running pseuDOS with an attached TCP
    Human Monitor socket, serial log capture, automatic watchdog timeout,
    keyboard/mouse synthesis, and PNG screenshot capture.
    """

    def __init__(
        self,
        repo_root: Optional[str] = None,
        iso_path: Optional[str] = None,
        memory_mb: int = 512,
        smp: int = 2,
        sata: bool = False,
        nvme: bool = False,
        usb: bool = False,
        usb2: bool = False,
        usb_hid: bool = False,
        boot_from: str = "iso",
        cmdline: str = "",
        monitor_port: int = 0,
        max_lifetime_sec: float = 120.0,
        screen_size: Tuple[int, int] = (1024, 768),
        extra_qemu_args: Optional[List[str]] = None,
    ):
        if repo_root is None:
            # Default: two levels up from UEFI-Files/scripts/qemu_agent.py
            self.repo_root = Path(__file__).resolve().parent.parent.parent
        else:
            self.repo_root = Path(repo_root).resolve()

        self.iso_path = (
            Path(iso_path).resolve()
            if iso_path
            else self.repo_root / "UEFI-Files" / "build" / "pseuDOS.iso"
        )
        self.disks_dir = self.repo_root / "UEFI-Files" / "build" / "disks"
        self.disks_dir.mkdir(parents=True, exist_ok=True)

        self.memory_mb = memory_mb
        self.smp = smp
        self.sata = sata
        self.nvme = nvme
        self.usb = usb
        self.usb2 = usb2
        self.usb_hid = usb_hid
        self.boot_from = boot_from.lower()
        self.cmdline = cmdline
        self.monitor_port = monitor_port or self._find_free_port()
        self.max_lifetime_sec = max_lifetime_sec
        self.screen_w, self.screen_h = screen_size
        self.extra_qemu_args = extra_qemu_args or []

        self.proc: Optional[subprocess.Popen] = None
        self.sock: Optional[socket.socket] = None
        self.temp_dir = Path(tempfile.mkdtemp(prefix="pseudos_qemu_"))
        self.serial_log_path = self.temp_dir / "serial.log"
        self._watchdog_timer: Optional[threading.Timer] = None

        # Track software cursor position (pseuDOS initializes mouse at center)
        self.cursor_x = self.screen_w // 2
        self.cursor_y = self.screen_h // 2
        self._mouse_calibrated = False

    @staticmethod
    def _find_free_port() -> int:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    @staticmethod
    def find_qemu_binary() -> str:
        exe = shutil.which("qemu-system-x86_64")
        if exe:
            return exe
        win_cand = Path(r"C:\Program Files\qemu\qemu-system-x86_64.exe")
        if win_cand.exists():
            return str(win_cand)
        raise FileNotFoundError("qemu-system-x86_64 not found in PATH or C:\\Program Files\\qemu")

    @staticmethod
    def find_ovmf_firmware() -> str:
        candidates = [
            r"C:\Program Files\qemu\share\edk2-x86_64-code.fd",
            r"C:\Program Files\qemu\share\OVMF.fd",
            r"C:\Program Files\qemu\OVMF.fd",
            "/usr/share/ovmf/OVMF.fd",
            "/usr/share/OVMF/OVMF_CODE.fd",
        ]
        for c in candidates:
            if os.path.exists(c):
                return c
        raise FileNotFoundError("OVMF / EDK2 firmware file not found.")

    def _ensure_disk(self, filename: str, size_mb: int = 500) -> Path:
        p = self.disks_dir / filename
        if not p.exists():
            with open(p, "wb") as f:
                f.seek(size_mb * 1024 * 1024 - 1)
                f.write(b"\x00")
        return p

    def start(self) -> "QemuAgentSession":
        qemu_exe = self.find_qemu_binary()
        ovmf_fw = self.find_ovmf_firmware()

        args = [qemu_exe]

        # Firmware
        if ovmf_fw.endswith("edk2-x86_64-code.fd"):
            args += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={ovmf_fw}"]
        else:
            args += ["-bios", ovmf_fw]

        # Boot media
        is_no_iso = self.boot_from in ("sata", "nvme", "usb", "usb2")
        if is_no_iso:
            args += ["-boot", "order=c,menu=off"]
        else:
            if not self.iso_path.exists():
                raise FileNotFoundError(f"ISO not found: {self.iso_path}. Run .\\build.ps1 first.")
            args += ["-cdrom", str(self.iso_path), "-boot", "order=d,menu=off"]

        # Core VM settings
        args += [
            "-accel", "tcg,thread=multi",
            "-m", f"{self.memory_mb}M",
            "-smp", str(self.smp),
            "-cpu", "max",
            "-net", "none",
            "-display", "none",
            "-serial", f"file:{self.serial_log_path}",
            "-monitor", f"tcp:127.0.0.1:{self.monitor_port},server,nowait",
        ]

        # Optional drives
        if self.sata or self.boot_from == "sata":
            img = self._ensure_disk("sata_disk.img")
            boot_prop = ",bootindex=1" if (is_no_iso and self.boot_from == "sata") else ""
            args += [
                "-drive", f"file={img},if=none,id=sata0,format=raw",
                "-device", "ich9-ahci,id=ahci",
                "-device", f"ide-hd,drive=sata0,bus=ahci.0{boot_prop}",
            ]

        if self.nvme or self.boot_from == "nvme":
            img = self._ensure_disk("nvme_disk.img")
            boot_prop = ",bootindex=1" if (is_no_iso and self.boot_from == "nvme") else ""
            args += [
                "-drive", f"file={img},if=none,id=nvm0,format=raw",
                "-device", f"nvme,serial=970EVO500M,drive=nvm0{boot_prop}",
            ]

        need_xhci = self.usb or self.boot_from == "usb" or self.usb_hid
        if need_xhci:
            args += ["-device", "qemu-xhci,id=xhci"]

        if self.usb or self.boot_from == "usb":
            img = self._ensure_disk("usb_disk.img")
            boot_prop = ",bootindex=1" if (is_no_iso and self.boot_from == "usb") else ""
            args += [
                "-drive", f"file={img},if=none,id=usb0,format=raw",
                "-device", f"usb-storage,bus=xhci.0,drive=usb0{boot_prop}",
            ]

        if self.usb2 or self.boot_from == "usb2":
            img = self._ensure_disk("usb2_disk.img")
            boot_prop = ",bootindex=1" if (is_no_iso and self.boot_from == "usb2") else ""
            args += [
                "-device", "usb-ehci,id=ehci",
                "-drive", f"file={img},if=none,id=usb2_0,format=raw",
                "-device", f"usb-storage,bus=ehci.0,drive=usb2_0{boot_prop}",
            ]

        if self.usb_hid:
            args += [
                "-device", "usb-mouse,bus=xhci.0,id=u_mouse",
                "-device", "usb-kbd,bus=xhci.0,id=u_kbd",
            ]

        if self.cmdline:
            args += ["-fw_cfg", f"name=opt/cmdline,string={self.cmdline}"]

        args += self.extra_qemu_args

        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # Arm automatic safety watchdog so QEMU can never hang indefinitely
        if self.max_lifetime_sec > 0:
            self._watchdog_timer = threading.Timer(self.max_lifetime_sec, self._watchdog_kill)
            self._watchdog_timer.daemon = True
            self._watchdog_timer.start()

        # Connect to QEMU TCP Monitor
        deadline = time.time() + 10.0
        while time.time() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", self.monitor_port), timeout=2.0)
                s.settimeout(0.5)
                try:
                    s.recv(4096)
                except socket.timeout:
                    pass
                self.sock = s
                break
            except OSError:
                time.sleep(0.1)

        if self.sock is None:
            self.stop()
            raise RuntimeError("Failed to connect to QEMU Monitor TCP socket.")

        return self

    def _watchdog_kill(self) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.kill()
            except Exception:
                pass

    def stop(self) -> None:
        if self._watchdog_timer:
            self._watchdog_timer.cancel()
            self._watchdog_timer = None
        if self.sock:
            try:
                self.sock.sendall(b"quit\n")
            except Exception:
                pass
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
        if self.proc:
            if self.proc.poll() is None:
                try:
                    self.proc.terminate()
                    self.proc.wait(timeout=3.0)
                except Exception:
                    try:
                        self.proc.kill()
                    except Exception:
                        pass
            self.proc = None
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def __enter__(self) -> "QemuAgentSession":
        return self.start()

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.stop()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)

    def monitor_cmd(self, cmd: str, wait_sec: float = 0.08) -> str:
        """Send a raw command to the QEMU Human Monitor and return its response."""
        if not self.sock:
            raise RuntimeError("QEMU Monitor socket is not connected.")
        self.sock.sendall((cmd.strip() + "\n").encode("utf-8"))
        time.sleep(wait_sec)
        chunks: List[bytes] = []
        while True:
            try:
                data = self.sock.recv(4096)
                if not data:
                    break
                chunks.append(data)
            except socket.timeout:
                break
        return b"".join(chunks).decode("utf-8", errors="replace")

    def send_key(self, keysym: str, delay: float = 0.05) -> None:
        """Send a single QEMU keysym (e.g. 'ret', 'spc', 'ctrl-c', 'shift-minus')."""
        self.monitor_cmd(f"sendkey {keysym}", wait_sec=delay)

    def type_text(self, text: str, char_delay: float = 0.04) -> None:
        """Type an arbitrary ASCII string into the guest via QEMU `sendkey`."""
        for ch in text:
            if ch in ASCII_TO_QEMU_KEY:
                keysym = ASCII_TO_QEMU_KEY[ch]
            elif "a" <= ch <= "z" or "0" <= ch <= "9":
                keysym = ch
            elif "A" <= ch <= "Z":
                keysym = f"shift-{ch.lower()}"
            else:
                continue
            self.send_key(keysym, delay=char_delay)

    def calibrate_mouse(self) -> None:
        """
        Drive the guest mouse cursor firmly into the top-left corner (0, 0)
        so subsequent `move_mouse_to(x, y)` calls are pixel-accurate.
        """
        for _ in range(8):
            self.monitor_cmd("mouse_move -400 -400", wait_sec=0.03)
        self.cursor_x = 0
        self.cursor_y = 0
        self._mouse_calibrated = True

    def move_mouse_rel(self, dx: int, dy: int, steps: int = 5) -> None:
        """Move the mouse by relative `(dx, dy)` in bounded increments."""
        rem_x = dx
        rem_y = dy
        for i in range(max(1, steps)):
            left = max(1, steps - i)
            step_x = int(round(rem_x / left))
            step_y = int(round(rem_y / left))
            if step_x != 0 or step_y != 0:
                self.monitor_cmd(f"mouse_move {step_x} {step_y}", wait_sec=0.025)
                rem_x -= step_x
                rem_y -= step_y
        self.cursor_x = max(0, min(self.screen_w - 1, self.cursor_x + dx))
        self.cursor_y = max(0, min(self.screen_h - 1, self.cursor_y + dy))

    def move_mouse_to(self, x: int, y: int, steps: int = 8) -> None:
        """Move the mouse cursor to absolute screen coordinates `(x, y)`."""
        if not self._mouse_calibrated:
            self.calibrate_mouse()
        target_x = max(0, min(self.screen_w - 1, int(x)))
        target_y = max(0, min(self.screen_h - 1, int(y)))
        dx = target_x - self.cursor_x
        dy = target_y - self.cursor_y
        if dx != 0 or dy != 0:
            self.move_mouse_rel(dx, dy, steps=steps)

    def click(
        self,
        x: Optional[int] = None,
        y: Optional[int] = None,
        button: str = "left",
        hold_sec: float = 0.08,
    ) -> None:
        """Move to `(x, y)` (if specified) and click `left`, `right`, or `middle`."""
        if x is not None and y is not None:
            self.move_mouse_to(x, y)
            time.sleep(0.05)
        mask = BUTTON_MASK.get(button.lower(), 1)
        self.monitor_cmd(f"mouse_button {mask}", wait_sec=hold_sec)
        self.monitor_cmd("mouse_button 0", wait_sec=0.06)

    def double_click(self, x: Optional[int] = None, y: Optional[int] = None, button: str = "left") -> None:
        """Double-click at `(x, y)`."""
        self.click(x, y, button=button, hold_sec=0.05)
        time.sleep(0.06)
        self.click(None, None, button=button, hold_sec=0.05)

    def drag(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        button: str = "left",
        steps: int = 10,
    ) -> None:
        """Press and drag from `(x1, y1)` to `(x2, y2)`."""
        self.move_mouse_to(x1, y1)
        time.sleep(0.05)
        mask = BUTTON_MASK.get(button.lower(), 1)
        self.monitor_cmd(f"mouse_button {mask}", wait_sec=0.08)
        self.move_mouse_to(x2, y2, steps=steps)
        time.sleep(0.05)
        self.monitor_cmd("mouse_button 0", wait_sec=0.08)

    def screenshot(self, out_png_path: str) -> str:
        """Capture the guest framebuffer via QEMU `screendump` and save as PNG."""
        out_path = Path(out_png_path).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        ppm_path = self.temp_dir / "frame.ppm"
        if ppm_path.exists():
            ppm_path.unlink()
        # Use forward slashes for QEMU monitor path parsing on Windows
        qemu_ppm_str = str(ppm_path).replace("\\", "/")
        self.monitor_cmd(f"screendump {qemu_ppm_str}", wait_sec=0.25)

        deadline = time.time() + 3.0
        while time.time() < deadline:
            if ppm_path.exists() and ppm_path.stat().st_size > 1024:
                break
            time.sleep(0.05)

        if not ppm_path.exists():
            raise RuntimeError(f"QEMU screendump failed to create {ppm_path}")

        ppm_to_png_pure_python(str(ppm_path), str(out_path))
        return str(out_path)

    def read_serial(self, tail_lines: Optional[int] = None) -> str:
        """Read the captured COM1 serial log (`klog` output)."""
        if not self.serial_log_path.exists():
            return ""
        text = self.serial_log_path.read_text(encoding="utf-8", errors="replace")
        if tail_lines is not None:
            lines = text.splitlines()
            return "\n".join(lines[-tail_lines:])
        return text

    def wait_for_serial(self, substring: str, timeout: float = 20.0) -> bool:
        """Block until `substring` appears in the COM1 serial log or `timeout` expires."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if substring in self.read_serial():
                return True
            time.sleep(0.2)
        return False

    def wait_for_desktop(self, wait_seconds: float = 10.0) -> None:
        """Wait for UEFI + vpkernel + splash screen to finish and enter the GUI desktop."""
        time.sleep(wait_seconds)


def _run_cli() -> int:
    parser = argparse.ArgumentParser(description="pseuDOS QEMU Agent Automation CLI")
    parser.add_argument("--boot-wait", type=float, default=10.0, help="Seconds to wait for initial boot")
    parser.add_argument("--sata", action="store_true", help="Attach 500MB AHCI SATA disk")
    parser.add_argument("--nvme", action="store_true", help="Attach 500MB NVMe disk")
    parser.add_argument("--usb", action="store_true", help="Attach 500MB USB 3.0 xHCI disk")
    parser.add_argument("--usb2", action="store_true", help="Attach 500MB USB 2.0 EHCI disk")
    parser.add_argument("--usb-hid", action="store_true", help="Attach xHCI USB mouse & keyboard")
    parser.add_argument("--boot-from", type=str, default="iso", help="Boot media: iso, sata, nvme, usb, usb2")
    parser.add_argument("--cmdline", type=str, default="", help="Kernel fw_cfg command line")
    parser.add_argument("--timeout", type=float, default=90.0, help="Max lifetime watchdog in seconds")
    parser.add_argument("--type", dest="type_cmds", action="append", default=[], help="Type text (supports \\n)")
    parser.add_argument("--key", dest="key_cmds", action="append", default=[], help="Send raw QEMU key")
    parser.add_argument("--click", dest="click_cmds", action="append", default=[], help="Click x,y[,left|right]")
    parser.add_argument("--sleep", type=float, default=1.5, help="Seconds to wait after input before screenshot")
    parser.add_argument("--screenshot", type=str, default="", help="Output PNG path for screenshot")
    parser.add_argument("--print-serial", action="store_true", help="Print captured COM1 serial log")
    args = parser.parse_args()

    with QemuAgentSession(
        sata=args.sata,
        nvme=args.nvme,
        usb=args.usb,
        usb2=args.usb2,
        usb_hid=args.usb_hid,
        boot_from=args.boot_from,
        cmdline=args.cmdline,
        max_lifetime_sec=args.timeout,
    ) as vm:
        vm.wait_for_desktop(args.boot_wait)

        try:
            for k in args.key_cmds:
                vm.send_key(k)
                vm.sleep(0.2)

            for t in args.type_cmds:
                expanded = t.encode("utf-8").decode("unicode_escape")
                vm.type_text(expanded)
                vm.sleep(0.5)

            for c in args.click_cmds:
                parts = [p.strip() for p in c.split(",")]
                cx, cy = int(parts[0]), int(parts[1])
                btn = parts[2] if len(parts) > 2 else "left"
                vm.click(cx, cy, button=btn)
                vm.sleep(0.3)

            if args.sleep > 0:
                vm.sleep(args.sleep)

            if args.screenshot:
                saved = vm.screenshot(args.screenshot)
                print(f"[qemu_agent] Saved screenshot: {saved}")
        finally:
            if args.print_serial:
                print("=== COM1 SERIAL LOG ===")
                print(vm.read_serial())

    return 0


if __name__ == "__main__":
    sys.exit(_run_cli())
