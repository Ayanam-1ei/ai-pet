#!/usr/bin/env python3
"""推送内置测试图到 Clawd Mochi，验证 M3 精灵协议。"""
from __future__ import annotations

import sys
import time
from pathlib import Path

# 允许直接复用 pixel-studio.py 里的函数（文件名带连字符，用 importlib）
import importlib.util

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("pixel_studio", HERE / "pixel-studio.py")
mod = importlib.util.module_from_spec(spec)
# 不执行 main，只加载模块级函数：手动 exec 到 stop 之前
src = (HERE / "pixel-studio.py").read_text(encoding="utf-8")
# 截断 main 调用
cut = src.find('if __name__ == "__main__":')
ns: dict = {
    "__file__": str(HERE / "pixel-studio.py"),
    "__name__": "pixel_studio",
}
exec(compile(src[:cut] if cut >= 0 else src, str(HERE / "pixel-studio.py"), "exec"), ns)

W = H = 48


def make_test_pixels() -> list[str]:
    px = ["#000000"] * (W * H)
    # 橙色底
    for i in range(W * H):
        px[i] = "#1a1220"
    # 麻薯身体
    body, ink, acc = "#ffeac8", "#3a2a28", "#ff783c"
    for y in range(8, 42):
        for x in range(6, 42):
            cx, cy = x - 24, y - 26
            if (cx * cx) / 280 + (cy * cy) / 200 < 1:
                px[y * W + x] = body
    # 眼睛
    for y in range(20, 28):
        for x in (16, 17, 18):
            px[y * W + x] = ink
        for x in (29, 30, 31):
            px[y * W + x] = ink
    # 嘴
    for x in range(20, 28):
        px[33 * W + x] = ink
    # 钳
    for y in range(24, 30):
        px[y * W + 4] = acc
        px[y * W + 5] = acc
        px[y * W + 42] = acc
        px[y * W + 43] = acc
    # 星星
    for x, y in [(8, 10), (38, 12), (10, 38), (36, 36)]:
        px[y * W + x] = "#ffd23c"
    return px


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else None
    print("等待设备就绪…")
    time.sleep(1.5)
    pixels = make_test_pixels()
    result = ns["push_sprite"](pixels, port)
    print("结果:", result)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
