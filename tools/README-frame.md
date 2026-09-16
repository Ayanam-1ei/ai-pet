# Clawd Mochi · 推图到麻薯（frame 协议）

已烧录 **M6 刷图固件** 后，可以把任意本地图片变成 240×240 RGB565 直接打到屏上。

## 1. 准备像素数据

推荐用网页工具 `S:\可视化\st7789\index.html`：

1. 选图 → 裁剪 → 预设 **ST7789 240×240**
2. 「导出 BIN」得到 `st7789_240x240.bin`（小端 RGB565）

或直接用 PNG/JPG（脚本会自动缩放到屏内）。

## 2. 推送到设备

```powershell
# 图片自动缩放
python tools\push-frame.py "C:\path\to\photo.png"

# 已导出的 BIN
python tools\push-frame.py "S:\可视化\st7789\st7789_240x240.bin"

# 显示 5 秒后自动回表情机
python tools\push-frame.py photo.png --quit-to-rest --hold-sec 5
```

依赖：`pip install pyserial pillow`

## 3. 协议摘要

```
host → device:  frame 240 240\n
device → host:  ok frame-ready\n
host → device:  <240*240*2 字节 RGB565 小端>
device → host:  ok frame\n
```

屏上会保持这张静态图；再发任意状态词（`coding` / `idle`…）或 `face:0` 即恢复表情动画。

| 指令 | 作用 |
|------|------|
| `frame W H` + 二进制 | 全屏静态图（W,H ≤ 240） |
| `sprite:begin` / `row` / `end` | 48×48 像素画板，×5 放大上屏 |
| `photo:off` | 退出静态图 → 休息表情 |

## 4. 注意

- 推图时停掉 `mochi-bridge.py`，避免抢串口。
- USB CDC 实际速度远高于 115200，整帧通常 1 秒内完成。
- 固件必须是带 `frame` 扩展的版本（`clawd_mochi.ino` 含 `frameBinaryTick`）。
