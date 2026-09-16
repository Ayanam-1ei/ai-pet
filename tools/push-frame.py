#!/usr/bin/env python3
"""
把本地图片 / RGB565 .bin 推到 Clawd Mochi（需已烧录带 frame 协议的固件）。

协议:
  发送一行 ASCII:  frame <W> <H>\\n
  紧接着发送 W*H*2 字节 RGB565 小端像素
  设备回 ok frame-ready → ok frame

用法:
  python push-frame.py photo.png
  python push-frame.py photo.png --port COM3
  python push-frame.py frame.bin --size 240x240
  python push-frame.py photo.png --quit-to-rest   # 推完 3 秒后 face:0 回表情机
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

COM_CACHE = Path.home() / ".clawd_mochi_com"


def find_com_port() -> str | None:
    if COM_CACHE.exists():
        cached = COM_CACHE.read_text(encoding="utf-8").strip()
        if cached:
            return cached
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            if p.vid == 0x303A and p.pid == 0x1001:
                COM_CACHE.write_text(p.device, encoding="utf-8")
                return p.device
        ports = list(list_ports.comports())
        if len(ports) == 1:
            return ports[0].device
    except Exception:
        pass
    return None


def open_serial(port: str | None):
    import serial
    name = port or find_com_port()
    if not name:
        raise RuntimeError("未找到串口。插好 USB，或用 --port COMx")
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = 115200
    ser.timeout = 0.5
    ser.write_timeout = 5.0
    # 打开/关闭都不要拉 DTR/RTS，否则 ESP32-C3 USB CDC 会复位回睡眠表情
    ser.dsrdtr = False
    ser.rtscts = False
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    time.sleep(0.25)
    return ser, name


def safe_close(ser) -> None:
    try:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.05)
        ser.close()
    except Exception:
        pass


def read_until(ser, token: str, timeout: float = 3.0) -> str:
    buf = ""
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(64)
        if chunk:
            buf += chunk.decode("ascii", errors="ignore")
            if token in buf:
                break
    return buf


def rgb_to_565_le(r: int, g: int, b: int) -> bytes:
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return struct.pack("<H", v)


def load_image_pixels(path: Path) -> tuple[int, int, bytes]:
    from PIL import Image
    img = Image.open(path).convert("RGB")
    w, h = img.size
    if w > 240 or h > 240:
        # 等比缩到装进 240×240
        scale = 240 / max(w, h)
        nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
        img = img.resize((nw, nh), Image.Resampling.LANCZOS)
        w, h = nw, nh
    raw = img.tobytes()
    out = bytearray()
    for i in range(0, len(raw), 3):
        out += rgb_to_565_le(raw[i], raw[i + 1], raw[i + 2])
    return w, h, bytes(out)


def load_bin_pixels(path: Path, size: str | None) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if size:
        w_s, h_s = size.lower().split("x")
        w, h = int(w_s), int(h_s)
    else:
        # 默认按 240×240
        w = h = 240
        if len(data) == 240 * 240 * 2:
            pass
        elif len(data) == 240 * 320 * 2:
            w, h = 240, 320
        elif len(data) == 135 * 240 * 2:
            w, h = 135, 240
        else:
            raise ValueError(f"无法从 {len(data)} 字节推断尺寸，请用 --size WxH")
    if len(data) != w * h * 2:
        raise ValueError(f"BIN 长度 {len(data)} ≠ {w}*{h}*2 = {w*h*2}")
    return w, h, data


def push_frame(ser, w: int, h: int, payload: bytes) -> None:
    ser.reset_input_buffer()
    ser.write(f"frame {w} {h}\n".encode("ascii"))
    ser.flush()
    ack = read_until(ser, "ok frame-ready", 2.0)
    if "ok frame-ready" not in ack:
        raise RuntimeError(f"设备未就绪（请确认已烧录带 frame 的固件）: {ack.strip()!r}")

    # 无中间 ACK：按行限速发送，结束时等待 ok frame
    row_bytes = w * 2
    delay = 0.012 if w * h > 48 * 48 else 0.002
    for y in range(h):
        ser.write(payload[y * row_bytes : (y + 1) * row_bytes])
        ser.flush()
        if y < h - 1:
            time.sleep(delay)

    ack = read_until(ser, "ok frame", 20.0)
    if "ok frame" not in ack:
        raise RuntimeError(f"上屏失败: {ack.strip()!r}")
    print(f"已上屏 {w}x{h}（{len(payload)} 字节）")


def send_cmd(ser, cmd: str, expect: str = "ok", timeout: float = 2.0) -> str:
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("ascii"))
    ser.flush()
    ack = read_until(ser, expect, timeout)
    return ack.strip()


def main() -> int:
    ap = argparse.ArgumentParser(description="Push image/bin to Clawd Mochi via frame protocol")
    ap.add_argument("path", nargs="?", help="图片 (.png/.jpg/.webp) 或 .bin；管理命令可省略")
    ap.add_argument("--port", help="串口，例如 COM3")
    ap.add_argument("--size", help="BIN 尺寸，例如 240x240")
    ap.add_argument("--slot", type=int, default=None, help="保存到槽 0-4（并成为开机图）")
    ap.add_argument("--show", type=int, metavar="N", help="只显示槽 N（不推图）")
    ap.add_argument("--next", action="store_true", help="切换到下一张已存图")
    ap.add_argument("--prev", action="store_true", help="切换到上一张已存图")
    ap.add_argument("--list", action="store_true", help="列出已占用槽位")
    ap.add_argument("--clear", help="N 或 all，删除槽位")
    ap.add_argument("--quit-to-rest", action="store_true", help="显示 N 秒后 face:0 回表情机")
    ap.add_argument("--hold-sec", type=float, default=3.0, help="配合 --quit-to-rest 的停留秒数")
    args = ap.parse_args()

    # 管理命令：不需要 path
    if args.show is not None or args.next or args.prev or args.list or args.clear:
        ser, name = open_serial(args.port)
        print(f"串口 {name}")
        try:
            if args.list:
                print(send_cmd(ser, "photo:list", "ok"))
            if args.show is not None:
                print(send_cmd(ser, f"photo:show {args.show}", "ok"))
            if args.next:
                print(send_cmd(ser, "photo:next", "ok"))
            if args.prev:
                print(send_cmd(ser, "photo:prev", "ok"))
            if args.clear:
                print(send_cmd(ser, f"photo:clear {args.clear}", "ok"))
        finally:
            safe_close(ser)
        return 0

    if not args.path:
        ap.error("需要图片路径，或使用 --show/--next/--list/--clear")

    path = Path(args.path)
    if not path.exists():
        print(f"文件不存在: {path}")
        return 1

    if path.suffix.lower() == ".bin":
        w, h, payload = load_bin_pixels(path, args.size)
    else:
        w, h, payload = load_image_pixels(path)

    print(f"准备 {w}x{h} RGB565，{len(payload)} 字节…")
    ser, name = open_serial(args.port)
    print(f"串口 {name}")
    try:
        if args.slot is not None:
            print(send_cmd(ser, f"photo:slot {args.slot}", "ok"))
        push_frame(ser, w, h, payload)
        if args.quit_to_rest:
            time.sleep(max(0.0, args.hold_sec))
            ser.write(b"face:0\n")
            ser.flush()
            print("已回到休息表情")
    finally:
        safe_close(ser)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:
        print(f"[x] {e}", file=sys.stderr)
        raise SystemExit(1)
