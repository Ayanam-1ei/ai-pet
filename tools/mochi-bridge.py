#!/usr/bin/env python3
"""
Clawd Mochi · M5 上位机桥接
轮询本地 mimocode.db，把当前 AI 活动映射成状态词，通过串口发给麻薯。

用法:
  python mochi-bridge.py              # 常驻运行
  python mochi-bridge.py --dry-run    # 只打印映射，不写串口
  python mochi-bridge.py --port COM3  # 手动指定端口
  python mochi-bridge.py --once       # 只探测并发送一次当前状态
  python mochi-bridge.py --selftest   # 离线验证映射逻辑

依赖: pyserial  (pip install --user pyserial)
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

# ---------------- 配置 ----------------

DB_PATH = Path.home() / ".local/share/mimocode/mimocode.db"
COM_CACHE = Path.home() / ".clawd_mochi_com"
POLL_MS = 400
IDLE_MS = 25_000          # 无新活动 → idle
SLEEP_MS = 180_000        # 无新活动 → sleep
DONE_HOLD_MS = 4_000      # 回合结束后 done 停留
RECENT_MSG_LIMIT = 3
RECENT_PART_LIMIT = 8

# 工具名 → 状态词
TOOL_MOOD = {
    "read": "reading",
    "glob": "reading",
    "grep": "reading",
    "webfetch": "reading",
    "websearch": "reading",
    "write": "coding",
    "edit": "coding",
    "multiedit": "coding",
    "patch": "coding",
    "bash": "running",
    "powershell": "running",
    "task": "delegating",
    "actor": "delegating",
}

VALID_MOODS = {
    "idle", "thinking", "reading", "coding", "running",
    "done", "error", "waiting", "sleep", "delegating",
    "planning", "compacting", "notify",
}

_LOG_FH = None


def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    if _LOG_FH:
        try:
            _LOG_FH.write(line + "\n")
            _LOG_FH.flush()
        except Exception:
            pass


# ---------------- 数据库 ----------------

def connect_db():
    import sqlite3
    conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True, timeout=1.0)
    conn.row_factory = sqlite3.Row
    return conn


def latest_session_id(conn) -> str | None:
    row = conn.execute(
        "SELECT id FROM session ORDER BY time_updated DESC LIMIT 1"
    ).fetchone()
    return row["id"] if row else None


def fetch_snapshot(conn, session_id: str) -> dict:
    msgs = conn.execute(
        "SELECT id, time_created, data FROM message "
        "WHERE session_id=? ORDER BY time_created DESC LIMIT ?",
        (session_id, RECENT_MSG_LIMIT),
    ).fetchall()
    parts = conn.execute(
        "SELECT id, message_id, time_created, data FROM part "
        "WHERE session_id=? ORDER BY time_created DESC LIMIT ?",
        (session_id, RECENT_PART_LIMIT),
    ).fetchall()

    parsed_msgs = []
    for m in msgs:
        try:
            d = json.loads(m["data"] or "{}")
        except json.JSONDecodeError:
            d = {}
        parsed_msgs.append({
            "id": m["id"],
            "time_created": m["time_created"],
            "role": d.get("role"),
            "finish": d.get("finish"),
        })

    parsed_parts = []
    for p in parts:
        try:
            d = json.loads(p["data"] or "{}")
        except json.JSONDecodeError:
            d = {}
        parsed_parts.append({
            "id": p["id"],
            "message_id": p["message_id"],
            "time_created": p["time_created"],
            "type": d.get("type"),
            "tool": d.get("tool"),
            "status": (d.get("state") or {}).get("status"),
        })

    return {"messages": parsed_msgs, "parts": parsed_parts}


def map_mood(snap: dict, now_ms: int) -> str:
    """把最新活动快照映射成麻薯状态词。"""
    parts = snap["parts"]
    msgs = snap["messages"]
    if not parts and not msgs:
        return "sleep"

    newest_part_ts = parts[0]["time_created"] if parts else 0
    newest_msg = msgs[0] if msgs else None
    activity_age = now_ms - max(newest_part_ts, newest_msg["time_created"] if newest_msg else 0)

    # 回合是否已真正结束
    turn_open = bool(newest_msg) and newest_msg.get("role") == "assistant" and newest_msg.get("finish") in (None, "tool-calls")

    if not turn_open:
        if newest_msg and newest_msg.get("finish") == "stop":
            if activity_age < DONE_HOLD_MS:
                return "done"
        if activity_age > SLEEP_MS:
            return "sleep"
        if activity_age > IDLE_MS:
            return "idle"
        return "idle"

    # 工作中：从最新 part 往回找可映射信号
    for p in parts:
        ptype = p.get("type")
        if ptype == "reasoning":
            return "thinking"
        if ptype == "step-start":
            return "thinking"
        if ptype == "text":
            return "thinking"
        if ptype == "tool":
            status = p.get("status")
            tool = (p.get("tool") or "").lower()
            if status == "error":
                return "error"
            if status == "running":
                return TOOL_MOOD.get(tool, "running")
            # completed 工具：回合仍在继续，按工具类型保持语义
            return TOOL_MOOD.get(tool, "running")
        # step-finish / 未知：继续往前找

    return "thinking"


# ---------------- 串口 ----------------

def find_com_port() -> str | None:
    # 1) 缓存
    if COM_CACHE.exists():
        cached = COM_CACHE.read_text(encoding="utf-8").strip()
        if cached:
            return cached

    # 2) 按 ESP32-C3 VID/PID 扫描
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            desc = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else ""
            if desc == "303A:1001" or "303A" in (p.hwid or "").upper():
                COM_CACHE.write_text(p.device, encoding="utf-8")
                return p.device
            if p.description and "JTAG" in p.description:
                COM_CACHE.write_text(p.device, encoding="utf-8")
                return p.device
    except Exception:
        pass

    # 3) 只有一个候选口时用它
    try:
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        if len(ports) == 1:
            return ports[0].device
    except Exception:
        pass
    return None


class SerialSink:
    def __init__(self, port: str | None, dry_run: bool):
        self.dry_run = dry_run
        self.port_name = port
        self.ser = None
        self.last_sent: str | None = None
        self.needs_resend = False
        self._next_connect_at = 0.0
        self._connect_fail_logged = False

    def connect(self) -> bool:
        if self.dry_run:
            return True
        now = time.time()
        if now < self._next_connect_at:
            return False
        if not self.port_name:
            self.port_name = find_com_port()
        if not self.port_name:
            if not self._connect_fail_logged:
                log("未找到串口设备")
                self._connect_fail_logged = True
            self._next_connect_at = now + 3.0
            return False
        try:
            import serial
            self.ser = serial.Serial(
                port=self.port_name,
                baudrate=115200,
                timeout=0.2,
                write_timeout=0.4,
                dsrdtr=False,
                exclusive=True,
            )
            time.sleep(0.12)
            log(f"串口已连接 {self.port_name}")
            self._connect_fail_logged = False
            return True
        except Exception as e:
            if not self._connect_fail_logged:
                log(f"串口连接失败 {self.port_name}: {e}（3s 后重试）")
                self._connect_fail_logged = True
            self.ser = None
            self._next_connect_at = now + 3.0
            return False

    def _write(self, mood: str) -> bool:
        try:
            self.ser.write(f"{mood}\n".encode("ascii"))
            self.ser.flush()
            log(f"→ {mood}")
            self.needs_resend = False
            return True
        except Exception as e:
            log(f"串口写失败: {e}")
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            self.last_sent = None
            self.needs_resend = True
            self._next_connect_at = time.time() + 3.0
            return False

    def send(self, mood: str) -> bool:
        if mood not in VALID_MOODS:
            return False
        if self.dry_run:
            if mood != self.last_sent:
                log(f"[dry-run] {self.last_sent or '-'} → {mood}")
                self.last_sent = mood
            return True

        # 串口在线：状态变了才写
        if self.ser is not None and self.ser.is_open:
            if mood != self.last_sent:
                ok = self._write(mood)
                if ok:
                    self.last_sent = mood
                return ok
            return True

        # 串口离线：记住目标状态，连接成功后补发
        if mood != self.last_sent:
            self.last_sent = mood
            self.needs_resend = True
        if self.connect() and self.ser is not None and self.last_sent:
            return self._write(self.last_sent)
        return False

    def close(self):
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass


# ---------------- 主循环 ----------------

def run(dry_run: bool, port: str | None, once: bool) -> int:
    if not DB_PATH.exists():
        log(f"找不到数据库: {DB_PATH}")
        return 1

    sink = SerialSink(port, dry_run)
    if not dry_run and not sink.connect():
        log("警告: 串口未就绪，将按映射打印并重试连接")

    session_id = None
    last_mood = None
    fail_streak = 0

    try:
        while True:
            now_ms = int(time.time() * 1000)
            mood = "idle"
            try:
                conn = connect_db()
                try:
                    sid = session_id or latest_session_id(conn)
                    if not sid:
                        mood = "sleep"
                    else:
                        session_id = sid
                        snap = fetch_snapshot(conn, sid)
                        mood = map_mood(snap, now_ms)
                finally:
                    conn.close()
                fail_streak = 0
            except Exception as e:
                fail_streak += 1
                if fail_streak <= 3 or fail_streak % 25 == 0:
                    log(f"数据库读取失败({fail_streak}): {e}")
                time.sleep(POLL_MS / 1000.0)
                continue

            if mood != last_mood:
                log(f"状态变化 → {mood}")
                sink.send(mood)
                last_mood = mood
            elif sink.needs_resend:
                sink.send(last_mood or mood)

            if once:
                log(f"当前映射: {mood}")
                return 0

            time.sleep(POLL_MS / 1000.0)
    except KeyboardInterrupt:
        log("已停止")
    finally:
        sink.close()
    return 0


def selftest() -> int:
    """离线验证映射逻辑，不访问数据库/串口。"""
    now = 1_000_000
    cases = [
        ("reasoning in progress", {
            "messages": [{"id": "m1", "time_created": now - 100, "role": "assistant", "finish": None}],
            "parts": [{"id": "p1", "time_created": now - 50, "type": "reasoning", "tool": None, "status": None}],
        }, "thinking"),
        ("tool bash running", {
            "messages": [{"id": "m1", "time_created": now - 100, "role": "assistant", "finish": None}],
            "parts": [{"id": "p1", "time_created": now - 50, "type": "tool", "tool": "bash", "status": "running"}],
        }, "running"),
        ("tool read running", {
            "messages": [{"id": "m1", "time_created": now - 100, "role": "assistant", "finish": None}],
            "parts": [{"id": "p1", "time_created": now - 50, "type": "tool", "tool": "read", "status": "running"}],
        }, "reading"),
        ("tool write completed", {
            "messages": [{"id": "m1", "time_created": now - 100, "role": "assistant", "finish": "tool-calls"}],
            "parts": [{"id": "p1", "time_created": now - 50, "type": "tool", "tool": "write", "status": "completed"}],
        }, "coding"),
        ("tool task running", {
            "messages": [{"id": "m1", "time_created": now - 100, "role": "assistant", "finish": None}],
            "parts": [{"id": "p1", "time_created": now - 50, "type": "tool", "tool": "task", "status": "running"}],
        }, "delegating"),
        ("tool error", {
            "messages": [{"id": "m1", "time_created": now - 100, "role": "assistant", "finish": None}],
            "parts": [{"id": "p1", "time_created": now - 50, "type": "tool", "tool": "bash", "status": "error"}],
        }, "error"),
        ("just finished", {
            "messages": [{"id": "m1", "time_created": now - 500, "role": "assistant", "finish": "stop"}],
            "parts": [{"id": "p1", "time_created": now - 600, "type": "text", "tool": None, "status": None}],
        }, "done"),
        ("idle after 30s", {
            "messages": [{"id": "m1", "time_created": now - 40_000, "role": "assistant", "finish": "stop"}],
            "parts": [{"id": "p1", "time_created": now - 40_100, "type": "text", "tool": None, "status": None}],
        }, "idle"),
        ("sleep after 3min", {
            "messages": [{"id": "m1", "time_created": now - 200_000, "role": "assistant", "finish": "stop"}],
            "parts": [{"id": "p1", "time_created": now - 200_100, "type": "text", "tool": None, "status": None}],
        }, "sleep"),
        ("step-start", {
            "messages": [{"id": "m1", "time_created": now - 100, "role": "assistant", "finish": None}],
            "parts": [{"id": "p1", "time_created": now - 20, "type": "step-start", "tool": None, "status": None}],
        }, "thinking"),
    ]

    failed = 0
    for name, snap, expect in cases:
        got = map_mood(snap, now)
        ok = got == expect
        mark = "OK " if ok else "FAIL"
        if not ok:
            failed += 1
        log(f"[{mark}] {name}: expect={expect} got={got}")
    log(f"selftest: {len(cases) - failed}/{len(cases)} passed")
    return 1 if failed else 0


def main() -> int:
    global _LOG_FH
    ap = argparse.ArgumentParser(description="Clawd Mochi M5 bridge")
    ap.add_argument("--port", help="串口号, 例如 COM3")
    ap.add_argument("--dry-run", action="store_true", help="只打印，不写串口")
    ap.add_argument("--once", action="store_true", help="只发送一次当前状态后退出")
    ap.add_argument("--selftest", action="store_true", help="离线验证映射逻辑")
    ap.add_argument("--log", help="同时写入日志文件")
    args = ap.parse_args()
    if args.log:
        Path(args.log).parent.mkdir(parents=True, exist_ok=True)
        _LOG_FH = open(args.log, "a", encoding="utf-8")
    try:
        if args.selftest:
            return selftest()
        return run(dry_run=args.dry_run, port=args.port, once=args.once)
    finally:
        if _LOG_FH:
            _LOG_FH.close()


if __name__ == "__main__":
    sys.exit(main())
