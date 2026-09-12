/*
 * Clawd Mochi · 最小诊断：只刷纯色，验证屏幕硬件
 * 绿→红→蓝→橙 循环，每色 1 秒
 */
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_CS   4
#define TFT_DC   1
#define TFT_RST  2
#define TFT_BLK  3

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  delay(50);

  SPI.begin(8, -1, 10, -1);
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(0x07E0);  // 绿
  Serial.begin(115200);
  Serial.println("diag green");
}

void loop() {
  tft.fillScreen(0x07E0); delay(1000);  // 绿
  tft.fillScreen(0xF800); delay(1000);  // 红
  tft.fillScreen(0x001F); delay(1000);  // 蓝
  tft.fillScreen(0xFD20); delay(1000);  // 橙
  Serial.println("diag cycle");
}
