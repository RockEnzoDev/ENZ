// ============================================================
//  ENZ T-DECK  --  LILYGO T-Deck (original, ESP32-S3) companion
//  DOS-green family firmware: clock + I2C sensors (option),
//  ESP-NOW chat (byte-compatible with the CYD / M5Core2 units),
//  LoRa text chat (920 MHz band, JP low-power rules), music.
//  v1 draft: text only. LoRa bridge and voice PTT come later.
// ============================================================
#include <Arduino.h>
#define LILYGO_LGFX_USE_T_DECK
#include <LilyGo_LovyanGFX.h>   // official LilyGo profile: the hand-rolled LGFX config stayed black on real HW; this one lights up (verified by TEST6)
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <time.h>
#include <sys/time.h>

// ---- T-Deck pins (LILYGO official utilities.h) ----
#define PIN_POWERON   10
#define PIN_I2C_SDA   18
#define PIN_I2C_SCL   8
#define PIN_SD_CS     39
#define PIN_SPI_SCK   40
#define PIN_SPI_MISO  38
#define PIN_SPI_MOSI  41
#define PIN_LORA_CS   9
#define PIN_LORA_DIO1 45
#define PIN_LORA_RST  17
#define PIN_LORA_BUSY 13
#define PIN_I2S_BCK   7
#define PIN_I2S_WS    5
#define PIN_I2S_DOUT  6
#define PIN_BAT_ADC   4
#define PIN_TB_R      2    // trackball direction pins (LILYGO UnitTest mapping: G02=right, G01=up, G04=left, G03=down)
#define PIN_TB_L      1
#define PIN_TB_U      3
#define PIN_TB_D      15
#define PIN_TB_PRESS  0
#define KB_ADDR       0x55

// ---- display: official LilyGo_T_Deck profile (LilyGo-display-library).
// A hand-rolled LGFX config (same pins, same panel) never lit the panel on
// real hardware, while the stock profile works -- so the display path is
// kept 100% manufacturer-stock, and the GT911 touch is polled manually
// over I2C port 1 instead of a LovyanGFX touch device. ----
static LilyGo_T_Deck Lcd;

// ---- GT911 touch (addr 0x5D on the shared I2C bus), register-level polling ----
#define GT911_ADDR 0x5D
static bool gtTouch(int *outX, int *outY) {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t st = 0;
  if (Wire.requestFrom(GT911_ADDR, 1) == 1) st = Wire.read();
  bool ready = (st & 0x80) && (st & 0x0F) > 0;
  int rx = 0, ry = 0;
  if (ready) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(0x81); Wire.write(0x50);
    if (Wire.endTransmission(false) != 0) ready = false;
    else {
      uint8_t b[4] = { 0, 0, 0, 0 };
      if (Wire.requestFrom(GT911_ADDR, 4) == 4) { for (int i = 0; i < 4; i++) b[i] = Wire.read(); }
      rx = b[0] | (b[1] << 8);   // panel-native portrait coords: x 0..239
      ry = b[2] | (b[3] << 8);   // y 0..319
    }
  }
  Wire.beginTransmission(GT911_ADDR);   // clear status so the next point can latch
  Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
  Wire.endTransmission();
  if (!ready) return false;
  *outX = ry;              // rotation 1 (landscape): sx = native y
  *outY = 239 - rx;        // sy = 239 - native x
  return true;
}
#define SCR_W 320
#define SCR_H 240

// ---- DOS green palette (same as the rest of the family) ----
#define UI_BG     Lcd.color565(2, 10, 4)
#define UI_CARD   Lcd.color565(4, 22, 10)
#define UI_BORDER Lcd.color565(16, 70, 34)
#define UI_TEXT   Lcd.color565(180, 255, 200)
#define UI_DIM    Lcd.color565(64, 200, 110)
#define UI_MUTE   Lcd.color565(24, 110, 58)
#define UI_ACCENT Lcd.color565(64, 255, 128)
#define UI_ACCENT2 Lcd.color565(24, 160, 80)
#define UI_GHOST  Lcd.color565(8, 26, 14)
#define UI_WARN   Lcd.color565(255, 176, 0)
#define UI_BAD    Lcd.color565(255, 80, 60)
#define KC_NORMAL UI_CARD
#define KC_ACT    Lcd.color565(10, 48, 22)

enum Screen { SC_HOME, SC_CHAT, SC_PEERS, SC_MUSIC, SC_TRACKS, SC_SET, SC_TIMESET };
static Screen screen = SC_HOME;
static bool needsRedraw = true;
static Preferences prefs;
static bool sdReady = false;
static String myName = "TDeck";

// ---- small draw helpers ----
static void txt(const char* s, int x, int y, uint16_t color, int size = 1) {
  Lcd.setFont(&lgfx::fonts::Font2);
  Lcd.setTextSize(size);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
  Lcd.setTextColor(color);
  Lcd.drawString(s, x, y);
}
static int txtW(const char* s, int size = 1) {
  Lcd.setFont(&lgfx::fonts::Font2);
  Lcd.setTextSize(size);
  return Lcd.textWidth(s);
}
static void card(int x, int y, int w, int h, uint16_t fill = UI_CARD, uint16_t border = UI_BORDER) {
  Lcd.fillRoundRect(x, y, w, h, 6, fill);
  Lcd.drawRoundRect(x, y, w, h, 6, border);
}
static void hbtn(int x, int y, int w, int h, const char* label, uint16_t fill, uint16_t color) {
  Lcd.fillRoundRect(x, y, w, h, 6, fill);
  Lcd.drawRoundRect(x, y, w, h, 6, UI_BORDER);
  Lcd.setFont(&lgfx::fonts::Font2);
  Lcd.setTextSize(1);
  Lcd.setTextDatum(lgfx::textdatum_t::middle_center);
  Lcd.setTextColor(color);
  Lcd.drawString(label, x + w / 2, y + h / 2);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
}
static void drawHeader(const char* title, bool back = true) {
  if (back) { Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(2); Lcd.setTextColor(UI_DIM); Lcd.setTextDatum(lgfx::textdatum_t::top_left); Lcd.drawString("<", 4, 0); }
  Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(1); Lcd.setTextColor(UI_ACCENT);
  Lcd.setTextDatum(lgfx::textdatum_t::top_center);
  Lcd.drawString(title, SCR_W / 2, 4);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
  Lcd.drawLine(0, 24, SCR_W, 24, UI_BORDER);
}

//  JAPANESE TEXT (16px bitmap font on SD: /jpfont16.bin) -- ported from the CYD
// ============================================================
#define JP_FONT_FILE "/jpfont16.bin"
static bool jpReady = false;
static File jpFile;
static uint16_t* jpCodes = nullptr;
static uint32_t jpCount = 0, jpDataOff = 0;
struct JpCacheEnt { int32_t idx; uint8_t d[64]; };
static JpCacheEnt jpCache[24];

static bool jpInit() {
  if (jpReady) return true;
  if (!sdReady) return false;
  jpFile = SD.open(JP_FONT_FILE, FILE_READ);
  if (!jpFile) return false;
  uint8_t h[12];
  if (jpFile.read(h, 12) != 12 || memcmp(h, "ENZF", 4) != 0 || h[4] != 1 || h[5] != 16) { jpFile.close(); return false; }
  jpCount = h[6] | (h[7] << 8);
  jpDataOff = h[8] | (h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
  jpCodes = (uint16_t*)malloc(jpCount * 2);
  if (!jpCodes) { jpFile.close(); return false; }
  if (jpFile.read((uint8_t*)jpCodes, jpCount * 2) != jpCount * 2) { free(jpCodes); jpCodes = nullptr; jpFile.close(); return false; }
  for (int i = 0; i < 24; i++) jpCache[i].idx = -1;
  jpReady = true;
  return true;
}

// ---- UTF-8 helpers ----
static uint32_t u8next(const char*& p) {
  uint8_t c = (uint8_t)*p++;
  if (c < 0x80) return c;
  int n; uint32_t cp;
  if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
  else return 0xFFFD;
  for (int i = 0; i < n; i++) {
    if (((uint8_t)*p & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
  }
  return cp;
}
static void u8append(String& s, uint32_t cp) {
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
  else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}
static void u8chop(String& s) {
  if (!s.length()) return;
  int i = s.length() - 1;
  while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
  s.remove(i);
}
static String u8cut(const String& s, unsigned maxBytes) {
  if (s.length() <= maxBytes) return s;
  unsigned n = maxBytes;
  while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
  return s.substring(0, n);
}
static bool hasNonAscii(const char* s) { while (*s) { if ((uint8_t)*s >= 0x80) return true; s++; } return false; }
static int jpCharW(uint32_t cp) { return (cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F)) ? 8 : 16; }
static int jpWidth(const char* s) { int w = 0; while (*s) w += jpCharW(u8next(s)); return w; }

// ---- glyph access ----
static int jpFind(uint32_t cp) {
  if (cp > 0xFFFF || !jpCodes) return -1;
  int lo = 0, hi = (int)jpCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1; uint16_t c = jpCodes[mid];
    if (c == cp) return mid;
    if (c < cp) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}
static const uint8_t* jpGlyph(int idx) {
  JpCacheEnt& e = jpCache[idx % 24];
  if (e.idx == idx) return e.d;
  jpFile.seek(jpDataOff + (uint32_t)idx * 64);
  if (jpFile.read(e.d, 64) != 64) return nullptr;
  e.idx = idx;
  return e.d;
}
// Draw one character, top-left at (x,y). Returns the advance.
// bg >= 0 fills the cell first (opaque); runs of lit pixels become fillRects.
static int jpDrawChar(uint32_t cp, int x, int y, uint16_t fg, int32_t bg) {
  int w = jpCharW(cp);
  if (bg >= 0) Lcd.fillRect(x, y, w, 16, (uint16_t)bg);
  if (cp == ' ') return w;
  int idx = jpFind(cp);
  const uint8_t* g = (idx >= 0) ? jpGlyph(idx) : nullptr;
  if (!g) { if (cp >= 0x80) Lcd.drawRect(x + 1, y + 1, w - 2, 14, fg); return w; }
  for (int row = 0; row < 16; row++) {
    int runStart = -1;
    for (int col = 0; col <= w; col++) {
      bool on = false;
      if (col < w) {
        int bit = (row * 16 + col) * 2;
        uint8_t a = (g[bit >> 3] >> (6 - (bit & 7))) & 3;
        on = (a >= 2);
      }
      if (on && runStart < 0) runStart = col;
      if (!on && runStart >= 0) { Lcd.fillRect(x + runStart, y + row, col - runStart, 1, fg); runStart = -1; }
    }
  }
  return w;
}
// Draw a UTF-8 string at 16px. ASCII uses the JP font too when loaded, so
// mixed text lines up; before the font loads, ASCII falls back to Font2.
static int jpText(const char* s, int x, int y, uint16_t fg, int32_t bg = -1, int maxW = 0) {
  if (!jpReady) { txt(s, x, y, fg, 1); return txtW(s, 1); }
  int cx = x;
  while (*s) {
    uint32_t cp = u8next(s);
    int w = jpCharW(cp);
    if (maxW && cx - x + w > maxW) break;
    cx += jpDrawChar(cp, cx, y, fg, bg);
  }
  return cx - x;
}
static int textW(const String& s) { return jpReady ? jpWidth(s.c_str()) : txtW(s.c_str(), 1); }
static String fitText(const String& s, int maxW) {
  if (textW(s) <= maxW) return s;
  String r = s;
  while (r.length() && textW(r) + 16 > maxW) u8chop(r);
  return r + "..";
}
// Wrap into lines of at most maxW px (JP-aware). Returns line count.
static int wrapText(const String& txtIn, int maxW, String* out, int maxN) {
  int n = 0;
  String cur; int w = 0;
  const char* p = txtIn.c_str();
  while (*p && n < maxN) {
    const char* st = p;
    uint32_t cp = u8next(p);
    if (cp == '\n') { out[n++] = cur; cur = ""; w = 0; continue; }
    int cw = jpReady ? jpCharW(cp) : 8;
    if (w + cw > maxW && cur.length()) { out[n++] = cur; cur = ""; w = 0; if (n >= maxN) break; }
    int len = p - st; if (len > 4) len = 4;
    char tmp[5]; memcpy(tmp, st, len); tmp[len] = 0;
    cur += tmp; w += cw;
  }
  if (n < maxN && (cur.length() || n == 0)) out[n++] = cur;
  return n;
}



// ============================================================
//  SENSORS on I2C (option): AHT20 humidity/temp + BME/BMP280
//  pressure. Auto-detected; the keyboard shares this bus (0x55).
// ============================================================
static bool sensAHT = false, sensBME = false, sensBMP = false;
static float senT = NAN, senH = NAN, senP = NAN;   // degC, %RH, hPa (sea-level corrected)
static float senPraw = NAN;
static int pressAltM = 0;
static void i2cWrite(uint8_t addr, std::initializer_list<uint8_t> b) {
  Wire.beginTransmission(addr);
  for (uint8_t v : b) Wire.write(v);
  Wire.endTransmission();
}
static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}
static bool ahtInit() {
  if (!i2cProbe(0x38)) return false;
  i2cWrite(0x38, {0xBE, 0x08, 0x00});
  delay(10);
  return true;
}
static bool ahtRead(float& t, float& h) {
  i2cWrite(0x38, {0xAC, 0x33, 0x00});
  delay(80);
  if (Wire.requestFrom(0x38, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (b[0] & 0x80) return false;
  uint32_t rh = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
  uint32_t tt = ((uint32_t)(b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];
  h = rh * 100.0f / 1048576.0f;
  t = tt * 200.0f / 1048576.0f - 50.0f;
  return true;
}
// BME280/BMP280 register driver (Bosch datasheet compensation)
static uint8_t bmeAddr = 0x76;
static uint16_t dT1, dP1; static int16_t dT2, dT3, dP2, dP3, dP4, dP5, dP6, dP7, dP8, dP9;
static uint8_t dH1, dH3; static int16_t dH2, dH4, dH5; static int8_t dH6;
static void bmeReadRegs(uint8_t reg, uint8_t* buf, int n) {
  Wire.beginTransmission(bmeAddr); Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom((int)bmeAddr, n);
  for (int i = 0; i < n; i++) buf[i] = Wire.available() ? Wire.read() : 0;
}
static uint16_t bmeU16(const uint8_t* b) { return b[0] | ((uint16_t)b[1] << 8); }
static bool bmeInit() {
  for (uint8_t a : {0x76, 0x77}) {
    bmeAddr = a;
    if (!i2cProbe(a)) continue;
    uint8_t id = 0; bmeReadRegs(0xD0, &id, 1);
    if (id != 0x60 && id != 0x58) continue;
    sensBME = (id == 0x60); sensBMP = (id != 0x60);
    uint8_t c[26]; bmeReadRegs(0x88, c, 26);
    dT1 = bmeU16(c); dT2 = (int16_t)bmeU16(c + 2); dT3 = (int16_t)bmeU16(c + 4);
    dP1 = bmeU16(c + 6); dP2 = (int16_t)bmeU16(c + 8); dP3 = (int16_t)bmeU16(c + 10);
    dP4 = (int16_t)bmeU16(c + 12); dP5 = (int16_t)bmeU16(c + 14); dP6 = (int16_t)bmeU16(c + 16);
    dP7 = (int16_t)bmeU16(c + 18); dP8 = (int16_t)bmeU16(c + 20); dP9 = (int16_t)bmeU16(c + 22);
    dH1 = c[25];
    if (sensBME) {
      uint8_t e[7]; bmeReadRegs(0xE1, e, 7);
      dH2 = (int16_t)bmeU16(e); dH3 = e[2];
      dH4 = (int16_t)((e[3] << 4) | (e[4] & 0x0F));
      dH5 = (int16_t)((e[5] << 4) | (e[4] >> 4));
      dH6 = (int8_t)e[6];
      i2cWrite(bmeAddr, {0xF2, 0x01});           // osrs_h x1
    }
    i2cWrite(bmeAddr, {0xF4, 0x27});             // osrs_t/p x1, normal mode
    i2cWrite(bmeAddr, {0xF5, 0xA0});             // standby 1000 ms, filter off
    return true;
  }
  return false;
}
static int32_t bmeTf = 0;
static bool bmeRead(float& t, float& h, float& p) {
  uint8_t b[8]; bmeReadRegs(0xF7, b, 8);
  int32_t adcP = ((uint32_t)b[0] << 12) | ((uint32_t)b[1] << 4) | (b[2] >> 4);
  int32_t adcT = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4);
  int32_t adcH = ((uint32_t)b[6] << 8) | b[7];
  int32_t v1 = ((((adcT >> 3) - ((int32_t)dT1 << 1))) * (int32_t)dT2) >> 11;
  int32_t v2 = (((((adcT >> 4) - (int32_t)dT1) * ((adcT >> 4) - (int32_t)dT1)) >> 12) * (int32_t)dT3) >> 14;
  bmeTf = v1 + v2;
  t = ((bmeTf * 5 + 128) >> 8) / 100.0f;
  int64_t pv1 = (int64_t)bmeTf - 128000;
  int64_t pv2 = pv1 * pv1 * (int64_t)dP6 + ((pv1 * (int64_t)dP5) << 17) + ((int64_t)dP4 << 35);
  pv1 = ((pv1 * pv1 * (int64_t)dP3) >> 8) + ((pv1 * (int64_t)dP2) << 12);
  pv1 = (((int64_t)1 << 47) + pv1) * (int64_t)dP1 >> 33;
  if (pv1 == 0) return false;
  int64_t pp = 1048576 - adcP;
  pp = (((pp << 31) - pv2) * 3125) / pv1;
  pv1 = ((int64_t)dP9 * (pp >> 13) * (pp >> 13)) >> 25;
  pv2 = ((int64_t)dP8 * pp) >> 19;
  pp = ((pp + pv1 + pv2) >> 8) + ((int64_t)dP7 << 4);
  p = pp / 256.0f / 100.0f;
  if (sensBME) {
    int32_t hv = bmeTf - 76800;
    hv = (((((adcH << 14) - ((int32_t)dH4 << 20) - ((int32_t)dH5 * hv)) + 16384) >> 15) *
          (((((((hv * (int32_t)dH6) >> 10) * (((hv * (int32_t)dH3) >> 11) + 32768)) >> 10) + 2097152) * (int32_t)dH2 + 8192) >> 14)) >> 15;
    hv -= (((((hv >> 15) * (hv >> 15)) >> 7) * (int32_t)dH1) >> 4);
    if (hv < 0) hv = 0; if (hv > 419430400) hv = 419430400;
    h = (hv >> 12) / 1024.0f;
  } else h = NAN;
  return true;
}
static bool sensorsPresent() { return sensAHT || sensBME || sensBMP; }
static void sensorsPoll() {
  if (sensAHT) { float t, h; if (ahtRead(t, h)) { senT = t; senH = h; } }
  if (sensBME || sensBMP) {
    float t, h, p;
    if (bmeRead(t, h, p)) {
      senPraw = p;
      if (!sensAHT) { senT = t; if (!isnan(h)) senH = h; }
    }
  }
  if (!isnan(senPraw)) senP = senPraw / powf(1.0f - pressAltM / 44330.0f, 5.255f);
}
// pressure history: one sample / 10 min, 24 h ring (for the home mini graph)
static float prHist[144];
static int prCount = 0, prHead = 0;
static unsigned long prLastSample = 0;
static void prSample(bool force = false) {
  if (isnan(senP)) return;
  if (!force && millis() - prLastSample < 600000UL) return;
  prLastSample = millis();
  prHist[prHead] = senP;
  prHead = (prHead + 1) % 144;
  if (prCount < 144) prCount++;
}
static float prAt(int i) { return prHist[(prHead - prCount + i + 288) % 144]; }   // i: 0 = oldest


// ============================================================
//  CHAT (ESP-NOW) -- byte-compatible with the CYD / M5Core2:
//  same 223-byte MsgPacket, HELLO every 5 s, 30 s peer timeout,
//  peer cap 8, group pseudo-peer, SD logs "/chats/*.log".
// ============================================================
struct __attribute__((packed)) MsgPacket {
  uint8_t type;          // 1=HELLO, 2=CHAT
  uint8_t to_mac[6];     // all zero = everyone
  char name[16];
  char text[200];
};
static_assert(sizeof(MsgPacket) == 223, "packet size");
#define MAX_PEERS 8
#define MAX_CHAT_MSGS 30
#define CHAT_DIR "/chats"
#define HELLO_INTERVAL_MS 5000
#define PEER_TIMEOUT_MS 30000
struct ChatPeer { uint8_t mac[6]; char name[16]; unsigned long lastSeen; bool hasUnread; };
static ChatPeer chatPeers[MAX_PEERS];
static int chatPeerCount = 0;
static uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static String chatLines[MAX_CHAT_MSGS];
static int chatLineCount = 0;
static int curPeer = -1;             // conversation shown on the chat screen
static int chatChannel = 1;
static bool chatLR = false;
static bool chatOn = false;
static unsigned long lastHello = 0;
struct RxSlot { uint8_t mac[6]; uint8_t data[sizeof(MsgPacket)]; int len; };
#define RX_SLOTS 6
static volatile RxSlot msgRx[RX_SLOTS];
static volatile uint8_t msgRxHead = 0, msgRxTail = 0;
static bool chatDirty = false;       // conversation content changed (for partial redraws)
static void termPush(const String& who, const String& text);   // home terminal stream (defined with the screens)

static bool macIsZero(const uint8_t* m) { for (int i = 0; i < 6; i++) if (m[i]) return false; return true; }
static bool macEq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }
static String macStr(const uint8_t* m) {
  char b[16]; snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]); return String(b);
}
static String chatPath(const uint8_t* mac) { return String(CHAT_DIR) + "/" + macStr(mac) + ".log"; }
static String peerName(int i) {
  if (i < 0 || i >= chatPeerCount) return "?";
  if (chatPeers[i].name[0]) return String(chatPeers[i].name);
  if (macIsZero(chatPeers[i].mac)) return "Broadcast";
  return macStr(chatPeers[i].mac).substring(8);
}
static void loadMyName() {
  prefs.begin("msg", true);
  String n = prefs.getString("name", "");
  prefs.end();
  if (n.length()) { myName = n; return; }
  uint8_t m[6]; WiFi.macAddress(m);
  char b[24]; snprintf(b, sizeof(b), "TDeck-%02X%02X", m[4], m[5]);
  myName = b;
}
static void saveMyName() { prefs.begin("msg", false); prefs.putString("name", myName); prefs.end(); }
static void loadChatPrefs() {
  prefs.begin("chat", true);
  chatChannel = prefs.getInt("ch", 1);
  chatLR = prefs.getBool("lr", false);
  prefs.end();
  if (chatChannel < 1 || chatChannel > 13) chatChannel = 1;
}
static void saveChatPrefs() { prefs.begin("chat", false); prefs.putInt("ch", chatChannel); prefs.putBool("lr", chatLR); prefs.end(); }
static void pushChatLine(const String& line) {
  if (chatLineCount == MAX_CHAT_MSGS) {
    for (int i = 1; i < MAX_CHAT_MSGS; i++) chatLines[i - 1] = chatLines[i];
    chatLineCount = MAX_CHAT_MSGS - 1;
  }
  chatLines[chatLineCount++] = line;
  chatDirty = true;
}
static void loadChatLines(int peerIdx) {
  chatLineCount = 0;
  if (!sdReady || peerIdx < 0) return;
  String path = macIsZero(chatPeers[peerIdx].mac) ? String("/chats/group.log") : chatPath(chatPeers[peerIdx].mac);
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  String all;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (ln.length()) pushChatLine(ln);
  }
  f.close();
  chatDirty = true;
}
static int findOrAddPeer(const uint8_t* mac) {
  for (int i = 0; i < chatPeerCount; i++) if (macEq(chatPeers[i].mac, mac)) return i;
  if (chatPeerCount >= MAX_PEERS) return -1;
  int idx = chatPeerCount++;
  memcpy(chatPeers[idx].mac, mac, 6);
  chatPeers[idx].name[0] = 0;
  chatPeers[idx].lastSeen = 0;
  chatPeers[idx].hasUnread = false;
  return idx;
}
static int ensureGroupPeer() {
  static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
  return findOrAddPeer(zero);
}
static void appendChatLog(int peerIdx, bool outgoing, const String& text) {
  if (!sdReady || peerIdx < 0) return;
  if (!SD.exists(CHAT_DIR)) SD.mkdir(CHAT_DIR);
  File f = SD.open(chatPath(chatPeers[peerIdx].mac), FILE_APPEND);
  if (!f) return;
  String who = outgoing ? "Me" : peerName(peerIdx);
  f.print(who); f.print('|'); f.print(text); f.print('\n');
  f.close();
}
static void appendGroupLog(const String& who, const String& text) {
  if (!sdReady) return;
  if (!SD.exists(CHAT_DIR)) SD.mkdir(CHAT_DIR);
  File f = SD.open("/chats/group.log", FILE_APPEND);
  if (!f) return;
  f.print(who); f.print('|'); f.print(text); f.print('\n');
  f.close();
}
static void msgRecvCommon(const uint8_t* mac, const uint8_t* data, int len) {
  uint8_t next = (msgRxHead + 1) % RX_SLOTS;
  if (next == msgRxTail) return;
  if (len > (int)sizeof(MsgPacket)) len = sizeof(MsgPacket);
  memcpy((void*)msgRx[msgRxHead].mac, mac, 6);
  memcpy((void*)msgRx[msgRxHead].data, data, len);
  msgRx[msgRxHead].len = len;
  msgRxHead = next;
}
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onMsgRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) { msgRecvCommon(info->src_addr, data, len); }
#else
void onMsgRecv(const uint8_t* mac, const uint8_t* data, int len) { msgRecvCommon(mac, data, len); }
#endif
static void ensurePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0; p.encrypt = false;
  esp_now_add_peer(&p);
}
static void applyChatProtocol() {
  esp_wifi_set_protocol(WIFI_IF_STA, chatLR ? WIFI_PROTOCOL_LR
                                            : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
}
void chatStart() {
  if (chatOn) return;
  loadMyName();
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel((uint8_t)chatChannel, WIFI_SECOND_CHAN_NONE);
  applyChatProtocol();
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(onMsgRecv);
  ensurePeer(BCAST_MAC);
  chatOn = true;
  lastHello = 0;
}
void chatStop() {
  if (!chatOn) return;
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  chatOn = false;
}
static void sendHello() {
  if (!chatOn) return;
  MsgPacket m; memset(&m, 0, sizeof(m));
  m.type = 1;
  { String nm = u8cut(myName, 15); strncpy(m.name, nm.c_str(), 15); }
  esp_now_send(BCAST_MAC, (uint8_t*)&m, sizeof(m));
}
static bool sendChatMsg(int peerIdx, const String& text) {
  if (!chatOn || peerIdx < 0 || peerIdx >= chatPeerCount) return false;
  MsgPacket m; memset(&m, 0, sizeof(m));
  m.type = 2;
  memcpy(m.to_mac, chatPeers[peerIdx].mac, 6);
  { String nm = u8cut(myName, 15); strncpy(m.name, nm.c_str(), 15); }
  { String tx = u8cut(text, 199); strncpy(m.text, tx.c_str(), 199); }
  const uint8_t* dst = macIsZero(chatPeers[peerIdx].mac) ? BCAST_MAC : chatPeers[peerIdx].mac;
  ensurePeer(dst);
  return esp_now_send(dst, (uint8_t*)&m, sizeof(m)) == ESP_OK;
}
static int onlinePeerCount() {
  int n = 0;
  for (int i = 0; i < chatPeerCount; i++)
    if (!macIsZero(chatPeers[i].mac) && chatPeers[i].lastSeen && millis() - chatPeers[i].lastSeen < PEER_TIMEOUT_MS) n++;
  return n;
}
static bool anyUnread() {
  for (int i = 0; i < chatPeerCount; i++) if (chatPeers[i].hasUnread) return true;
  return false;
}
void processChatRx() {
  while (msgRxTail != msgRxHead) {
    RxSlot slot;
    memcpy(slot.mac, (const void*)msgRx[msgRxTail].mac, 6);
    slot.len = msgRx[msgRxTail].len;
    memcpy(slot.data, (const void*)msgRx[msgRxTail].data, slot.len);
    msgRxTail = (msgRxTail + 1) % RX_SLOTS;
    if (slot.len < 8) continue;
    MsgPacket m;
    memcpy(&m, slot.data, slot.len < (int)sizeof(m) ? slot.len : (int)sizeof(m));
    uint8_t myMac[6]; WiFi.macAddress(myMac);
    if (macEq(slot.mac, myMac)) continue;
    int idx = findOrAddPeer(slot.mac);
    if (idx < 0) continue;
    if (m.name[0]) { strncpy(chatPeers[idx].name, m.name, 15); chatPeers[idx].name[15] = 0; }
    chatPeers[idx].lastSeen = millis();
    if (m.type == 1) {                       // HELLO
      if (screen == SC_PEERS) needsRedraw = true;
      chatDirty = true;
    } else if (m.type == 2) {                // CHAT
      bool toMe = macEq(m.to_mac, myMac) || macIsZero(m.to_mac);
      if (!toMe) continue;
      bool group = macIsZero(m.to_mac);
      appendChatLog(idx, false, String(m.text));
      if (group) { appendGroupLog(peerName(idx), String(m.text)); termPush(peerName(idx), String(m.text)); }
      bool viewing = (screen == SC_CHAT && ((group && curPeer >= 0 && macIsZero(chatPeers[curPeer].mac)) || curPeer == idx));
      if (viewing) pushChatLine(peerName(idx) + "|" + String(m.text));
      else {
        chatPeers[idx].hasUnread = true;
        if (group) { int gi = ensureGroupPeer(); if (gi >= 0 && gi != idx) chatPeers[gi].hasUnread = true; }
      }
      chatDirty = true;
      if (screen != SC_CHAT) needsRedraw = true;   // home badges / terminal
    }
  }
  if (chatOn && millis() - lastHello > HELLO_INTERVAL_MS) { lastHello = millis(); sendHello(); }
}

// ============================================================
//  LoRa TEXT (SX1262 via RadioLib) -- 920 MHz JP low-power plan:
//  921.0 MHz, BW125, SF8, CR 4/5, TX 13 dBm, LBT (CAD) before TX.
//  Frame: [type][nameLen][name][text]  HELLO every 90 s.
// ============================================================
#define LORA_FREQ 921.0f
static SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
static bool loraReady = false;
static volatile bool loraRxFlag = false;
static unsigned long loraLastHello = 0, loraHeardAt = 0;
static String loraHeardName = "";
static int loraHeardRssi = 0;
static uint16_t loraMsgId = 1;
static void loraIsr() { loraRxFlag = true; }
static bool loraInit() {
  int st = radio.begin(LORA_FREQ, 125.0f, 8, 5, 0x12, 13, 8);
  if (st != RADIOLIB_ERR_NONE) return false;
  radio.setDio1Action(loraIsr);
  st = radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) return false;
  loraReady = true;
  return true;
}
static bool loraSendFrame(uint8_t type, const String& text) {
  if (!loraReady) return false;
  String nm = u8cut(myName, 15);
  int nl = nm.length(), tl = text.length();
  if (tl > 190) tl = 190;
  uint8_t buf[224];
  int n = 0;
  buf[n++] = type;
  buf[n++] = (uint8_t)(loraMsgId & 0xFF); buf[n++] = (uint8_t)(loraMsgId >> 8);
  loraMsgId++;
  buf[n++] = (uint8_t)nl;
  memcpy(buf + n, nm.c_str(), nl); n += nl;
  if (type == 2) { memcpy(buf + n, text.c_str(), tl); n += tl; }
  // listen before talk: a few CAD tries, then give up (someone is using the channel)
  bool free = false;
  for (int i = 0; i < 6; i++) {
    if (radio.scanChannel() == RADIOLIB_CHANNEL_FREE) { free = true; break; }
    delay(35);
  }
  if (!free) return false;
  int st = radio.transmit(buf, n);
  radio.startReceive();
  return st == RADIOLIB_ERR_NONE;
}
static void loraPoll() {
  if (!loraReady) return;
  if (loraRxFlag) {
    loraRxFlag = false;
    uint8_t buf[224];
    int len = radio.getPacketLength();
    int st = (len > 0 && len <= (int)sizeof(buf)) ? radio.readData(buf, len) : RADIOLIB_ERR_NONE;
    if (st == RADIOLIB_ERR_NONE && len >= 4) {
      uint8_t type = buf[0];
      int nl = buf[3];
      if (nl <= 15 && 4 + nl <= len) {
        String name = String((const char*)buf + 4).substring(0, nl);
        loraHeardName = name;
        loraHeardRssi = (int)radio.getRSSI();
        loraHeardAt = millis();
        if (type == 2) {
          String text = String((const char*)buf + 4 + nl).substring(0, len - 4 - nl);
          appendGroupLog(name + " (L)", text);
          termPush(name + " (L)", text);
          int gi = ensureGroupPeer();
          if (screen == SC_CHAT && gi >= 0 && curPeer == gi) pushChatLine(name + " (L)|" + text);
          else if (gi >= 0) chatPeers[gi].hasUnread = true;
          chatDirty = true;
          if (screen != SC_CHAT) needsRedraw = true;
        }
      }
    }
  }
  if (millis() - loraLastHello > 90000UL) {
    loraLastHello = millis();
    loraSendFrame(1, "");
  }
}

// ============================================================
//  INPUT: physical keyboard (I2C 0x55), trackball, touch
// ============================================================
static int cursorMode = 0;      // trackball cursor: 0 Auto, 1 On, 2 Off (pref "sys"/"cur"); declared here because drawSettings reads it
static bool touchSeen = false;  // any real touch since boot
static String inputBuf = "";
static int kbPoll() {   // -1 = bus error, 0 = queue empty, else the key byte
  if (Wire.requestFrom(KB_ADDR, 1) != 1) return -1;
  int c = 0;
  while (Wire.available() > 0) c = Wire.read();
  return c;
}
static volatile int tbDx = 0, tbDy = 0;
static void trackballPoll() {
  static uint8_t prev = 0x0F;
  uint8_t now = (digitalRead(PIN_TB_R) ? 1 : 0) | (digitalRead(PIN_TB_L) ? 2 : 0) |
                (digitalRead(PIN_TB_U) ? 4 : 0) | (digitalRead(PIN_TB_D) ? 8 : 0);
  uint8_t changed = prev ^ now;   // the module toggles its lines per step: count both edges
  if (changed & 1) tbDx++;
  if (changed & 2) tbDx--;
  if (changed & 4) tbDy--;
  if (changed & 8) tbDy++;
  prev = now;
}
static bool tbPressed() { return digitalRead(PIN_TB_PRESS) == LOW; }
// ============================================================
//  JAPANESE INPUT (Alt+C toggles EN/JP): romaji -> kana + SKK
//  conversion against /skk.dic -- the same dictionary and the
//  same lookup as the CYD and the M5Core2 builds.
// ============================================================
static bool jpMode = false;
static String imeComp = "";        // kana composition (the SKK reading)
static String imeRoma = "";        // pending romaji tail, not yet kana
static String imeCand[24];
static int imeCandN = 0, imeCandIdx = -1;   // idx >= 0: conversion active
static bool skkMissing = false;

static String toKata(const String& s) {
  String r; const char* p = s.c_str();
  while (*p) { uint32_t c = u8next(p); if (c >= 0x3041 && c <= 0x3096) c += 0x60; u8append(r, c); }
  return r;
}
static String toHira(const String& s) {
  String r; const char* p = s.c_str();
  while (*p) { uint32_t c = u8next(p); if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60; u8append(r, c); }
  return r;
}
static void popLastChar(String& s) {
  if (!s.length()) return;
  int i = (int)s.length() - 1;
  while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
  s.remove(i);
}
static bool skkLookup(const String& key, String* out, int& n, int maxN) {
  File f = SD.open("/skk.dic", FILE_READ);
  if (!f) { skkMissing = true; return false; }
  skkMissing = false;
  uint32_t lo = 0, hi = f.size();
  String line;
  while (lo < hi) {
    uint32_t mid = (lo + hi) >> 1, st = mid;
    if (st > lo) {
      f.seek(st - 1);
      int c = f.read();
      if (c != '\n') { while (f.available()) { c = f.read(); st++; if (c == '\n') break; } }
    }
    if (st >= hi) { hi = mid; continue; }
    f.seek(st);
    line = "";
    while (f.available() && line.length() < 600) { int c = f.read(); if (c == '\n') break; line += (char)c; }
    uint32_t end = st + line.length() + 1;
    int sp = line.indexOf(' ');
    String k = (sp > 0) ? line.substring(0, sp) : line;
    int cmp = strcmp(k.c_str(), key.c_str());
    if (cmp == 0 && sp > 0) {
      int pos = sp + 1;
      while (pos < (int)line.length() && n < maxN) {
        if (line[pos] == '/') pos++;
        int e = line.indexOf('/', pos); if (e < 0) e = line.length();
        String cand = line.substring(pos, e);
        int semi = cand.indexOf(';'); if (semi >= 0) cand = cand.substring(0, semi);
        if (cand.length() && cand[0] != '(') out[n++] = cand;
        pos = e;
      }
      f.close();
      return n > 0;
    }
    if (cmp < 0) lo = end; else hi = st;
  }
  f.close();
  return false;
}
static void imeAddCand(const String& c) {
  if (!c.length() || imeCandN >= 24) return;
  for (int i = 0; i < imeCandN; i++) if (imeCand[i] == c) return;
  imeCand[imeCandN++] = c;
}
static void imeConvert() {
  if (!imeComp.length()) return;
  String reading = toHira(imeComp);
  imeCandN = 0;
  String tmp[20]; int n = 0;
  skkLookup(reading, tmp, n, 20);
  for (int i = 0; i < n; i++) imeAddCand(tmp[i]);
  imeAddCand(reading);
  imeAddCand(toKata(reading));
  imeCandIdx = imeCandN ? 0 : -1;
}

// ---- romaji -> kana ----
struct RomaEnt { const char* r; const char* k; };
static const RomaEnt ROMA[] = {
  {"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
  {"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
  {"sa","さ"},{"shi","し"},{"si","し"},{"su","す"},{"se","せ"},{"so","そ"},
  {"ta","た"},{"chi","ち"},{"ti","ち"},{"tsu","つ"},{"tu","つ"},{"te","て"},{"to","と"},
  {"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},
  {"ha","は"},{"hi","ひ"},{"fu","ふ"},{"hu","ふ"},{"he","へ"},{"ho","ほ"},
  {"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
  {"ya","や"},{"yu","ゆ"},{"yo","よ"},
  {"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
  {"wa","わ"},{"wo","を"},
  {"ga","が"},{"gi","ぎ"},{"gu","ぐ"},{"ge","げ"},{"go","ご"},
  {"za","ざ"},{"ji","じ"},{"zi","じ"},{"zu","ず"},{"ze","ぜ"},{"zo","ぞ"},
  {"da","だ"},{"di","ぢ"},{"du","づ"},{"de","で"},{"do","ど"},
  {"ba","ば"},{"bi","び"},{"bu","ぶ"},{"be","べ"},{"bo","ぼ"},
  {"pa","ぱ"},{"pi","ぴ"},{"pu","ぷ"},{"pe","ぺ"},{"po","ぽ"},
  {"kya","きゃ"},{"kyu","きゅ"},{"kyo","きょ"},
  {"gya","ぎゃ"},{"gyu","ぎゅ"},{"gyo","ぎょ"},
  {"sha","しゃ"},{"shu","しゅ"},{"sho","しょ"},
  {"sya","しゃ"},{"syu","しゅ"},{"syo","しょ"},
  {"cha","ちゃ"},{"chu","ちゅ"},{"cho","ちょ"},
  {"cya","ちゃ"},{"cyu","ちゅ"},{"cyo","ちょ"},
  {"tya","ちゃ"},{"tyu","ちゅ"},{"tyo","ちょ"},
  {"ja","じゃ"},{"ju","じゅ"},{"jo","じょ"},
  {"jya","じゃ"},{"jyu","じゅ"},{"jyo","じょ"},
  {"zya","じゃ"},{"zyu","じゅ"},{"zyo","じょ"},
  {"nya","にゃ"},{"nyu","にゅ"},{"nyo","にょ"},
  {"hya","ひゃ"},{"hyu","ひゅ"},{"hyo","ひょ"},
  {"bya","びゃ"},{"byu","びゅ"},{"byo","びょ"},
  {"pya","ぴゃ"},{"pyu","ぴゅ"},{"pyo","ぴょ"},
  {"mya","みゃ"},{"myu","みゅ"},{"myo","みょ"},
  {"rya","りゃ"},{"ryu","りゅ"},{"ryo","りょ"},
  {"xa","ぁ"},{"xi","ぃ"},{"xu","ぅ"},{"xe","ぇ"},{"xo","ぉ"},
  {"xtu","っ"},{"xtsu","っ"},{"xya","ゃ"},{"xyu","ゅ"},{"xyo","ょ"},{"xwa","ゎ"},
  {"la","ぁ"},{"li","ぃ"},{"lu","ぅ"},{"le","ぇ"},{"lo","ぉ"},
  {"ltu","っ"},{"ltsu","っ"},{"lya","ゃ"},{"lyu","ゅ"},{"lyo","ょ"},
};
static const char* romaKana(const String& p) {
  for (auto& e : ROMA) if (p == e.r) return e.k;
  return nullptr;
}
static bool romaIsPrefix(const String& p) {
  for (auto& e : ROMA) {
    size_t n = strlen(e.r);
    if (n > p.length() && strncmp(e.r, p.c_str(), p.length()) == 0) return true;
  }
  return false;
}
static void imeRomaPush() {
  while (imeRoma.length()) {
    const char* k = romaKana(imeRoma);
    bool pref = romaIsPrefix(imeRoma);
    if (k && !pref) { imeComp += k; imeRoma = ""; return; }
    if (pref) return;   // may still grow (includes the exact-but-ambiguous case)
    if (imeRoma.length() >= 2) {
      char c0 = imeRoma[0], c1 = imeRoma[1];
      if (c0 == 'n' && c1 == 'n') { imeComp += "ん"; imeRoma.remove(0, 2); continue; }
      if (c0 == 'n' && !strchr("aiueoy", c1)) { imeComp += "ん"; imeRoma.remove(0, 1); continue; }
      if (c0 == c1 && strchr("bcdfghjklmpqrstvwxz", c0)) { imeComp += "っ"; imeRoma.remove(0, 1); continue; }
      if (c0 == 't' && c1 == 'c') { imeComp += "っ"; imeRoma.remove(0, 1); continue; }   // "tch"
    }
    if (imeRoma[0] == 'n') return;   // a lone n may still grow
    imeComp += imeRoma[0];
    imeRoma.remove(0, 1);
  }
}
static void imeRomaFeed(char c) { imeRoma += c; imeRomaPush(); }
static void imeRomaFlush() {
  while (imeRoma.length()) {
    const char* k = romaKana(imeRoma);
    if (k) { imeComp += k; imeRoma = ""; break; }
    char c0 = imeRoma[0];
    if (c0 == 'n') { imeComp += "ん"; imeRoma.remove(0, 1); continue; }
    if (imeRoma.length() >= 2 && imeRoma[1] == c0 && strchr("bcdfghjklmpqrstvwxz", c0)) { imeComp += "っ"; imeRoma.remove(0, 1); continue; }
    imeComp += c0;
    imeRoma.remove(0, 1);
  }
}
static void imeCommitAll() {
  String add;
  if (imeCandIdx >= 0) add = imeCand[imeCandIdx];
  else { imeRomaFlush(); add = imeComp; }
  if (add.length()) {
    if (inputBuf.length() + add.length() > 195) add = u8cut(add, 195 - inputBuf.length());
    inputBuf += add;
  }
  imeComp = ""; imeRoma = ""; imeCandN = 0; imeCandIdx = -1;
}
// One key in JP mode. Returns true when the IME consumed it; false lets the
// caller apply its default action (send, plain space, Tab, Esc, BS on inputBuf).
static bool imeHandleKey(int c) {
  if (c == 0x0D) {   // Enter: commit only; sending needs a second Enter
    if (imeCandIdx >= 0 || imeComp.length() || imeRoma.length()) { imeCommitAll(); return true; }
    return false;
  }
  if (c == 0x08) {   // BS peels one layer at a time
    if (imeCandIdx >= 0) { imeCandIdx = -1; imeCandN = 0; return true; }
    if (imeRoma.length()) { imeRoma.remove(imeRoma.length() - 1); return true; }
    if (imeComp.length()) { popLastChar(imeComp); return true; }
    return false;
  }
  if (c == 0x1B) {   // Esc cancels conversion / composition first
    if (imeCandIdx >= 0) { imeCandIdx = -1; imeCandN = 0; return true; }
    if (imeComp.length() || imeRoma.length()) { imeComp = ""; imeRoma = ""; return true; }
    return false;
  }
  if (c == 0x20) {   // Space: convert, or cycle candidates
    if (imeCandIdx >= 0) { imeCandIdx = (imeCandIdx + 1) % imeCandN; return true; }
    if (imeComp.length() || imeRoma.length()) { imeRomaFlush(); imeConvert(); return true; }
    return false;
  }
  if (c >= 0x21 && c < 0x7F) {
    if (isalpha(c)) {
      if (imeCandIdx >= 0) imeCommitAll();
      imeRomaFeed((char)tolower(c));
      return true;
    }
    if (c == '-') { if (imeCandIdx >= 0) imeCommitAll(); imeRomaFlush(); imeComp += "ー"; return true; }
    if (c == '\'') { if (imeCandIdx >= 0) imeCommitAll(); if (imeRoma == "n") { imeComp += "ん"; imeRoma = ""; } return true; }
    imeCommitAll();   // digits / punctuation commit the composition, then insert
    if (inputBuf.length() < 190) inputBuf += (char)c;
    return true;
  }
  return false;
}
// The composer line shared by the home terminal and the chat screen:
// committed text, then the composition (underlined) or the SKK candidate
// with its position, then the cursor. A あ tag marks JP mode.
static void drawComposerLine(int x, int y, uint16_t baseCol) {
  String comp = "";
  if (imeCandIdx >= 0) comp = imeCand[imeCandIdx] + " (" + String(imeCandIdx + 1) + "/" + String(imeCandN) + ")";
  else comp = imeComp + imeRoma;
  String body = inputBuf;
  int maxW = SCR_W - x - (jpMode ? 36 : 16);
  while (body.length() && jpWidth(body.c_str()) + jpWidth(comp.c_str()) + 34 > maxW) {
    const char* pp = body.c_str(); u8next(pp); body.remove(0, (size_t)(pp - body.c_str()));
  }
  int cx = x;
  cx += jpText("C:\\>", cx, y, UI_DIM, -1);
  cx += jpText(body.c_str(), cx, y, baseCol, -1);
  if (comp.length()) {
    int w = jpText(comp.c_str(), cx, y, UI_ACCENT, -1);
    Lcd.drawLine(cx, y + 17, cx + w, y + 17, UI_ACCENT);
    cx += w;
  }
  txt("_", cx + 2, y + 1, UI_DIM, 1);
  if (jpMode) jpText("あ", SCR_W - 24, y, UI_WARN, -1);
}


// ============================================================
//  MUSIC (MP3 + WAV from /music, ESP8266Audio -> I2S MAX98357A)
//  Decode task pattern (same lesson as the M5 build): the UI loop
//  never touches the decoder; controls only set request flags.
// ============================================================
#define MAX_TRACKS 64
static String musicNames[MAX_TRACKS];
static int musicCount = 0;
static volatile int musicCur = -1;
static int musicTop = 0;
static volatile bool musicPlaying = false, musicPaused = false;
static int musicVol = 200;
static volatile unsigned long musicElapsedBase = 0, musicPlayStarted = 0;
static AudioOutputI2S* i2sOut = nullptr;
static AudioGenerator* gen = nullptr;
static AudioFileSourceSD* srcFile = nullptr;
static AudioGeneratorMP3* gMp3 = nullptr;
static AudioGeneratorWAV* gWav = nullptr;
static volatile int reqPlay = -1;
static volatile bool reqStop = false;
static volatile uint32_t musicPosSnap = 0, musicSizeSnap = 0;   // UI reads these, never srcFile
static bool hasAudioExt(const String& n) {
  String l = n; l.toLowerCase();
  return l.endsWith(".mp3") || l.endsWith(".wav");
}
void musicScan() {
  musicCount = 0;
  if (!sdReady) return;
  File dir = SD.open("/music");
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory() && musicCount < MAX_TRACKS) {
      String n = String(f.name());
      int slash = n.lastIndexOf('/');
      if (slash >= 0) n = n.substring(slash + 1);
      if (n.length() && n[0] != '.' && hasAudioExt(n)) musicNames[musicCount++] = n;
    }
    f.close();
  }
  dir.close();
}
static void doStop() {
  if (gen) gen->stop();
  if (gMp3) { delete gMp3; gMp3 = nullptr; }
  if (gWav) { delete gWav; gWav = nullptr; }
  gen = nullptr;
  if (srcFile) { delete srcFile; srcFile = nullptr; }
  musicPlaying = false;
  musicPaused = false;
  musicPosSnap = 0; musicSizeSnap = 0;
  if (screen == SC_MUSIC) needsRedraw = true;
}
static bool doPlay(int idx) {
  if (idx < 0 || idx >= musicCount) return false;
  doStop();
  String path = "/music/" + musicNames[idx];
  srcFile = new AudioFileSourceSD(path.c_str());
  String l = musicNames[idx]; l.toLowerCase();
  bool ok = false;
  if (l.endsWith(".mp3")) { gMp3 = new AudioGeneratorMP3(); gen = gMp3; ok = gMp3->begin(srcFile, i2sOut); }
  else                    { gWav = new AudioGeneratorWAV(); gen = gWav; ok = gWav->begin(srcFile, i2sOut); }
  if (!ok) { doStop(); return false; }
  musicCur = idx;
  musicPosSnap = 0;
  musicSizeSnap = srcFile ? srcFile->getSize() : 0;
  musicPlaying = true;
  musicPaused = false;
  musicElapsedBase = 0;
  musicPlayStarted = millis();
  if (screen == SC_MUSIC) needsRedraw = true;
  return true;
}
static void musicTask(void*) {
  for (;;) {
    if (reqStop) { reqStop = false; doStop(); }
    if (reqPlay >= 0) { int i = reqPlay; reqPlay = -1; doPlay(i); }
    if (gen && musicPlaying && !musicPaused) {
      bool alive = gen->loop();
      if (srcFile) { musicPosSnap = srcFile->getPos(); musicSizeSnap = srcFile->getSize(); }
      if (!alive) {
        int n = (int)musicCur + 1;
        if (n < musicCount) doPlay(n); else doStop();
      }
    } else {
      vTaskDelay(10);
    }
  }
}
void musicStop() { reqPlay = -1; reqStop = true; }
bool musicPlay(int idx) {
  if (idx < 0 || idx >= musicCount) return false;
  reqStop = false;
  reqPlay = idx;
  return true;
}
void musicTogglePause() {
  if (!musicPlaying) { if (musicCur >= 0) musicPlay((int)musicCur); return; }
  if (musicPaused) { musicPaused = false; musicPlayStarted = millis(); }
  else { musicElapsedBase += (millis() - musicPlayStarted) / 1000; musicPaused = true; }
  if (screen == SC_MUSIC) needsRedraw = true;
}
void musicNext() { if (musicCount) { int n = (int)musicCur + 1; if (n >= musicCount) n = 0; musicPlay(n); } }
void musicPrev() { if (musicCount) { int n = (int)musicCur - 1; if (n < 0) n = musicCount - 1; musicPlay(n); } }
static unsigned long musicElapsedSec() {
  if (!musicPlaying) return 0;
  if (musicPaused) return musicElapsedBase;
  return musicElapsedBase + (millis() - musicPlayStarted) / 1000;
}
static float musicProgress() {
  // Snapshot values maintained by the decode task. The UI must never touch
  // srcFile/gen: they belong to the decode task, and racing them from the
  // UI wedged the whole interface the moment playback started.
  uint32_t sz = musicSizeSnap;
  if (!sz) return 0;
  float r = (float)musicPosSnap / (float)sz;
  return r > 1.0f ? 1.0f : r;
}
void loadMusicVol() {
  prefs.begin("music", true);
  musicVol = prefs.getInt("vol", 200);
  prefs.end();
  if (musicVol < 0) musicVol = 0;
  if (musicVol > 255) musicVol = 255;
}
void saveMusicVol() { prefs.begin("music", false); prefs.putInt("vol", musicVol); prefs.end(); }
static void applyGain() { if (i2sOut) i2sOut->SetGain(musicVol / 255.0f); }
static String trackTitle(int idx) {
  String n = musicNames[idx];
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  return n;
}

// ============================================================
//  SCREENS
// ============================================================
static int brightnessVal = 200;
static const char* WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static void nowTm(struct tm& t) { time_t tt = time(nullptr); localtime_r(&tt, &t); }
static bool timeIsSet() { struct tm t; nowTm(t); return (t.tm_year + 1900) >= 2025; }
static int batteryPct() {
  int raw = analogRead(PIN_BAT_ADC);
  float v = raw / 4095.0f * 3.3f * 2.0f;
  int pct = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}
// ---- 7-segment time (same renderer as the M5 build) ----
static void segH(int x, int y, int len, int t, uint16_t c) {
  Lcd.fillRect(x + t / 2, y, len - t, t, c);
  Lcd.fillTriangle(x, y + t / 2, x + t / 2, y, x + t / 2, y + t, c);
  Lcd.fillTriangle(x + len, y + t / 2, x + len - t / 2, y, x + len - t / 2, y + t, c);
}
static void segV(int x, int y, int len, int t, uint16_t c) {
  Lcd.fillRect(x, y + t / 2, t, len - t, c);
  Lcd.fillTriangle(x + t / 2, y, x, y + t / 2, x + t, y + t / 2, c);
  Lcd.fillTriangle(x + t / 2, y + len, x, y + len - t / 2, x + t, y + len - t / 2, c);
}
static void segDigit(int x, int y, int w, int h, int d) {
  static const uint8_t SEGMAP[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
  uint8_t m = (d >= 0 && d <= 9) ? SEGMAP[d] : 0x40;   // out of range = dash
  uint16_t off = Lcd.color565(7, 22, 12);
  const int t = 6;
  auto one = [&](int bit, uint16_t c) {
    switch (bit) {
      case 0: segH(x + 1, y, w - 2, t, c); break;
      case 1: segV(x + w - t, y + 1, h / 2 - 1, t, c); break;
      case 2: segV(x + w - t, y + h / 2, h / 2 - 1, t, c); break;
      case 3: segH(x + 1, y + h - t, w - 2, t, c); break;
      case 4: segV(x, y + h / 2, h / 2 - 1, t, c); break;
      case 5: segV(x, y + 1, h / 2 - 1, t, c); break;
      case 6: segH(x + 1, y + (h - t) / 2, w - 2, t, c); break;
    }
  };
  for (int b = 0; b < 7; b++) one(b, off);                          // ghost underlay first
  for (int b = 0; b < 7; b++) if ((m >> b) & 1) one(b, UI_ACCENT);  // lit segments on top
}
#define SEG_X0 8
#define SEG_Y0 52
static void drawSevenSegTime(int hh, int mm) {
  const int w = 24, h = 38;
  segDigit(SEG_X0, SEG_Y0, w, h, hh / 10);
  segDigit(SEG_X0 + 30, SEG_Y0, w, h, hh % 10);
  Lcd.fillRect(SEG_X0 + 60, SEG_Y0 + 9, 6, 6, UI_ACCENT);
  Lcd.fillRect(SEG_X0 + 60, SEG_Y0 + 25, 6, 6, UI_ACCENT);
  segDigit(SEG_X0 + 70, SEG_Y0, w, h, mm / 10);
  segDigit(SEG_X0 + 100, SEG_Y0, w, h, mm % 10);
}
// ---- home terminal stream (group conversation, both radios) ----
#define TERM_N 6
static String termLines[TERM_N];
static int termCount = 0;
static bool termDirty = true;
static int termScroll = 0;   // trackball scrollback on the home terminal
static void termPush(const String& who, const String& text) {
  if (termCount == TERM_N) { for (int i = 1; i < TERM_N; i++) termLines[i - 1] = termLines[i]; termCount = TERM_N - 1; }
  termLines[termCount++] = who + "|" + text;
  termDirty = true;
}
static void termInitFromLog() {
  termCount = 0;
  if (!sdReady) return;
  File f = SD.open("/chats/group.log", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (ln.length()) termPush(ln.substring(0, ln.indexOf('|')), ln.substring(ln.indexOf('|') + 1));
  }
  f.close();
}
// ---- home screen ----
static int lastSec = -1, lastMin = -1, lastDay = -1;
static unsigned long lastSensorDraw = 0;
static String lastHomeStatus = "\x01";
static void drawHomeDateTime() {
  struct tm t; nowTm(t);
  Lcd.fillRect(0, 17, 190, 75, UI_BG);
  char db[40];
  if (timeIsSet()) snprintf(db, sizeof(db), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  else snprintf(db, sizeof(db), "----/--/--");
  Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(2); Lcd.setTextColor(UI_TEXT, UI_BG);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
  Lcd.drawString(db, 8, 18);
  if (timeIsSet()) drawSevenSegTime(t.tm_hour, t.tm_min);
  else drawSevenSegTime(-1, -1);
  char sb[12]; snprintf(sb, sizeof(sb), "%s %02d", WD[t.tm_wday % 7], t.tm_sec);
  Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(1); Lcd.setTextColor(UI_DIM, UI_BG);
  Lcd.drawString(sb, 140, 68);
}
static void drawHomeSeconds() {
  struct tm t; nowTm(t);
  Lcd.fillRect(136, 64, 84, 20, UI_BG);
  char sb[12]; snprintf(sb, sizeof(sb), "%s %02d", WD[t.tm_wday % 7], t.tm_sec);
  Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(1); Lcd.setTextColor(UI_DIM, UI_BG);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
  Lcd.drawString(sb, 140, 68);
}
static void drawHomeCard() {
  Lcd.fillRect(9, 95, SCR_W - 18, 46, UI_CARD);
  if (sensorsPresent()) {
    char b1[48], b2[48];
    if (!isnan(senT)) snprintf(b1, sizeof(b1), "IN  %.1fC   %.0f%%", senT, senH); else snprintf(b1, sizeof(b1), "IN  --.-C   --%%");
    txt(b1, 16, 100, UI_TEXT, 1);
    if (!isnan(senP)) {
      float trend = prCount >= 2 ? senP - prAt(0) : 0;
      snprintf(b2, sizeof(b2), "%.1f hPa  %s", senP, trend > 0.3f ? "RISING" : (trend < -0.3f ? "FALLING" : "STEADY"));
    } else snprintf(b2, sizeof(b2), "-- hPa");
    txt(b2, 16, 118, UI_DIM, 1);
    if (prCount >= 2) {   // always-on mini graph, right half of the card
      float mn = prAt(0), mx = prAt(0);
      for (int i = 0; i < prCount; i++) { float v = prAt(i); if (v < mn) mn = v; if (v > mx) mx = v; }
      if (mx - mn < 1.0f) { mx += 0.5f; mn -= 0.5f; }
      int gx = 196, gw = 108, gy = 98, gh = 40;
      Lcd.drawRect(gx - 2, gy - 2, gw + 4, gh + 4, UI_BORDER);
      int prevX = -1, prevY = -1;
      for (int i = 0; i < prCount; i++) {
        int x = gx + i * (gw - 1) / (prCount - 1);
        int y = gy + gh - 1 - (int)((prAt(i) - mn) / (mx - mn) * (gh - 1));
        if (prevX >= 0) Lcd.drawLine(prevX, prevY, x, y, UI_ACCENT);
        prevX = x; prevY = y;
      }
    }
  } else {
    char b1[64], b2[64];
    snprintf(b1, sizeof(b1), "ESP-NOW %s ch%d%s Online:%d%s", chatOn ? "ON" : "OFF", chatChannel, chatLR ? " LR" : "", onlinePeerCount(), anyUnread() ? " [NEW]" : "");
    txt(b1, 16, 100, UI_TEXT, 1);
    if (loraReady) {
      if (loraHeardAt && millis() - loraHeardAt < 3600000UL) snprintf(b2, sizeof(b2), "LoRa 921.0 SF8  %s  %ddBm", loraHeardName.c_str(), loraHeardRssi);
      else snprintf(b2, sizeof(b2), "LoRa 921.0 SF8  listening");
    } else snprintf(b2, sizeof(b2), "LoRa -- (not found)");
    txt(b2, 16, 118, UI_DIM, 1);
  }
}
static void drawHomeTerminal() {
  Lcd.fillRect(9, 169, SCR_W - 18, 50, UI_BG);
  if (termScroll > termCount - 2) termScroll = termCount > 2 ? termCount - 2 : 0;
  if (termScroll < 0) termScroll = 0;
  int showTo = termCount - termScroll;
  int start = showTo - 2; if (start < 0) start = 0;
  int y = 170;
  for (int i = start; i < showTo; i++) {
    int bar = termLines[i].indexOf('|');
    String who = bar >= 0 ? termLines[i].substring(0, bar) : "?";
    String body = bar >= 0 ? termLines[i].substring(bar + 1) : termLines[i];
    String ln = "[" + who + "] " + body;
    bool mine = who == "Me";
    jpText(fitText(ln, SCR_W - 26).c_str(), 12, y, mine ? UI_ACCENT : UI_TEXT, -1);
    y += 18;
  }
  drawComposerLine(12, 202, UI_ACCENT);
  termDirty = false;
}
static void drawHome() {
  Lcd.fillScreen(UI_BG);
  // top-right: name + battery
  char bb[24]; snprintf(bb, sizeof(bb), "BAT %d%%", batteryPct());
  txt(bb, SCR_W - 6 - txtW(bb), 1, UI_MUTE, 1);
  txt(fitText(myName, 150).c_str(), 8, 1, UI_MUTE, 1);
  hbtn(196, 26, 38, 34, "CHAT", KC_ACT, UI_TEXT);
  if (anyUnread()) Lcd.fillCircle(230, 30, 4, UI_WARN);
  hbtn(238, 26, 38, 34, "MUS", KC_NORMAL, UI_TEXT);
  hbtn(280, 26, 34, 34, "SET", KC_NORMAL, UI_TEXT);
  card(8, 94, SCR_W - 16, 48);
  card(8, 150, SCR_W - 16, 70);
  txt(chatOn ? "COMIC CHAT" : "COMIC CHAT (off)", 14, 152, UI_MUTE, 1);
  lastMin = -1; lastDay = -1; lastSensorDraw = 0;
  drawHomeDateTime();
  struct tm t; nowTm(t);
  lastMin = t.tm_min; lastDay = t.tm_mday; lastSec = t.tm_sec;
  drawHomeCard();
  drawHomeTerminal();
}
static void homeTick() {
  struct tm t; nowTm(t);
  if (t.tm_mday != lastDay || t.tm_min != lastMin) {
    drawHomeDateTime();
    lastDay = t.tm_mday; lastMin = t.tm_min; lastSec = t.tm_sec;
  } else if (t.tm_sec != lastSec) {
    drawHomeSeconds();
    lastSec = t.tm_sec;
  }
  if (millis() - lastSensorDraw > 5000) { lastSensorDraw = millis(); drawHomeCard(); }
  if (termDirty) drawHomeTerminal();
}
// ---- chat screen ----
static int chatScroll = 0;
static void drawChat() {
  Lcd.fillScreen(UI_BG);
  String title = "CHAT";
  if (curPeer >= 0 && curPeer < chatPeerCount) {
    bool on = !macIsZero(chatPeers[curPeer].mac) && chatPeers[curPeer].lastSeen && millis() - chatPeers[curPeer].lastSeen < PEER_TIMEOUT_MS;
    title = peerName(curPeer) + (macIsZero(chatPeers[curPeer].mac) ? " [ALL]" : (on ? " [ON]" : " [--]"));
  }
  drawHeader(fitText(title, 200).c_str());
  String disp[40]; int dn = 0;
  for (int i = 0; i < chatLineCount; i++) {
    int bar = chatLines[i].indexOf('|');
    String who = bar >= 0 ? chatLines[i].substring(0, bar) : "?";
    String body = bar >= 0 ? chatLines[i].substring(bar + 1) : chatLines[i];
    String wtmp[3]; int wn = wrapText(body, SCR_W - 24 - textW("[" + who + "] "), wtmp, 3);
    for (int k = 0; k < wn && dn < 40; k++) disp[dn++] = (k == 0 ? ("[" + who + "] ") : "    ") + wtmp[k];
  }
  int show = dn < 9 ? dn : 9;
  int start = dn - show - chatScroll;
  if (start < 0) start = 0;
  for (int i = 0; i < show && start + i < dn; i++) {
    const String& ln = disp[start + i];
    jpText(ln.c_str(), 10, 30 + i * 19, ln.startsWith("[Me]") ? UI_ACCENT : UI_TEXT, -1, SCR_W - 18);
  }
  Lcd.fillRoundRect(6, 204, SCR_W - 12, 30, 6, UI_CARD);
  Lcd.drawRoundRect(6, 204, SCR_W - 12, 30, 6, UI_BORDER);
  drawComposerLine(12, 211, UI_TEXT);
  txt("Enter=send Tab=peer", SCR_W - 4 - txtW("Enter=send Tab=peer"), 188, UI_MUTE, 1);
  if (cursorActive()) {
    hbtn(290, 60, 26, 30, "^", KC_NORMAL, UI_TEXT);
    hbtn(290, 140, 26, 30, "v", KC_NORMAL, UI_TEXT);
  }
}
// ---- peer list ----
static int peerSel = 0;
static void drawPeers() {
  Lcd.fillScreen(UI_BG);
  drawHeader("COMIC CHAT");
  txt(myName.c_str(), 196, 3, UI_MUTE, 1);
  int y = 30;
  for (int i = 0; i < chatPeerCount && y < 196; i++) {
    bool on = !macIsZero(chatPeers[i].mac) && chatPeers[i].lastSeen && millis() - chatPeers[i].lastSeen < PEER_TIMEOUT_MS;
    if (i == peerSel) Lcd.fillRoundRect(4, y - 2, SCR_W - 8, 20, 4, KC_ACT);
    String ln = peerName(i) + (macIsZero(chatPeers[i].mac) ? " [ALL]" : (on ? " [ON]" : " [--]")) + (chatPeers[i].hasUnread ? " [NEW]" : "");
    jpText(fitText(ln, SCR_W - 24).c_str(), 10, y, UI_TEXT, -1);
    y += 22;
  }
  if (chatPeerCount == 0) txt("No peers yet (HELLO every 5 s)", 10, 34, UI_MUTE, 1);
  char lb[64];
  if (loraReady && loraHeardAt) snprintf(lb, sizeof(lb), "LoRa last: %s (%ddBm, %lus ago)", loraHeardName.c_str(), loraHeardRssi, (millis() - loraHeardAt) / 1000);
  else snprintf(lb, sizeof(lb), "LoRa: %s", loraReady ? "listening" : "not found");
  txt(lb, 10, 204, UI_MUTE, 1);
  txt("TAP or trackball+press to open", 10, 220, UI_MUTE, 1);
}
// ---- music screens (same layout language as the M5 build) ----
static void drawMusicScreen() {
  Lcd.fillScreen(UI_BG);
  drawHeader("MUSIC");
  card(6, 26, SCR_W - 12, 64);
  if (musicCur >= 0) {
    jpText(fitText(trackTitle((int)musicCur), SCR_W - 28).c_str(), 14, 32, UI_TEXT, -1);
    unsigned long el = musicElapsedSec();
    char eb[24]; snprintf(eb, sizeof(eb), "%s  %lu:%02lu", musicPaused ? "PAUSE" : (musicPlaying ? "PLAY" : "STOP"), el / 60, el % 60);
    txt(eb, 14, 58, UI_DIM, 1);
    Lcd.drawRect(120, 62, 180, 8, UI_BORDER);
    Lcd.fillRect(121, 63, (int)(178 * musicProgress()), 6, UI_ACCENT);
  } else {
    txt("No track selected", 14, 44, UI_MUTE, 1);
  }
  hbtn(6, 96, 98, 44, "|<<", KC_NORMAL, UI_TEXT);
  hbtn(111, 96, 98, 44, musicPlaying && !musicPaused ? "||" : ">", KC_ACT, UI_TEXT);
  hbtn(216, 96, 98, 44, ">>|", KC_NORMAL, UI_TEXT);
  txt("VOL", 10, 157, UI_DIM, 1);
  hbtn(48, 148, 48, 34, "-", KC_NORMAL, UI_TEXT);
  Lcd.drawRect(108, 158, 132, 12, UI_BORDER);
  Lcd.fillRect(109, 159, (int)(130 * musicVol / 255.0f), 10, UI_ACCENT2);
  hbtn(252, 148, 48, 34, "+", KC_NORMAL, UI_TEXT);
  hbtn(6, 192, 308, 40, "TRACKS >", KC_NORMAL, UI_TEXT);
}
static void updateMusicScreen() {
  if (musicCur < 0) return;
  Lcd.fillRect(12, 56, 106, 18, UI_CARD);
  unsigned long el = musicElapsedSec();
  char eb[24]; snprintf(eb, sizeof(eb), "%s  %lu:%02lu", musicPaused ? "PAUSE" : (musicPlaying ? "PLAY" : "STOP"), el / 60, el % 60);
  txt(eb, 14, 58, UI_DIM, 1);
  Lcd.drawRect(120, 62, 180, 8, UI_BORDER);
  Lcd.fillRect(121, 63, 178, 6, UI_CARD);
  Lcd.fillRect(121, 63, (int)(178 * musicProgress()), 6, UI_ACCENT);
}
static int tracksTop = 0, trackSel = 0;
static void drawTracks() {
  Lcd.fillScreen(UI_BG);
  drawHeader("TRACKS");
  if (musicCount == 0) txt(sdReady ? "No .mp3/.wav in /music" : "No SD card", 10, 44, UI_MUTE, 1);
  for (int r = 0; r < 4; r++) {
    int idx = tracksTop + r;
    if (idx >= musicCount) break;
    int y = 34 + r * 42;
    Lcd.fillRoundRect(6, y, SCR_W - 12, 38, 6, idx == musicCur ? KC_ACT : UI_CARD);
    Lcd.drawRoundRect(6, y, SCR_W - 12, 38, 6, idx == trackSel ? UI_ACCENT : UI_BORDER);
    String line = String(idx == musicCur && musicPlaying ? "> " : "  ") + trackTitle(idx);
    jpText(fitText(line, SCR_W - 44).c_str(), 14, y + 11, idx == musicCur ? UI_ACCENT : UI_TEXT, -1);
  }
  if (tracksTop > 0) hbtn(6, 208, 150, 28, "^ PAGE", KC_NORMAL, UI_TEXT);
  if (tracksTop + 4 < musicCount) hbtn(164, 208, 150, 28, "PAGE v", KC_NORMAL, UI_TEXT);
}
// ---- settings ----
static bool editingName = false, editingAlt = false;
static String editBuf = "";
static void drawSettings() {
  Lcd.fillScreen(UI_BG);
  drawHeader("Settings");
  const int x = 8, w = SCR_W - 16;
  int y = 28;
  auto row = [&](const char* label, const String& value) {
    card(x, y, w, 24);
    txt(label, x + 10, y + 5, UI_DIM, 1);
    jpText(fitText(value, 190).c_str(), x + 118, y + 5, UI_TEXT, -1);
    y += 26;
  };
  if (editingName) row("Name*", editBuf + "_");
  else row("Name", myName);
  row("ESP-NOW", chatOn ? "ON" : "OFF");
  row("ESP-NOW Ch", String(chatChannel));
  row("ESP-NOW", chatLR ? "LongRange" : "Normal");
  if (editingAlt) row("Altitude*", editBuf + "_");
  else row("Altitude", String(pressAltM) + " m");
  {
    struct tm t; nowTm(t);
    char cb[32];
    snprintf(cb, sizeof(cb), "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    row("Clock", cb);
  }
  row("Bright", String(brightnessVal));
  row("Cursor", cursorMode == 0 ? "Auto" : (cursorMode == 1 ? "ON" : "OFF"));
}
// ---- time set ----
static int tsF[6] = {2026, 1, 1, 0, 0, 0};
static void drawTimeSet() {
  Lcd.fillScreen(UI_BG);
  drawHeader("Set clock");
  const char* LB[6] = {"Year", "Month", "Day", "Hour", "Min", "Sec"};
  for (int i = 0; i < 6; i++) {
    int y = 32 + i * 30;
    txt(LB[i], 16, y + 5, UI_DIM, 1);
    hbtn(120, y, 36, 24, "-", KC_NORMAL, UI_TEXT);
    char vb[12]; snprintf(vb, sizeof(vb), "%d", tsF[i]);
    Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(1); Lcd.setTextColor(UI_TEXT);
    Lcd.setTextDatum(lgfx::textdatum_t::middle_center);
    Lcd.drawString(vb, 196, y + 12);
    Lcd.setTextDatum(lgfx::textdatum_t::top_left);
    hbtn(236, y, 36, 24, "+", KC_NORMAL, UI_TEXT);
  }
  hbtn(96, 216, 128, 22, "SET TIME", KC_ACT, UI_TEXT);
}
static void applyTimeSet() {
  struct tm t = {};
  t.tm_year = tsF[0] - 1900; t.tm_mon = tsF[1] - 1; t.tm_mday = tsF[2];
  t.tm_hour = tsF[3]; t.tm_min = tsF[4]; t.tm_sec = tsF[5];
  time_t tt = mktime(&t);
  struct timeval tv = {tt, 0};
  settimeofday(&tv, nullptr);
}
static void drawCurrent() {
  switch (screen) {
    case SC_HOME: drawHome(); break;
    case SC_CHAT: drawChat(); break;
    case SC_PEERS: drawPeers(); break;
    case SC_MUSIC: drawMusicScreen(); break;
    case SC_TRACKS: drawTracks(); break;
    case SC_SET: drawSettings(); break;
    case SC_TIMESET: drawTimeSet(); break;
  }
}
static void goHome() { screen = SC_HOME; needsRedraw = true; }
// ---- sending ----
static void doSend(const String& text) {
  if (!text.length()) return;
  if (!chatOn) chatStart();
  int gi = ensureGroupPeer();
  if (screen == SC_CHAT && curPeer >= 0 && !macIsZero(chatPeers[curPeer].mac)) {
    if (sendChatMsg(curPeer, text)) {
      appendChatLog(curPeer, true, text);
      pushChatLine("Me|" + text);
    }
    return;
  }
  if (gi >= 0 && sendChatMsg(gi, text)) {
    appendChatLog(gi, true, text);
    appendGroupLog("Me", text);
    if (curPeer == gi) pushChatLine("Me|" + text);
  }
  termPush("Me", text);
  loraSendFrame(2, text);         // the same message also goes out on LoRa
  chatDirty = true;
}
// ---- keyboard routing ----
static void handleKey(int c) {
  if (editingName || editingAlt) {
    if (c == 0x0D) {
      if (editingName) { if (editBuf.length()) { myName = editBuf; saveMyName(); } editingName = false; }
      if (editingAlt) { pressAltM = editBuf.toInt(); prefs.begin("wx", false); prefs.putInt("alt", pressAltM); prefs.end(); editingAlt = false; }
      needsRedraw = true;
    } else if (c == 0x1B) { editingName = editingAlt = false; needsRedraw = true; }
    else if (c == 0x08) { if (editBuf.length()) editBuf.remove(editBuf.length() - 1); needsRedraw = true; }
    else if (c >= 0x20 && c < 0x7F) {
      if (editingName && editBuf.length() < 15) editBuf += (char)c;
      if (editingAlt && editBuf.length() < 6 && (isdigit(c) || c == '-')) editBuf += (char)c;
      needsRedraw = true;
    }
    return;
  }
  switch (screen) {
    case SC_HOME:
    case SC_CHAT:
      if (c == 0x0C) { if (jpMode) imeCommitAll(); jpMode = !jpMode; needsRedraw = true; termDirty = true; }   // Alt+C
      else if (jpMode && imeHandleKey(c)) { needsRedraw = true; termDirty = true; }
      else if (c == 0x0D) { doSend(inputBuf); inputBuf = ""; needsRedraw = true; if (screen == SC_HOME) termDirty = true; }
      else if (c == 0x08) { if (inputBuf.length()) { popLastChar(inputBuf); needsRedraw = true; termDirty = true; } }
      else if (c == 0x09 && screen == SC_CHAT) {   // Tab: next peer
        if (chatPeerCount) {
          curPeer = (curPeer + 1) % chatPeerCount;
          chatPeers[curPeer].hasUnread = false;
          loadChatLines(curPeer);
          chatScroll = 0;
        }
        needsRedraw = true;
      }
      else if (c == 0x1B) { if (screen == SC_CHAT) goHome(); else inputBuf = ""; needsRedraw = true; termDirty = true; }
      else if (c >= 0x20 && c < 0x7F && inputBuf.length() < 180) { inputBuf += (char)c; needsRedraw = true; termDirty = true; }
      break;
    case SC_MUSIC:
      if (c == ' ') { musicTogglePause(); needsRedraw = true; }
      else if (c == 'n') musicNext();
      else if (c == 'p') musicPrev();
      else if (c == 0x1B) goHome();
      break;
    case SC_TRACKS:
      if (c == 0x0D) { musicPlay(trackSel); screen = SC_MUSIC; needsRedraw = true; }
      else if (c == 0x1B) { screen = SC_MUSIC; needsRedraw = true; }
      break;
    case SC_PEERS:
      if (c == 0x0D && peerSel >= 0 && peerSel < chatPeerCount) {
        curPeer = peerSel;
        chatPeers[curPeer].hasUnread = false;
        loadChatLines(curPeer);
        screen = SC_CHAT;
        needsRedraw = true;
      } else if (c == 0x1B) goHome();
      break;
    case SC_SET:
      if (c == 0x1B) goHome();
      break;
    case SC_TIMESET:
      if (c == 0x1B) { screen = SC_SET; needsRedraw = true; }
      break;
  }
}


// ---- trackball mouse cursor (insurance for a unit whose touch is flaky) ----
// Pref "sys"/"cur": 0 = Auto (cursor until the first real touch of this
// boot), 1 = On, 2 = Off. While active, the trackball moves the cursor
// and its press clicks at the cursor; the per-screen trackball jobs are
// replaced. The arrow is drawn straight onto the screen and erased by
// REPAINTING (never by restoring a saved background patch -- that scheme
// smeared stale pixels whenever the screen changed under the cursor).
// Every partial draw in this firmware fully repaints its own region, so
// re-drawing the arrow after each draw pass keeps it consistent.
#define CUR_W 12
#define CUR_H 14
static int curX = 154, curY = 113;
static bool curMoved = false;
static unsigned long lastTbMoveAt = 0, lastCurPaint = 0;
static bool cursorActive() { return cursorMode == 1 || (cursorMode == 0 && !touchSeen); }
static void cursorShow() {
  if (!cursorActive()) return;
  int x = curX, y = curY;
  Lcd.fillTriangle(x, y, x, y + 12, x + 5, y + 9, UI_WARN);
  Lcd.fillTriangle(x, y, x + 5, y + 9, x + 10, y + 9, UI_WARN);
  Lcd.fillRect(x + 4, y + 8, 3, 6, UI_WARN);
  Lcd.drawLine(x, y, x, y + 12, 0x0000);
  Lcd.drawLine(x, y, x + 10, y + 9, 0x0000);
}

// ---- touch routing ----
static void handleTouch(int tx, int ty) {
  switch (screen) {
    case SC_HOME:
      if (ty >= 26 && ty < 60) {
        if (tx >= 196 && tx < 234) { int gi = ensureGroupPeer(); curPeer = gi; if (gi >= 0) { chatPeers[gi].hasUnread = false; loadChatLines(gi); } screen = SC_PEERS; needsRedraw = true; }
        else if (tx >= 238 && tx < 276) { screen = SC_MUSIC; needsRedraw = true; }
        else if (tx >= 280) { screen = SC_SET; needsRedraw = true; }
      } else if (ty >= 150) { screen = SC_CHAT; if (curPeer < 0) { curPeer = ensureGroupPeer(); if (curPeer >= 0) loadChatLines(curPeer); } needsRedraw = true; }
      break;
    case SC_CHAT:
      if (ty < 24 && tx < 50) goHome();
      else if (cursorActive() && tx >= 288) {
        if (ty >= 60 && ty < 90) { chatScroll += 3; if (chatScroll > 20) chatScroll = 20; needsRedraw = true; }
        else if (ty >= 140 && ty < 170) { chatScroll -= 3; if (chatScroll < 0) chatScroll = 0; needsRedraw = true; }
      }
      break;
    case SC_PEERS:
      if (ty < 24 && tx < 50) { goHome(); break; }
      if (ty >= 28) {
        int i = (ty - 28) / 22;
        if (i >= 0 && i < chatPeerCount) {
          curPeer = i;
          chatPeers[i].hasUnread = false;
          loadChatLines(i);
          screen = SC_CHAT;
          needsRedraw = true;
        }
      }
      break;
    case SC_MUSIC:
      if (ty < 24 && tx < 50) { goHome(); break; }
      if (ty >= 96 && ty < 140) {
        if (tx < 108) musicPrev();
        else if (tx < 213) musicTogglePause();
        else musicNext();
        needsRedraw = true;
      } else if (ty >= 148 && ty < 182) {
        if (tx >= 48 && tx < 96) { musicVol = max(0, musicVol - 16); saveMusicVol(); applyGain(); needsRedraw = true; }
        else if (tx >= 252 && tx < 300) { musicVol = min(255, musicVol + 16); saveMusicVol(); applyGain(); needsRedraw = true; }
      } else if (ty >= 192) { screen = SC_TRACKS; trackSel = musicCur >= 0 ? (int)musicCur : 0; tracksTop = trackSel >= 4 ? trackSel - 3 : 0; needsRedraw = true; }
      break;
    case SC_TRACKS:
      if (ty < 24 && tx < 50) { screen = SC_MUSIC; needsRedraw = true; break; }
      if (ty >= 208) {
        if (tx < 160 && tracksTop > 0) { tracksTop = max(0, tracksTop - 4); needsRedraw = true; }
        else if (tx >= 160 && tracksTop + 4 < musicCount) { tracksTop += 4; needsRedraw = true; }
        break;
      }
      if (ty >= 34) {
        int off = ty - 34, r = off / 42;
        if (r >= 0 && r < 4 && (off % 42) < 38) {
          int idx = tracksTop + r;
          if (idx < musicCount) { trackSel = idx; musicPlay(idx); screen = SC_MUSIC; needsRedraw = true; }
        }
      }
      break;
    case SC_SET: {
      if (editingName || editingAlt) {
        int er = editingName ? 0 : 4;
        if (ty >= 28 && (ty - 28) / 26 == er) { editingName = editingAlt = false; needsRedraw = true; }
        break;
      }
      if (ty < 24 && tx < 50) { goHome(); break; }
      int rowIdx = (ty - 28) / 26;
      if (ty >= 28 && rowIdx >= 0 && rowIdx < 8) {
        if (rowIdx == 0) { editingName = true; editingAlt = false; editBuf = myName; }
        else if (rowIdx == 1) { if (chatOn) chatStop(); else chatStart(); }
        else if (rowIdx == 2) { chatChannel = chatChannel % 13 + 1; saveChatPrefs(); if (chatOn) { chatStop(); chatStart(); } }
        else if (rowIdx == 3) { chatLR = !chatLR; saveChatPrefs(); if (chatOn) applyChatProtocol(); }
        else if (rowIdx == 4) { editingAlt = true; editingName = false; editBuf = String(pressAltM); }
        else if (rowIdx == 5) {
          struct tm t; nowTm(t);
          tsF[0] = t.tm_year + 1900; tsF[1] = t.tm_mon + 1; tsF[2] = t.tm_mday;
          tsF[3] = t.tm_hour; tsF[4] = t.tm_min; tsF[5] = t.tm_sec;
          screen = SC_TIMESET;
        }
        else if (rowIdx == 6) {
          brightnessVal += 40; if (brightnessVal > 255) brightnessVal = 40;
          Lcd.setBrightness((uint8_t)brightnessVal);
          prefs.begin("sys", false); prefs.putInt("bright", brightnessVal); prefs.end();
        }
        else if (rowIdx == 7) {
          cursorMode = (cursorMode + 1) % 3;
          prefs.begin("sys", false); prefs.putInt("cur", cursorMode); prefs.end();
        }
        needsRedraw = true;
      }
      break;
    }
    case SC_TIMESET: {
      if (ty < 24 && tx < 50) { screen = SC_SET; needsRedraw = true; break; }
      if (ty >= 216) { applyTimeSet(); screen = SC_SET; needsRedraw = true; break; }
      int i = (ty - 32) / 30;
      if (i >= 0 && i < 6 && ((ty - 32) % 30) < 24) {
        static const int LIM[6][2] = {{2020, 2099}, {1, 12}, {1, 31}, {0, 23}, {0, 59}, {0, 59}};
        if (tx >= 120 && tx < 156) tsF[i]--;
        else if (tx >= 236 && tx < 272) tsF[i]++;
        if (tsF[i] < LIM[i][0]) tsF[i] = LIM[i][0];
        if (tsF[i] > LIM[i][1]) tsF[i] = LIM[i][1];
        needsRedraw = true;
      }
      break;
    }
  }
}
// ---- boot screen ----
static int bootY = 30;
static void bootType(const char* s, int x, int y, uint16_t color) {
  int cx = x;
  for (int i = 0; s[i]; i++) {
    char b[2] = { s[i], 0 };
    txt(b, cx, y, color, 1);
    cx += txtW(b, 1);
    delay(13 + ((i * 37 + (int)millis()) % 11));   // 13-23 ms per char, slightly uneven
  }
}
static void bootLine(const String& s) {
  bootType(s.c_str(), 12, bootY, UI_DIM);
  bootY += 16;
  delay(230);   // a beat before the next line starts
}
static void bootGibberish() {
  static const char CS[] = "abcdefghijklmnopqrstuvwxyz0123456789#%&@$?!/<>=+*";
  for (int ln = 0; ln < 2; ln++) {
    int cx = 12;
    int n = 22 + (int)(esp_random() % 7);
    for (int i = 0; i < n; i++) {
      char b[2] = { CS[esp_random() % (sizeof(CS) - 1)], 0 };
      txt(b, cx, bootY, UI_MUTE, 1);
      cx += txtW(b, 1);
      delay(3);
    }
    bootY += 16;
  }
}
static void bootTypeBig(const char* s, int x, int y, uint16_t color) {
  int cx = x;
  for (int i = 0; s[i]; i++) {
    char b[2] = { s[i], 0 };
    txt(b, cx, y, color, 2);
    cx += txtW(b, 2);
    delay(45);
  }
}
void setup() {
  // Shared SPI bus: deselect SD + LoRa BEFORE the display init, or their
  // floating CS lines garble the panel's init traffic and it stays black
  // (the official LilyGo_T_Deck profile does exactly this, plus a settle delay).
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_LORA_CS, OUTPUT);
  digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_POWERON, OUTPUT);
  digitalWrite(PIN_POWERON, HIGH);
  delay(500);
  pinMode(PIN_TB_R, INPUT_PULLUP);
  pinMode(PIN_TB_L, INPUT_PULLUP);
  pinMode(PIN_TB_U, INPUT_PULLUP);
  pinMode(PIN_TB_D, INPUT_PULLUP);
  pinMode(PIN_TB_PRESS, INPUT_PULLUP);
  analogSetAttenuation(ADC_11db);
  Serial.begin(115200);
  // saved settings first (Preferences needs no peripherals)
  loadChatPrefs();
  prefs.begin("sys", true); cursorMode = prefs.getInt("cur", 0); prefs.end();
  if (cursorMode < 0 || cursorMode > 2) cursorMode = 0;
  loadMusicVol();
  prefs.begin("sys", true); brightnessVal = prefs.getInt("bright", 200); prefs.end();
  prefs.begin("wx", true); pressAltM = prefs.getInt("alt", 0); prefs.end();
  setenv("TZ", "JST-9", 1); tzset();
  Lcd.begin(1, (uint8_t)brightnessVal);   // official init: CS pre-deselect + rotation 1
  Lcd.invertDisplay(true);               // this panel batch needs INVON, else colours are inverted
  Lcd.fillScreen(UI_BG);
  bootType("C:\\> BOOT  ENZ T-DECK v1.0", 12, 8, UI_ACCENT);
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  sdReady = SD.begin(PIN_SD_CS, SPI, 20000000);
  bootLine(sdReady ? "SD card ............ OK" : "SD card ............ NONE");
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);   // keyboard + touch + sensors all share this one bus/controller
  bool jp = jpInit();
  bootLine(jp ? "Japanese font ...... OK" : "Font: ASCII only");
  sensAHT = ahtInit();
  bool bme = bmeInit();
  if (sensorsPresent()) {
    sensorsPoll();
    prSample(true);
    bootLine(sensBME || sensBMP ? "Sensors ............ AHT+BME OK" : "Sensors ............ AHT OK");
  } else bootLine("Sensors ............ none (radio card)");
  loadMyName();
  chatStart();
  bootLine(chatOn ? "ESP-NOW ............ ON" : "ESP-NOW ............ FAIL");
  bool lr = loraInit();
  bootLine(lr ? "LoRa 921.0 SF8 ..... RX" : "LoRa ............... FAIL");
  i2sOut = new AudioOutputI2S();
  i2sOut->SetPinout(PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DOUT);
  applyGain();
  musicScan();
  xTaskCreatePinnedToCore(musicTask, "enzdec", 8192, nullptr, 2, nullptr, 1);
  int gi = ensureGroupPeer();
  curPeer = gi;
  if (gi >= 0) loadChatLines(gi);
  termInitFromLog();
  {
    char sb[48];
    snprintf(sb, sizeof(sb), "Starting %s OS...", myName.c_str());
    bootLine(sb);
  }
  delay(500);            // the OS is loading...
  bootLine("done.");
  bootY += 16;           // blank line
  bootGibberish();
  bootY += 8;
  bootTypeBig("Hellorld!", 12, bootY, UI_ACCENT);
  delay(1300);
  screen = SC_HOME;
  needsRedraw = true;
}
void loop() {
  // touch: one physical press = one event. The GT911 keeps re-latching
  // points while the finger is down and its status flag flickers between
  // polls, so a plain edge trigger refires like auto-repeat. A press only
  // ends after 60 ms of continuous no-touch.
  static bool touchDown = false;
  static unsigned long touchSeenAt = 0;
  int tx = 0, ty = 0;
  if (gtTouch(&tx, &ty)) {
    if (!touchSeen) { touchSeen = true; needsRedraw = true; }
    if (!touchDown) { handleTouch(tx, ty); touchDown = true; }
    touchSeenAt = millis();
  } else if (touchDown && millis() - touchSeenAt > 60) {
    touchDown = false;
  }
  // keyboard
  static unsigned long lastKb = 0;
  static int lastKbByte = 0;
  if (millis() - lastKb > 15) {
    lastKb = millis();
    int c = kbPoll();
    if (c > 0) { if (c != lastKbByte) handleKey(c); lastKbByte = c; }
    else if (c == 0) lastKbByte = 0;
  }
  // trackball: scroll chat / lists, press = select
  trackballPoll();
  if (tbDy != 0 || tbDx != 0) {
    if (cursorActive()) {
      curX += tbDx * 9; if (curX < 0) curX = 0; if (curX > SCR_W - CUR_W) curX = SCR_W - CUR_W;
      curY += tbDy * 9; if (curY < 0) curY = 0; if (curY > SCR_H - CUR_H) curY = SCR_H - CUR_H;
      curMoved = true;
      lastTbMoveAt = millis();
    } else if (screen == SC_HOME && tbDy != 0) {
      int ns = termScroll + tbDy;
      int mx = termCount > 2 ? termCount - 2 : 0;
      if (ns < 0) ns = 0; if (ns > mx) ns = mx;
      if (ns != termScroll) { termScroll = ns; termDirty = true; }
    }
    if (!cursorActive() && screen == SC_CHAT && tbDx != 0 && chatPeerCount) {
      curPeer = (curPeer + (tbDx > 0 ? 1 : chatPeerCount - 1)) % chatPeerCount;
      chatPeers[curPeer].hasUnread = false;
      loadChatLines(curPeer);
      chatScroll = 0;
      needsRedraw = true;
    }
    if (!cursorActive() && screen == SC_CHAT) { chatScroll += tbDy; if (chatScroll < 0) chatScroll = 0; if (chatScroll > 20) chatScroll = 20; needsRedraw = true; }
    else if (!cursorActive() && screen == SC_TRACKS) {
      trackSel += tbDy;
      if (trackSel < 0) trackSel = 0;
      if (trackSel >= musicCount) trackSel = max(0, musicCount - 1);
      if (trackSel < tracksTop) tracksTop = trackSel;
      if (trackSel >= tracksTop + 4) tracksTop = trackSel - 3;
      needsRedraw = true;
    } else if (!cursorActive() && screen == SC_PEERS) {
      peerSel += tbDy;
      if (peerSel < 0) peerSel = 0;
      if (peerSel >= chatPeerCount) peerSel = max(0, chatPeerCount - 1);
      needsRedraw = true;
    }
    tbDy = 0; tbDx = 0;
  }
  static bool wasTbPress = false;
  bool tbp = tbPressed();
  if (tbp && !wasTbPress) {
    if (cursorActive()) handleTouch(curX, curY);
    else if (screen == SC_PEERS) handleKey(0x0D);
    else if (screen == SC_TRACKS) handleKey(0x0D);
    else if (screen == SC_HOME) { screen = SC_CHAT; needsRedraw = true; }
  }
  wasTbPress = tbp;
  processChatRx();
  loraPoll();
  static unsigned long lastSens = 0;
  if (millis() - lastSens > 5000) { lastSens = millis(); sensorsPoll(); prSample(); }
  if (screen == SC_HOME) { homeTick(); cursorShow(); }
  if (screen == SC_CHAT && chatDirty) { chatDirty = false; needsRedraw = true; }
  if (screen == SC_MUSIC && musicPlaying) {
    static unsigned long lastUi = 0;
    if (millis() - lastUi > 500) { lastUi = millis(); updateMusicScreen(); }
  }
  if (needsRedraw) { drawCurrent(); needsRedraw = false; cursorShow(); }
  if (curMoved && cursorActive() && (millis() - lastCurPaint >= 40 || millis() - lastTbMoveAt >= 30)) {
    drawCurrent();
    cursorShow();
    curMoved = false;
    lastCurPaint = millis();
  }
  delay(4);
}
