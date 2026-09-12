#!/usr/bin/env python3
"""
Clawd Mochi · 像素画板本地服务
- 打开浏览器画 48×48 像素图
- 「推送到麻薯」→ 串口发给 ESP32 显示

用法:
  python pixel-studio.py                 # 默认 8765 端口，自动找 COM
  python pixel-studio.py --port COM3
  python pixel-studio.py --no-open       # 不自动打开浏览器
  python pixel-studio.py --stop-bridge   # 启动前停掉 mochi-bridge（避免抢串口）
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
HTML_PATH = HERE / "pixel-studio.html"
COM_CACHE = Path.home() / ".clawd_mochi_com"
SPRITE_W = 48
SPRITE_H = 48

_serial = None
_serial_lock = threading.Lock()
_port_name = None


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def stop_bridge() -> None:
    try:
        import subprocess
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
             "Where-Object { $_.CommandLine -like '*mochi-bridge.py*' } | "
             "ForEach-Object { $_.ProcessId }"],
            text=True,
        ).strip()
        if not out:
            return
        for pid in out.splitlines():
            pid = pid.strip()
            if pid.isdigit():
                subprocess.call(
                    ["powershell", "-NoProfile", "-Command",
                     f"Stop-Process -Id {pid} -Force -ErrorAction SilentlyContinue"]
                )
                log(f"已停止 mochi-bridge PID={pid}")
    except Exception as e:
        log(f"停止 bridge 失败（可忽略）: {e}")


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
            if p.description and "JTAG" in (p.description or ""):
                COM_CACHE.write_text(p.device, encoding="utf-8")
                return p.device
        ports = list(list_ports.comports())
        if len(ports) == 1:
            return ports[0].device
    except Exception:
        pass
    return None


def ensure_serial(port: str | None):
    global _serial, _port_name
    with _serial_lock:
        if _serial is not None and _serial.is_open:
            return _serial
        name = port or find_com_port()
        if not name:
            raise RuntimeError("未找到串口设备，请插好 USB 或用 --port COMx")
        import serial
        _serial = serial.Serial(
            port=name,
            baudrate=115200,
            timeout=0.3,
            write_timeout=1.0,
            dsrdtr=False,
            exclusive=True,
        )
        time.sleep(0.15)
        _port_name = name
        log(f"串口已连接 {name}")
        return _serial


def rgb_hex_to_565(hex_color: str) -> int:
    h = hex_color.lstrip("#")
    if len(h) < 6:
        h = h[:6].ljust(6, "0")
    r = int(h[0:2], 16)
    g = int(h[2:4], 16)
    b = int(h[4:6], 16)
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def push_sprite(pixels: list[str], port: str | None) -> dict:
    if len(pixels) != SPRITE_W * SPRITE_H:
        raise ValueError(f"需要 {SPRITE_W * SPRITE_H} 个像素，收到 {len(pixels)}")

    ser = ensure_serial(port)

    def send_line(s: str) -> None:
        ser.write((s + "\n").encode("ascii"))
        ser.flush()

    with _serial_lock:
        try:
            ser.reset_input_buffer()
        except Exception:
            pass

        send_line("sprite:begin")
        time.sleep(0.08)

        def read_until(token: str, timeout: float = 1.0) -> str:
            buf = ""
            end = time.time() + timeout
            while time.time() < end:
                chunk = ser.read(32)
                if chunk:
                    buf += chunk.decode("ascii", errors="ignore")
                    if token in buf:
                        break
            return buf

        begin_ack = read_until("ok sprite", 1.0)
        if "ok sprite" not in begin_ack:
            raise RuntimeError(f"设备未就绪: {begin_ack!r}")

        for y in range(SPRITE_H):
            row_hex = []
            base = y * SPRITE_W
            for x in range(SPRITE_W):
                v = rgb_hex_to_565(pixels[base + x])
                row_hex.append(f"{v:04x}")
            send_line(f"sprite:row:{y}:{''.join(row_hex)}")
            ack = read_until(f"ok {y}", 1.0)
            if f"ok {y}" not in ack:
                raise RuntimeError(f"第 {y} 行确认失败: {ack.strip()!r}")

        send_line("sprite:end")
        end_ack = read_until("ok face", 2.0)

    ok = "ok face" in end_ack
    log(f"推送完成 ok={ok} ack={end_ack.strip()!r}")
    return {"ok": ok, "port": _port_name, "ack": end_ack.strip()}


class Handler(BaseHTTPRequestHandler):
    server_version = "MochiPixel/1.0"

    def _json(self, code: int, obj: dict) -> None:
        raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            data = HTML_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self._json(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if self.path != "/api/push":
            self._json(404, {"ok": False, "error": "not found"})
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n)
            data = json.loads(body.decode("utf-8"))
            pixels = data.get("pixels") or []
            port = data.get("port") or getattr(self.server, "serial_port", None)
            result = push_sprite(pixels, port)
            self._json(200, result)
        except Exception as e:
            log(f"推送失败: {e}")
            self._json(500, {"ok": False, "error": str(e)})

    def log_message(self, fmt, *args):
        # 静默默认 access log，只留我们自己的 log()
        pass


def main() -> int:
    ap = argparse.ArgumentParser(description="Clawd Mochi pixel studio")
    ap.add_argument("--port", help="串口，例如 COM3")
    ap.add_argument("--http", type=int, default=8765, help="HTTP 端口，默认 8765")
    ap.add_argument("--no-open", action="store_true", help="不自动打开浏览器")
    ap.add_argument("--stop-bridge", action="store_true", help="启动前停止 mochi-bridge")
    args = ap.parse_args()

    if not HTML_PATH.exists():
        log(f"找不到画板页面: {HTML_PATH}")
        return 1

    if args.stop_bridge:
        stop_bridge()

    # 预检串口（失败不阻止启动，推送时再报错）
    try:
        ensure_serial(args.port)
    except Exception as e:
        log(f"串口暂不可用: {e}（画板仍可打开，推送时再试）")

    httpd = ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    httpd.serial_port = args.port
    url = f"http://127.0.0.1:{args.http}/"
    log(f"像素画板: {url}")
    log("提示: 若 mochi-bridge 正在占用串口，请先停掉，或加 --stop-bridge")

    if not args.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("已退出")
    finally:
        global _serial
        if _serial and _serial.is_open:
            try:
                _serial.close()
            except Exception:
                pass
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
