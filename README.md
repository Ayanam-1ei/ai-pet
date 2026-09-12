# Clawd Mochi · AI 桌面情绪伴宠（有线版）

一块巴掌大的 ESP32 小屏，跑着会「卖萌」的麻薯（mochi）。它通过 USB 串口接收上位机状态，把 AI 助手当前在干什么，变成对应的表情动画。

```
┌─────────────┐    串口状态词     ┌──────────────┐    SPI     ┌────────────┐
│ 电脑 / AI   │  ──────────────► │   ESP32-C3   │ ─────────► │ ST7789 屏  │
│ 桥接脚本    │   @115200 一行  │  状态机固件   │            │ 240×240    │
└─────────────┘                  └──────────────┘            └────────────┘
```

---

## 硬件

| 部件 | 说明 |
|------|------|
| 主控 | ESP32-C3 Super Mini（USB-C，原生 USB 串口） |
| 屏幕 | ST7789 1.54" 240×240 SPI 彩屏 |
| 接线 | CS→GPIO4，DC→GPIO1，RST→GPIO2，背光→GPIO3 |
| SPI | SCK=GPIO8，MOSI=GPIO10 |

---

## 仓库结构

```
ai-pet/
├── clawd_mochi/
│   └── clawd_mochi.ino          # 固件本体（Arduino）
├── clawd_mochi_diag/
│   └── clawd_mochi_diag.ino     # 屏幕诊断：只刷纯色，查花屏/白屏
└── tools/
    ├── build-and-flash.ps1      # 编译 / 烧录（arduino-cli）
    ├── send-state.ps1           # 手动发一个状态词
    ├── mochi-bridge.py          # M5 桥接：跟 AI 活动自动变脸
    ├── start-bridge.ps1         # 启动桥接
    ├── pixel-studio.html        # 像素画板（浏览器）
    ├── pixel-studio.py          # 画板本地服务 + 串口推送
    ├── start-pixel-studio.ps1   # 启动画板
    └── push-test-sprite.py      # 命令行推一张测试图
```

---

## 串口协议（与成品固件兼容）

- 波特率 **115200**，8N1
- **一词一行**，以 `\n` 结尾
- 设备 VID/PID：`303A:1001`

### 状态词（13 个）

| 词 | 表情含义 |
|----|----------|
| `idle` | 待机，轻轻浮动 |
| `thinking` | 思考 |
| `reading` | 读文件 / 搜索 |
| `coding` | 写代码 |
| `running` | 跑命令 |
| `delegating` | 派子任务 |
| `planning` | 规划 |
| `waiting` | 等待确认 |
| `compacting` | 压缩上下文 |
| `notify` | 提醒 |
| `done` | 本轮完成（约 5 秒后回落） |
| `error` | 出错 |
| `sleep` | 休眠 |

未知词按 `idle` 处理（安全默认）。

### 可选指令

| 指令 | 作用 |
|------|------|
| `face:0` | 休息脸 = 待机动画 |
| `face:1` | 休息脸 = 睡眠动画 |
| `face:2` | 休息脸 = 终端文字视图 |
| `bg:#RRGGBB` | 休息底色（工作态始终品牌橙） |
| `light:on` / `light:off` | 背光开关 |

---

## 快速开始

### 1. 烧录固件

**Arduino IDE**

1. 安装 ESP32 板支持与库：`Adafruit GFX`、`Adafruit ST7735 and ST7789`
2. 板子选 **ESP32C3 Dev Module**
3. 关键选项：
   - USB CDC On Boot = **Enabled**
   - Partition Scheme = **Huge APP (3MB)**
   - CPU = 160 MHz
   - Upload Speed = 921600
4. 打开 `clawd_mochi/clawd_mochi.ino`，上传

**命令行（Windows）**

```powershell
# 首次：安装 ESP32 核心与依赖库（约 300MB）
.\tools\build-and-flash.ps1 -Setup

# 仅编译
.\tools\build-and-flash.ps1

# 编译 + 烧录（自动找 COM 口）
.\tools\build-and-flash.ps1 -Flash
```

> 提示：项目路径尽量用纯英文，中文路径偶发 arduino-cli 兼容问题。

### 2. 手动测试表情

```powershell
.\tools\send-state.ps1 thinking
.\tools\send-state.ps1 coding
.\tools\send-state.ps1 done
.\tools\send-state.ps1 sleep
```

发送时不要同时开着 Arduino 串口监视器（会占用 COM 口）。

### 3. 跟 AI 活动自动变脸（M5 桥接）

桥接会轮询本地会话数据库，把「思考 / 读文件 / 写代码 / 跑命令」映射成状态词，写入串口。

```powershell
# 启动（会占用串口）
.\tools\start-bridge.ps1

# 停止
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -like '*mochi-bridge*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

依赖：Python 3 + `pyserial`

```powershell
pip install --user pyserial
```

说明：

- 约 400ms 轮询一次，状态没变不发串口，对设备压力很小
- 串口识别靠 `VID_303A&PID_1001`，认对一次后会写入缓存
- 与画板**不要同时开**（会抢 COM 口）

### 4. 像素画板（自定义图案）

```powershell
.\tools\start-pixel-studio.ps1
```

浏览器打开 `http://127.0.0.1:8765/`，48×48 格子画完点「推送到麻薯」。

> 自定义像素图存在内存里，断电/复位会丢失；收到状态词会切回表情引擎。

---

## 固件行为（对齐成品）

| 规则 | 说明 |
|------|------|
| 工作态底色 | 固定品牌橙 `RGB(205,52,0)` |
| 休息 / 睡眠底色 | `bg:` 自选色 / 深色 |
| thinking 粘滞 | 约 1.1s 内工具状态不覆盖思考脸 |
| done 回落 | 约 5s 后回到休息态 |
| 工作超时 | 约 120s 无事件回到休息态 |
| 开机流程 | 橙色 CLAWD MOCHI → 绿色 USB Ready → 8s 后入睡 |
| 帧率 | 约 8fps；sleep 约 4fps |

表情全部用 Adafruit GFX 图元程序化绘制，不依赖图片素材。改脸型 / 配色直接改 `clawd_mochi.ino` 里的常量与 `drawCurrent()`。

---

## 故障排查

| 现象 | 处理 |
|------|------|
| 白屏 / 花屏 | 烧 `clawd_mochi_diag` 看纯色循环；仍异常则检查接线与供电 |
| 找不到 COM 口 | 用数据线（非纯充电线）；设备管理器应出现 `VID_303A&PID_1001` |
| 桥接打不开串口 | 关掉串口监视器 / 画板 / 其它占用 COM 的程序 |
| 中文路径编译失败 | 把项目移到纯英文路径再试 |
| 表情切得很快 | 桥接跟手是正常现象；想安静可停掉桥接 |

---

## 里程碑

| 阶段 | 内容 | 状态 |
|------|------|------|
| M0 | 屏幕点亮 | 完成 |
| M1 | 串口状态机 | 完成 |
| M2 | 程序化表情引擎 | 完成 |
| M3 | 像素精灵推送（实验） | 完成（协议层） |
| M4 | 仲裁 / 自愈 | 完成（对齐成品规则） |
| M5 | 上位机闭环（桥接） | 完成（本仓库） |

---

## 与交付成品固件的关系

- 协议、状态词、自愈节奏与成品 `firmware.bin` **兼容**
- 成品使用 RLE 像素精灵；本仓库源码用图元绘制，观感更简
- 成品 bin 适合「装完就用」；本仓库适合学习、改脸、二次开发
- 桥接脚本可同时驱动成品 bin 与本仓库固件

---

## License

按你的仓库需要自行补充。
