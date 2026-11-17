#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <vector>
#include "esp_sleep.h"
#include "video_frames.h"
#include "secrets.h"

#define SDA_PIN 3
#define SCL_PIN 4
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

const uint32_t CHECK_INTERVAL_MS   = 5UL * 60UL * 1000UL;  // poll every 5 min
const uint32_t CONNECT_TIMEOUT_MS  = 15000;
const uint32_t SHOW_MS             = 20000;  // how long a message stays up
const uint32_t PAGE_MS             = 4000;   // text page time
const size_t   MAX_PAYLOAD         = 80 * 1024;
const uint8_t  COLS = 21, ROWS = 8;          // size-1 font on 128x64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---- received message ----
enum MsgType : uint8_t { MSG_TEXT = 0, MSG_IMAGE = 1, MSG_ANIM = 2, MSG_NONE = 255 };
uint8_t *msgData = nullptr;      // text bytes or frames
MsgType  msgType = MSG_NONE;
uint8_t  msgFps = 8;
uint16_t msgCount = 0;
uint32_t lastId = 0;
std::vector<String> textLines;

// ---- display state ----
bool     showing = false;
uint32_t showStart = 0, showUntil = 0, nextFrame = 0;
uint8_t  eyeFrame = 0, animFrame = 0;

// ---- network state ----
bool     connecting = false;
bool     firstCheckDone = false;
uint32_t lastCheck = 0, connectStart = 0;

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  connecting = false;
  lastCheck = millis();
  firstCheckDone = true;
}

void layoutText() {
  textLines.clear();
  String cur;
  String word;
  auto flushWord = [&]() {
    while (word.length() > COLS) {            // hard-split very long words
      if (cur.length()) { textLines.push_back(cur); cur = ""; }
      textLines.push_back(word.substring(0, COLS));
      word = word.substring(COLS);
    }
    if (cur.length() && cur.length() + 1 + word.length() > COLS) { textLines.push_back(cur); cur = ""; }
    if (word.length()) { cur += (cur.length() ? " " : "") + word; }
    word = "";
  };
  for (uint16_t i = 0; i < msgCount; i++) {
    char c = (char)msgData[i];
    if (c == '\n') { flushWord(); textLines.push_back(cur); cur = ""; }
    else if (c == ' ') flushWord();
    else word += (c >= 32 && c < 127) ? c : '?';   // GFX font is ASCII only
  }
  flushWord();
  if (cur.length()) textLines.push_back(cur);
}

// Returns true if a new message was downloaded.
bool fetchLatest() {
  WiFiClientSecure client;
  client.setInsecure();   // no cert pinning; the DEVICE_KEY is the only protection
  HTTPClient http;
  String url = String(ENDPOINT_URL) + "/latest?key=" + DEVICE_KEY + "&since=" + lastId;
  if (!http.begin(client, url)) return false;
  http.setTimeout(15000);
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len < 8 || len > (int)MAX_PAYLOAD) { http.end(); return false; }

  uint8_t *buf = (uint8_t *)malloc(len);
  if (!buf) { http.end(); return false; }
  WiFiClient *s = http.getStreamPtr();
  int got = 0;
  uint32_t deadline = millis() + 20000;
  while (got < len && millis() < deadline && http.connected()) {
    int n = s->readBytes(buf + got, len - got);
    if (n > 0) got += n; else delay(5);
  }
  http.end();
  if (got != len) { free(buf); return false; }

  uint32_t id = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
  uint8_t type = buf[4], fps = buf[5];
  uint16_t count = buf[6] | buf[7] << 8;
  bool ok = (type == MSG_TEXT && (int)count == len - 8) ||
            ((type == MSG_IMAGE || type == MSG_ANIM) && count > 0 && (int)count * 1024 == len - 8);
  if (!ok) { free(buf); return false; }

  // compact: move payload to the front and shrink
  memmove(buf, buf + 8, len - 8);
  free(msgData);
  msgData = buf;
  msgType = (MsgType)type;
  msgFps = fps ? fps : 8;
  msgCount = count;
  lastId = id;
  if (msgType == MSG_TEXT) layoutText();

  showing = true;
  showStart = millis();
  uint32_t pages = msgType == MSG_TEXT ? (textLines.size() + ROWS - 1) / ROWS : 1;
  showUntil = showStart + max(SHOW_MS, pages * PAGE_MS);
  animFrame = 0;
  nextFrame = 0;
  return true;
}

void netTick() {
  uint32_t now = millis();
  if (!connecting) {
    if (!firstCheckDone || now - lastCheck >= CHECK_INTERVAL_MS) {
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(true);                // modem sleep
      WiFi.begin(WIFI_SSID, WIFI_PASS);   // async: eyes keep blinking meanwhile
      WiFi.setTxPower(WIFI_POWER_11dBm);  // short range is enough, saves power
      connecting = true;
      connectStart = now;
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    fetchLatest();
    wifiOff();                            // radio off until next check to save power
  } else if (now - connectStart > CONNECT_TIMEOUT_MS) {
    wifiOff();
  }
}

void drawEyes() {
  display.clearDisplay();
  display.drawBitmap(0, 0, videoFrames[eyeFrame], SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
  eyeFrame = (eyeFrame + 1) % VIDEO_FRAME_COUNT;
  nextFrame = millis() + 125;
}

void drawMessage() {
  uint32_t now = millis();
  display.clearDisplay();
  if (msgType == MSG_TEXT) {
    uint32_t pages = max<uint32_t>(1, (textLines.size() + ROWS - 1) / ROWS);
    uint32_t page = ((now - showStart) / PAGE_MS) % pages;
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    for (uint8_t r = 0; r < ROWS; r++) {
      size_t i = page * ROWS + r;
      if (i >= textLines.size()) break;
      display.setCursor(0, r * 8);
      display.print(textLines[i]);
    }
    nextFrame = now + 500;
  } else {
    display.drawBitmap(0, 0, msgData + (size_t)animFrame * 1024, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    if (msgType == MSG_ANIM) animFrame = (animFrame + 1) % msgCount;
    nextFrame = now + (msgType == MSG_ANIM ? 1000 / msgFps : 500);
  }
  display.display();
}

void setup() {
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (true) { delay(100); }
  }
  display.setRotation(2);   // 180 degrees: applies to eyes, text, images, video
  display.dim(true);        // lower OLED brightness = less current
  display.clearDisplay();
  display.display();
  setCpuFrequencyMhz(80);   // lowest speed that still supports WiFi
}

void loop() {
  netTick();
  uint32_t now = millis();
  if (showing && now >= showUntil) { showing = false; eyeFrame = 0; nextFrame = 0; }
  if (now >= nextFrame) {
    if (showing) drawMessage(); else drawEyes();
  }
  // Low power: light-sleep until the next frame (CPU halts, RAM kept).
  // Skipped while WiFi is connecting so the radio isn't disturbed.
  if (!connecting) {
    now = millis();
    if (nextFrame > now + 5) {
      esp_sleep_enable_timer_wakeup((uint64_t)(nextFrame - now) * 1000ULL);
      esp_light_sleep_start();
    }
  } else {
    delay(5);
  }
}
