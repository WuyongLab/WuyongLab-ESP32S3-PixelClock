/*
 * ESP32-S3 64×64 HUB75E 像素时钟 v1.0
 * 基于 Clockwise 项目移植并二次开发。
 * 许可证和第三方来源见 LICENSE 与 THIRD_PARTY_NOTICES.md。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "gfx/assets.h"
#include "gfx/Super_Mario_Bros__24pt7b.h"
#include "gfx/cf0x06_assets.h"
#include "gfx/PKMN_RBYGSC4pt7b.h"
#include "web_ui.h"
#include "project_identity.h"
#include "integrity_guard.h"

// ---------------- 屏幕尺寸 ----------------
#define PANEL_RES_X 64
#define PANEL_RES_Y 64
#define PANEL_CHAIN 1

// ---------------- HUB75E 接线 ----------------
#define R1_PIN   4
#define G1_PIN   5
#define B1_PIN   6
#define R2_PIN   7
#define G2_PIN   15
#define B2_PIN   16

#define A_PIN    17
#define B_PIN    18
#define C_PIN    8
#define D_PIN    9
#define E_PIN    10

#define CLK_PIN  11
#define LAT_PIN  12
#define OE_PIN   13

// 实体息屏按键：按键一端接 GPIO14，另一端接 GND。
// 使用 INPUT_PULLUP，无需外接上拉电阻。
#define SCREEN_BUTTON_PIN 14

// ---------------- 默认设置 ----------------
static const char *AP_SSID = "Clockwise-Setup";
static const char *AP_PASS = "12345678";
static const char *DEFAULT_TZ = "CST-8";         // 中国标准时间 UTC+8 的 POSIX 写法
static const char *DEFAULT_NTP = "ntp.aliyun.com";

MatrixPanel_I2S_DMA *display = nullptr;
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

static const uint8_t DNS_PORT = 53;
static const IPAddress SETUP_AP_IP(192, 168, 4, 1);
static const IPAddress SETUP_AP_MASK(255, 255, 255, 0);
bool captivePortalActive = false;
bool webServerStarted = false;

String wifiSsid;
String wifiPass;
String tzInfo;
String ntpServer;
uint8_t brightness = 30;
uint8_t rotation = 0;
uint8_t faceMode = 0;
uint8_t appliedBrightness = 255;
uint8_t appliedRotation = 255;
bool apMode = false;
bool timeReady = false;
unsigned long lastDrawMs = 0;
unsigned long rebootAtMs = 0;
// 设置保存后立即应用；Wi-Fi 变更在响应完成后重新连接。
bool wifiReconnectPending = false;
unsigned long wifiReconnectAtMs = 0;
bool wifiReconnectRunning = false;
unsigned long wifiReconnectStartedMs = 0;
bool timeReconfigurePending = false;
unsigned long timeReconfigureAtMs = 0;
bool timeSyncRunning = false;
unsigned long timeSyncStartedMs = 0;
unsigned long timeSyncLastCheckMs = 0;
uint16_t animOffset = 0;

// ---------------- 开机 IP 提示 ----------------
static const unsigned long STARTUP_IP_DURATION_MS = 15000;
static const unsigned long STARTUP_IP_SCROLL_STEP_MS = 85;
bool startupIpActive = false;
String startupIpText;
String startupIpTitle;
unsigned long startupIpStartedMs = 0;
unsigned long startupIpLastScrollMs = 0;
unsigned long startupIpLastFrameMs = 0;
int16_t startupIpScrollX = PANEL_RES_X;

// ---------------- 本地媒体、像素画板与息屏按键 ----------------
static const uint8_t MAX_SLIDES = 8;                  // 最多保存 8 张 64x64 图片
static const size_t PIXEL_COUNT = PANEL_RES_X * PANEL_RES_Y;
static const size_t RGB565_FILE_BYTES = PIXEL_COUNT * 2;
static const uint32_t DEFAULT_CAROUSEL_MS = 3000;

bool storageReady = false;
bool slideExists[MAX_SLIDES] = {false};
uint8_t slideOrder[MAX_SLIDES] = {0};
uint8_t slideCount = 0;
uint8_t carouselPosition = 0;
uint8_t currentCarouselSlot = 255;
uint32_t carouselIntervalMs = DEFAULT_CAROUSEL_MS;
unsigned long lastCarouselChangeMs = 0;

// RGB565 像素缓冲区。
uint16_t mediaPixels[PIXEL_COUNT] = {0};
uint16_t drawingPixels[PIXEL_COUNT] = {0};

bool screenSleeping = false;
bool buttonStableState = HIGH;
bool buttonLastReading = HIGH;
unsigned long buttonChangedMs = 0;
static const unsigned long BUTTON_DEBOUNCE_MS = 35;

File imageUploadFile;
String imageUploadPath;
size_t imageUploadBytes = 0;
bool imageUploadOk = false;
bool imageUploadOverflow = false;
bool imageUploadStarted = false;
String imageUploadMessage;
int imageUploadSlot = -1;

// 画板整帧上传状态。
size_t drawingFrameUploadBytes = 0;
bool drawingFrameUploadOk = false;
bool drawingFrameUploadOverflow = false;
bool drawingFrameUploadLive = false;
uint8_t drawingFrameHighByte = 0;
bool drawingFrameHasHighByte = false;
String drawingFrameUploadMessage;

// 自定义时钟背景与精灵上传状态。
File customUploadFile;
String customUploadPath;
size_t customUploadBytes = 0;
size_t customUploadExpectedBytes = 0;
bool customUploadOk = false;
bool customUploadOverflow = false;
bool customUploadStarted = false;
String customUploadMessage;
int customUploadSlot = -1;
int customUploadFrame = -1;

// 双缓冲减少刷新撕裂。
static const bool USE_DOUBLE_BUFFER = true;

// 黑场兼容设置。
static const bool TRUE_BLACK_BACKGROUND_FIX = true;
static const bool DISABLE_RANDOM_SECOND_BLINK = true;


// ---------------- cf_0x01 / Mario 像素风表盘状态 ----------------
static const uint8_t FACE_DIGITAL = 0;
static const uint8_t FACE_ANALOG  = 1;
static const uint8_t FACE_RAINBOW = 2;
static const uint8_t FACE_MARIO   = 3;
static const uint8_t FACE_POKEDEX = 4;
static const uint8_t FACE_CAROUSEL = 5;
static const uint8_t FACE_DRAWING = 6;
static const uint8_t FACE_CUSTOM_CLOCK = 7;
static const uint8_t FACE_COUNT = 8;

// ---------------- 自定义静态/动态时钟 ----------------
static const uint8_t CUSTOM_CLOCK_SLOTS = 4;
static const uint8_t CUSTOM_MAX_FRAMES = 12;
static const uint8_t CUSTOM_MAX_SPRITE_W = 32;
static const uint8_t CUSTOM_MAX_SPRITE_H = 32;
static const uint16_t CUSTOM_TRANSPARENT_DEFAULT = 0xF81F;
static const uint32_t CUSTOM_CFG_MAGIC = 0x43434C4BUL; // CCLK
static const uint8_t CUSTOM_CFG_VERSION = 2;

enum CustomAnimationMode : uint8_t {
  CUSTOM_ANIM_STATIC = 0,
  CUSTOM_ANIM_LOOP = 1,
  CUSTOM_ANIM_EVERY_SECOND = 2,
  CUSTOM_ANIM_EVERY_MINUTE = 3,
  CUSTOM_ANIM_EVERY_HOUR = 4
};

enum CustomMotionMode : uint8_t {
  CUSTOM_MOTION_NONE = 0,
  CUSTOM_MOTION_HORIZONTAL = 1,
  CUSTOM_MOTION_VERTICAL = 2,
  CUSTOM_MOTION_PINGPONG = 3,
  CUSTOM_MOTION_JUMP = 4
};

enum CustomTimeFont : uint8_t {
  CUSTOM_FONT_BLOCK_5X7 = 0,    // 清晰 5x7 点阵字
  CUSTOM_FONT_SEVEN_SEG = 1     // 高辨识度七段数码字
};

enum CustomEffectMode : uint8_t {
  CUSTOM_EFFECT_NONE = 0,
  CUSTOM_EFFECT_FIRE = 1,
  CUSTOM_EFFECT_STARS = 2,
  CUSTOM_EFFECT_RAIN = 3
};

#pragma pack(push, 1)
// 旧配置结构，用于兼容迁移。
struct CustomClockConfigV1 {
  uint32_t magic;
  uint8_t version;
  uint8_t timeFormat;
  uint8_t timeSize;
  int8_t timeX;
  int8_t timeY;
  uint16_t timeColor;
  uint8_t blinkColon;
  uint8_t showDate;
  uint8_t dateFormat;
  int8_t dateX;
  int8_t dateY;
  uint16_t dateColor;
  uint8_t showWeekday;
  int8_t weekdayX;
  int8_t weekdayY;
  uint16_t weekdayColor;
  uint8_t spriteEnabled;
  uint8_t spriteFront;
  uint8_t animationMode;
  uint8_t motionMode;
  uint8_t frameCount;
  uint8_t spriteW;
  uint8_t spriteH;
  int8_t spriteX;
  int8_t spriteY;
  int8_t endX;
  int8_t endY;
  uint8_t jumpHeight;
  uint16_t frameIntervalMs;
  uint16_t durationMs;
  uint8_t framePingPong;
  uint16_t transparentColor;
};

struct CustomClockConfig {
  uint32_t magic;
  uint8_t version;
  uint8_t timeFormat;       // 0 HH:MM, 1 HH:MM:SS, 2 HH MM
  uint8_t timeSize;         // 1 或 2；HH:MM:SS 会自动限制为 1 倍
  int8_t timeX;             // -1 自动居中
  int8_t timeY;
  uint16_t timeColor;
  uint8_t blinkColon;
  uint8_t showDate;
  uint8_t dateFormat;       // 0 MM-DD, 1 YYYY-MM-DD, 2 MM/DD
  int8_t dateX;             // -1 自动居中
  int8_t dateY;
  uint16_t dateColor;
  uint8_t showWeekday;
  int8_t weekdayX;          // -1 自动居中
  int8_t weekdayY;
  uint16_t weekdayColor;
  uint8_t spriteEnabled;
  uint8_t spriteFront;
  uint8_t animationMode;
  uint8_t motionMode;
  uint8_t frameCount;
  uint8_t spriteW;
  uint8_t spriteH;
  int8_t spriteX;
  int8_t spriteY;
  int8_t endX;
  int8_t endY;
  uint8_t jumpHeight;
  uint16_t frameIntervalMs;
  uint16_t durationMs;
  uint8_t framePingPong;
  uint16_t transparentColor;
  uint8_t timeFont;         // CustomTimeFont
  uint8_t effectMode;       // CustomEffectMode
  uint8_t effectIntensity;  // 10~100
};
#pragma pack(pop)

struct CustomAnimationState {
  bool initialized;
  bool running;
  int lastSecond;
  int lastMinute;
  int lastHour;
  unsigned long startedAtMs;
};

CustomClockConfig customClockConfigs[CUSTOM_CLOCK_SLOTS];
CustomAnimationState customAnimState = {false, false, -1, -1, -1, 0};
uint8_t customClockSlot = 0;
uint8_t loadedCustomBackgroundSlot = 255;
uint8_t loadedCustomSpriteSlot = 255;
uint8_t loadedCustomSpriteFrame = 255;
uint16_t customBackgroundPixels[PIXEL_COUNT] = {0};
uint16_t customSpritePixels[CUSTOM_MAX_SPRITE_W * CUSTOM_MAX_SPRITE_H] = {0};
bool customBackgroundAvailable[CUSTOM_CLOCK_SLOTS] = {false};

// 内置像素特效运行状态。所有效果直接按 LED 像素绘制，不使用抗锯齿。
static const uint8_t CUSTOM_FIRE_HEIGHT = 20;
uint8_t customFireHeat[PANEL_RES_X * CUSTOM_FIRE_HEIGHT] = {0};
unsigned long customEffectLastMs = 0;
uint32_t customEffectFrame = 0;
uint8_t customEffectSlot = 255;

// cw-cf-0x01 的素材里 0x000E 是透明占位色。
// 旧版用 drawRGBBitmap 直接画出来，会出现许多极暗蓝色像素点，
// HUB75 低灰阶 PWM 下这些点最容易看起来像“闪烁”。
static const uint16_t CF0X01_TRANSPARENT = 0x000E;

uint8_t activeFaceMode = 255;
int lastMarioMinute = -1;
bool marioNeedsFullRedraw = true;

// ---------------- cf_0x06 / Pokemon Pokedex 表盘状态 ----------------
#define CF0X06_LIGHT_GREEN 0x754d
#define CF0X06_DARK_GREEN  0x0264
#define CF0X06_DARK_BLUE   0x016D
#define CF0X06_LIGHT_BLUE  0x24fe
#define CF0X06_LIGHT_BLACK 0x10c4

bool cf0x06NeedsFullRedraw = true;
int cf0x06LastMinute = -1;
int cf0x06LastYday = -1;
uint8_t cf0x06PokemonIndex = 0;


struct BounceBlockState {
  int16_t x;
  int16_t y;
  int16_t firstY;
  int16_t lastY;
  bool hit;
  bool movingUp;
  String text;
  unsigned long lastStepMs;
};

BounceBlockState marioHourBlock = {13, 8, 8, 8, false, true, "", 0};
BounceBlockState marioMinuteBlock = {32, 8, 8, 8, false, true, "", 0};

bool marioJumping = false;
bool marioMovingUp = true;
bool marioCollisionDone = false;
int16_t marioX = 23;
int16_t marioY = 40;
int16_t marioLastY = 40;
uint8_t marioW = 13;
uint8_t marioH = 16;
const uint16_t *marioSprite = MARIO_IDLE;
unsigned long marioLastStepMs = 0;

void applyDisplaySettingsIfNeeded();
void reconnectWiFiRuntime();
void processWiFiReconnect();
void processTimeSync();

uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return display->color565(r, g, b);
}

void commitFrame() {
  if (!display) return;
  if (USE_DOUBLE_BUFFER) {
    display->flipDMABuffer();
  }
}

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

bool timeReached(unsigned long targetMs) {
  return static_cast<long>(millis() - targetMs) >= 0;
}

String jsonEscape(const String &text) {
  String out;
  out.reserve(text.length() + 8);
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c != '\r') {
      out += c;
    }
  }
  return out;
}

void centerText(const String &text, int16_t y, uint16_t color, uint8_t textSize = 1) {
  display->setFont(NULL);
  display->setTextSize(textSize);
  display->setTextWrap(false);
  int16_t x1, y1;
  uint16_t w, h;
  display->getTextBounds(text.c_str(), 0, y, &x1, &y1, &w, &h);
  int16_t x = (PANEL_RES_X - (int16_t)w) / 2;
  if (x < 0) x = 0;
  display->setCursor(x, y);
  display->setTextColor(color);
  display->print(text);
}

void showStatus(const String &line1, const String &line2 = "", uint16_t color = 0xffff) {
  display->fillScreen(0);
  display->drawRect(0, 0, 64, 64, C(0, 90, 160));
  centerText(line1, 18, color, 1);
  if (line2.length() > 0) centerText(line2, 34, C(255, 220, 80), 1);
  commitFrame();
}

void loadSettings() {
  prefs.begin("clockwise", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  tzInfo = prefs.getString("tz", DEFAULT_TZ);
  ntpServer = prefs.getString("ntp", DEFAULT_NTP);
  brightness = prefs.getUChar("bright", 35);
  rotation = prefs.getUChar("rot", 0);
  faceMode = prefs.getUChar("face", 0);
  carouselIntervalMs = prefs.getUInt("slideMs", DEFAULT_CAROUSEL_MS);
  customClockSlot = prefs.getUChar("ccSlot", 0);
  if (brightness > 255) brightness = 35;
  if (rotation > 3) rotation = 0;
  if (faceMode >= FACE_COUNT) faceMode = 0;
  if (customClockSlot >= CUSTOM_CLOCK_SLOTS) customClockSlot = 0;
  carouselIntervalMs = constrain(carouselIntervalMs, (uint32_t)200, (uint32_t)60000);
}

void saveSettings() {
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("tz", tzInfo);
  prefs.putString("ntp", ntpServer);
  prefs.putUChar("bright", brightness);
  prefs.putUChar("rot", rotation);
  prefs.putUChar("face", faceMode);
  prefs.putUInt("slideMs", carouselIntervalMs);
  prefs.putUChar("ccSlot", customClockSlot);
}


String slidePath(uint8_t slot) {
  return String("/slide") + String(slot) + ".rgb";
}

void scanSlides() {
  slideCount = 0;
  for (uint8_t i = 0; i < MAX_SLIDES; i++) {
    bool valid = false;
    if (storageReady) {
      String path = slidePath(i);
      if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        valid = f && f.size() == RGB565_FILE_BYTES;
        if (f) f.close();
        if (!valid) LittleFS.remove(path);
      }
    }
    slideExists[i] = valid;
    if (valid) slideOrder[slideCount++] = i;
  }

  if (slideCount == 0) {
    carouselPosition = 0;
    currentCarouselSlot = 255;
  } else {
    bool currentStillExists = currentCarouselSlot < MAX_SLIDES && slideExists[currentCarouselSlot];
    if (!currentStillExists) {
      carouselPosition = 0;
      currentCarouselSlot = 255;
    }
  }
}

bool loadRgb565File(const String &path, uint16_t *target) {
  if (!storageReady || !target) return false;
  File f = LittleFS.open(path, "r");
  if (!f || f.size() != RGB565_FILE_BYTES) {
    if (f) f.close();
    return false;
  }

  uint8_t pair[2];
  for (size_t i = 0; i < PIXEL_COUNT; i++) {
    if (f.read(pair, 2) != 2) {
      f.close();
      return false;
    }
    target[i] = ((uint16_t)pair[0] << 8) | pair[1];
  }
  f.close();
  return true;
}

bool saveRgb565File(const String &path, const uint16_t *source) {
  if (!storageReady || !source) return false;
  File f = LittleFS.open(path, "w");
  if (!f) return false;

  uint8_t chunk[128];
  size_t chunkUsed = 0;
  for (size_t i = 0; i < PIXEL_COUNT; i++) {
    uint16_t color = source[i];
    chunk[chunkUsed++] = (uint8_t)(color >> 8);
    chunk[chunkUsed++] = (uint8_t)(color & 0xff);
    if (chunkUsed == sizeof(chunk)) {
      if (f.write(chunk, chunkUsed) != chunkUsed) {
        f.close();
        return false;
      }
      chunkUsed = 0;
    }
  }
  if (chunkUsed > 0 && f.write(chunk, chunkUsed) != chunkUsed) {
    f.close();
    return false;
  }
  f.close();
  return true;
}


String customConfigPath(uint8_t slot) {
  return String("/cc") + String(slot) + ".cfg";
}

String customBackgroundPath(uint8_t slot) {
  return String("/cc") + String(slot) + "_bg.rgb";
}

String customSpritePath(uint8_t slot, uint8_t frame) {
  char path[24];
  snprintf(path, sizeof(path), "/cc%u_sp%02u.rgb", slot, frame);
  return String(path);
}

void setDefaultCustomClockConfig(CustomClockConfig &cfg) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CUSTOM_CFG_MAGIC;
  cfg.version = CUSTOM_CFG_VERSION;
  cfg.timeFormat = 0;
  cfg.timeSize = 2;
  cfg.timeX = -1;
  cfg.timeY = 14;
  cfg.timeColor = 0xFFFF;
  cfg.blinkColon = 1;
  cfg.showDate = 1;
  cfg.dateFormat = 0;
  cfg.dateX = -1;
  cfg.dateY = 36;
  cfg.dateColor = 0x07FF;
  cfg.showWeekday = 1;
  cfg.weekdayX = -1;
  cfg.weekdayY = 49;
  cfg.weekdayColor = 0xFFE0;
  cfg.spriteEnabled = 0;
  cfg.spriteFront = 0;
  cfg.animationMode = CUSTOM_ANIM_STATIC;
  cfg.motionMode = CUSTOM_MOTION_NONE;
  cfg.frameCount = 0;
  cfg.spriteW = 16;
  cfg.spriteH = 16;
  cfg.spriteX = 0;
  cfg.spriteY = 46;
  cfg.endX = 48;
  cfg.endY = 46;
  cfg.jumpHeight = 12;
  cfg.frameIntervalMs = 120;
  cfg.durationMs = 1600;
  cfg.framePingPong = 0;
  cfg.transparentColor = CUSTOM_TRANSPARENT_DEFAULT;
  cfg.timeFont = CUSTOM_FONT_BLOCK_5X7;
  cfg.effectMode = CUSTOM_EFFECT_NONE;
  cfg.effectIntensity = 65;
}

bool saveCustomClockConfig(uint8_t slot) {
  if (!storageReady || slot >= CUSTOM_CLOCK_SLOTS) return false;
  File f = LittleFS.open(customConfigPath(slot), "w");
  if (!f) return false;
  size_t written = f.write((const uint8_t *)&customClockConfigs[slot], sizeof(CustomClockConfig));
  f.close();
  return written == sizeof(CustomClockConfig);
}

void scanCustomFrames(uint8_t slot) {
  if (slot >= CUSTOM_CLOCK_SLOTS) return;
  CustomClockConfig &cfg = customClockConfigs[slot];
  size_t expected = (size_t)cfg.spriteW * cfg.spriteH * 2;
  uint8_t count = 0;
  if (cfg.spriteW == 0 || cfg.spriteH == 0 || cfg.spriteW > CUSTOM_MAX_SPRITE_W || cfg.spriteH > CUSTOM_MAX_SPRITE_H) {
    cfg.spriteW = 16;
    cfg.spriteH = 16;
    expected = 512;
  }
  for (uint8_t i = 0; i < CUSTOM_MAX_FRAMES; i++) {
    String path = customSpritePath(slot, i);
    bool valid = false;
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      valid = f && f.size() == expected;
      if (f) f.close();
      if (!valid) LittleFS.remove(path);
    }
    if (valid && i == count) count++;
  }
  cfg.frameCount = count;
}

void migrateCustomClockConfigV1(const CustomClockConfigV1 &oldCfg, CustomClockConfig &cfg) {
  memset(&cfg, 0, sizeof(cfg));
  memcpy(&cfg, &oldCfg, sizeof(oldCfg));
  cfg.version = CUSTOM_CFG_VERSION;
  cfg.timeFont = CUSTOM_FONT_BLOCK_5X7;
  cfg.effectMode = CUSTOM_EFFECT_NONE;
  cfg.effectIntensity = 65;
}

void loadAllCustomClockConfigs() {
  for (uint8_t slot = 0; slot < CUSTOM_CLOCK_SLOTS; slot++) {
    bool valid = false;
    bool migrated = false;
    String path = customConfigPath(slot);
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      if (f && f.size() == sizeof(CustomClockConfig)) {
        valid = f.read((uint8_t *)&customClockConfigs[slot], sizeof(CustomClockConfig)) == sizeof(CustomClockConfig);
        valid = valid && customClockConfigs[slot].magic == CUSTOM_CFG_MAGIC && customClockConfigs[slot].version == CUSTOM_CFG_VERSION;
      } else if (f && f.size() == sizeof(CustomClockConfigV1)) {
        CustomClockConfigV1 oldCfg;
        bool readOk = f.read((uint8_t *)&oldCfg, sizeof(oldCfg)) == sizeof(oldCfg);
        if (readOk && oldCfg.magic == CUSTOM_CFG_MAGIC && oldCfg.version == 1) {
          migrateCustomClockConfigV1(oldCfg, customClockConfigs[slot]);
          valid = true;
          migrated = true;
        }
      }
      if (f) f.close();
    }
    if (!valid) {
      setDefaultCustomClockConfig(customClockConfigs[slot]);
      saveCustomClockConfig(slot);
    } else if (migrated) {
      saveCustomClockConfig(slot);
    }
    CustomClockConfig &cfg = customClockConfigs[slot];
    cfg.timeSize = constrain(cfg.timeSize, (uint8_t)1, (uint8_t)2);
    cfg.timeFormat = constrain(cfg.timeFormat, (uint8_t)0, (uint8_t)2);
    cfg.timeFont = constrain(cfg.timeFont, (uint8_t)CUSTOM_FONT_BLOCK_5X7, (uint8_t)CUSTOM_FONT_SEVEN_SEG);
    cfg.effectMode = constrain(cfg.effectMode, (uint8_t)CUSTOM_EFFECT_NONE, (uint8_t)CUSTOM_EFFECT_RAIN);
    cfg.effectIntensity = constrain(cfg.effectIntensity, (uint8_t)10, (uint8_t)100);
    cfg.dateFormat = constrain(cfg.dateFormat, (uint8_t)0, (uint8_t)2);
    cfg.animationMode = constrain(cfg.animationMode, (uint8_t)CUSTOM_ANIM_STATIC, (uint8_t)CUSTOM_ANIM_EVERY_HOUR);
    cfg.motionMode = constrain(cfg.motionMode, (uint8_t)CUSTOM_MOTION_NONE, (uint8_t)CUSTOM_MOTION_JUMP);
    cfg.frameIntervalMs = constrain(cfg.frameIntervalMs, (uint16_t)40, (uint16_t)2000);
    cfg.durationMs = constrain(cfg.durationMs, (uint16_t)200, (uint16_t)60000);
    scanCustomFrames(slot);
    customBackgroundAvailable[slot] = LittleFS.exists(customBackgroundPath(slot));
  }
}

void resetCustomEffectState() {
  memset(customFireHeat, 0, sizeof(customFireHeat));
  customEffectLastMs = 0;
  customEffectFrame = 0;
  customEffectSlot = customClockSlot;
}

void resetCustomAnimationState() {
  customAnimState.initialized = false;
  customAnimState.running = false;
  customAnimState.lastSecond = -1;
  customAnimState.lastMinute = -1;
  customAnimState.lastHour = -1;
  customAnimState.startedAtMs = millis();
  loadedCustomSpriteSlot = 255;
  loadedCustomSpriteFrame = 255;
  resetCustomEffectState();
}

bool loadCustomBackground(uint8_t slot) {
  if (slot >= CUSTOM_CLOCK_SLOTS || !customBackgroundAvailable[slot]) {
    memset(customBackgroundPixels, 0, sizeof(customBackgroundPixels));
    loadedCustomBackgroundSlot = slot;
    return false;
  }
  bool ok = loadRgb565File(customBackgroundPath(slot), customBackgroundPixels);
  if (!ok) {
    memset(customBackgroundPixels, 0, sizeof(customBackgroundPixels));
    customBackgroundAvailable[slot] = false;
  }
  loadedCustomBackgroundSlot = slot;
  return ok;
}

bool loadCustomSpriteFrame(uint8_t slot, uint8_t frame) {
  if (slot >= CUSTOM_CLOCK_SLOTS) return false;
  CustomClockConfig &cfg = customClockConfigs[slot];
  if (frame >= cfg.frameCount || cfg.spriteW == 0 || cfg.spriteH == 0) return false;
  if (loadedCustomSpriteSlot == slot && loadedCustomSpriteFrame == frame) return true;
  File f = LittleFS.open(customSpritePath(slot, frame), "r");
  size_t pixels = (size_t)cfg.spriteW * cfg.spriteH;
  if (!f || f.size() != pixels * 2) {
    if (f) f.close();
    return false;
  }
  uint8_t pair[2];
  for (size_t i = 0; i < pixels; i++) {
    if (f.read(pair, 2) != 2) {
      f.close();
      return false;
    }
    customSpritePixels[i] = ((uint16_t)pair[0] << 8) | pair[1];
  }
  f.close();
  loadedCustomSpriteSlot = slot;
  loadedCustomSpriteFrame = frame;
  return true;
}

void setupStorage() {
  storageReady = LittleFS.begin(false);
  if (!storageReady && !prefs.getBool("fsInit", false)) {
    Serial.println("[INFO] LittleFS first-use initialization");
    LittleFS.end();
    storageReady = LittleFS.begin(true);
  }
  if (!storageReady) {
    Serial.println("[ERROR] LittleFS mount failed; saved media functions disabled");
    return;
  }
  prefs.putBool("fsInit", true);

  memset(drawingPixels, 0, sizeof(drawingPixels));
  if (LittleFS.exists("/drawing.rgb")) {
    if (!loadRgb565File("/drawing.rgb", drawingPixels)) {
      LittleFS.remove("/drawing.rgb");
      memset(drawingPixels, 0, sizeof(drawingPixels));
    }
  }
  scanSlides();
  loadAllCustomClockConfigs();
  Serial.printf("[OK] LittleFS ready, %u carousel image(s), custom clocks loaded\n", slideCount);
}

void drawRgb565Buffer(const uint16_t *pixels) {
  if (!display || !pixels) return;
  display->setFont(NULL);
  display->drawRGBBitmap(0, 0, pixels, PANEL_RES_X, PANEL_RES_Y);
}

void drawMediaUnavailable(const String &line1, const String &line2) {
  display->setFont(NULL);
  display->fillScreen(0);
  display->drawRect(0, 0, 64, 64, C(120, 70, 0));
  centerText(line1, 18, C(255, 200, 80), 1);
  centerText(line2, 34, C(120, 220, 255), 1);
}

void activateFace(uint8_t mode, bool persist) {
  if (mode >= FACE_COUNT) return;
  if (faceMode != mode) {
    faceMode = mode;
    activeFaceMode = 255;
    lastDrawMs = 0;
  }
  if (mode == FACE_CAROUSEL) {
    currentCarouselSlot = 255;
    lastCarouselChangeMs = 0;
  }
  if (mode == FACE_CUSTOM_CLOCK) {
    loadedCustomBackgroundSlot = 255;
    resetCustomAnimationState();
  }
  if (persist) prefs.putUChar("face", faceMode);
}

void drawCarouselFace() {
  if (!storageReady) {
    drawMediaUnavailable("NO FS", "LittleFS");
    return;
  }
  if (slideCount == 0) {
    drawMediaUnavailable("NO IMAGE", "Open /studio");
    return;
  }

  unsigned long now = millis();
  bool needLoad = currentCarouselSlot == 255;
  if (!needLoad && now - lastCarouselChangeMs >= carouselIntervalMs) {
    carouselPosition = (carouselPosition + 1) % slideCount;
    needLoad = true;
  }

  if (needLoad) {
    uint8_t slot = slideOrder[carouselPosition % slideCount];
    if (loadRgb565File(slidePath(slot), mediaPixels)) {
      currentCarouselSlot = slot;
      lastCarouselChangeMs = now;
    } else {
      scanSlides();
      drawMediaUnavailable("BAD IMAGE", "Upload again");
      return;
    }
  }
  drawRgb565Buffer(mediaPixels);
}

void drawDrawingFace() {
  drawRgb565Buffer(drawingPixels);
}

void forceMediaRedraw() {
  activeFaceMode = 255;
  lastDrawMs = 0;
  if (faceMode == FACE_CAROUSEL) currentCarouselSlot = 255;
  if (faceMode == FACE_MARIO) marioNeedsFullRedraw = true;
  if (faceMode == FACE_POKEDEX) cf0x06NeedsFullRedraw = true;
  if (faceMode == FACE_CUSTOM_CLOCK) {
    loadedCustomBackgroundSlot = 255;
    resetCustomAnimationState();
  }
}

void setScreenSleeping(bool sleepNow) {
  if (!display || screenSleeping == sleepNow) return;
  screenSleeping = sleepNow;

  if (screenSleeping) {
    // 把前后 DMA 缓冲区都清成黑色，然后关闭亮度输出。
    display->setFont(NULL);
    display->fillScreen(0);
    commitFrame();
    display->fillScreen(0);
    commitFrame();
    display->setBrightness8(0);
    appliedBrightness = 0;
    Serial.println("[INFO] Screen sleeping");
  } else {
    appliedBrightness = 255;
    applyDisplaySettingsIfNeeded();
    forceMediaRedraw();
    Serial.println("[INFO] Screen awake");
  }
}

void setupScreenButton() {
  pinMode(SCREEN_BUTTON_PIN, INPUT_PULLUP);
  buttonStableState = digitalRead(SCREEN_BUTTON_PIN);
  buttonLastReading = buttonStableState;
  buttonChangedMs = millis();
}

void handleScreenButton() {
  bool reading = digitalRead(SCREEN_BUTTON_PIN);
  if (reading != buttonLastReading) {
    buttonLastReading = reading;
    buttonChangedMs = millis();
  }

  if (millis() - buttonChangedMs >= BUTTON_DEBOUNCE_MS && reading != buttonStableState) {
    buttonStableState = reading;
    if (buttonStableState == LOW) {
      setScreenSleeping(!screenSleeping);
    }
  }
}

void setupDisplay() {
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);

  mxconfig.gpio.r1 = R1_PIN;
  mxconfig.gpio.g1 = G1_PIN;
  mxconfig.gpio.b1 = B1_PIN;
  mxconfig.gpio.r2 = R2_PIN;
  mxconfig.gpio.g2 = G2_PIN;
  mxconfig.gpio.b2 = B2_PIN;
  mxconfig.gpio.a  = A_PIN;
  mxconfig.gpio.b  = B_PIN;
  mxconfig.gpio.c  = C_PIN;
  mxconfig.gpio.d  = D_PIN;
  mxconfig.gpio.e  = E_PIN;
  mxconfig.gpio.lat = LAT_PIN;
  mxconfig.gpio.oe  = OE_PIN;
  mxconfig.gpio.clk = CLK_PIN;

  // 保持当前时钟相位；出现错位或鬼影时可改为 true。
  mxconfig.clkphase = false;

  // 降低时钟并增加锁存消隐，提升长线连接稳定性。
  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;
  mxconfig.min_refresh_rate = 120;
  mxconfig.double_buff = USE_DOUBLE_BUFFER;
  mxconfig.latch_blanking = 4;

  // 特殊 FM6126A/FM6124 芯片屏黑屏时可尝试取消下一行注释。
  // mxconfig.driver = HUB75_I2S_CFG::FM6126A;

  display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!display->begin()) {
    Serial.println("[ERROR] Matrix begin failed");
    while (true) delay(1000);
  }
  applyDisplaySettingsIfNeeded();

  // 清空前后缓冲区。
  display->clearScreen();
  commitFrame();
  display->clearScreen();
  commitFrame();
}

void showBoot() {
  display->fillScreen(0);
  display->drawRect(0, 0, 64, 64, C(0, 120, 255));
  display->drawRect(1, 1, 62, 62, C(0, 255, 100));
  centerText("WYL LAB", 8, C(255, 255, 255), 1);
  centerText("ESP32-S3", 22, C(255, 180, 0), 1);
  centerText("64x64", 36, C(0, 255, 255), 1);
  centerText("Arduino", 50, C(255, 80, 160), 1);
  commitFrame();
  delay(1200);
}

uint16_t wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) {
    return C(255 - pos * 3, 0, pos * 3);
  }
  if (pos < 170) {
    pos -= 85;
    return C(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return C(pos * 3, 255 - pos * 3, 0);
}

bool readTime(struct tm &timeinfo) {
  // 非阻塞读取本地时间，避免影响屏幕刷新。
  time_t now = time(nullptr);
  if (now < 1600000000) return false;
  localtime_r(&now, &timeinfo);
  return true;
}

void applyDisplaySettingsIfNeeded() {
  if (!display) return;
  if (!screenSleeping && appliedBrightness != brightness) {
    display->setBrightness8(brightness);
    appliedBrightness = brightness;
  }
  if (appliedRotation != rotation) {
    display->setRotation(rotation);
    appliedRotation = rotation;
    if (faceMode == FACE_MARIO) marioNeedsFullRedraw = true;
    if (faceMode == FACE_POKEDEX) cf0x06NeedsFullRedraw = true;
  }
}

void configureTime() {
  if (WiFi.status() != WL_CONNECTED) return;
  showStatus("NTP Sync", ntpServer, C(120, 220, 255));
  configTzTime(tzInfo.c_str(), ntpServer.c_str(), "time.google.com", "pool.ntp.org");
  timeReady = false;
  timeSyncRunning = true;
  timeSyncStartedMs = millis();
  timeSyncLastCheckMs = 0;
}

void processTimeSync() {
  if (!timeSyncRunning) return;
  if (WiFi.status() != WL_CONNECTED) {
    timeSyncRunning = false;
    return;
  }

  const unsigned long now = millis();
  if (now - timeSyncLastCheckMs < 250) return;
  timeSyncLastCheckMs = now;

  struct tm timeinfo;
  if (readTime(timeinfo)) {
    timeReady = true;
    timeSyncRunning = false;
    Serial.println("[OK] Time synchronized");
    forceMediaRedraw();
  } else if (now - timeSyncStartedMs >= 10000) {
    timeSyncRunning = false;
    Serial.println("[WARN] NTP sync timeout");
  }
}

void drawDateLine(const struct tm &t) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  centerText(buf, 2, C(90, 180, 255), 1);
}

void drawDigitalFace(const struct tm &t) {
  display->setFont(NULL);
  display->fillScreen(0);
  drawDateLine(t);

  char hhmm[8];
  if (t.tm_sec % 2 == 0) {
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
  } else {
    snprintf(hhmm, sizeof(hhmm), "%02d %02d", t.tm_hour, t.tm_min);
  }

  display->setTextSize(2);
  display->setTextWrap(false);
  display->setTextColor(C(255, 255, 255));
  display->setCursor(2, 22);
  display->print(hhmm);

  char sec[6];
  snprintf(sec, sizeof(sec), "%02d", t.tm_sec);
  display->setTextSize(1);
  display->setTextColor(C(255, 210, 80));
  display->setCursor(49, 49);
  display->print(sec);

  const char *week[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  display->setCursor(2, 49);
  display->setTextColor(C(0, 220, 120));
  display->print(week[t.tm_wday]);
}

void drawHand(float deg, uint8_t len, uint16_t color) {
  float rad = deg * 3.1415926f / 180.0f;
  int x = 32 + (int)(cos(rad) * len);
  int y = 32 + (int)(sin(rad) * len);
  display->drawLine(32, 32, x, y, color);
}

void drawAnalogFace(const struct tm &t) {
  display->setFont(NULL);
  display->fillScreen(0);
  display->drawCircle(32, 32, 30, C(80, 160, 255));
  display->drawCircle(32, 32, 29, C(20, 50, 80));

  for (int i = 0; i < 60; i++) {
    float deg = i * 6.0f - 90.0f;
    float rad = deg * 3.1415926f / 180.0f;
    int r = (i % 5 == 0) ? 25 : 28;
    int x = 32 + (int)(cos(rad) * r);
    int y = 32 + (int)(sin(rad) * r);
    uint16_t color = (i % 5 == 0) ? C(255, 255, 255) : C(80, 80, 80);
    display->drawPixel(x, y, color);
  }

  float hourDeg = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 30.0f - 90.0f;
  float minDeg = (t.tm_min + t.tm_sec / 60.0f) * 6.0f - 90.0f;
  float secDeg = t.tm_sec * 6.0f - 90.0f;

  drawHand(hourDeg, 13, C(255, 200, 80));
  drawHand(minDeg, 21, C(255, 255, 255));
  drawHand(secDeg, 25, C(255, 60, 60));
  display->fillCircle(32, 32, 2, C(0, 220, 120));

  char sec[4];
  snprintf(sec, sizeof(sec), "%02d", t.tm_sec);
  display->setTextSize(1);
  display->setTextColor(C(160, 220, 255));
  display->setCursor(26, 53);
  display->print(sec);
}

void drawRainbowFace(const struct tm &t) {
  display->setFont(NULL);
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 64; x++) {
      byte idx = (byte)((x * 3 + y * 2 + animOffset) & 0xff);
      if (((x + y + animOffset / 3) % 9) == 0) {
        display->drawPixel(x, y, wheel(idx));
      } else {
        display->drawPixel(x, y, C(0, 0, 0));
      }
    }
  }

  display->fillRect(0, 20, 64, 23, C(0, 0, 0));
  display->drawRect(0, 20, 64, 23, C(80, 80, 80));
  char hhmm[8];
  snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
  display->setTextSize(2);
  display->setTextColor(C(255, 255, 255));
  display->setCursor(2, 24);
  display->print(hhmm);
  animOffset += 3;
}


void drawCf0x01Bitmap(int16_t x, int16_t y, const uint16_t *bitmap, uint8_t w, uint8_t h) {
  // 跳过透明占位色。
  for (uint8_t yy = 0; yy < h; yy++) {
    int16_t py = y + yy;
    if (py < 0 || py >= PANEL_RES_Y) continue;
    for (uint8_t xx = 0; xx < w; xx++) {
      int16_t px = x + xx;
      if (px < 0 || px >= PANEL_RES_X) continue;
      uint16_t color = bitmap[yy * w + xx];
      if (color == CF0X01_TRANSPARENT) continue;
      display->drawPixel(px, py, color);
    }
  }
}

void drawCf0x01TextOnBlock(BounceBlockState &block) {
  display->setFont(&Super_Mario_Bros__24pt7b);
  display->setTextSize(1);
  display->setTextColor(0x0000);
  display->setTextWrap(false);
  int16_t tx = block.x + (block.text.length() == 1 ? 6 : 2);
  display->setCursor(tx, block.y + 12);
  display->print(block.text);
}

void drawCf0x01Block(BounceBlockState &block) {
  drawCf0x01Bitmap(block.x, block.y, BLOCK, 19, 19);
  drawCf0x01TextOnBlock(block);
}

void drawCf0x01Ground() {
  for (int x = 0; x < 64; x += 8) {
    drawCf0x01Bitmap(x, 56, GROUND, 8, 8);
  }
}

void drawCf0x01StaticScene() {
  display->setFont(NULL);
  display->fillScreen(TRUE_BLACK_BACKGROUND_FIX ? 0x0000 : SKY_COLOR);
  drawCf0x01Ground();
  drawCf0x01Bitmap(43, 47, BUSH, 21, 9);
  drawCf0x01Bitmap(0, 34, HILL, 20, 22);
  drawCf0x01Bitmap(0, 21, CLOUD1, 13, 12);
  drawCf0x01Bitmap(51, 7, CLOUD2, 13, 12);
}

void cf0x01SetTimeText(const struct tm &t) {
  marioHourBlock.text = String(t.tm_hour);
  char minuteBuf[4];
  snprintf(minuteBuf, sizeof(minuteBuf), "%02d", t.tm_min);
  marioMinuteBlock.text = String(minuteBuf);
}

void cf0x01DrawMario() {
  drawCf0x01Bitmap(marioX, marioY, marioSprite, marioW, marioH);
}

void cf0x01SetupFace(const struct tm &t) {
  cf0x01SetTimeText(t);
  marioHourBlock.y = marioHourBlock.firstY;
  marioMinuteBlock.y = marioMinuteBlock.firstY;
  marioHourBlock.lastY = marioHourBlock.y;
  marioMinuteBlock.lastY = marioMinuteBlock.y;
  marioHourBlock.hit = false;
  marioMinuteBlock.hit = false;
  marioHourBlock.movingUp = true;
  marioMinuteBlock.movingUp = true;

  marioX = 23;
  marioY = 40;
  marioLastY = marioY;
  marioW = MARIO_IDLE_SIZE[0];
  marioH = MARIO_IDLE_SIZE[1];
  marioSprite = MARIO_IDLE;
  marioJumping = false;
  marioMovingUp = true;
  marioCollisionDone = false;
  marioLastStepMs = 0;
  lastMarioMinute = t.tm_min;

  // 整帧渲染，减少局部残影。
}

void cf0x01StartJump() {
  if (marioJumping && millis() - marioLastStepMs < 500) return;
  marioW = MARIO_JUMP_SIZE[0];
  marioH = MARIO_JUMP_SIZE[1];
  marioSprite = MARIO_JUMP;
  marioMovingUp = true;
  marioJumping = true;
  marioCollisionDone = false;
  marioLastY = marioY;
}

void cf0x01HitBlock(BounceBlockState &block) {
  if (!block.hit) {
    block.hit = true;
    block.movingUp = true;
    block.lastStepMs = 0;
  }
}

void cf0x01UpdateBlock(BounceBlockState &block) {
  if (!block.hit) return;
  unsigned long nowMs = millis();
  if (nowMs - block.lastStepMs < 60) return;

  block.lastY = block.y;
  block.y += block.movingUp ? -2 : 2;

  if (block.firstY - block.y >= 4) {
    block.movingUp = false;
  }
  if (!block.movingUp && block.y >= block.firstY) {
    block.y = block.firstY;
    block.hit = false;
  }

  block.lastStepMs = nowMs;
}

void cf0x01UpdateMario() {
  if (!marioJumping) return;
  unsigned long nowMs = millis();
  if (nowMs - marioLastStepMs < 50) return;

  marioY += marioMovingUp ? -3 : 3;

  if ((marioLastY - marioY) >= 14) {
    marioMovingUp = false;
  }

  // Mario 顶到时间砖块下方时，让两个砖块弹起，模拟原 cf_0x01 的碰撞效果。
  if (!marioCollisionDone && marioY <= 28) {
    marioCollisionDone = true;
    marioMovingUp = false;
    cf0x01HitBlock(marioHourBlock);
    cf0x01HitBlock(marioMinuteBlock);
  }

  if (marioY + marioH >= 56) {
    marioY = 40;
    marioW = MARIO_IDLE_SIZE[0];
    marioH = MARIO_IDLE_SIZE[1];
    marioSprite = MARIO_IDLE;
    marioJumping = false;
  }

  marioLastStepMs = nowMs;
}

void cf0x01RenderFrame() {
  drawCf0x01StaticScene();
  drawCf0x01Block(marioHourBlock);
  drawCf0x01Block(marioMinuteBlock);
  cf0x01DrawMario();
}

void drawMarioCf0x01Face(const struct tm &t) {
  if (marioNeedsFullRedraw || activeFaceMode != FACE_MARIO) {
    cf0x01SetupFace(t);
    marioNeedsFullRedraw = false;
  }

  if (t.tm_min != lastMarioMinute) {
    lastMarioMinute = t.tm_min;
    cf0x01SetTimeText(t);
    cf0x01StartJump();
  }

  cf0x01UpdateMario();
  cf0x01UpdateBlock(marioHourBlock);
  cf0x01UpdateBlock(marioMinuteBlock);
  cf0x01RenderFrame();
}


const uint16_t *cf0x06PokemonByIndex(uint8_t idx) {
  switch (idx % 7) {
    case 0: return pokemon1;
    case 1: return pokemon2;
    case 2: return pokemon3;
    case 3: return pokemon4;
    case 4: return pokemon5;
    case 5: return pokemon6;
    default: return pokemon7;
  }
}

void cf0x06DrawWeekday(uint8_t weekday, uint16_t color) {
  uint8_t x = 36 + ((weekday > 3 ? (weekday - 4) : weekday) * 6);
  uint8_t y = 35 + (weekday > 3 ? 5 : 0);
  display->fillRect(x, y, 5, 4, color);
}

void cf0x06DrawTime(const struct tm &t) {
  display->setFont(&PKMN_RBYGSC4pt7b);
  display->setTextSize(1);
  display->setTextWrap(false);
  display->setTextColor(0xffff);

  // 原主题的时间区域较小，小时不强制补 0，更接近原版排版。
  display->fillRect(35, 17, 26, 14, CF0X06_LIGHT_BLACK);
  display->setCursor(35, 22);
  display->print(t.tm_hour);

  char minutes[3];
  snprintf(minutes, sizeof(minutes), "%02d", t.tm_min);
  display->setCursor(46, 30);
  display->print(minutes);

  display->setFont(NULL);
}

void cf0x06DrawDate(const struct tm &t) {
  // 先把 7 个日期格全部恢复为浅蓝，再把今天标成深蓝。
  for (uint8_t i = 0; i < 7; i++) cf0x06DrawWeekday(i, CF0X06_LIGHT_BLUE);
  cf0x06DrawWeekday(t.tm_wday, CF0X06_DARK_BLUE);
}

void cf0x06DrawLoadingBar(uint8_t seconds) {
  display->fillRect(9, 53, 11, 5, CF0X06_LIGHT_GREEN);
  uint8_t w = seconds == 0 ? 1 : ((10 * seconds) / 59) + 1;
  if (w > 11) w = 11;
  display->fillRect(9, 53, w, 5, CF0X06_DARK_GREEN);
}

void drawPokedexCf0x06Face(const struct tm &t) {
  if (cf0x06NeedsFullRedraw || t.tm_min != cf0x06LastMinute) {
    cf0x06PokemonIndex = random(7);
    cf0x06LastMinute = t.tm_min;
    cf0x06NeedsFullRedraw = false;
  }

  display->setFont(NULL);
  display->drawRGBBitmap(0, 0, POKEDEX_BG, 64, 64);
  display->drawRGBBitmap(8, 21, cf0x06PokemonByIndex(cf0x06PokemonIndex), 16, 16);

  cf0x06DrawTime(t);
  cf0x06DrawDate(t);
  cf0x06DrawLoadingBar(t.tm_sec);

  // 默认关闭左上角随机秒闪烁；修改开关可恢复。
  if (DISABLE_RANDOM_SECOND_BLINK) {
    display->fillRect(5, 4, 2, 4, 0x0000);
    display->fillRect(4, 5, 4, 2, 0x0000);
  } else {
    uint16_t blinkColor = wheel((uint8_t)((t.tm_sec * 31 + millis() / 500) & 0xff));
    display->fillRect(5, 4, 2, 4, blinkColor);
    display->fillRect(4, 5, 4, 2, blinkColor);
  }
}


bool getPixelGlyph(char ch, uint8_t rows[7], uint8_t &width) {
  ch = (char)toupper((unsigned char)ch);
  width = 5;
  memset(rows, 0, 7);
  switch (ch) {
    case '0': { uint8_t r[7]={0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(rows,r,7); return true; }
    case '1': { uint8_t r[7]={0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,r,7); return true; }
    case '2': { uint8_t r[7]={0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(rows,r,7); return true; }
    case '3': { uint8_t r[7]={0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; memcpy(rows,r,7); return true; }
    case '4': { uint8_t r[7]={0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(rows,r,7); return true; }
    case '5': { uint8_t r[7]={0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; memcpy(rows,r,7); return true; }
    case '6': { uint8_t r[7]={0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}; memcpy(rows,r,7); return true; }
    case '7': { uint8_t r[7]={0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(rows,r,7); return true; }
    case '8': { uint8_t r[7]={0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(rows,r,7); return true; }
    case '9': { uint8_t r[7]={0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}; memcpy(rows,r,7); return true; }
    case ':': width=1; rows[2]=1; rows[5]=1; return true;
    case '.': width=1; rows[6]=1; return true;
    case '-': width=3; rows[3]=0x07; return true;
    case '/': width=5; rows[0]=0x01; rows[1]=0x02; rows[2]=0x02; rows[3]=0x04; rows[4]=0x08; rows[5]=0x08; rows[6]=0x10; return true;
    case ' ': width=3; return true;
    case 'A': { uint8_t r[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,r,7); return true; }
    case 'D': { uint8_t r[7]={0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; memcpy(rows,r,7); return true; }
    case 'E': { uint8_t r[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; memcpy(rows,r,7); return true; }
    case 'F': { uint8_t r[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; memcpy(rows,r,7); return true; }
    case 'H': { uint8_t r[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,r,7); return true; }
    case 'I': { uint8_t r[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}; memcpy(rows,r,7); return true; }
    case 'M': { uint8_t r[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; memcpy(rows,r,7); return true; }
    case 'N': { uint8_t r[7]={0x11,0x19,0x15,0x13,0x11,0x11,0x11}; memcpy(rows,r,7); return true; }
    case 'O': { uint8_t r[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,r,7); return true; }
    case 'P': { uint8_t r[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; memcpy(rows,r,7); return true; }
    case 'R': { uint8_t r[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; memcpy(rows,r,7); return true; }
    case 'S': { uint8_t r[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; memcpy(rows,r,7); return true; }
    case 'T': { uint8_t r[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); return true; }
    case 'U': { uint8_t r[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,r,7); return true; }
    case 'W': { uint8_t r[7]={0x11,0x11,0x11,0x15,0x15,0x1B,0x11}; memcpy(rows,r,7); return true; }
    case 'Y': { uint8_t r[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); return true; }
    default: return false;
  }
}

uint8_t sevenSegmentMask(char ch) {
  switch (ch) {
    case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F;
    case '4': return 0x66; case '5': return 0x6D; case '6': return 0x7D; case '7': return 0x07;
    case '8': return 0x7F; case '9': return 0x6F; case '-': return 0x40; default: return 0;
  }
}

int16_t customPixelCharWidth(char ch, uint8_t fontStyle, uint8_t scale) {
  if (fontStyle == CUSTOM_FONT_SEVEN_SEG) {
    if (ch == ':') return 1 * scale;
    if (ch == ' ') return 3 * scale;
    return 5 * scale;
  }
  uint8_t rows[7], width = 5;
  getPixelGlyph(ch, rows, width);
  return width * scale;
}

int16_t customPixelTextWidth(const String &text, uint8_t fontStyle, uint8_t scale) {
  int16_t width = 0;
  for (size_t i = 0; i < text.length(); i++) {
    width += customPixelCharWidth(text[i], fontStyle, scale);
    if (i + 1 < text.length()) width += scale;
  }
  return width;
}

void drawBlockGlyph(char ch, int16_t x, int16_t y, uint16_t color, uint8_t scale) {
  uint8_t rows[7], width = 5;
  if (!getPixelGlyph(ch, rows, width)) return;
  for (uint8_t py = 0; py < 7; py++) {
    for (uint8_t px = 0; px < width; px++) {
      if (rows[py] & (1U << (width - 1 - px))) {
        display->fillRect(x + px * scale, y + py * scale, scale, scale, color);
      }
    }
  }
}

void drawSevenSegmentGlyph(char ch, int16_t x, int16_t y, uint16_t color, uint8_t scale) {
  if (ch == ':') {
    display->fillRect(x, y + 2 * scale, scale, scale, color);
    display->fillRect(x, y + 6 * scale, scale, scale, color);
    return;
  }
  uint8_t mask = sevenSegmentMask(ch);
  if (!mask) return;
  if (mask & 0x01) display->fillRect(x + scale, y, 3 * scale, scale, color);             // A
  if (mask & 0x02) display->fillRect(x + 4 * scale, y + scale, scale, 3 * scale, color); // B
  if (mask & 0x04) display->fillRect(x + 4 * scale, y + 5 * scale, scale, 3 * scale, color); // C
  if (mask & 0x08) display->fillRect(x + scale, y + 8 * scale, 3 * scale, scale, color);  // D
  if (mask & 0x10) display->fillRect(x, y + 5 * scale, scale, 3 * scale, color);          // E
  if (mask & 0x20) display->fillRect(x, y + scale, scale, 3 * scale, color);              // F
  if (mask & 0x40) display->fillRect(x + scale, y + 4 * scale, 3 * scale, scale, color);  // G
}

void drawCustomPixelTextRaw(const String &text, int16_t x, int16_t y, uint16_t color, uint8_t scale, uint8_t fontStyle) {
  scale = constrain(scale, (uint8_t)1, (uint8_t)2);
  if (fontStyle > CUSTOM_FONT_SEVEN_SEG) fontStyle = CUSTOM_FONT_BLOCK_5X7;
  int16_t drawX = x;
  for (size_t i = 0; i < text.length(); i++) {
    char ch = text[i];
    if (fontStyle == CUSTOM_FONT_SEVEN_SEG) drawSevenSegmentGlyph(ch, drawX, y, color, scale);
    else drawBlockGlyph(ch, drawX, y, color, scale);
    drawX += customPixelCharWidth(ch, fontStyle, scale) + scale;
  }
}

void drawCustomPixelText(const String &text, int8_t x, int8_t y, uint16_t color, uint8_t scale, uint8_t fontStyle) {
  int16_t drawX = x;
  if (x < 0) drawX = (PANEL_RES_X - customPixelTextWidth(text, fontStyle, scale)) / 2;
  drawCustomPixelTextRaw(text, drawX, y, color, scale, fontStyle);
}

void beginStartupIpDisplay(const String &ip, bool isAp) {
  startupIpText = ip;
  startupIpTitle = isAp ? "AP IP" : "WIFI IP";
  startupIpStartedMs = millis();
  startupIpLastScrollMs = startupIpStartedMs;
  startupIpLastFrameMs = 0;
  startupIpScrollX = PANEL_RES_X;
  startupIpActive = startupIpText.length() > 0;
  lastDrawMs = 0;
}

bool handleStartupIpDisplay() {
  if (!startupIpActive || !display) return false;

  unsigned long now = millis();
  if (now - startupIpStartedMs >= STARTUP_IP_DURATION_MS) {
    startupIpActive = false;
    forceMediaRedraw();
    return false;
  }

  const uint8_t scale = 1;
  const int16_t textWidth = customPixelTextWidth(startupIpText, CUSTOM_FONT_BLOCK_5X7, scale);
  const unsigned long frameInterval = textWidth > PANEL_RES_X ? STARTUP_IP_SCROLL_STEP_MS : 250UL;
  if (startupIpLastFrameMs != 0 && now - startupIpLastFrameMs < frameInterval) return true;
  startupIpLastFrameMs = now;

  if (textWidth > PANEL_RES_X && now - startupIpLastScrollMs >= STARTUP_IP_SCROLL_STEP_MS) {
    unsigned long steps = (now - startupIpLastScrollMs) / STARTUP_IP_SCROLL_STEP_MS;
    startupIpLastScrollMs += steps * STARTUP_IP_SCROLL_STEP_MS;
    startupIpScrollX -= (int16_t)steps;
    if (startupIpScrollX < -textWidth) startupIpScrollX = PANEL_RES_X;
  }

  display->setFont(NULL);
  display->fillScreen(0);
  display->drawRect(0, 0, PANEL_RES_X, PANEL_RES_Y, C(0, 150, 255));
  display->drawRect(1, 1, PANEL_RES_X - 2, PANEL_RES_Y - 2, C(0, 55, 95));
  drawCustomPixelText(startupIpTitle, -1, 9, C(100, 225, 255), 1, CUSTOM_FONT_BLOCK_5X7);
  display->drawFastHLine(6, 22, 52, C(0, 95, 150));

  int16_t drawX = textWidth <= PANEL_RES_X ? (PANEL_RES_X - textWidth) / 2 : startupIpScrollX;
  drawCustomPixelTextRaw(startupIpText, drawX, 30, C(255, 255, 255), 1, CUSTOM_FONT_BLOCK_5X7);

  // 底部进度条直观表示剩余显示时间，不使用抗锯齿文字。
  unsigned long elapsed = now - startupIpStartedMs;
  int16_t remainingWidth = (int16_t)((STARTUP_IP_DURATION_MS - elapsed) * 52UL / STARTUP_IP_DURATION_MS);
  display->drawRect(6, 53, 52, 4, C(45, 70, 85));
  if (remainingWidth > 0) display->fillRect(6, 53, remainingWidth, 4, C(0, 220, 135));
  commitFrame();
  return true;
}

uint16_t customFireColor(uint8_t heat) {
  if (heat < 18) return 0x0000;
  if (heat < 70) return C((uint8_t)map(heat, 18, 69, 40, 150), 0, 0);
  if (heat < 135) return C(255, (uint8_t)map(heat, 70, 134, 0, 72), 0);
  if (heat < 200) return C(255, (uint8_t)map(heat, 135, 199, 72, 190), 0);
  if (heat < 238) return C(255, 220, (uint8_t)map(heat, 200, 237, 0, 90));
  return C(255, 255, 190);
}

void updateCustomFire(uint8_t intensity) {
  for (uint8_t x = 0; x < PANEL_RES_X; x++) {
    uint8_t chance = (uint8_t)random(0, 100);
    customFireHeat[(CUSTOM_FIRE_HEIGHT - 1) * PANEL_RES_X + x] =
      chance < intensity ? (uint8_t)random(150, 256) : (uint8_t)random(0, 70);
  }
  for (uint8_t y = 0; y < CUSTOM_FIRE_HEIGHT - 1; y++) {
    uint8_t below = y + 1;
    uint8_t below2 = min((uint8_t)(y + 2), (uint8_t)(CUSTOM_FIRE_HEIGHT - 1));
    for (uint8_t x = 0; x < PANEL_RES_X; x++) {
      uint8_t xl = x == 0 ? PANEL_RES_X - 1 : x - 1;
      uint8_t xr = x == PANEL_RES_X - 1 ? 0 : x + 1;
      uint16_t sum = customFireHeat[below * PANEL_RES_X + xl]
                   + customFireHeat[below * PANEL_RES_X + x]
                   + customFireHeat[below * PANEL_RES_X + xr]
                   + customFireHeat[below2 * PANEL_RES_X + x];
      int value = (int)(sum / 4) - random(0, 5);
      customFireHeat[y * PANEL_RES_X + x] = value > 0 ? (uint8_t)value : 0;
    }
  }
}

uint32_t customHash(uint32_t v) {
  v ^= v >> 16; v *= 0x7feb352dUL; v ^= v >> 15; v *= 0x846ca68bUL; v ^= v >> 16; return v;
}

void drawCustomBuiltInEffect(const CustomClockConfig &cfg) {
  if (cfg.effectMode == CUSTOM_EFFECT_NONE) return;
  if (customEffectSlot != customClockSlot) resetCustomEffectState();
  unsigned long now = millis();
  uint16_t stepMs = cfg.effectMode == CUSTOM_EFFECT_FIRE ? 55 : 85;
  if (customEffectLastMs == 0 || now - customEffectLastMs >= stepMs) {
    customEffectLastMs = now;
    customEffectFrame++;
    if (cfg.effectMode == CUSTOM_EFFECT_FIRE) updateCustomFire(cfg.effectIntensity);
  }

  if (cfg.effectMode == CUSTOM_EFFECT_FIRE) {
    int16_t y0 = PANEL_RES_Y - CUSTOM_FIRE_HEIGHT;
    for (uint8_t y = 0; y < CUSTOM_FIRE_HEIGHT; y++) {
      for (uint8_t x = 0; x < PANEL_RES_X; x++) {
        uint16_t color = customFireColor(customFireHeat[y * PANEL_RES_X + x]);
        if (color != 0) display->drawPixel(x, y0 + y, color);
      }
    }
  } else if (cfg.effectMode == CUSTOM_EFFECT_STARS) {
    uint8_t count = 6 + cfg.effectIntensity / 5;
    for (uint8_t i = 0; i < count; i++) {
      uint32_t seed = customHash((uint32_t)i * 7919UL + customClockSlot * 131UL);
      uint8_t x = seed % 64;
      uint8_t y = (seed >> 8) % 46;
      uint8_t phase = (customEffectFrame + (seed >> 16)) % 20;
      if (phase < 2) {
        uint16_t color = phase == 0 ? 0xFFFF : C(130, 190, 255);
        display->drawPixel(x, y, color);
        if (phase == 0 && (i % 3 == 0)) {
          if (x > 0) display->drawPixel(x - 1, y, color);
          if (x < 63) display->drawPixel(x + 1, y, color);
          if (y > 0) display->drawPixel(x, y - 1, color);
          if (y < 63) display->drawPixel(x, y + 1, color);
        }
      }
    }
  } else if (cfg.effectMode == CUSTOM_EFFECT_RAIN) {
    uint8_t count = 8 + cfg.effectIntensity / 4;
    for (uint8_t i = 0; i < count; i++) {
      uint32_t seed = customHash((uint32_t)i * 3571UL + customClockSlot * 173UL);
      uint8_t x = seed % 64;
      int16_t y = (int16_t)((customEffectFrame * (2 + (seed & 1)) + (seed >> 8)) % 72) - 6;
      uint16_t c1 = C(35, 125, 255);
      uint16_t c2 = C(10, 55, 130);
      if (y >= 0 && y < 64) display->drawPixel(x, y, c1);
      if (y - 1 >= 0 && y - 1 < 64) display->drawPixel(x, y - 1, c2);
      if (y - 2 >= 0 && y - 2 < 64 && cfg.effectIntensity > 60) display->drawPixel(x, y - 2, c2);
    }
  }
}

void drawCustomSprite(uint8_t slot, uint8_t frame, int16_t x, int16_t y) {
  CustomClockConfig &cfg = customClockConfigs[slot];
  if (!loadCustomSpriteFrame(slot, frame)) return;
  for (uint8_t py = 0; py < cfg.spriteH; py++) {
    for (uint8_t px = 0; px < cfg.spriteW; px++) {
      uint16_t color = customSpritePixels[(size_t)py * cfg.spriteW + px];
      if (color == cfg.transparentColor) continue;
      int16_t sx = x + px;
      int16_t sy = y + py;
      if (sx >= 0 && sx < PANEL_RES_X && sy >= 0 && sy < PANEL_RES_Y) {
        display->drawPixel(sx, sy, color);
      }
    }
  }
}

void customAnimationValues(const struct tm &t, uint8_t &frame, int16_t &x, int16_t &y) {
  CustomClockConfig &cfg = customClockConfigs[customClockSlot];
  unsigned long now = millis();
  bool trigger = false;
  if (!customAnimState.initialized) {
    customAnimState.initialized = true;
    customAnimState.lastSecond = t.tm_sec;
    customAnimState.lastMinute = t.tm_min;
    customAnimState.lastHour = t.tm_hour;
    customAnimState.startedAtMs = now;
    customAnimState.running = cfg.animationMode == CUSTOM_ANIM_LOOP;
  } else {
    if (cfg.animationMode == CUSTOM_ANIM_EVERY_SECOND && t.tm_sec != customAnimState.lastSecond) trigger = true;
    if (cfg.animationMode == CUSTOM_ANIM_EVERY_MINUTE && t.tm_min != customAnimState.lastMinute) trigger = true;
    if (cfg.animationMode == CUSTOM_ANIM_EVERY_HOUR && t.tm_hour != customAnimState.lastHour) trigger = true;
    customAnimState.lastSecond = t.tm_sec;
    customAnimState.lastMinute = t.tm_min;
    customAnimState.lastHour = t.tm_hour;
  }
  if (trigger) {
    customAnimState.running = true;
    customAnimState.startedAtMs = now;
  }

  x = cfg.spriteX;
  y = cfg.spriteY;
  frame = 0;
  if (!cfg.spriteEnabled || cfg.frameCount == 0) return;

  uint32_t duration = max((uint32_t)cfg.durationMs, (uint32_t)cfg.frameIntervalMs * max((uint8_t)1, cfg.frameCount));
  uint32_t elapsed = now - customAnimState.startedAtMs;
  bool continuous = cfg.animationMode == CUSTOM_ANIM_LOOP;
  if (continuous) customAnimState.running = true;
  if (!continuous && customAnimState.running && elapsed >= duration) {
    customAnimState.running = false;
  }
  if (!customAnimState.running && cfg.animationMode != CUSTOM_ANIM_STATIC) return;
  if (cfg.animationMode == CUSTOM_ANIM_STATIC) elapsed = 0;
  else if (continuous && duration > 0) elapsed %= duration;

  uint32_t rawFrame = cfg.frameIntervalMs ? elapsed / cfg.frameIntervalMs : 0;
  if (cfg.framePingPong && cfg.frameCount > 1) {
    uint8_t sequence = cfg.frameCount * 2 - 2;
    uint8_t q = rawFrame % sequence;
    frame = q < cfg.frameCount ? q : sequence - q;
  } else {
    frame = rawFrame % cfg.frameCount;
  }

  float p = duration > 0 ? min(1.0f, (float)elapsed / (float)duration) : 0.0f;
  if (cfg.motionMode == CUSTOM_MOTION_HORIZONTAL) {
    x = (int16_t)roundf(cfg.spriteX + (cfg.endX - cfg.spriteX) * p);
  } else if (cfg.motionMode == CUSTOM_MOTION_VERTICAL) {
    y = (int16_t)roundf(cfg.spriteY + (cfg.endY - cfg.spriteY) * p);
  } else if (cfg.motionMode == CUSTOM_MOTION_PINGPONG) {
    float q = p <= 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
    x = (int16_t)roundf(cfg.spriteX + (cfg.endX - cfg.spriteX) * q);
    y = (int16_t)roundf(cfg.spriteY + (cfg.endY - cfg.spriteY) * q);
  } else if (cfg.motionMode == CUSTOM_MOTION_JUMP) {
    x = (int16_t)roundf(cfg.spriteX + (cfg.endX - cfg.spriteX) * p);
    y = (int16_t)roundf(cfg.spriteY + (cfg.endY - cfg.spriteY) * p - cfg.jumpHeight * 4.0f * p * (1.0f - p));
  }
}

void drawCustomClockFace(const struct tm &t) {
  if (!storageReady) {
    drawMediaUnavailable("NO FS", "LittleFS");
    return;
  }
  if (customClockSlot >= CUSTOM_CLOCK_SLOTS) customClockSlot = 0;
  CustomClockConfig &cfg = customClockConfigs[customClockSlot];
  if (loadedCustomBackgroundSlot != customClockSlot) loadCustomBackground(customClockSlot);
  drawRgb565Buffer(customBackgroundPixels);
  drawCustomBuiltInEffect(cfg);

  uint8_t frame = 0;
  int16_t spriteX = cfg.spriteX;
  int16_t spriteY = cfg.spriteY;
  customAnimationValues(t, frame, spriteX, spriteY);
  if (cfg.spriteEnabled && !cfg.spriteFront && cfg.frameCount > 0) {
    drawCustomSprite(customClockSlot, frame, spriteX, spriteY);
  }

  char timeBuf[16];
  if (cfg.timeFormat == 1) snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else if (cfg.timeFormat == 2) snprintf(timeBuf, sizeof(timeBuf), "%02d %02d", t.tm_hour, t.tm_min);
  else snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.tm_hour, t.tm_min);
  if (cfg.blinkColon && (t.tm_sec & 1)) {
    for (char *p = timeBuf; *p; p++) if (*p == ':') *p = ' ';
  }
  uint8_t timeScale = cfg.timeFormat == 1 ? 1 : cfg.timeSize;
  drawCustomPixelText(String(timeBuf), cfg.timeX, cfg.timeY, cfg.timeColor, timeScale, cfg.timeFont);

  if (cfg.showDate) {
    char dateBuf[16];
    if (cfg.dateFormat == 1) snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    else if (cfg.dateFormat == 2) snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d", t.tm_mon + 1, t.tm_mday);
    else snprintf(dateBuf, sizeof(dateBuf), "%02d-%02d", t.tm_mon + 1, t.tm_mday);
    drawCustomPixelText(String(dateBuf), cfg.dateX, cfg.dateY, cfg.dateColor, 1, CUSTOM_FONT_BLOCK_5X7);
  }
  if (cfg.showWeekday) {
    static const char *weekdays[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    drawCustomPixelText(String(weekdays[constrain(t.tm_wday, 0, 6)]), cfg.weekdayX, cfg.weekdayY, cfg.weekdayColor, 1, CUSTOM_FONT_BLOCK_5X7);
  }
  if (cfg.spriteEnabled && cfg.spriteFront && cfg.frameCount > 0) {
    drawCustomSprite(customClockSlot, frame, spriteX, spriteY);
  }
}

void drawNoTime() {
  display->setFont(NULL);
  display->fillScreen(0);
  display->drawRect(0, 0, 64, 64, C(120, 70, 0));
  centerText("NO TIME", 16, C(255, 200, 80), 1);
  if (WiFi.status() == WL_CONNECTED) {
    centerText("NTP FAIL", 32, C(255, 80, 80), 1);
    centerText(WiFi.localIP().toString(), 48, C(120, 220, 255), 1);
  } else {
    centerText("NO WIFI", 32, C(255, 80, 80), 1);
  }
}

void drawApScreen() {
  display->setFont(NULL);
  display->fillScreen(0);
  display->drawRect(0, 0, 64, 64, C(0, 120, 180));
  centerText("SETUP AP", 7, C(255, 220, 80), 1);
  centerText(AP_SSID, 21, C(255, 255, 255), 1);
  centerText("PWD:12345678", 35, C(0, 255, 140), 1);
  centerText("192.168.4.1", 49, C(120, 200, 255), 1);
  commitFrame();
}



String htmlPage() {
  const String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  const String mode = apMode ? "配置热点" : "正常联网";
  const String credit = IntegrityGuard::validatedCredit();

  String html;
  html.reserve(6900);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32-S3 像素时钟</title>";
  html += "<style>*{box-sizing:border-box}body{font-family:Arial,'Microsoft YaHei',sans-serif;background:#0b141d;color:#eef;margin:0;padding:10px;font-size:14px}";
  html += ".card{max-width:760px;margin:auto;background:#142331;border:1px solid #29475d;border-radius:11px;padding:13px;box-shadow:0 6px 18px #0005}h1{font-size:19px;margin:0 0 4px}.credit{color:#73e2a7;font-size:13px;margin:0 0 7px}.row{margin:7px 0}label{display:block;margin-bottom:3px;color:#a9e7d5;font-size:12px}input,select{width:100%;padding:7px 8px;min-height:34px;border-radius:7px;border:1px solid #3e6076;background:#08131c;color:#fff}button,.btn{display:inline-flex;align-items:center;justify-content:center;min-height:35px;padding:7px 11px;border:0;border-radius:7px;background:#07856f;color:#fff;font-weight:bold;text-decoration:none;cursor:pointer}.secondary{background:#3d566a}.danger{background:#9c3f43}.warn{color:#ffd166}.muted{color:#9ab;font-size:12px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}.actions{display:flex;gap:7px;flex-wrap:wrap;align-items:center;margin-top:9px}.actions form{margin:0}.links{display:flex;gap:7px;flex-wrap:wrap;margin:8px 0}a{color:#7dd3fc}@media(max-width:600px){body{padding:5px}.card{padding:9px}.grid{grid-template-columns:1fr 1fr;gap:6px}.row{margin:5px 0}.actions{display:grid;grid-template-columns:1fr 1fr}.actions button{width:100%}.links{font-size:12px}}</style>";
  html += "</head><body><div class='card'>";
  html += "<h1>ESP32-S3 像素时钟</h1>";
  html += "<p class='credit'>" + htmlEscape(credit) + " · " + ProjectIdentity::version() + "</p>";
  html += "<p class='muted'>状态：" + mode + " ｜ IP：" + ip + "</p>";
  if (apMode) html += "<p class='warn'>已进入配网模式。保存 Wi-Fi 后设备会自动尝试联网。</p>";
  html += "<div class='links'><a class='btn secondary' href='/studio'>打开媒体工作室</a><a href='/nextface'>切换显示模式</a></div>";
  html += "<form method='POST' action='/save'>";
  html += "<div class='row'><label>Wi-Fi 名称</label><input name='ssid' value='" + htmlEscape(wifiSsid) + "'></div>";
  html += "<div class='row'><label>Wi-Fi 密码，留空则保持原密码</label><input name='pass' type='password'></div>";
  html += "<div class='grid'>";
  html += "<div class='row'><label>亮度 0-255</label><input name='bright' type='number' min='0' max='255' value='" + String(brightness) + "'></div>";
  html += "<div class='row'><label>屏幕旋转 0-3</label><input name='rot' type='number' min='0' max='3' value='" + String(rotation) + "'></div>";
  html += "</div>";
  html += "<div class='row'><label>显示模式</label><select name='face'>";
  const char *names[FACE_COUNT] = {"数字时钟", "模拟指针", "彩点动态", "Mario 像素风", "Pokemon 图鉴", "图片轮播", "像素画板", "自定义时钟"};
  for (uint8_t i = 0; i < FACE_COUNT; i++) {
    html += "<option value='" + String(i) + "' " + (faceMode == i ? "selected" : "") + ">" + names[i] + "</option>";
  }
  html += "</select></div>";
  html += "<div class='row'><label>图片轮播间隔，毫秒</label><input name='slideMs' type='number' min='200' max='60000' value='" + String(carouselIntervalMs) + "'></div>";
  html += "<div class='row'><label>时区</label><input name='tz' value='" + htmlEscape(tzInfo) + "'></div>";
  html += "<div class='row'><label>NTP 服务器</label><input name='ntp' value='" + htmlEscape(ntpServer) + "'></div>";
  html += "<div class='actions'><button type='submit'>保存设置</button></form>";
  html += "<form method='POST' action='/reboot' onsubmit=\"return confirm('确认重启设备？')\"><button type='submit' class='secondary'>重启设备</button></form></div>";
  html += "<div class='links'><a class='btn danger' href='/resetwifi' onclick=\"return confirm('确认清除 Wi-Fi 并重启？')\">清除 Wi-Fi</a></div>";
  html += "<p class='muted'>GPIO14 按键：按一下息屏，再按一下唤醒。</p>";
  if (!IntegrityGuard::identityValid()) html += "<p class='warn'>构建身份校验异常，已使用内置备用标识。</p>";
  html += "</div></body></html>";
  return html;
}

void handleStudio() {
  static const char marker[] = "{{PROJECT_CREDIT}}";
  const char *position = strstr(STUDIO_HTML, marker);
  if (!position) {
    server.send_P(200, "text/html; charset=utf-8", STUDIO_HTML);
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  server.sendContent_P(STUDIO_HTML, position - STUDIO_HTML);
  server.sendContent(IntegrityGuard::validatedCredit());
  server.sendContent_P(position + strlen(marker));
  server.sendContent("");
}

void sendRgb565Binary(const uint16_t *pixels) {
  // 分段发送二进制数据。
  server.setContentLength(RGB565_FILE_BYTES);
  server.send(200, "application/octet-stream", "");
  uint8_t chunk[128];
  size_t used = 0;
  for (size_t i = 0; i < PIXEL_COUNT; i++) {
    uint16_t c = pixels[i];
    chunk[used++] = (uint8_t)(c >> 8);
    chunk[used++] = (uint8_t)c;
    if (used == sizeof(chunk)) {
      server.sendContent((const char *)chunk, used);
      used = 0;
    }
  }
  if (used) server.sendContent((const char *)chunk, used);
}

void handleApiStatus() {
  String json;
  json.reserve(180);
  json = "{\"face\":" + String(faceMode) + ",\"speed\":" + String(carouselIntervalMs) + ",\"count\":" + String(slideCount) + ",\"sleep\":" + String(screenSleeping ? "true" : "false") + ",\"storage\":" + String(storageReady ? "true" : "false") + ",\"customSlot\":" + String(customClockSlot) + ",\"identity\":" + String(IntegrityGuard::identityValid() ? "true" : "false") + ",\"slots\":[";
  for (uint8_t i = 0; i < MAX_SLIDES; i++) {
    if (i) json += ',';
    json += slideExists[i] ? "true" : "false";
  }
  json += "]}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleApiAbout() {
  const String credit = IntegrityGuard::validatedCredit();
  String json;
  json.reserve(180);
  json = "{\"product\":\"" + String(ProjectIdentity::productId()) + "\",\"version\":\"" + String(ProjectIdentity::version()) + "\",\"build\":\"" + String(ProjectIdentity::buildId()) + "\",\"credit\":\"" + jsonEscape(credit) + "\",\"integrity\":" + String(IntegrityGuard::identityValid() ? "true" : "false") + "}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleApiSlide() {
  int slot = server.arg("slot").toInt();
  if (!storageReady || slot < 0 || slot >= MAX_SLIDES || !slideExists[slot]) {
    server.send(404, "text/plain; charset=utf-8", "图片不存在");
    return;
  }
  File f = LittleFS.open(slidePath(slot), "r");
  if (!f) {
    server.send(500, "text/plain; charset=utf-8", "图片读取失败");
    return;
  }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void handleApiDrawing() {
  sendRgb565Binary(drawingPixels);
}

void handleImageUploadData() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    imageUploadOk = false;
    imageUploadOverflow = false;
    imageUploadStarted = false;
    imageUploadMessage = "上传初始化失败";
    imageUploadBytes = 0;
    imageUploadPath = "";
    imageUploadSlot = server.arg("slot").toInt();
    if (!storageReady) {
      imageUploadMessage = "LittleFS 未就绪";
      return;
    }
    if (imageUploadSlot < 0 || imageUploadSlot >= MAX_SLIDES) {
      imageUploadMessage = "图片位置无效";
      return;
    }
    imageUploadPath = slidePath((uint8_t)imageUploadSlot);
    imageUploadFile = LittleFS.open(imageUploadPath, "w");
    if (!imageUploadFile) {
      imageUploadMessage = "无法创建图片文件";
      return;
    }
    imageUploadStarted = true;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!imageUploadStarted || !imageUploadFile || imageUploadOverflow) return;
    if (imageUploadBytes + upload.currentSize > RGB565_FILE_BYTES) {
      imageUploadOverflow = true;
      imageUploadMessage = "图片数据超过 8192 字节";
      return;
    }
    const size_t written = imageUploadFile.write(upload.buf, upload.currentSize);
    imageUploadBytes += written;
    if (written != upload.currentSize) {
      imageUploadOverflow = true;
      imageUploadMessage = "图片数据写入失败";
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (imageUploadFile) imageUploadFile.close();
    if (!imageUploadStarted) {
      if (imageUploadPath.length()) LittleFS.remove(imageUploadPath);
      scanSlides();
      return;
    }
    imageUploadOk = !imageUploadOverflow && imageUploadBytes == RGB565_FILE_BYTES;
    if (imageUploadOk) {
      imageUploadMessage = "图片上传成功";
      scanSlides();
      currentCarouselSlot = 255;
      lastCarouselChangeMs = 0;
    } else {
      if (imageUploadPath.length()) LittleFS.remove(imageUploadPath);
      imageUploadMessage = String("图片数据长度错误，应为 8192 字节，实际为 ") + imageUploadBytes;
      scanSlides();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (imageUploadFile) imageUploadFile.close();
    if (imageUploadPath.length()) LittleFS.remove(imageUploadPath);
    imageUploadMessage = "上传已中止";
    scanSlides();
  }
}

void handleImageUploadDone() {
  server.send(imageUploadOk ? 200 : 400, "text/plain; charset=utf-8", imageUploadMessage);
}

void handleApiDelete() {
  int slot = server.arg("slot").toInt();
  if (!storageReady || slot < 0 || slot >= MAX_SLIDES) {
    server.send(400, "text/plain; charset=utf-8", "图片位置无效");
    return;
  }
  bool removed = LittleFS.remove(slidePath((uint8_t)slot));
  scanSlides();
  currentCarouselSlot = 255;
  lastCarouselChangeMs = 0;
  server.send(removed ? 200 : 404, "text/plain; charset=utf-8", removed ? "图片已删除" : "该位置没有图片");
}

void handleApiCarousel() {
  uint32_t speed = (uint32_t)server.arg("speed").toInt();
  carouselIntervalMs = constrain(speed, (uint32_t)200, (uint32_t)60000);
  prefs.putUInt("slideMs", carouselIntervalMs);
  prefs.putUChar("ccSlot", customClockSlot);
  activateFace(FACE_CAROUSEL, true);
  server.send(200, "text/plain; charset=utf-8", String("轮播已启动，每张停留 ") + carouselIntervalMs + " 毫秒");
}

void handleApiShow() {
  int mode = server.arg("face").toInt();
  if (mode < 0 || mode >= FACE_COUNT) {
    server.send(400, "text/plain; charset=utf-8", "显示模式无效");
    return;
  }
  activateFace((uint8_t)mode, true);
  server.send(200, "text/plain; charset=utf-8", "显示模式已切换");
}

void handleApiPixels() {
  String data = server.arg("d");
  bool live = server.arg("live") == "1";
  int applied = 0;
  int start = 0;
  while (start < data.length()) {
    int end = data.indexOf(';', start);
    if (end < 0) end = data.length();
    String item = data.substring(start, end);
    int c1 = item.indexOf(',');
    int c2 = item.indexOf(',', c1 + 1);
    if (c1 > 0 && c2 > c1) {
      int x = item.substring(0, c1).toInt();
      int y = item.substring(c1 + 1, c2).toInt();
      String hex = item.substring(c2 + 1);
      uint16_t color = (uint16_t)strtoul(hex.c_str(), nullptr, 16);
      if (x >= 0 && x < PANEL_RES_X && y >= 0 && y < PANEL_RES_Y) {
        drawingPixels[y * PANEL_RES_X + x] = color;
        applied++;
      }
    }
    start = end + 1;
  }
  if (live && applied > 0) {
    activateFace(FACE_DRAWING, false);
    forceMediaRedraw();
  }
  server.send(200, "text/plain", "OK");
}

void handleApiDrawingClear() {
  memset(drawingPixels, 0, sizeof(drawingPixels));
  if (server.arg("live") == "1") activateFace(FACE_DRAWING, false);
  forceMediaRedraw();
  server.send(200, "text/plain; charset=utf-8", "画板已清空");
}

void handleApiDrawingSave() {
  if (!saveRgb565File("/drawing.rgb", drawingPixels)) {
    server.send(500, "text/plain; charset=utf-8", "保存失败，请检查 LittleFS");
    return;
  }
  activateFace(FACE_DRAWING, true);
  server.send(200, "text/plain; charset=utf-8", "画板已保存到 LittleFS，并切换为画板显示");
}


void handleDrawingFrameUploadData() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    drawingFrameUploadBytes = 0;
    drawingFrameUploadOk = false;
    drawingFrameUploadOverflow = false;
    drawingFrameHasHighByte = false;
    drawingFrameUploadLive = server.arg("live") == "1";
    drawingFrameUploadMessage = "整帧接收中";
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (drawingFrameUploadOverflow) return;
    if (drawingFrameUploadBytes + upload.currentSize > RGB565_FILE_BYTES) {
      drawingFrameUploadOverflow = true;
      drawingFrameUploadMessage = "画板数据超过 8192 字节";
      return;
    }
    for (size_t i = 0; i < upload.currentSize; i++) {
      const uint8_t value = upload.buf[i];
      if (!drawingFrameHasHighByte) {
        drawingFrameHighByte = value;
        drawingFrameHasHighByte = true;
      } else {
        const size_t pixel = drawingFrameUploadBytes / 2;
        drawingPixels[pixel] = ((uint16_t)drawingFrameHighByte << 8) | value;
        drawingFrameHasHighByte = false;
      }
      drawingFrameUploadBytes++;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    drawingFrameUploadOk = !drawingFrameUploadOverflow && drawingFrameUploadBytes == RGB565_FILE_BYTES && !drawingFrameHasHighByte;
    drawingFrameUploadMessage = drawingFrameUploadOk ? "画板整帧已更新" : String("画板数据长度错误：") + drawingFrameUploadBytes;
    if (drawingFrameUploadOk && drawingFrameUploadLive) activateFace(FACE_DRAWING, false);
    forceMediaRedraw();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    drawingFrameUploadMessage = "画板上传已中止";
  }
}

void handleDrawingFrameUploadDone() {
  server.send(drawingFrameUploadOk ? 200 : 400, "text/plain; charset=utf-8", drawingFrameUploadMessage);
}

void handleApiCustomConfigGet() {
  int slot = server.arg("slot").toInt();
  if (slot < 0 || slot >= CUSTOM_CLOCK_SLOTS) {
    server.send(400, "application/json", "{\"error\":\"slot\"}");
    return;
  }
  CustomClockConfig &c = customClockConfigs[slot];
  String json = "{";
  json += "\"slot\":" + String(slot);
  json += ",\"background\":" + String(customBackgroundAvailable[slot] ? "true" : "false");
  json += ",\"timeFormat\":" + String(c.timeFormat);
  json += ",\"timeSize\":" + String(c.timeSize);
  json += ",\"timeFont\":" + String(c.timeFont);
  json += ",\"timeX\":" + String(c.timeX);
  json += ",\"timeY\":" + String(c.timeY);
  json += ",\"timeColor\":" + String(c.timeColor);
  json += ",\"blinkColon\":" + String(c.blinkColon);
  json += ",\"showDate\":" + String(c.showDate);
  json += ",\"dateFormat\":" + String(c.dateFormat);
  json += ",\"dateX\":" + String(c.dateX);
  json += ",\"dateY\":" + String(c.dateY);
  json += ",\"dateColor\":" + String(c.dateColor);
  json += ",\"showWeekday\":" + String(c.showWeekday);
  json += ",\"weekdayX\":" + String(c.weekdayX);
  json += ",\"weekdayY\":" + String(c.weekdayY);
  json += ",\"weekdayColor\":" + String(c.weekdayColor);
  json += ",\"spriteEnabled\":" + String(c.spriteEnabled);
  json += ",\"spriteFront\":" + String(c.spriteFront);
  json += ",\"animationMode\":" + String(c.animationMode);
  json += ",\"motionMode\":" + String(c.motionMode);
  json += ",\"frameCount\":" + String(c.frameCount);
  json += ",\"spriteW\":" + String(c.spriteW);
  json += ",\"spriteH\":" + String(c.spriteH);
  json += ",\"spriteX\":" + String(c.spriteX);
  json += ",\"spriteY\":" + String(c.spriteY);
  json += ",\"endX\":" + String(c.endX);
  json += ",\"endY\":" + String(c.endY);
  json += ",\"jumpHeight\":" + String(c.jumpHeight);
  json += ",\"frameIntervalMs\":" + String(c.frameIntervalMs);
  json += ",\"durationMs\":" + String(c.durationMs);
  json += ",\"framePingPong\":" + String(c.framePingPong);
  json += ",\"transparentColor\":" + String(c.transparentColor);
  json += ",\"effectMode\":" + String(c.effectMode);
  json += ",\"effectIntensity\":" + String(c.effectIntensity);
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

uint16_t parseRgb565Arg(const String &name, uint16_t fallback) {
  if (!server.hasArg(name)) return fallback;
  return (uint16_t)strtoul(server.arg(name).c_str(), nullptr, 10);
}

void handleApiCustomConfigPost() {
  int slot = server.arg("slot").toInt();
  if (slot < 0 || slot >= CUSTOM_CLOCK_SLOTS) {
    server.send(400, "text/plain; charset=utf-8", "表盘位置无效");
    return;
  }
  CustomClockConfig &c = customClockConfigs[slot];
  c.timeFormat = constrain(server.arg("timeFormat").toInt(), 0, 2);
  c.timeSize = constrain(server.arg("timeSize").toInt(), 1, 2);
  c.timeFont = constrain(server.arg("timeFont").toInt(), 0, 1);
  c.timeX = constrain(server.arg("timeX").toInt(), -1, 63);
  c.timeY = constrain(server.arg("timeY").toInt(), -16, 63);
  c.timeColor = parseRgb565Arg("timeColor", c.timeColor);
  c.blinkColon = server.arg("blinkColon") == "1";
  c.showDate = server.arg("showDate") == "1";
  c.dateFormat = constrain(server.arg("dateFormat").toInt(), 0, 2);
  c.dateX = constrain(server.arg("dateX").toInt(), -1, 63);
  c.dateY = constrain(server.arg("dateY").toInt(), -8, 63);
  c.dateColor = parseRgb565Arg("dateColor", c.dateColor);
  c.showWeekday = server.arg("showWeekday") == "1";
  c.weekdayX = constrain(server.arg("weekdayX").toInt(), -1, 63);
  c.weekdayY = constrain(server.arg("weekdayY").toInt(), -8, 63);
  c.weekdayColor = parseRgb565Arg("weekdayColor", c.weekdayColor);
  c.spriteEnabled = server.arg("spriteEnabled") == "1";
  c.spriteFront = server.arg("spriteFront") == "1";
  c.animationMode = constrain(server.arg("animationMode").toInt(), 0, 4);
  c.motionMode = constrain(server.arg("motionMode").toInt(), 0, 4);
  c.spriteW = constrain(server.arg("spriteW").toInt(), 1, CUSTOM_MAX_SPRITE_W);
  c.spriteH = constrain(server.arg("spriteH").toInt(), 1, CUSTOM_MAX_SPRITE_H);
  c.spriteX = constrain(server.arg("spriteX").toInt(), -31, 63);
  c.spriteY = constrain(server.arg("spriteY").toInt(), -31, 63);
  c.endX = constrain(server.arg("endX").toInt(), -31, 63);
  c.endY = constrain(server.arg("endY").toInt(), -31, 63);
  c.jumpHeight = constrain(server.arg("jumpHeight").toInt(), 0, 63);
  c.frameIntervalMs = constrain(server.arg("frameIntervalMs").toInt(), 40, 2000);
  c.durationMs = constrain(server.arg("durationMs").toInt(), 200, 60000);
  c.framePingPong = server.arg("framePingPong") == "1";
  c.transparentColor = parseRgb565Arg("transparentColor", CUSTOM_TRANSPARENT_DEFAULT);
  c.effectMode = constrain(server.arg("effectMode").toInt(), 0, 3);
  c.effectIntensity = constrain(server.arg("effectIntensity").toInt(), 10, 100);
  scanCustomFrames(slot);
  bool saveNow = server.arg("save") == "1";
  bool showNow = server.arg("show") == "1";
  if (saveNow && !saveCustomClockConfig(slot)) {
    server.send(500, "text/plain; charset=utf-8", "配置保存失败");
    return;
  }
  if (showNow) {
    customClockSlot = slot;
    prefs.putUChar("ccSlot", customClockSlot);
    activateFace(FACE_CUSTOM_CLOCK, true);
  }
  loadedCustomBackgroundSlot = 255;
  resetCustomAnimationState();
  server.send(200, "text/plain; charset=utf-8", saveNow ? "自定义时钟已保存" : "配置已应用到内存");
}

void sendFileBinary(const String &path, size_t expected) {
  File f = LittleFS.open(path, "r");
  if (!f || (expected && f.size() != expected)) {
    if (f) f.close();
    server.send(404, "text/plain; charset=utf-8", "文件不存在");
    return;
  }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void handleApiCustomBackgroundGet() {
  int slot = server.arg("slot").toInt();
  if (slot < 0 || slot >= CUSTOM_CLOCK_SLOTS || !customBackgroundAvailable[slot]) {
    server.send(404, "text/plain; charset=utf-8", "背景不存在");
    return;
  }
  sendFileBinary(customBackgroundPath(slot), RGB565_FILE_BYTES);
}

void handleApiCustomSpriteGet() {
  int slot = server.arg("slot").toInt();
  int frame = server.arg("frame").toInt();
  if (slot < 0 || slot >= CUSTOM_CLOCK_SLOTS || frame < 0 || frame >= customClockConfigs[slot].frameCount) {
    server.send(404, "text/plain; charset=utf-8", "精灵帧不存在");
    return;
  }
  size_t bytes = (size_t)customClockConfigs[slot].spriteW * customClockConfigs[slot].spriteH * 2;
  sendFileBinary(customSpritePath(slot, frame), bytes);
}

void handleCustomBackgroundUploadData() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    customUploadSlot = server.arg("slot").toInt();
    customUploadFrame = -1;
    customUploadBytes = 0;
    customUploadExpectedBytes = RGB565_FILE_BYTES;
    customUploadOk = false;
    customUploadOverflow = false;
    customUploadStarted = false;
    customUploadPath = "";
    customUploadMessage = "背景上传初始化失败";
    if (!storageReady) {
      customUploadMessage = "LittleFS 未就绪";
      return;
    }
    if (customUploadSlot < 0 || customUploadSlot >= CUSTOM_CLOCK_SLOTS) {
      customUploadMessage = "自定义表盘位置无效";
      return;
    }
    customUploadPath = customBackgroundPath(customUploadSlot);
    customUploadFile = LittleFS.open(customUploadPath, "w");
    if (!customUploadFile) {
      customUploadMessage = "无法创建背景文件";
      return;
    }
    customUploadStarted = true;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!customUploadStarted || !customUploadFile || customUploadOverflow) return;
    if (customUploadBytes + upload.currentSize > customUploadExpectedBytes) {
      customUploadOverflow = true;
      customUploadMessage = "背景数据过长";
      return;
    }
    const size_t written = customUploadFile.write(upload.buf, upload.currentSize);
    customUploadBytes += written;
    if (written != upload.currentSize) customUploadOverflow = true;
  } else if (upload.status == UPLOAD_FILE_END) {
    if (customUploadFile) customUploadFile.close();
    if (!customUploadStarted) {
      if (customUploadPath.length()) LittleFS.remove(customUploadPath);
      return;
    }
    customUploadOk = !customUploadOverflow && customUploadBytes == customUploadExpectedBytes;
    if (customUploadOk) {
      customBackgroundAvailable[customUploadSlot] = true;
      loadedCustomBackgroundSlot = 255;
      customUploadMessage = "自定义时钟背景上传成功";
    } else {
      if (customUploadPath.length()) LittleFS.remove(customUploadPath);
      customUploadMessage = String("背景长度错误：") + customUploadBytes;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (customUploadFile) customUploadFile.close();
    if (customUploadPath.length()) LittleFS.remove(customUploadPath);
    customUploadMessage = "背景上传已中止";
  }
}

void handleCustomUploadDone() {
  server.send(customUploadOk ? 200 : 400, "text/plain; charset=utf-8", customUploadMessage);
}

void handleCustomSpriteUploadData() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    customUploadSlot = server.arg("slot").toInt();
    customUploadFrame = server.arg("frame").toInt();
    const int w = constrain(server.arg("w").toInt(), 1, CUSTOM_MAX_SPRITE_W);
    const int h = constrain(server.arg("h").toInt(), 1, CUSTOM_MAX_SPRITE_H);
    customUploadBytes = 0;
    customUploadExpectedBytes = (size_t)w * h * 2;
    customUploadOk = false;
    customUploadOverflow = false;
    customUploadStarted = false;
    customUploadPath = "";
    customUploadMessage = "精灵上传初始化失败";
    if (!storageReady) {
      customUploadMessage = "LittleFS 未就绪";
      return;
    }
    if (customUploadSlot < 0 || customUploadSlot >= CUSTOM_CLOCK_SLOTS || customUploadFrame < 0 || customUploadFrame >= CUSTOM_MAX_FRAMES) {
      customUploadMessage = "精灵帧位置无效";
      return;
    }
    customClockConfigs[customUploadSlot].spriteW = w;
    customClockConfigs[customUploadSlot].spriteH = h;
    customUploadPath = customSpritePath(customUploadSlot, customUploadFrame);
    customUploadFile = LittleFS.open(customUploadPath, "w");
    if (!customUploadFile) {
      customUploadMessage = "无法创建精灵文件";
      return;
    }
    customUploadStarted = true;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!customUploadStarted || !customUploadFile || customUploadOverflow) return;
    if (customUploadBytes + upload.currentSize > customUploadExpectedBytes) {
      customUploadOverflow = true;
      customUploadMessage = "精灵数据过长";
      return;
    }
    const size_t written = customUploadFile.write(upload.buf, upload.currentSize);
    customUploadBytes += written;
    if (written != upload.currentSize) customUploadOverflow = true;
  } else if (upload.status == UPLOAD_FILE_END) {
    if (customUploadFile) customUploadFile.close();
    if (!customUploadStarted) {
      if (customUploadPath.length()) LittleFS.remove(customUploadPath);
      return;
    }
    customUploadOk = !customUploadOverflow && customUploadBytes == customUploadExpectedBytes;
    if (customUploadOk) {
      scanCustomFrames(customUploadSlot);
      saveCustomClockConfig(customUploadSlot);
      loadedCustomSpriteSlot = 255;
      customUploadMessage = "精灵帧上传成功";
    } else {
      if (customUploadPath.length()) LittleFS.remove(customUploadPath);
      customUploadMessage = String("精灵长度错误：") + customUploadBytes;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (customUploadFile) customUploadFile.close();
    if (customUploadPath.length()) LittleFS.remove(customUploadPath);
    customUploadMessage = "精灵上传已中止";
  }
}

void handleApiCustomClearSprites() {
  int slot = server.arg("slot").toInt();
  if (slot < 0 || slot >= CUSTOM_CLOCK_SLOTS) {
    server.send(400, "text/plain; charset=utf-8", "表盘位置无效");
    return;
  }
  for (uint8_t i = 0; i < CUSTOM_MAX_FRAMES; i++) LittleFS.remove(customSpritePath(slot, i));
  customClockConfigs[slot].frameCount = 0;
  customClockConfigs[slot].spriteEnabled = 0;
  saveCustomClockConfig(slot);
  loadedCustomSpriteSlot = 255;
  resetCustomAnimationState();
  server.send(200, "text/plain; charset=utf-8", "精灵动画已清空");
}

void handleApiCustomClearBackground() {
  int slot = server.arg("slot").toInt();
  if (slot < 0 || slot >= CUSTOM_CLOCK_SLOTS) {
    server.send(400, "text/plain; charset=utf-8", "表盘位置无效");
    return;
  }
  bool removed = LittleFS.remove(customBackgroundPath(slot));
  customBackgroundAvailable[slot] = false;
  loadedCustomBackgroundSlot = 255;
  server.send(200, "text/plain; charset=utf-8", removed ? "背景已清空" : "背景原本为空");
}

void handleApiCustomUseDrawing() {
  int slot = server.arg("slot").toInt();
  if (slot < 0 || slot >= CUSTOM_CLOCK_SLOTS || !saveRgb565File(customBackgroundPath(slot), drawingPixels)) {
    server.send(500, "text/plain; charset=utf-8", "复制画板背景失败");
    return;
  }
  customBackgroundAvailable[slot] = true;
  loadedCustomBackgroundSlot = 255;
  server.send(200, "text/plain; charset=utf-8", "当前画板已复制为时钟背景");
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

void handleSave() {
  const String oldSsid = wifiSsid;
  const String oldPass = wifiPass;
  const String oldTz = tzInfo;
  const String oldNtp = ntpServer;
  const uint8_t oldFace = faceMode;
  const uint32_t oldSlideMs = carouselIntervalMs;

  if (server.hasArg("ssid")) wifiSsid = server.arg("ssid");
  if (server.hasArg("pass") && server.arg("pass").length() > 0) wifiPass = server.arg("pass");
  if (server.hasArg("tz")) tzInfo = server.arg("tz");
  if (server.hasArg("ntp")) ntpServer = server.arg("ntp");
  if (server.hasArg("bright")) brightness = (uint8_t)constrain(server.arg("bright").toInt(), 0, 255);
  if (server.hasArg("rot")) rotation = (uint8_t)constrain(server.arg("rot").toInt(), 0, 3);
  uint8_t requestedFace = faceMode;
  if (server.hasArg("face")) requestedFace = (uint8_t)constrain(server.arg("face").toInt(), 0, FACE_COUNT - 1);
  if (server.hasArg("slideMs")) carouselIntervalMs = constrain((uint32_t)server.arg("slideMs").toInt(), (uint32_t)200, (uint32_t)60000);

  const bool wifiChanged = (wifiSsid != oldSsid) || (wifiPass != oldPass);
  const bool timeChanged = (tzInfo != oldTz) || (ntpServer != oldNtp);

  if (requestedFace != oldFace) activateFace(requestedFace, false);
  saveSettings();
  applyDisplaySettingsIfNeeded();

  // 修改轮播速度后重新计时。
  if (carouselIntervalMs != oldSlideMs) lastCarouselChangeMs = millis();
  forceMediaRedraw();

  String message = "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  message += "<style>body{font-family:sans-serif;background:#111827;color:#f3f4f6;padding:22px;line-height:1.7}a{color:#67e8f9}</style>";
  message += "<h3>设置已实时应用</h3><p>设备不会重启。</p>";
  if (wifiChanged) {
    message += "<p>WiFi 参数已保存，设备将重新连接网络。当前网页可能暂时断开，IP 地址也可能改变。</p>";
  } else if (timeChanged) {
    message += "<p>时区/NTP 设置将在后台立即重新同步。</p>";
  }
  message += "<p><a href='/'>返回设备设置</a> ｜ <a href='/studio'>打开媒体工作室</a></p>";
  server.send(200, "text/html; charset=utf-8", message);

  if (wifiChanged) {
    wifiReconnectPending = true;
    wifiReconnectAtMs = millis() + 900;
    timeReconfigurePending = false;
  } else if (timeChanged && WiFi.status() == WL_CONNECTED) {
    timeReconfigurePending = true;
    timeReconfigureAtMs = millis() + 300;
  }
}

void handleNextFace() {
  activateFace((faceMode + 1) % FACE_COUNT, true);
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleReboot() {
  server.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{font-family:sans-serif;background:#111827;color:#f3f4f6;padding:20px}</style><p>设备即将重启，WiFi 和全部设置均会保留。</p>");
  showStatus("Restart", "Rebooting", C(80, 190, 255));
  rebootAtMs = millis() + 1200;
}

void handleResetWiFi() {
  prefs.putString("ssid", "");
  prefs.putString("pass", "");
  server.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'><p>WiFi 已清除，设备即将重启并进入配置热点...</p>");
  showStatus("WiFi Clear", "Restarting", C(255, 180, 0));
  rebootAtMs = millis() + 1200;
}

void redirectToSetupPage() {
  server.sendHeader("Location", "http://" + SETUP_AP_IP.toString() + "/", true);
  server.send(302, "text/plain", "");
}

void handlePortalProbe() {
  if (apMode) redirectToSetupPage();
  else server.send(204, "text/plain", "");
}

void handleNotFound() {
  if (apMode) redirectToSetupPage();
  else server.send(404, "text/plain; charset=utf-8", "页面不存在");
}

void startCaptivePortal() {
  dnsServer.stop();
  captivePortalActive = dnsServer.start(DNS_PORT, "*", SETUP_AP_IP);
  Serial.println(captivePortalActive ? "[OK] Captive portal started" : "[WARN] Captive portal DNS failed");
}

void stopCaptivePortal() {
  if (captivePortalActive) dnsServer.stop();
  captivePortalActive = false;
}

void startWebServer() {
  if (webServerStarted) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/studio", HTTP_GET, handleStudio);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/nextface", HTTP_GET, handleNextFace);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/resetwifi", HTTP_GET, handleResetWiFi);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/about", HTTP_GET, handleApiAbout);
  server.on("/api/slide", HTTP_GET, handleApiSlide);
  server.on("/api/drawing", HTTP_GET, handleApiDrawing);
  server.on("/api/image", HTTP_POST, handleImageUploadDone, handleImageUploadData);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/carousel", HTTP_POST, handleApiCarousel);
  server.on("/api/show", HTTP_POST, handleApiShow);
  server.on("/api/pixels", HTTP_POST, handleApiPixels);
  server.on("/api/drawing/clear", HTTP_POST, handleApiDrawingClear);
  server.on("/api/drawing/save", HTTP_POST, handleApiDrawingSave);
  server.on("/api/drawing/frame", HTTP_POST, handleDrawingFrameUploadDone, handleDrawingFrameUploadData);
  server.on("/api/custom/config", HTTP_GET, handleApiCustomConfigGet);
  server.on("/api/custom/config", HTTP_POST, handleApiCustomConfigPost);
  server.on("/api/custom/bg", HTTP_GET, handleApiCustomBackgroundGet);
  server.on("/api/custom/bg", HTTP_POST, handleCustomUploadDone, handleCustomBackgroundUploadData);
  server.on("/api/custom/sprite", HTTP_GET, handleApiCustomSpriteGet);
  server.on("/api/custom/sprite/upload", HTTP_POST, handleCustomUploadDone, handleCustomSpriteUploadData);
  server.on("/api/custom/sprites/clear", HTTP_POST, handleApiCustomClearSprites);
  server.on("/api/custom/bg/clear", HTTP_POST, handleApiCustomClearBackground);
  server.on("/api/custom/use-drawing", HTTP_POST, handleApiCustomUseDrawing);

  const char *portalPaths[] = {
      "/generate_204", "/gen_204", "/hotspot-detect.html", "/canonical.html",
      "/success.txt", "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink"};
  for (const char *path : portalPaths) server.on(path, HTTP_ANY, handlePortalProbe);
  server.onNotFound(handleNotFound);
  server.begin();
  webServerStarted = true;
  Serial.println("[OK] Web server started");
}

bool connectWiFi() {
  if (wifiSsid.length() == 0) return false;

  stopCaptivePortal();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  showStatus("WiFi", "Connecting", C(120, 220, 255));
  Serial.print("[INFO] Connecting WiFi: ");
  Serial.println(wifiSsid);

  const unsigned long started = millis();
  while (millis() - started < 15000) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[OK] WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
      showStatus("WiFi OK", WiFi.localIP().toString(), C(0, 255, 120));
      delay(500);
      return true;
    }
    delay(50);
  }

  Serial.println("[WARN] WiFi connect failed");
  return false;
}

void startSetupAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(SETUP_AP_IP, SETUP_AP_IP, SETUP_AP_MASK);
  if (!WiFi.softAP(AP_SSID, AP_PASS)) Serial.println("[ERROR] Setup AP start failed");
  startCaptivePortal();
  startWebServer();
  Serial.print("[INFO] Setup AP IP: ");
  Serial.println(WiFi.softAPIP());
  drawApScreen();
}

void reconnectWiFiRuntime() {
  if (wifiSsid.length() == 0 || wifiReconnectRunning) return;

  stopCaptivePortal();
  showStatus("WiFi", "Reconnecting", C(120, 220, 255));
  Serial.print("[INFO] Runtime WiFi reconnect: ");
  Serial.println(wifiSsid);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  wifiReconnectStartedMs = millis();
  wifiReconnectRunning = true;
}

void processWiFiReconnect() {
  if (!wifiReconnectRunning) return;

  if (WiFi.status() == WL_CONNECTED) {
    wifiReconnectRunning = false;
    apMode = false;
    stopCaptivePortal();
    Serial.print("[OK] Runtime WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    showStatus("WiFi OK", WiFi.localIP().toString(), C(0, 255, 120));
    configureTime();
    beginStartupIpDisplay(WiFi.localIP().toString(), false);
    forceMediaRedraw();
    return;
  }

  if (millis() - wifiReconnectStartedMs < 15000) return;

  wifiReconnectRunning = false;
  Serial.println("[WARN] WiFi reconnect failed, setup AP restored");
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(SETUP_AP_IP, SETUP_AP_IP, SETUP_AP_MASK);
  WiFi.softAP(AP_SSID, AP_PASS);
  apMode = true;
  startCaptivePortal();
  beginStartupIpDisplay(WiFi.softAPIP().toString(), true);
  drawApScreen();
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  delay(300);
  Serial.println();
  Serial.print("WYL Pixel Clock ");
  Serial.print(ProjectIdentity::version());
  Serial.print(" [");
  Serial.print(ProjectIdentity::buildId());
  Serial.println("] starting...");
  Serial.print("[INFO] ");
  Serial.println(IntegrityGuard::validatedCredit());
  if (!IntegrityGuard::identityValid()) Serial.println("[WARN] Project identity check failed");

  loadSettings();
  setupStorage();
  setupScreenButton();
  setupDisplay();
  showBoot();

  if (connectWiFi()) {
    apMode = false;
    startWebServer();
    configureTime();
    beginStartupIpDisplay(WiFi.localIP().toString(), false);
  } else {
    startSetupAP();
    beginStartupIpDisplay(WiFi.softAPIP().toString(), true);
  }
}

void loop() {
  if (captivePortalActive) dnsServer.processNextRequest();
  server.handleClient();
  handleScreenButton();
  processWiFiReconnect();
  processTimeSync();

  if (rebootAtMs > 0 && timeReached(rebootAtMs)) ESP.restart();

  if (wifiReconnectPending && timeReached(wifiReconnectAtMs)) {
    wifiReconnectPending = false;
    reconnectWiFiRuntime();
  }

  if (timeReconfigurePending && timeReached(timeReconfigureAtMs)) {
    timeReconfigurePending = false;
    configureTime();
  }

  // 息屏只暂停 LED 绘制。
  if (screenSleeping) return;

  // 开机显示当前 IP。
  if (handleStartupIpDisplay()) return;

  // 配网模式显示热点信息。
  if (apMode && faceMode != FACE_CAROUSEL && faceMode != FACE_DRAWING) {
    static unsigned long lastApDraw = 0;
    if (millis() - lastApDraw > 3000) {
      drawApScreen();
      lastApDraw = millis();
    }
    return;
  }

  unsigned long interval = 1000;
  if (faceMode == FACE_RAINBOW) interval = 125;
  else if (faceMode == FACE_MARIO) interval = 50;
  else if (faceMode == FACE_CAROUSEL || faceMode == FACE_DRAWING) interval = 100;
  else if (faceMode == FACE_CUSTOM_CLOCK) {
    CustomClockConfig &cfg = customClockConfigs[customClockSlot];
    interval = (cfg.effectMode != CUSTOM_EFFECT_NONE || (cfg.spriteEnabled && cfg.animationMode != CUSTOM_ANIM_STATIC)) ? 50 : (cfg.timeFormat == 1 || cfg.blinkColon ? 200 : 1000);
  }

  if (millis() - lastDrawMs < interval) return;
  lastDrawMs = millis();
  applyDisplaySettingsIfNeeded();

  if (activeFaceMode != faceMode) {
    activeFaceMode = faceMode;
    if (faceMode == FACE_MARIO) marioNeedsFullRedraw = true;
    else if (faceMode == FACE_POKEDEX) cf0x06NeedsFullRedraw = true;
    else if (faceMode == FACE_CAROUSEL) {
      currentCarouselSlot = 255;
      lastCarouselChangeMs = 0;
    } else if (faceMode == FACE_CUSTOM_CLOCK) {
      loadedCustomBackgroundSlot = 255;
      resetCustomAnimationState();
    } else {
      display->setFont(NULL);
    }
  }

  // 图片轮播和画板不依赖 NTP，即使没有联网时间也能显示。
  if (faceMode == FACE_CAROUSEL) {
    drawCarouselFace();
    commitFrame();
    return;
  }
  if (faceMode == FACE_DRAWING) {
    drawDrawingFace();
    commitFrame();
    return;
  }

  struct tm timeinfo;
  if (!readTime(timeinfo)) {
    drawNoTime();
    commitFrame();
    return;
  }

  timeReady = true;
  if (faceMode == FACE_DIGITAL) drawDigitalFace(timeinfo);
  else if (faceMode == FACE_ANALOG) drawAnalogFace(timeinfo);
  else if (faceMode == FACE_RAINBOW) drawRainbowFace(timeinfo);
  else if (faceMode == FACE_MARIO) drawMarioCf0x01Face(timeinfo);
  else if (faceMode == FACE_POKEDEX) drawPokedexCf0x06Face(timeinfo);
  else drawCustomClockFace(timeinfo);

  commitFrame();
}
