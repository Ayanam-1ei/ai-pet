/*
 * Clawd Mochi · 有线版自研固件（对齐成品行为 M0–M5 骨架）
 * 硬件: ESP32-C3 Super Mini + ST7789 1.54" 240x240 SPI
 *
 * 与交付成品 firmware.bin 同一套串口协议与仲裁语义:
 *   状态词 13 个 + face:0/1/2 + bg:#RRGGBB + light:on/off
 *   sticky thinking / done 5s 回落 / 120s 工作超时 / 开机 8s 入睡
 *   工作态固定品牌橙底；休息态用 bg: 自选色
 *
 * 差异: 成品用 RLE 精灵图；本源码用 GFX 图元程序化绘制动画。
 *       协议兼容，上位机桥接可直接复用。
 *
 * 烧录(Arduino IDE): 板=ESP32C3 Dev Module, USB CDC On Boot=Enabled,
 *                    Partition=Huge APP(3MB), CPU=160MHz, Upload=921600
 * 烧录(命令行):      tools\build-and-flash.ps1 -Flash
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ---------------- 接线（ST7789 -> ESP32-C3）----------------
#define TFT_CS   4
#define TFT_DC   1
#define TFT_RST  2
#define TFT_BLK  3

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ---------------- 状态 ID（与成品一致）----------------
#define MOOD_IDLE 0
#define MOOD_THINKING 1
#define MOOD_READING 2
#define MOOD_CODING 3
#define MOOD_RUNNING 4
#define MOOD_DONE 5
#define MOOD_ERROR 6
#define MOOD_WAITING 7
#define MOOD_SLEEP 8
#define MOOD_DELEGATING 9
#define MOOD_PLANNING 10
#define MOOD_COMPACTING 11
#define MOOD_NOTIFY 12
#define NUM_MOODS 13
#define MOOD_MANUAL 99

const char* const MOOD_WORDS[NUM_MOODS] = {
  "idle", "thinking", "reading", "coding", "running",
  "done", "error", "waiting", "sleep", "delegating",
  "planning", "compacting", "notify"
};

// 成品同款节奏
const uint32_t THINKING_STICKY_MS  = 1100;
const uint32_t DONE_RELAX_MS       = 5000;
const uint32_t ACTIVITY_TIMEOUT_MS = 120000;
const uint32_t BOOT_INFO_MS        = 8000;
const uint32_t FRAME_MS            = 125;   // 8fps
const uint32_t FRAME_SLEEP_MS      = 250;   // sleep 4fps

// ---------------- 运行状态 ----------------
uint8_t  mood = MOOD_MANUAL;
uint8_t  manualView = 0;          // 0=idle 1=sleeping 2=terminal
uint16_t manualBg;                // bg: 自选休息底色
uint16_t animBgColor;             // 当前动画底色
bool     showingInfo = true;
bool     usbReady = true;         // 成品里 staConnected 的有线版含义
bool     termMode = false;        // face:2 终端视图
uint32_t moodTick = 0;
uint32_t moodStartMs = 0;
uint32_t lastEventMs = 0;
uint32_t thinkingUntil = 0;
uint32_t infoShownAt = 0;
uint32_t animPhase = 0;           // 动画相位

uint16_t C_ORANGE, C_DARKBG, C_BLOB, C_INK, C_ACCENT, C_WHITE, C_TERM;

// ================= setup / loop =================

void setBacklight(bool on) {
  digitalWrite(TFT_BLK, on ? HIGH : LOW);
}

void setup() {
  pinMode(TFT_BLK, OUTPUT);
  setBacklight(true);

  SPI.begin(8, -1, 10, -1);
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);

  C_ORANGE = tft.color565(205, 52, 0);    // 品牌橙（工作态固定）
  C_DARKBG = tft.color565(16, 14, 24);    // 睡眠深底
  C_BLOB   = tft.color565(255, 234, 200); // 麻薯身体
  C_INK    = tft.color565(35, 26, 20);    // 五官
  C_ACCENT = tft.color565(255, 120, 60);  // 点缀
  C_WHITE  = 0xFFFF;
  C_TERM   = tft.color565(12, 18, 12);    // face:2 终端绿黑

  manualBg = C_DARKBG;
  animBgColor = C_DARKBG;

  Serial.begin(115200);
  bootSplash();
  infoShownAt = millis();
  lastEventMs = millis();
  moodStartMs = millis();
  moodTick = millis();
}

void loop() {
  serialTick();
  uint32_t now = millis();

  // 开机信息页 → 睡眠
  if (showingInfo && usbReady && now - infoShownAt > BOOT_INFO_MS) {
    showingInfo = false;
    mood = MOOD_SLEEP;
    moodStartMs = now;
    animPhase = 0;
    moodTick = now;
    drawCurrent(true);
  }

  // 自愈：done → 休息；工作超时 → 休息
  if (mood == MOOD_DONE && now - lastEventMs > DONE_RELAX_MS) {
    showManual();
  }
  if (isWorking(mood) && now - lastEventMs > ACTIVITY_TIMEOUT_MS) {
    showManual();
  }

  uint8_t animMood = resolveAnimMood();
  uint32_t fps = (animMood == MOOD_SLEEP) ? FRAME_SLEEP_MS : FRAME_MS;
  if (!showingInfo && now - moodTick >= fps) {
    moodTick = now;
    animPhase++;
    drawCurrent(false);
  }
}

bool isWorking(uint8_t m) {
  switch (m) {
    case MOOD_THINKING: case MOOD_READING: case MOOD_CODING: case MOOD_RUNNING:
    case MOOD_DELEGATING: case MOOD_PLANNING: case MOOD_COMPACTING:
    case MOOD_NOTIFY: case MOOD_ERROR:
      return true;
    default:
      return false;
  }
}

// 当前应播放哪套动画（MANUAL 按 manualView 映射）
uint8_t resolveAnimMood() {
  if (mood != MOOD_MANUAL) return mood;
  if (manualView == 1) return MOOD_SLEEP;
  return MOOD_IDLE;  // face:0；face:2 走 termMode 分支
}

void bootSplash() {
  tft.fillScreen(C_ORANGE);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(3);
  tft.setCursor(40, 100); tft.print("CLAWD");
  tft.setCursor(40, 130); tft.print("MOCHI");
  tft.setTextSize(1);
  tft.setCursor(64, 172); tft.print("self-built firmware v0.2");
  delay(900);
  tft.fillScreen(0x07E0);
  tft.setTextColor(0x0000);
  tft.setTextSize(2);
  tft.setCursor(52, 112); tft.print("USB Ready");
}

// ================= 串口（与成品协议一致）=================

void serialTick() {
  static char buf[40];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) { buf[len] = '\0'; len = 0; handleSerialLine(String(buf)); }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;
    }
  }
}

uint16_t hexToRgb565(const String& s) {
  String h = s; h.trim();
  if (h.startsWith("#")) h = h.substring(1);
  if (h.length() < 6) return manualBg;
  long v = strtol(h.substring(0, 6).c_str(), NULL, 16);
  return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line.startsWith("face:")) {
    manualView = (uint8_t)constrain(line.substring(5).toInt(), 0, 2);
    termMode = (manualView == 2);
    showManual();
    return;
  }
  if (line.startsWith("bg:")) {
    manualBg = hexToRgb565(line.substring(3));
    if (mood == MOOD_MANUAL || mood == MOOD_IDLE) showManual();
    return;
  }
  if (line == "light:on")  { setBacklight(true);  return; }
  if (line == "light:off") { setBacklight(false); return; }

  applyState(line);
}

void applyState(const String& s) {
  lastEventMs = millis();
  showingInfo = false;
  termMode = false;
  animBgColor = C_ORANGE;   // 工作态固定品牌橙

  bool activeTool = (s == "coding" || s == "reading" || s == "running" ||
                     s == "delegating" || s == "compacting");
  if (activeTool && millis() < thinkingUntil) return;

  // 同状态不重置动画
  auto enter = [&](uint8_t id) {
    if (mood != id) {
      mood = id;
      moodStartMs = millis();
      animPhase = 0;
      moodTick = 0;
    }
  };

  if (s == "thinking") {
    enter(MOOD_THINKING);
    thinkingUntil = millis() + THINKING_STICKY_MS;
  } else if (s == "reading")         enter(MOOD_READING);
  else if (s == "coding")            enter(MOOD_CODING);
  else if (s == "running")           enter(MOOD_RUNNING);
  else if (s == "delegating")        enter(MOOD_DELEGATING);
  else if (s == "compacting")        enter(MOOD_COMPACTING);
  else if (s == "planning")          enter(MOOD_PLANNING);
  else if (s == "notify")            enter(MOOD_NOTIFY);
  else if (s == "done")              enter(MOOD_DONE);
  else if (s == "error")             enter(MOOD_ERROR);
  else if (s == "waiting")           enter(MOOD_WAITING);
  else if (s == "sleep")             enter(MOOD_SLEEP);
  else                               showManual();  // idle / 未知
}

// 回到休息态（face:0/1/2 + bg:）
void showManual() {
  mood = MOOD_MANUAL;
  animPhase = 0;
  moodStartMs = millis();
  moodTick = 0;
  animBgColor = manualBg;
  drawCurrent(true);
}

// ================= 绘制 =================

uint16_t currentBg() {
  if (termMode) return C_TERM;
  if (resolveAnimMood() == MOOD_SLEEP) return C_DARKBG;
  if (mood == MOOD_MANUAL) return manualBg;
  return animBgColor;
}

const int CX = 120;
const int EYE_Y = 148, EYE_DX = 28, EYE_R = 16;
const int MOUTH_Y = 190;

void drawBlob(int offY) {
  tft.fillRoundRect(48, 96 + offY, 144, 128, 40, C_BLOB);
  tft.fillCircle(42, 158 + offY, 11, C_ACCENT);
  tft.fillCircle(198, 158 + offY, 11, C_ACCENT);
}

void eyeOpen(int cx, int cy, int r, int pdx, int pdy) {
  tft.fillCircle(cx, cy, r, C_WHITE);
  tft.drawCircle(cx, cy, r, C_INK);
  tft.fillCircle(cx + pdx, cy + pdy, r / 2 + 2, C_INK);
}

void eyeClosed(int cx, int cy, int r) {
  tft.fillRoundRect(cx - r, cy - 3, r * 2, 6, 3, C_INK);
}

void eyeX(int cx, int cy, int r) {
  int d = r - 4;
  for (int k = 0; k < 2; k++) {
    tft.drawLine(cx - d, cy - d + k, cx + d, cy + d + k, C_INK);
    tft.drawLine(cx - d, cy + d + k, cx + d, cy - d + k, C_INK);
  }
}

void starEye(int cx, int cy, int d) {
  tft.drawLine(cx - d, cy, cx + d, cy, C_INK);
  tft.drawLine(cx, cy - d, cx, cy + d, C_INK);
  tft.drawLine(cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2, C_INK);
  tft.drawLine(cx - d / 2, cy + d / 2, cx + d / 2, cy - d / 2, C_INK);
}

void mouthSmile(int cy, int w, int depth) {
  for (int i = 0; i <= w; i += 5) {
    float t = (i - w / 2.0f) / (w / 2.0f);
    int y = cy + (int)((1.0f - t * t) * depth);
    tft.fillCircle(CX - w / 2 + i, y, 3, C_INK);
  }
}

void mouthFlat(int cy, int w) {
  tft.fillRoundRect(CX - w / 2, cy - 2, w, 5, 2, C_INK);
}

void mouthOpen(int cy, int w, int h) {
  tft.fillRoundRect(CX - w / 2, cy - h / 2, w, h, h / 2, C_INK);
}

void sparkle(int x, int y, uint16_t c) {
  tft.drawLine(x - 7, y, x + 7, y, c);
  tft.drawLine(x, y - 7, x, y + 7, c);
}

void drawTerminalView() {
  tft.fillScreen(C_TERM);
  tft.setTextColor(tft.color565(80, 220, 120));
  tft.setTextSize(1);
  tft.setCursor(16, 28);  tft.print("> clawd mochi / wired");
  tft.setCursor(16, 48);  tft.print("status: connected");
  tft.setCursor(16, 64);  tft.print("face:  terminal");
  tft.setCursor(16, 80);  tft.print("hook:  serial @115200");
  tft.setCursor(16, 108); tft.print("waiting for events...");
  // 光标闪烁
  if ((animPhase / 4) % 2 == 0) {
    tft.fillRect(16, 132, 8, 12, tft.color565(80, 220, 120));
  }
  tft.setCursor(16, 200);
  tft.print("face:0 idle | face:1 sleep");
}

void drawCurrent(bool fullRedraw) {
  uint16_t bg = currentBg();

  if (termMode) {
    drawTerminalView();
    return;
  }

  if (fullRedraw) tft.fillScreen(bg);

  uint8_t am = resolveAnimMood();
  uint32_t p = animPhase;
  int eL = CX - EYE_DX, eR = CX + EYE_DX;

  switch (am) {

    case MOOD_IDLE: {
      // 轻轻浮动
      if (!fullRedraw) tft.fillScreen(bg);
      int offY = (int)(sinf(p * 0.12f) * 6);
      drawBlob(offY);
      int ey = EYE_Y + offY, my = MOUTH_Y + offY;
      bool blink = (p % 28) < 2;
      if (blink) { eyeClosed(eL, ey, EYE_R); eyeClosed(eR, ey, EYE_R); }
      else       { eyeOpen(eL, ey, EYE_R, 0, 0); eyeOpen(eR, ey, EYE_R, 0, 0); }
      mouthSmile(my, 56, 12);
      break;
    }

    case MOOD_THINKING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      eyeOpen(eL, ey, EYE_R, -6, -5);
      eyeOpen(eR, ey, EYE_R, -6, -5);
      mouthFlat(my, 40);
      // 粒子向上旋
      for (int i = 0; i < 6; i++) {
        float a = p * 0.45f + i * 1.05f;
        int sx = CX + (int)(cosf(a) * (28 + (i % 3) * 8));
        int sy = 78 - (int)((p * 2 + i * 7) % 36) + (int)(sinf(a) * 6);
        uint16_t c = (i % 2) ? C_BLOB : C_WHITE;
        tft.fillCircle(sx, sy, 3, c);
      }
      break;
    }

    case MOOD_READING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      int dx = (int)(sinf(p * 0.4f) * 7);
      eyeOpen(eL, ey, EYE_R, dx, 2);
      eyeOpen(eR, ey, EYE_R, dx, 2);
      mouthFlat(my, 40);
      // 眼部扫描线
      int sx = 40 + (int)((p * 6) % 160);
      tft.fillRect(sx - 18, ey + 30, 36, 3, C_WHITE);
      tft.fillRect(sx - 8, ey + 36, 16, 2, C_ACCENT);
      break;
    }

    case MOOD_CODING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      eyeOpen(eL, ey, EYE_R, 0, 3);
      eyeOpen(eR, ey, EYE_R, 0, 3);
      if (p % 2) mouthOpen(my, 22, 14); else mouthFlat(my, 40);
      // 手指/键盘刻度
      tft.fillRoundRect(60, 218, 120, 10, 3, C_BLOB);
      for (int i = 0; i < 5; i++) {
        int x = 68 + i * 22;
        int h = 4 + ((p + i) % 3) * 2;
        tft.fillRect(x, 218 - h, 10, h, C_WHITE);
      }
      break;
    }

    case MOOD_RUNNING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      // 认真眉
      tft.drawLine(eL - EYE_R, ey - EYE_R - 10, eL + EYE_R - 4, ey - EYE_R - 4, C_INK);
      tft.drawLine(eR + EYE_R, ey - EYE_R - 10, eR - EYE_R + 4, ey - EYE_R - 4, C_INK);
      eyeOpen(eL, ey, EYE_R, 0, 0);
      eyeOpen(eR, ey, EYE_R, 0, 0);
      mouthFlat(my, 34);
      // 吊车臂摆动
      int arm = 50 + (int)((p % 16) * 6);
      tft.drawLine(CX, 96, arm, 70, C_WHITE);
      tft.fillCircle(arm, 70, 4, C_ACCENT);
      tft.drawRoundRect(48, 214, 144, 12, 4, C_BLOB);
      tft.fillRoundRect(50, 216, ((p % 48) + 1) * 3, 8, 2, C_BLOB);
      break;
    }

    case MOOD_DONE: {
      if (!fullRedraw) tft.fillScreen(bg);
      int offY = -(int)(fabsf(sinf(p * 0.55f)) * 16);
      drawBlob(offY);
      int ey = EYE_Y + offY, my = MOUTH_Y + offY;
      tft.fillRoundRect(eL - EYE_R, ey - 2, EYE_R * 2, 5, 2, C_INK);
      tft.fillRoundRect(eR - EYE_R, ey - 2, EYE_R * 2, 5, 2, C_INK);
      tft.fillCircle(CX, my + 4, 24, C_INK);
      tft.fillCircle(CX, my - 6, 22, C_BLOB);
      if (p % 2) { sparkle(52, 100, C_WHITE); sparkle(188, 120, C_BLOB); }
      else       { sparkle(188, 100, C_BLOB); sparkle(52, 120, C_WHITE); }
      break;
    }

    case MOOD_ERROR: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      eyeX(eL, ey, EYE_R);
      eyeX(eR, ey, EYE_R);
      // 头晕螺旋
      for (int i = 0; i < 20; i++) {
        float a = p * 0.35f + i * 0.45f;
        int r = 8 + i;
        tft.fillCircle(CX + (int)(cosf(a) * r), 70 + (int)(sinf(a) * r * 0.5f), 2, C_WHITE);
      }
      for (int i = 0; i <= 52; i += 6) {
        int y = my + (int)(sinf(i * 0.4f + p * 0.35f) * 5);
        tft.fillCircle(CX - 26 + i, y, 3, C_INK);
      }
      break;
    }

    case MOOD_WAITING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      eyeOpen(eL, ey, EYE_R + 3, 0, 0);
      eyeOpen(eR, ey, EYE_R + 3, 0, 0);
      mouthOpen(my, 22, 22);
      // 警示闪
      if (p % 2) {
        tft.fillTriangle(CX, 52, CX - 18, 84, CX + 18, 84, C_WHITE);
        tft.fillTriangle(CX, 58, CX - 12, 78, CX + 12, 78, C_ORANGE);
        tft.setTextColor(C_INK); tft.setTextSize(2);
        tft.setCursor(CX - 4, 64); tft.print("!");
      }
      break;
    }

    case MOOD_SLEEP: {
      if (!fullRedraw) tft.fillScreen(C_DARKBG);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      eyeClosed(eL, ey, EYE_R);
      eyeClosed(eR, ey, EYE_R);
      mouthFlat(my, 28);
      tft.setTextColor(C_WHITE);
      tft.setTextSize(2);
      for (int i = 0; i < 3; i++) {
        int zz = (int)((p * 2 + i * 14) % 48);
        if (zz < 40) {
          tft.setCursor(150 + i * 18 + zz / 5, 120 - zz);
          tft.print("Z");
        }
      }
      break;
    }

    case MOOD_DELEGATING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      // 交替眨（指挥）
      if (p % 2) { eyeOpen(eL, ey, EYE_R, 0, 0); eyeClosed(eR, ey, EYE_R); }
      else       { eyeClosed(eL, ey, EYE_R); eyeOpen(eR, ey, EYE_R, 0, 0); }
      mouthSmile(my, 40, 10);
      // 指挥棒点
      for (int i = 0; i < 4; i++) {
        int x = 70 + i * 30;
        int y = 220 - ((p + i * 2) % 4) * 4;
        tft.fillCircle(x, y, 4, C_BLOB);
      }
      tft.drawLine(CX - 40, 210, CX + 40, 200 + (int)(sinf(p * 0.6f) * 8), C_WHITE);
      break;
    }

    case MOOD_PLANNING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      starEye(eL, ey, EYE_R + 2);
      starEye(eR, ey, EYE_R + 2);
      mouthSmile(my, 36, 10);
      // 法师星尘
      for (int i = 0; i < 8; i++) {
        float a = p * 0.28f + i * 0.78f;
        int sx = CX + (int)(cosf(a) * 90);
        int sy = 150 + (int)(sinf(a) * 55);
        if (((p + i) % 2) == 0) {
          tft.fillCircle(sx, sy, 3, C_WHITE);
          sparkle(sx, sy - 14, C_BLOB);
        }
      }
      break;
    }

    case MOOD_COMPACTING: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      eyeOpen(eL, ey, EYE_R, 0, 7);
      eyeOpen(eR, ey, EYE_R, 0, 7);
      mouthFlat(my, 34);
      // 螃蟹扫帚来回
      int bx = CX + (int)(sinf(p * 0.45f) * 50);
      tft.fillRoundRect(bx - 28, 214, 56, 7, 3, C_BLOB);
      for (int i = 0; i < 4; i++) {
        tft.fillCircle(bx - 30 + i * 8, 224 + (i % 2) * 3, 2, C_WHITE);
      }
      tft.fillCircle(bx - 8, 210, 5, C_ACCENT);
      tft.fillCircle(bx + 8, 210, 5, C_ACCENT);
      break;
    }

    case MOOD_NOTIFY: {
      if (!fullRedraw) tft.fillScreen(bg);
      drawBlob(0);
      int ey = EYE_Y, my = MOUTH_Y;
      eyeOpen(eL, ey, EYE_R, 0, -4);
      eyeOpen(eR, ey, EYE_R, 0, -4);
      mouthFlat(my, 36);
      // 天线 + 扩散波
      tft.drawLine(CX, 96, CX, 62, C_BLOB);
      tft.fillCircle(CX, 58, 5, C_ACCENT);
      int r = (int)((p % 14) * 3 + 8);
      tft.drawCircle(CX, 58, r, C_WHITE);
      if (r > 22) tft.drawCircle(CX, 58, r - 16, C_BLOB);
      break;
    }

    default:
      // fallback → idle
      tft.fillScreen(manualBg);
      drawBlob(0);
      eyeOpen(eL, EYE_Y, EYE_R, 0, 0);
      eyeOpen(eR, EYE_Y, EYE_R, 0, 0);
      mouthSmile(MOUTH_Y, 56, 12);
      break;
  }
}
