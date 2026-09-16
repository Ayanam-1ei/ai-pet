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
 * 刷图扩展 (M6):
 *   frame <W> <H>\n  + W*H*2 字节 RGB565 小端二进制   → 全屏静态图
 *   sprite:begin / sprite:row:Y:hex / sprite:end      → 48×48 放大到 240×240
 *   任意状态词或 face: 即退出静态图，恢复表情机
 *
 * 烧录(Arduino IDE): 板=ESP32C3 Dev Module, USB CDC On Boot=Enabled,
 *                    Partition=Huge APP(3MB), CPU=160MHz, Upload=921600
 * 烧录(命令行):      tools\build-and-flash.ps1 -Flash
 */

#include <SPI.h>
#include <string.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// 开机静态图：推送成功后写入 SPIFFS，上电自动显示
// 多槽位：每张 240×240≈112KB，SPIFFS 约 1MB → 放 5 张较稳
#define PHOTO_SLOTS 5
#define PHOTO_IDX_PATH "/photo.idx"
uint8_t photoSlot = 0;   // 下次 frame 保存到的槽
uint8_t bootSlot = 0;    // 开机显示的槽

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

// 静态刷图色彩极性：本板 + Adafruit writeColor/BIN 通路实测需反相
// （与 fillScreen 画表情时的表现不一致；若换屏/换库后偏色，改成 0x0000 再试）
const uint16_t PHOTO_INVERT_MASK   = 0xFFFF;

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

// ---- 静态刷图 / sprite 协议 ----
enum RxMode : uint8_t {
  RX_LINE = 0,
  RX_FRAME_BINARY = 1,
};
RxMode rxMode = RX_LINE;
bool     photoHold = false;       // 静态图停留中，暂停表情动画
uint16_t frameW = 0, frameH = 0;
uint16_t frameX = 0, frameY = 0;
uint8_t  frameBytePhase = 0;
uint16_t framePixel = 0;
uint16_t *frameBuf = nullptr;     // 整帧 RGB565 缓冲

#define SPRITE_N 48
uint16_t spriteBuf[SPRITE_N * SPRITE_N];
bool     spriteActive = false;
uint8_t  spriteRowRecv = 0;

uint16_t C_ORANGE, C_DARKBG, C_BLOB, C_INK, C_ACCENT, C_WHITE, C_TERM;

// ================= setup / loop =================

void setBacklight(bool on) {
  digitalWrite(TFT_BLK, on ? HIGH : LOW);
}

// slot 文件: /pN.bin  头 [w:u16][h:u16] + RGB565 LE
void photoPath(char *out, size_t n, uint8_t slot) {
  snprintf(out, n, "/p%u.bin", (unsigned)(slot % PHOTO_SLOTS));
}

bool savePhotoSlot(uint8_t slot, const uint16_t *buf, uint16_t w, uint16_t h) {
  if (!buf || w < 1 || h < 1 || w > 240 || h > 240) return false;
  char path[16];
  photoPath(path, sizeof(path), slot);
  File f = SPIFFS.open(path, FILE_WRITE);
  if (!f) return false;
  uint16_t hdr[2] = {w, h};
  size_t ok1 = f.write((const uint8_t *)hdr, 4);
  size_t ok2 = f.write((const uint8_t *)buf, (size_t)w * h * 2);
  f.close();
  return ok1 == 4 && ok2 == (size_t)w * h * 2;
}

bool loadPhotoSlot(uint8_t slot) {
  char path[16];
  photoPath(path, sizeof(path), slot);
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return false;
  uint16_t hdr[2] = {0, 0};
  if (f.read((uint8_t *)hdr, 4) != 4) { f.close(); return false; }
  uint16_t w = hdr[0], h = hdr[1];
  if (w < 1 || h < 1 || w > 240 || h > 240) { f.close(); return false; }
  size_t need = (size_t)w * h * 2;
  uint16_t *buf = (uint16_t *)malloc(need);
  if (!buf) { f.close(); return false; }
  size_t got = f.read((uint8_t *)buf, need);
  f.close();
  if (got != need) { free(buf); return false; }
  blitFramebuffer(buf, w, h);
  free(buf);
  bootSlot = slot % PHOTO_SLOTS;
  return true;
}

void saveBootIdx() {
  File f = SPIFFS.open(PHOTO_IDX_PATH, FILE_WRITE);
  if (!f) return;
  f.write(bootSlot);
  f.close();
}

uint8_t loadBootIdx() {
  File f = SPIFFS.open(PHOTO_IDX_PATH, FILE_READ);
  if (!f) return 0;
  int v = f.read();
  f.close();
  if (v < 0 || v >= PHOTO_SLOTS) return 0;
  return (uint8_t)v;
}

// 兼容旧单文件 /photo.bin → 槽 0
void migrateLegacyPhoto() {
  if (SPIFFS.exists("/photo.bin") && !SPIFFS.exists("/p0.bin")) {
    SPIFFS.rename("/photo.bin", "/p0.bin");
  }
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

  Serial.setRxBufferSize(8192);
  Serial.begin(115200);

  SPIFFS.begin(true);
  migrateLegacyPhoto();
  lastEventMs = millis();
  moodStartMs = millis();
  moodTick = millis();

  bootSlot = loadBootIdx();
  photoSlot = bootSlot;
  if (loadPhotoSlot(bootSlot)) {
    showingInfo = false;
  } else {
    bootSplash();
    infoShownAt = millis();
  }
}

void loop() {
  serialTick();
  // 收二进制帧时绝不绘制，避免刷屏挤掉 USB CDC 数据
  if (rxMode == RX_FRAME_BINARY) return;
  // 静态刷图优先：开机睡眠/表情动画都不得覆盖
  if (photoHold) return;

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

// ================= 串口（与成品协议一致 + 刷图扩展）=================

void serialTick() {
  if (rxMode == RX_FRAME_BINARY) {
    frameBinaryTick();
    return;
  }

  static char buf[256];   // sprite:row 的 hex 行约 192 字符，需加大
  static uint16_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = '\0';
        len = 0;
        handleSerialLine(String(buf));
        if (rxMode == RX_FRAME_BINARY) return; // 刚进入二进制接收
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;
    }
  }
}

void leavePhoto() {
  photoHold = false;
  spriteActive = false;
  if (rxMode == RX_FRAME_BINARY) {
    rxMode = RX_LINE;
    frameW = frameH = frameX = frameY = 0;
    frameBytePhase = 0;
  }
  if (frameBuf) {
    free(frameBuf);
    frameBuf = nullptr;
  }
}

void beginFrameReceive(uint16_t w, uint16_t h) {
  leavePhoto();
  if (w < 1 || h < 1 || w > 240 || h > 240) {
    Serial.println("err size");
    return;
  }
  frameBuf = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
  if (!frameBuf) {
    Serial.println("err oom");
    return;
  }
  frameW = w;
  frameH = h;
  frameX = 0;
  frameY = 0;
  frameBytePhase = 0;
  rxMode = RX_FRAME_BINARY;
  Serial.println("ok frame-ready");
}

void blitFramebuffer(uint16_t *buf, uint16_t w, uint16_t h) {
  // 必须走 writeColor（与 GFX 同路径），不要用 writePixels / drawRGBBitmap：
  // - drawRGBBitmap 逐像素太慢会 WDT
  // - ESP32 SPI.writePixels 的 endian-swap 会打乱 RGB565
  tft.startWrite();
  tft.setAddrWindow(0, 0, w, h);
  const uint32_t n = (uint32_t)w * (uint32_t)h;
  for (uint32_t i = 0; i < n; i++) {
    tft.writeColor(buf[i] ^ PHOTO_INVERT_MASK, 1);
    if ((i & 0x3FF) == 0) delay(0);
  }
  tft.endWrite();
  showingInfo = false;
  termMode = false;
  photoHold = true;
  lastEventMs = millis();
}

void frameBinaryTick() {
  bool got = false;
  while (Serial.available()) {
    got = true;
    uint8_t b = (uint8_t)Serial.read();
    if (frameBytePhase == 0) {
      framePixel = b;
      frameBytePhase = 1;
    } else {
      framePixel |= ((uint16_t)b << 8);
      frameBytePhase = 0;
      frameBuf[frameY * frameW + frameX] = framePixel;
      frameX++;
      if (frameX >= frameW) {
        frameX = 0;
        frameY++;
        if (frameY >= frameH) {
          savePhotoSlot(photoSlot, frameBuf, frameW, frameH);
          bootSlot = photoSlot;
          saveBootIdx();
          blitFramebuffer(frameBuf, frameW, frameH);
          free(frameBuf);
          frameBuf = nullptr;
          rxMode = RX_LINE;
          Serial.println("ok frame");
          return;
        }
        // 每 8 行让出 CPU，避免 USB CDC 任务饿死
        if ((frameY & 7) == 0) delay(0);
      }
    }
  }
  if (!got) delay(0);
}

// 48×48 最近邻放大到 240×240（×5）
void drawSpriteScaled() {
  const int scale = 240 / SPRITE_N;
  uint16_t *fb = (uint16_t *)malloc(240 * 240 * sizeof(uint16_t));
  if (!fb) {
    Serial.println("err oom");
    return;
  }
  for (int y = 0; y < 240; y++) {
    int sy = y / scale;
    for (int x = 0; x < 240; x++) {
      fb[y * 240 + x] = spriteBuf[sy * SPRITE_N + (x / scale)];
    }
  }
  blitFramebuffer(fb, 240, 240);
  savePhotoSlot(photoSlot, fb, 240, 240);
  bootSlot = photoSlot;
  saveBootIdx();
  free(fb);
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

  // ---- 刷图 / sprite 扩展 ----
  if (line.startsWith("frame ")) {
    // frame <W> <H>
    int sp1 = line.indexOf(' ');
    int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) { Serial.println("err frame"); return; }
    uint16_t w = (uint16_t)line.substring(sp1 + 1, sp2).toInt();
    uint16_t h = (uint16_t)line.substring(sp2 + 1).toInt();
    beginFrameReceive(w, h);
    return;
  }

  if (line == "sprite:begin") {
    leavePhoto();
    spriteActive = true;
    spriteRowRecv = 0;
    memset(spriteBuf, 0, sizeof(spriteBuf));
    Serial.println("ok sprite");
    return;
  }

  if (line.startsWith("sprite:row:")) {
    if (!spriteActive) { Serial.println("err no-begin"); return; }
    // sprite:row:<y>:<hex 48*4>
    int p1 = line.indexOf(':', 11);
    if (p1 < 0) { Serial.println("err row"); return; }
    int y = line.substring(11, p1).toInt();
    String hex = line.substring(p1 + 1);
    if (y < 0 || y >= SPRITE_N || hex.length() < SPRITE_N * 4) {
      Serial.println("err row");
      return;
    }
    for (int x = 0; x < SPRITE_N; x++) {
      String cell = hex.substring(x * 4, x * 4 + 4);
      spriteBuf[y * SPRITE_N + x] = (uint16_t)strtoul(cell.c_str(), NULL, 16);
    }
    spriteRowRecv = y + 1;
    Serial.print("ok ");
    Serial.println(y);
    return;
  }

  if (line == "sprite:end") {
    if (!spriteActive) { Serial.println("err no-begin"); return; }
    spriteActive = false;
    drawSpriteScaled();
    Serial.println("ok face");
    return;
  }

  if (line == "photo:off") {
    leavePhoto();
    showManual();
    Serial.println("ok rest");
    return;
  }

  // photo:slot N — 之后 frame 存到槽 N
  if (line.startsWith("photo:slot ")) {
    int n = line.substring(11).toInt();
    if (n < 0 || n >= PHOTO_SLOTS) { Serial.println("err slot"); return; }
    photoSlot = (uint8_t)n;
    Serial.print("ok slot ");
    Serial.println(photoSlot);
    return;
  }

  // photo:show N — 显示槽 N 并记为开机槽
  if (line.startsWith("photo:show ")) {
    int n = line.substring(11).toInt();
    if (n < 0 || n >= PHOTO_SLOTS) { Serial.println("err slot"); return; }
    if (loadPhotoSlot((uint8_t)n)) {
      saveBootIdx();
      Serial.print("ok show ");
      Serial.println(n);
    } else {
      Serial.println("err empty");
    }
    return;
  }

  if (line == "photo:next" || line == "photo:prev") {
    int step = (line == "photo:next") ? 1 : -1;
    for (int k = 1; k <= PHOTO_SLOTS; k++) {
      int n = (bootSlot + step * k + PHOTO_SLOTS * 2) % PHOTO_SLOTS;
      if (loadPhotoSlot((uint8_t)n)) {
        saveBootIdx();
        Serial.print("ok show ");
        Serial.println(n);
        return;
      }
    }
    Serial.println("err empty");
    return;
  }

  // photo:clear [N|all]
  if (line.startsWith("photo:clear")) {
    String arg = line.substring(11);
    arg.trim();
    if (arg == "all" || arg == "") {
      char path[16];
      for (int i = 0; i < PHOTO_SLOTS; i++) {
        photoPath(path, sizeof(path), (uint8_t)i);
        SPIFFS.remove(path);
      }
      SPIFFS.remove(PHOTO_IDX_PATH);
      leavePhoto();
      showManual();
      Serial.println("ok cleared all");
      return;
    }
    int n = arg.toInt();
    if (n < 0 || n >= PHOTO_SLOTS) { Serial.println("err slot"); return; }
    char path[16];
    photoPath(path, sizeof(path), (uint8_t)n);
    SPIFFS.remove(path);
    Serial.print("ok cleared ");
    Serial.println(n);
    return;
  }

  // photo:list — 回哪些槽有图
  if (line == "photo:list") {
    Serial.print("ok slots");
    char path[16];
    for (int i = 0; i < PHOTO_SLOTS; i++) {
      photoPath(path, sizeof(path), (uint8_t)i);
      if (SPIFFS.exists(path)) {
        Serial.print(' ');
        Serial.print(i);
      }
    }
    Serial.println();
    return;
  }

  // 任意表情/配置命令：先退出静态图，再走原逻辑
  if (photoHold && (line == "idle" || line == "thinking" || line == "reading" ||
                    line == "coding" || line == "running" || line == "done" ||
                    line == "error" || line == "waiting" || line == "sleep" ||
                    line == "delegating" || line == "planning" ||
                    line == "compacting" || line == "notify" ||
                    line.startsWith("face:"))) {
    leavePhoto();
  }

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
