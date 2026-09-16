# Clawd Mochi · AI 桌面情绪伴宠（有线版）

ESP32-C3 + ST7789 小屏麻薯。USB 串口收状态词，把 AI 在干什么变成表情；也可推全屏静态图。

```
电脑（桥接/脚本） ──串口 115200──► ESP32-C3 ──SPI──► ST7789 240×240
```

---

## 硬件

| 项目 | 内容 |
|------|------|
| 主控 | ESP32-C3 Super Mini |
| 屏幕 | ST7789 1.54" 240×240 SPI |
| 接线 | VCC→3V3，GND→GND，SCL→GPIO8，SDA→GPIO10，RES→GPIO2，DC→GPIO1，CS→GPIO4，BL→GPIO3 |
| 线材 | **必须用 USB 数据线**（纯充电线无串口） |

**VCC 只能接 3.3V，不要接 5V。**

---

## 仓库结构

```
clawd_mochi/          固件（表情机 + frame 刷图）
clawd_mochi_diag/     屏幕诊断（纯色循环）
docs/                 开发对话记录
tools/
  build-and-flash.ps1 编译烧录
  send-state.ps1      手动发状态词
  mochi-bridge.py     跟 AI 活动自动变脸
  start-bridge.ps1
  push-frame.py       推全屏静态图
  st7789-convert/     图片 → RGB565 网页工具
```

---

## 串口协议

一行一个词，`\n` 结尾，115200 8N1。设备 `VID:PID = 303A:1001`。

**状态词：** `idle` `thinking` `reading` `coding` `running` `delegating` `planning` `waiting` `compacting` `notify` `done` `error` `sleep`  
未知词按 `idle`。

**可选：**

| 指令 | 作用 |
|------|------|
| `face:0/1/2` | 休息脸：待机 / 睡眠 / 终端 |
| `bg:#RRGGBB` | 休息底色 |
| `light:on/off` | 背光 |
| `frame W H` + RGB565 二进制 | 全屏静态图 |

---

## 快速开始

### 烧录

```powershell
.\tools\build-and-flash.ps1 -Setup   # 首次
.\tools\build-and-flash.ps1 -Flash
```

Arduino IDE：板 **ESP32C3 Dev Module**，USB CDC On Boot=Enabled，Huge APP(3MB)。

### 测表情

```powershell
.\tools\send-state.ps1 thinking
.\tools\send-state.ps1 done
```

### 跟 AI 变脸

```powershell
pip install pyserial
.\tools\start-bridge.ps1
```

桥接轮询会话状态并写串口。推图时先停掉它，避免抢 COM 口。

### 推图片上屏

1. 打开 `tools/st7789-convert/index.html`，裁剪导出 BIN  
2. 推送：

```powershell
pip install pyserial pillow
python tools\push-frame.py tools\st7789-convert\st7789_240x240.bin
python tools\push-frame.py photo.png --quit-to-rest
```

详见 `tools/README-frame.md` 与 `docs/st7789-开发对话.md`。

---

## 固件要点

- 工作态固定橙底；休息可用 `bg:`
- thinking 粘滞约 1.1s；done 约 5s 回休息；工作约 120s 超时
- 开机：CLAWD MOCHI → USB Ready → 约 8s 入睡
- 表情为 GFX 程序化绘制，改 `clawd_mochi.ino` 中 `drawCurrent()`

与交付成品 `firmware.bin` 协议兼容；成品用 RLE 精灵，本仓库用图元绘制。

---

## 排查

| 现象 | 处理 |
|------|------|
| 白屏/花屏 | 烧 `clawd_mochi_diag` 查接线 |
| 无 COM 口 | 换数据线；应出现 `303A:1001` |
| 串口被占 | 关串口监视器 / 其它推图脚本 |
| 中文路径编译失败 | 移到英文路径 |
