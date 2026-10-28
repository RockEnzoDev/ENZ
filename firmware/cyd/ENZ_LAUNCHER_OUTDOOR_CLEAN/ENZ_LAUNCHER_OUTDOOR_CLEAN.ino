// ============================================================
//  ESP32 ENZ Multi-App Launcher - OUTDOOR / PORTABLE version
//  Portrait 240x320, sensors + always-on pressure graph, manual weather,
//  ESP-NOW only while Chat is open, no Games / Notes. Based on the home unit v1.0.
//  
// ============================================================
//
// REQUIRED LIBRARIES (Arduino Library Manager):
//   - TFT_eSPI by Bodmer
//   - XPT2046_Touchscreen by Paul Stoffregen
//   - ArduinoJson by Benoit Blanchon (v7+)
//
// TFT_eSPI SETUP (CRITICAL):
//   Edit User_Setup_Select.h in the TFT_eSPI library folder and
//   uncomment the line for the ENZ (ILI9341 240x320):
//     #include <User_Setups/Setup42_ILI9341_ESP32.h>
//   If yours is the ST7789 variant, pick the matching setup file.
//
// FILL IN THE CONFIG SECTION BELOW BEFORE FLASHING.
// ============================================================

#include <TFT_eSPI.h>
#if !defined(LOAD_FONT2) || !defined(LOAD_FONT4)
  #warning "The ENZ UI uses TFT_eSPI fonts 2 and 4: enable LOAD_FONT2 and LOAD_FONT4 in your User_Setup (Setup42_ILI9341_ESP32.h has them)."
#endif
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ---- Indoor sensor: AHT10 over I2C ----
// Driven directly over I2C: no extra library is needed (works with AHT10 and AHT20).
// Wiring on the ENZ CN1 connector (nearest the microSD slot):
//   AHT10 VIN -> 3V3 | GND -> GND | SDA -> GPIO27 | SCL -> GPIO22
// Set USE_INDOOR_SENSOR to 0 if you ever want to disable it.
#define USE_INDOOR_SENSOR 1
#define I2C_SDA 27
#define I2C_SCL 22
#if USE_INDOOR_SENSOR
  #include <Wire.h>
  uint8_t ahtAddr = 0x38;      // AHT10 default (0x39 is tried too)
  bool ahtFound = false;
#endif

// ---- Air pressure: BMP280 (or BME280) on the same two wires (address 0x76 or 0x77) ----
// Works with a stand-alone BMP280 module or an AHT20+BMP280 combo module. Tap the Room card for the 24 h graph.
#define ENABLE_PRESSURE 1
#define PRESSURE_ALT_M 0.0f        // default only: the live value is pressAltM (Setting -> Altitude), saved in flash
float pressAltM = PRESSURE_ALT_M;   // altitude in metres used for the sea-level pressure correction
#if ENABLE_PRESSURE && USE_INDOOR_SENSOR
  #define HAVE_BARO 1
#else
  #define HAVE_BARO 0
#endif

// ---- SD card + JPEG ----
#include "FS.h"
#include "SD.h"
// JPEG viewer needs the "TJpg_Decoder" library (by Bodmer). If you don't want
// it / haven't installed it, set this to 0 and the reader is text-only.
#define USE_JPEG_VIEWER 1
#if USE_JPEG_VIEWER
  #include <TJpg_Decoder.h>
#endif

// ========================= CONFIG ===========================
// WiFi credentials are entered on-device via the WiFi app and saved to flash.
// (No need to hardcode them here anymore.)

// YouTube Data API v3 key (https://console.cloud.google.com)
const char* YT_API_KEY    = "*****************";
const char* YT_CHANNEL_ID = "UC*****************"; // 24-char channel ID

// OPTIONAL: free CoinGecko Demo API key for reliable crypto data.
// Get one at coingecko.com/en/api/pricing -> Create Free Account.
// Leave "" to use the keyless public API (works but heavily rate-limited).
const char* CG_DEMO_KEY = "";

// Weather location (latitude / longitude). Default below; change it in Setting -> Location.
float WX_LAT = 35.6895;   // default: Tokyo Metropolitan Government Bldg (no personal location in the firmware)
float WX_LON = 139.6917;

// Timezone: Japan (JST = UTC+9, no DST)
long GMT_OFFSET_SEC = 9 * 3600;   // default: Japan Standard Time (UTC+9); changeable in Settings
int  DST_OFFSET_SEC = 0;          // daylight saving (0 = off); changeable in Settings
const char* NTP_SERVER    = "pool.ntp.org";
// ============================================================

// ENZ touchscreen pins
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define SCREEN_W 240
#define SCREEN_H 320

SPIClass touchSPI = SPIClass(HSPI);  // HSPI: keeps touch off the display's VSPI bus
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

// -------------------- App state --------------------
enum AppState { APP_MENU, APP_CLOCK, APP_LIGHT, APP_WIFI, APP_CHAT, APP_FRAME, APP_SETTINGS, APP_LOCATION, APP_NAME, APP_MUSIC };

// Set to 1 once you have a second ENZ to chat with; 0 keeps the messenger out of the menu.
#define ENABLE_CHAT 1
// ---- Optional features. If the board does not start, set these to 0 one at a time to find the cause. ----
#define ENABLE_JAPANESE 1   // Japanese text + kana input (needs jpfont16.bin on the SD card)
bool bootDone = false;       // set at the end of setup()
#define ENABLE_MUSIC 1      // WAV music player (set to 0 to remove it; speaker on GPIO26 via the on-board amplifier)
#if ENABLE_MUSIC
  #include <esp_idf_version.h>
  #if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
    #include "driver/dac_continuous.h"      // new DAC driver (ESP32 core 3.x). The legacy I2S driver must NOT be used here.
  #else
    #include <driver/i2s.h>                 // ESP32 core 2.x: built-in DAC through the I2S peripheral
  #endif
#endif

// Set to 0 to leave these apps out of the build (e.g. no crypto interest / no channel).

//ENABLE_BDAY 0 also stops birthday alerts popping up mid-recording.
#define ENABLE_PHOTOS 1

// ESP-NOW camera viewer (needs an ESP32-CAM running esp32cam_espnow_sender.ino).
// Requires USE_JPEG_VIEWER 1 (default). Set to 0 to leave the camera app out.

// Onboard RGB LED pins (setup() parks them off; the LED app itself was removed)
#define LED_R 4
#define LED_G 16
#define LED_B 17

AppState currentApp = APP_MENU;
bool needsRedraw = true;
#if ENABLE_CAMERA
#endif
unsigned long lastTouch = 0;
const unsigned long TOUCH_DEBOUNCE = 250;

// -------------------- Touch helpers --------------------
struct TouchPoint { int x; int y; bool valid; };

bool screenFlip = false;   // screen turned 180 degrees (Setting -> Display -> Rotate screen)

// Calibration: if buttons feel offset, tweak these min/max values.
TouchPoint readTouch() {
  TouchPoint p = {0, 0, false};
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point raw = ts.getPoint();
    p.x = map(raw.x, 200, 3700, 0, SCREEN_W);
    p.y = map(raw.y, 240, 3800, 0, SCREEN_H);
    p.x = constrain(p.x, 0, SCREEN_W - 1);
    p.y = constrain(p.y, 0, SCREEN_H - 1);
    if (screenFlip) { p.x = SCREEN_W - 1 - p.x; p.y = SCREEN_H - 1 - p.y; }   // matches the rotated display
    p.valid = true;
  }
  return p;
}

// -------------------- Back button (shared) --------------------
#define BACK_X 0
#define BACK_Y 0
#define BACK_W 60
#define BACK_H 28

// ============================================================
//  ENZ UI KIT  (palette, text, anti-aliased shapes)
// ============================================================
// 1 = anti-aliased rounded shapes (needs TFT_eSPI 2.4 or newer). Set to 0 if the build says
// "fillSmoothRoundRect was not declared" -> shapes fall back to plain rounded rectangles.
#define UI_SMOOTH 1

String brandName = "JohnDoe";  // launcher name (change: Setting -> Name)

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define UI_BG      RGB565(0, 0, 0)         // screen background (pure black)
#define UI_CARD    RGB565(4, 22, 10)      // card fill
#define UI_BORDER  RGB565(16, 70, 34)      // card outline
#define UI_TEXT    RGB565(180, 255, 200)   // main text
#define UI_DIM     RGB565(64, 200, 110)   // secondary text
#define UI_MUTE    RGB565(24, 110, 58)     // hints
#define UI_ACCENT  RGB565(64, 255, 128)     // cyan accent
#define UI_ACCENT2 RGB565(24, 160, 80)    // violet accent
#define UI_GHOST   RGB565(8, 26, 14)      // unlit clock segments
#define UI_GOOD    RGB565(70, 224, 140)
#define UI_WARN    RGB565(255, 176, 64)
#define UI_BAD     RGB565(255, 96, 96)
// Key / button colours
#define KC_NORMAL  RGB565(10, 34, 18)
#define KC_ACT     RGB565(16, 84, 40)
#define KC_GOOD    RGB565(20, 110, 52)
#define KC_BAD     RGB565(150, 50, 60)
#define KC_MUTE    RGB565(14, 40, 22)
#define KC_WARN    RGB565(205, 120, 30)

// Mix two RGB565 colours: t=0 -> a, t=255 -> b
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) {
  int r  = (((a >> 11) & 31) * (255 - t) + ((b >> 11) & 31) * t) / 255;
  int g  = (((a >> 5)  & 63) * (255 - t) + ((b >> 5)  & 63) * t) / 255;
  int bl = (( a        & 31) * (255 - t) + ( b        & 31) * t) / 255;
  return (r << 11) | (g << 5) | bl;
}

// ============================================================
//  JAPANESE TEXT SUPPORT
//  UTF-8 text is drawn with a 16px bitmap font read from the SD card (/jpfont16.bin, made with
//  make_jpfont.py). Without that file the launcher behaves as before (Japanese shows as boxes).
// ============================================================
#define JP_FONT_FILE "/jpfont16.bin"
extern bool sdReady;
static bool jpReady = false;
static File jpFile;
static uint16_t* jpCodes = nullptr;          // sorted Unicode code points that have a glyph
static uint32_t jpCount = 0, jpDataOff = 0;
struct JpCacheEnt { int32_t idx; uint8_t d[64]; };
static JpCacheEnt jpCache[24];               // small glyph cache (direct mapped)

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
static uint32_t u8next(const char*& p) {            // decode one character and advance
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
static void u8chop(String& s) {                      // remove the last character
  if (!s.length()) return;
  int i = s.length() - 1;
  while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
  s.remove(i);
}
static String u8cut(const String& s, unsigned maxBytes) {   // cut to maxBytes without splitting a character
  if (s.length() <= maxBytes) return s;
  unsigned n = maxBytes;
  while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
  return s.substring(0, n);
}
static bool jpHasNonAscii(const char* s) { while (*s) { if ((uint8_t)*s >= 0x80) return true; s++; } return false; }
static int jpCharW(uint32_t cp) { return (cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F)) ? 8 : 16; }
static int jpWidth(const char* s) { int w = 0; while (*s) w += jpCharW(u8next(s)); return w; }

// ---- glyph access ----
static int jpFind(uint32_t cp) {
  if (cp > 0xFFFF) return -1;
  int lo = 0, hi = (int)jpCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1; uint16_t c = jpCodes[mid];
    if (c == cp) return mid;
    if (c < cp) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}
static const uint8_t* jpGlyph(int idx) {             // 64 bytes: 16x16 px, 2 bits per pixel
  JpCacheEnt& e = jpCache[idx % 24];
  if (e.idx == idx) return e.d;
  jpFile.seek(jpDataOff + (uint32_t)idx * 64);
  if (jpFile.read(e.d, 64) != 64) return nullptr;
  e.idx = idx;
  return e.d;
}

// Draw one character with its top-left at (x,y). bg >= 0: anti-aliased against that colour;
// bg < 0: unknown background -> crisp (threshold) drawing with transparency. Returns the advance.
static int jpDrawChar(uint32_t cp, int x, int y, uint16_t fg, int32_t bg) {
  int w = jpCharW(cp);
  if (cp == ' ') return w;
  int idx = jpFind(cp);
  const uint8_t* g = (idx >= 0) ? jpGlyph(idx) : nullptr;
  if (!g) {                                           // no glyph: hollow box so the gap is visible
    if (cp >= 0x80) tft.drawRect(x + 1, y + 1, w - 2, 14, fg);
    return w;
  }
  uint16_t buf[16 * 16];
  uint16_t trans = (fg == 0x0001) ? 0x0002 : 0x0001;
  for (int row = 0; row < 16; row++) {
    for (int col = 0; col < w; col++) {
      int bit = (row * 16 + col) * 2;
      uint8_t a = (g[bit >> 3] >> (6 - (bit & 7))) & 3;
      uint16_t px;
      if (bg >= 0) px = (a == 0) ? (uint16_t)bg : (a == 3 ? fg : blend565((uint16_t)bg, fg, a * 85));
      else         px = (a >= 2) ? fg : trans;
      buf[row * w + col] = px;
    }
  }
  bool sb = tft.getSwapBytes();
  tft.setSwapBytes(true);                             // our buffer holds native RGB565 values -> TFT_eSPI must swap the bytes
  if (bg >= 0) tft.pushImage(x, y, w, 16, buf);
  else         tft.pushImage(x, y, w, 16, buf, trans);
  tft.setSwapBytes(sb);
  return w;
}

// Draw a UTF-8 string (16px high). maxW > 0 stops before the string gets wider. Returns the width drawn.
static int jpText(const char* s, int x, int y, uint16_t fg, int32_t bg = -1, int maxW = 0) {
  int cx = x;
  while (*s) {
    uint32_t cp = u8next(s);
    int w = jpCharW(cp);
    if (maxW && cx - x + w > maxW) break;
    cx += jpDrawChar(cp, cx, y, fg, bg);
  }
  return cx - x;
}

// Split text into lines of at most the given pixel widths (the first line may be narrower). Honors '\n'.
static void jpWrap(const String& txt, int firstW, int contW, String* out, int& n, int maxN) {
  n = 0;
  String cur; int w = 0, lim = firstW;
  const char* p = txt.c_str();
  while (*p && n < maxN) {
    const char* st = p;
    uint32_t cp = u8next(p);
    if (cp == '\n') { out[n++] = cur; cur = ""; w = 0; lim = contW; continue; }
    int cw = jpCharW(cp);
    if (w + cw > lim && cur.length()) { out[n++] = cur; cur = ""; w = 0; lim = contW; if (n >= maxN) break; }
    char tmp[5]; int len = p - st; if (len > 4) len = 4;
    memcpy(tmp, st, len); tmp[len] = 0;
    cur += tmp; w += cw;
  }
  if (n < maxN && (cur.length() || n == 0)) out[n++] = cur;
}

// ---- Text: TFT_eSPI built-in fonts passed per call (no font state to leak into other apps) ----
// font 2 = 16px (captions, labels)   font 4 = 26px (titles, values)
// Strings with Japanese (non-ASCII UTF-8) are drawn with the 16px bitmap font from the SD card.
static void uiText(const char* s, int x, int y, uint8_t datum, uint16_t fg, int font = 2, bool bold = false) {
  char tmp[96];
  if (jpHasNonAscii(s)) {
    if (jpReady) {
      int w = jpWidth(s), fh = (font == 4) ? 26 : 16;
      int lx = x, hh = datum % 3, vv = datum / 3, top = y;
      if (hh == 1) lx = x - w / 2; else if (hh == 2) lx = x - w;
      if (vv == 1) top = y - fh / 2; else if (vv == 2) top = y - fh;
      top += (fh - 16) / 2;
      jpText(s, lx, top, fg, -1);
      if (bold) jpText(s, lx + 1, top, fg, -1);
      return;
    }
    int n = 0;                                        // no font file: keep the ASCII part, '?' for the rest
    for (; *s && n < 95; s++) { uint8_t c = (uint8_t)*s; if (c < 0x80) tmp[n++] = c; else if (c >= 0xC0) tmp[n++] = '?'; }
    tmp[n] = 0; s = tmp;
  }
  tft.setTextDatum(datum);
  tft.setTextColor(fg);
  tft.drawString(s, x, y, font);
  if (bold) tft.drawString(s, x + 1, y, font);       // 1px offset = faux bold
  tft.setTextDatum(TL_DATUM);
}
static int uiW(const char* s, int font = 2) { if (jpReady && jpHasNonAscii(s)) return jpWidth(s); return tft.textWidth(s, font); }

// ---- Shapes ----
static void uiRRect(int x, int y, int w, int h, int r, uint16_t c, uint16_t bg) {   // filled rounded rect
#if UI_SMOOTH
  tft.fillSmoothRoundRect(x, y, w, h, r, c, bg);
#else
  tft.fillRoundRect(x, y, w, h, r, c);
#endif
}
static void uiDot(int x, int y, int r, uint16_t c, uint16_t bg) {                   // filled circle
#if UI_SMOOTH
  tft.fillSmoothCircle(x, y, r, c, bg);
#else
  tft.fillCircle(x, y, r, c);
#endif
}
static void uiCard(int x, int y, int w, int h, int r, uint16_t fill, uint16_t border, uint16_t bg) {   // card with 1px outline
  uiRRect(x, y, w, h, r, border, bg);
  uiRRect(x + 1, y + 1, w - 2, h - 2, r - 1, fill, border);
}
// Rounded tile with a vertical gradient (c1 top -> c2 bottom) and a soft highlight on top.
static void uiGradTile(int x, int y, int s, int r, uint16_t c1, uint16_t c2, uint16_t bg) {
  uiRRect(x, y, s, s, r, blend565(c1, c2, 128), bg);                                // outline + AA edge
  for (int i = 1; i < s - 1; i++) {
    float v = 0;
    if (i < r) v = r - (i + 0.5f); else if (i >= s - r) v = (i + 0.5f) - (s - r);
    int inset = 1;
    if (v > 0) inset = (int)ceilf(r - sqrtf((float)r * r - v * v)) + 1;
    uint16_t c = blend565(c1, c2, i * 255 / (s - 1));
    if (i == 1) c = blend565(c, TFT_WHITE, 70);                                      // top highlight
    tft.drawFastHLine(x + inset, y + i, s - 2 * inset, c);
  }
}
// Thin cyan->violet line (used under headers)
static void uiAccentLine(int x, int y, int w, uint8_t strength) {
  for (int i = 0; i < w; i++) tft.drawFastVLine(x + i, y, 1, blend565(blend565(UI_ACCENT, UI_ACCENT2, i * 255 / (w > 1 ? w - 1 : 1)), UI_BG, 255 - strength));
}

// ---- Shared Back button (all apps) ----
void drawBack() {
  uiCard(3, 3, 52, 22, 11, RGB565(6, 34, 14), RGB565(24, 120, 60), UI_BG);
  tft.drawLine(18, 8, 12, 14, UI_TEXT);  tft.drawLine(12, 14, 18, 20, UI_TEXT);
  tft.drawLine(19, 8, 13, 14, UI_TEXT);  tft.drawLine(13, 14, 19, 20, UI_TEXT);
  uiText("Back", 24, 6, TL_DATUM, UI_TEXT, 2);
}

// ---- Standard screen header for sub-screens: Back button + title + accent line ----
static void drawScreenFrame(const char* title) {
  tft.fillScreen(UI_BG);
  drawBack();
  uiText(title, 66, 1, TL_DATUM, UI_TEXT, 4, true);
  uiAccentLine(0, 32, SCREEN_W, 140);
}

bool hitBack(int x, int y) {
  return (x >= BACK_X && x < BACK_X + BACK_W && y >= BACK_Y && y < BACK_Y + BACK_H);
}

// Forward declarations
void drawMenu(); void handleMenu(int, int);
void drawClock(); void updateClock(); void handleClock(int, int);
void drawSensorsCard(); void drawSensorsValues(); void drawPressureCard(); void drawPressureValues();
void drawHomeChatTerm(); void drawHomeButtons(); bool termEnsurePeer(); void termCyclePeer(); void termOpenComposer();
extern bool bootWifi;                                        // defined with the WiFi section
static String uiFit(const String& s, int maxW, int font);   // defined in the UI helpers below
bool gHit(int tx, int ty, int x, int y, int w, int h) { return tx >= x && tx < x + w && ty >= y && ty < y + h; }
int clockScreen = 0;   // 0 = clock home, 1 = 5-day forecast, 2 = pressure graph (early for menu/Back)
int settingsScreen = 0;   // 0 = list, 1 = time zone (early for menu/Back)
void drawSettings(); void handleSettings(int, int); void loadTimezone();
void fetchWeather(); void readIndoor(); void initIndoor();
void drawLight(); void handleLight(int, int); void updateLight(); void autoBacklight();
void drawKey(int x, int y, int w, int h, const char* label, uint16_t bg);
void rotateScreen();
void drawPressure(); void pressureTick(); void prLoad();
static unsigned long lastPrDraw = 0;   // when the pressure graph was last drawn
#if ENABLE_MUSIC
extern bool musicPlaying; extern bool musicPaused;
void musicUpdate(); void musicScan(); void homeMusicToggle();
void loadMusicVol();
#else
#define musicPlaying false
#define musicPaused false
#endif
#define ROT_X (SCREEN_W - 64)   // rotate button (home screen, top-right)
#define ROT_Y 2
void drawWifi(); void handleWifi(int, int);
void drawWifiIcon(int, int, bool); void updateMenuWifiIcon();
void loadWifiCreds(); void connectSaved();
void initSD();
extern bool sdReady;   // defined with the SD code below (Setting shows the card status)
void drawChat(); void handleChat(int, int); void initChat(); void processChatRx(); void sendHello();
void chatStart(); void chatStop();
#define TERM_Y 192          // home chat terminal top edge
bool homeTerm = false;      // a chat session is running on the home screen terminal
String termPath = "";       // log file the terminal shows while no peer is selected
void drawPeerList(); void drawChatView(); void drawMsgKeyboard(); void drawMsgDelConfirm();
void drawMsgChannel(); void handleMsgChannel(int, int); void setChatChannel(int);
int  msgScreen = 0;    // 0=peers,1=chat,2=compose,3=nickname,4=delete-confirm,5=channel picker
int  chatManualCh = 0; // 0 = Auto (follow WiFi), 1..13 = fixed ESP-NOW channel
int  msgPendingDel = -1;
int  curPeer = -1;
bool chatLR = false;   // ESP-NOW Long Range mode (Settings > ESP-NOW); both units must match
void drawFrame(); void handleFrame(int, int); void updateSlideshow();
int  frameScreen = 0;  // 0=config, 1=running (early for the Back handler)

// -------------------- WiFi state (used across the sketch) --------------------
Preferences prefs;
String  savedSSID = "";
String  savedPass = "";
int     wifiScreen = 0;       // 0=network list, 1=keyboard, 2=result
int     wifiNetCount = 0;
bool    wifiResultOK = false;
String  kbSSID = "";          // chosen network
String  kbInput = "";         // password being typed
bool    kbUpper = false;      // shift
int     kbLayer = 0;          // 0=letters, 1=symbols

// =================== MAIN MENU ===================

// ---- Home-screen icons ----
// Icons are designed on a 24x24 grid and scaled by sc (sc=1 -> 24px, sc=2 -> 48px).
// fg = icon colour, bg = tile fill (used for cut-outs).
static void drawMenuIcon(AppState app, int ox, int oy, uint16_t fg, uint16_t bg, int sc) {
  auto X = [&](int v) { return ox + v * sc; };
  auto Y = [&](int v) { return oy + v * sc; };
  auto fr  = [&](int x, int y, int w, int h, uint16_t c) { tft.fillRect(X(x), Y(y), w * sc, h * sc, c); };
  auto hl  = [&](int x, int y, int w, uint16_t c) { tft.fillRect(X(x), Y(y), w * sc, sc, c); };
  auto frr = [&](int x, int y, int w, int h, int r, uint16_t c) { tft.fillRoundRect(X(x), Y(y), w * sc, h * sc, r * sc, c); };
  auto fc  = [&](int x, int y, int r, uint16_t c) { tft.fillCircle(X(x), Y(y), r * sc, c); };
  auto ft  = [&](int x1, int y1, int x2, int y2, int x3, int y3, uint16_t c) { tft.fillTriangle(X(x1), Y(y1), X(x2), Y(y2), X(x3), Y(y3), c); };
  auto ln  = [&](int x1, int y1, int x2, int y2, uint16_t c) {
    for (int a = 0; a < sc; a++) for (int b = 0; b < sc; b++) tft.drawLine(X(x1) + a, Y(y1) + b, X(x2) + a, Y(y2) + b, c);
  };
  auto rc  = [&](int x, int y, int r, uint16_t c) { for (int t = 0; t < sc; t++) tft.drawCircle(X(x), Y(y), r * sc - t, c); };
  auto ro  = [&](int x, int y, int w, int h, uint16_t c) { for (int t = 0; t < sc; t++) tft.drawRect(X(x) + t, Y(y) + t, w * sc - 2 * t, h * sc - 2 * t, c); };
  auto rro = [&](int x, int y, int w, int h, int r, uint16_t c) { for (int t = 0; t < sc; t++) tft.drawRoundRect(X(x) + t, Y(y) + t, w * sc - 2 * t, h * sc - 2 * t, r * sc, c); };

  switch (app) {
    case APP_CLOCK:
      rc(12, 12, 11, fg);
      ln(12, 12, 12, 5, fg); ln(12, 12, 17, 15, fg);
      break;
    case APP_LIGHT:
      fc(12, 12, 5, fg);
      for (int k = 0; k < 8; k++) {
        float a = k * 0.785398f;
        ln(12 + (int)roundf(cosf(a) * 8), 12 + (int)roundf(sinf(a) * 8),
           12 + (int)roundf(cosf(a) * 11), 12 + (int)roundf(sinf(a) * 11), fg);
      }
      break;
    case APP_WIFI:
      for (int r = 5; r <= 15; r += 5) {                      // three arcs, upper cone only
        for (int k = 0; k < 2; k++) {
          for (int a = -45; a <= 45; a += 2) {
            float rad = a * 0.0174533f;
            fr(12 + (int)roundf((r - k) * sinf(rad)), 20 - (int)roundf((r - k) * cosf(rad)), 1, 1, fg);
          }
        }
      }
      fc(12, 20, 2, fg);
      break;
    case APP_FRAME:
      ro(1, 3, 22, 18, fg);
      fc(7, 9, 2, fg);
      ft(3, 19, 10, 11, 16, 19, fg); ft(11, 19, 16, 14, 21, 19, fg);
      break;
    case APP_CHAT:
      frr(1, 3, 22, 14, 4, fg);
      ft(6, 16, 6, 22, 12, 16, fg);
      fc(7, 10, 1, bg); fc(12, 10, 1, bg); fc(17, 10, 1, bg);
      break;
    case APP_MUSIC:        // beamed notes
      fc(7, 18, 3, fg); fr(9, 5, 2, 13, fg);
      fc(17, 16, 3, fg); fr(19, 3, 2, 13, fg);
      ft(9, 5, 21, 3, 21, 8, fg); ft(9, 5, 21, 8, 9, 10, fg);
      break;
    case APP_NAME:         // label / badge
      frr(2, 5, 20, 14, 3, fg);
      fr(6, 9, 12, 2, bg); fr(6, 13, 8, 2, bg);
      break;
    case APP_LOCATION:     // map pin
      fc(12, 9, 7, fg);
      ft(5, 12, 19, 12, 12, 23, fg);
      fc(12, 9, 3, bg);
      break;
    case APP_SETTINGS:
      for (int k = 0; k < 8; k++) {
        float a = k * 0.785398f;
        fc(12 + (int)roundf(cosf(a) * 9), 12 + (int)roundf(sinf(a) * 9), 2, fg);   // gear teeth
      }
      fc(12, 12, 8, fg); fc(12, 12, 3, bg);
      break;
    default:
      frr(4, 4, 16, 16, 3, fg);
      break;
  }
}

// ---- App colours (gradient top -> bottom) ----
static void appColors(AppState a, uint16_t &c1, uint16_t &c2) {
  switch (a) {
    case APP_FRAME:    c1 = RGB565(150, 255, 170); c2 = RGB565(50, 190, 90);   break;
    case APP_CHAT:     c1 = RGB565(80, 230, 160);  c2 = RGB565(30, 170, 112);  break;
    case APP_SETTINGS: c1 = RGB565(140, 255, 190); c2 = RGB565(70, 170, 110);  break;
    case APP_WIFI:     c1 = RGB565(64, 255, 128);  c2 = RGB565(24, 160, 80);   break;
    case APP_CLOCK:    c1 = RGB565(110, 255, 150); c2 = RGB565(45, 185, 95);   break;   // time zone
    case APP_LOCATION: c1 = RGB565(80, 230, 160);  c2 = RGB565(30, 170, 112);  break;
    case APP_NAME:     c1 = RGB565(190, 255, 140); c2 = RGB565(100, 190, 60);  break;
    case APP_MUSIC:    c1 = RGB565(96, 255, 180);  c2 = RGB565(36, 175, 105);  break;
    case APP_LIGHT:    c1 = RGB565(200, 255, 130); c2 = RGB565(110, 200, 70);  break;
    default:           c1 = RGB565(90, 160, 110);  c2 = RGB565(50, 110, 70);   break;
  }
}

// Gradient app tile (s px) with the white 24px icon in the middle
static void drawAppTile(AppState a, int x, int y, int s, int r, uint16_t bg) {
  uint16_t c1, c2;
  appColors(a, c1, c2);
  uiGradTile(x, y, s, r, c1, c2, bg);
  drawMenuIcon(a, x + (s - 24) / 2, y + (s - 24) / 2, TFT_WHITE, blend565(c1, c2, 128), 1);
}

// ---- Dock: one row of app icons along the bottom of the home screen ----
#define DOCK_Y 180
#define DOCK_H 58
#define DOCK_BG RGB565(3, 14, 7)
void drawMenu() { drawClock(); }

// ---- Boot splash: the launcher name drawn with a geometric stroke font ----
// Glyphs live on a 3x5 node grid: nodes are "cr" (column 0-2, row 0-4); strokes are separated by '/'.
static const char* brandGlyph(char ch) {
  static const char* const AZ[26] = {
    "04 01 10 21 24/02 22",              // A
    "00 04/00 10 21 12 02/02 12 23 14 04",// B
    "21 10 01 03 14 23",                 // C
    "00 04/00 10 21 23 14 04",           // D
    "20 00 04 24/02 12",                 // E
    "20 00 04/02 12",                    // F
    "21 10 01 03 14 23 22 12",           // G
    "00 04/20 24/02 22",                 // H
    "00 20/10 14/04 24",                 // I
    "20 23 14 03",                       // J
    "00 04/20 02 24",                    // K
    "00 04 24",                          // L
    "04 00 12 20 24",                    // M
    "04 00 24 20",                       // N
    "10 01 03 14 23 21 10",              // O
    "04 00 10 21 12 02",                 // P
    "10 01 03 14 23 21 10/13 24",        // Q
    "04 00 10 21 12 02/12 24",           // R
    "21 10 01 12 23 14 03",              // S
    "00 20/10 14",                       // T
    "00 03 14 23 20",                    // U
    "00 14 20",                          // V
    "00 04 12 24 20",                    // W
    "00 24/20 04",                       // X
    "00 12 20/12 14",                    // Y
    "00 20 04 24",                       // Z
  };
  static const char* const DIG[10] = {
    "10 01 03 14 23 21 10/03 21", "01 10 14/04 24", "01 10 21 04 24", "00 20 24 04/12 22", "00 02 22/20 24",
    "20 00 02 22 24 04", "20 02 04 24 22 02", "00 20 14", "00 20 24 04 00/02 22", "22 02 00 20 24 04"
  };
  if (ch >= 'A' && ch <= 'Z') return AZ[ch - 'A'];
  if (ch >= '0' && ch <= '9') return DIG[ch - '0'];
  if (ch == '-') return "02 22";
  if (ch == '.') return "14";
  return "";
}

// DOS-style boot log (outdoor version design)
static const uint16_t BOOT_GREEN = RGB565(64, 255, 128);
static int bootLineY = 0;
static void bootBar(int pct) {
  const int x = 14, y = SCREEN_H - 44, seg = 16, cw = 7;
  tft.fillRect(x, y - 2, SCREEN_W - x - 6, 22, UI_BG);   // erase the previous value (text is transparent)
  uiText("[", x, y, TL_DATUM, BOOT_GREEN, 2, true);
  int filled = seg * pct / 100;
  for (int i = 0; i < seg; i++)
    tft.fillRect(x + 10 + i * cw, y + 2, cw - 1, 12, i < filled ? BOOT_GREEN : UI_GHOST);
  int rx = x + 10 + seg * cw + 2;
  uiText("]", rx, y, TL_DATUM, BOOT_GREEN, 2, true);
  char b[16];
  snprintf(b, sizeof(b), "LOAD: %d%%", pct);
  uiText(b, rx + 12, y, TL_DATUM, BOOT_GREEN, 2);
}
static void bootLine(const char* s) {
  uiText(s, 14, bootLineY, TL_DATUM, BOOT_GREEN, 2);
  bootLineY += 20;
}
void drawSplash() {
  tft.fillScreen(UI_BG);
  uiText("C:\\> BOOT", 14, 12, TL_DATUM, BOOT_GREEN, 4, true);
  bootLineY = 64;
  bootBar(0);
}
void drawSplashProgress(int pct) { bootBar(pct); }

void handleMenu(int tx, int ty) {
  if (clockScreen != 0) { handleClock(tx, ty); return; }   // forecast / pressure sub-screen
  // Rotate button (top right)
  if (tx >= ROT_X - 6 && tx < ROT_X + 28 && ty < 26) { rotateScreen(); return; }
  // Icon buttons (top right): Photo / Music / Settings
  if (ty >= 30 && ty < 60) {
    if (tx >= 138 && tx < 168) { if (homeTerm) chatStop(); currentApp = APP_FRAME; frameScreen = 0; needsRedraw = true; return; }
    if (tx >= 173 && tx < 203) {
#if ENABLE_MUSIC
      homeMusicToggle();
      drawHomeButtons();
      drawHomeNote();
#endif
      return;
    }
    if (tx >= 208 && tx < 238) { if (homeTerm) chatStop(); currentApp = APP_SETTINGS; settingsScreen = 0; needsRedraw = true; return; }
  }
  // Date + weather line = manual fetch
  if (ty >= 74 && ty < 94) { manualWeatherFetch(); return; }
  // Pressure card = full-screen graph
  if (tx >= 124 && tx < SCREEN_W - 8 && ty >= 100 && ty < 184) { clockScreen = 2; drawPressure(); return; }
#if ENABLE_CHAT
  // Chat terminal (lower half of the home screen)
  if (tx >= 8 && tx < SCREEN_W - 8 && ty >= TERM_Y && ty < TERM_Y + 120) {
    if (homeTerm && tx >= SCREEN_W - 64 && ty >= TERM_Y + 94) { chatStop(); drawClock(); return; }   // [END]
    if (!homeTerm) {
      chatStart(); homeTerm = true;
      bool ok = termEnsurePeer();
      if (ok && ty >= TERM_Y + 94) { termOpenComposer(); currentApp = APP_CHAT; needsRedraw = true; }
      else drawClock();
      return;
    }
    if (ty < TERM_Y + 22) { termCyclePeer(); drawHomeChatTerm(); return; }                          // header = next peer
    if (curPeer < 0) termEnsurePeer();
    if (curPeer >= 0) {   // prompt / log = write a message; the composer returns here
      termOpenComposer();
      currentApp = APP_CHAT; needsRedraw = true;
    }
    return;
  }
#endif
}

// =================== CLOCK (time + weather + room) ===================
// Weather state (Open-Meteo)
float wxTemp = 0; int wxHum = 0; int wxCode = -1; bool wxIsDay = true; bool wxOK = false;
unsigned long lastWxFetch = 0;
// Indoor state
float inTemp = 0, inHum = 0; bool inOK = false;
float inPress = 0; bool pressOK = false; bool inHumOK = false;   // air pressure (hPa) / is a humidity value available?
unsigned long lastInRead = 0;
int  lastDateDay = -1;

// Palette helpers (565)
#define COL_GREY   0x8410
#define COL_DKGREY 0x3186
#define COL_COND   0xB596   // muted condition text
#define COL_INHUM  0x6F16   // indoor humidity (green)

// ---- Indoor sensor ----
#if USE_INDOOR_SENSOR
static bool ahtAck(uint8_t addr) { Wire.beginTransmission(addr); return Wire.endTransmission() == 0; }
static bool ahtCmd(uint8_t c0, uint8_t c1, uint8_t c2) {
  Wire.beginTransmission(ahtAddr); Wire.write(c0); Wire.write(c1); Wire.write(c2);
  return Wire.endTransmission() == 0;
}
static int ahtStatus() { if (Wire.requestFrom((int)ahtAddr, 1) != 1) return -1; return Wire.read(); }

static void i2cStart() {
  static bool started = false;
  if (!started) { Wire.begin(I2C_SDA, I2C_SCL); Wire.setClock(100000); started = true; }
}
static bool ahtBegin() {
  i2cStart();
  if      (ahtAck(0x38)) ahtAddr = 0x38;
  else if (ahtAck(0x39)) ahtAddr = 0x39;     // AHT10 with its address pin pulled high
  else return false;                         // nothing answers on the bus -> check wiring
  Wire.beginTransmission(ahtAddr); Wire.write(0xBA); Wire.endTransmission();   // soft reset
  delay(25);
  ahtCmd(0xE1, 0x08, 0x00);                  // AHT10 init / calibration
  delay(15);
  int st = ahtStatus();
  if (st >= 0 && !(st & 0x08)) { ahtCmd(0xBE, 0x08, 0x00); delay(15); }   // AHT20-style init as a fallback
  return true;
}

static bool ahtMeasure(float &t, float &h) {
  if (!ahtCmd(0xAC, 0x33, 0x00)) return false;     // trigger a measurement
  delay(80);
  for (int i = 0; i < 10; i++) { int st = ahtStatus(); if (st >= 0 && !(st & 0x80)) break; delay(10); }   // wait while busy
  if (Wire.requestFrom((int)ahtAddr, 6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  if (d[0] & 0x80) return false;                   // still busy
  uint32_t rh = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t rt = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  h = rh * 100.0f / 1048576.0f;
  t = rt * 200.0f / 1048576.0f - 50.0f;
  return (h >= 0 && h <= 100 && t > -41 && t < 86);
}
#endif

// ---- Air pressure: BMP280 / BME280 (I2C address 0x76 or 0x77, same bus as the AHT sensor) ----
#if HAVE_BARO
static uint8_t  bmpAddr = 0x76;
static bool     bmpFound = false;
static uint16_t bT1, bP1; static int16_t bT2, bT3, bP2, bP3, bP4, bP5, bP6, bP7, bP8, bP9;
static float    bmpTemp = 0;
static unsigned long lastBmpMs = 0;

static bool bmpWriteReg(uint8_t r, uint8_t v) { Wire.beginTransmission(bmpAddr); Wire.write(r); Wire.write(v); return Wire.endTransmission() == 0; }
static bool bmpReadRegs(uint8_t r, uint8_t* buf, int n) {
  Wire.beginTransmission(bmpAddr); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return false;           // repeated start
  if (Wire.requestFrom((int)bmpAddr, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
static bool bmpBegin() {
  i2cStart();
  const uint8_t addrs[2] = { 0x76, 0x77 };
  for (int a = 0; a < 2; a++) {
    bmpAddr = addrs[a];
    uint8_t id = 0;
    if (!bmpReadRegs(0xD0, &id, 1)) continue;
    if (id != 0x58 && id != 0x56 && id != 0x57 && id != 0x60) continue;        // BMP280 (0x56-0x58) or BME280 (0x60)
    bmpWriteReg(0xE0, 0xB6); delay(10);                                         // soft reset
    uint8_t c[24];
    if (!bmpReadRegs(0x88, c, 24)) continue;                                    // factory calibration
    bT1 = c[0] | (c[1] << 8);  bT2 = (int16_t)(c[2] | (c[3] << 8));  bT3 = (int16_t)(c[4] | (c[5] << 8));
    bP1 = c[6] | (c[7] << 8);  bP2 = (int16_t)(c[8] | (c[9] << 8));  bP3 = (int16_t)(c[10] | (c[11] << 8));
    bP4 = (int16_t)(c[12] | (c[13] << 8)); bP5 = (int16_t)(c[14] | (c[15] << 8)); bP6 = (int16_t)(c[16] | (c[17] << 8));
    bP7 = (int16_t)(c[18] | (c[19] << 8)); bP8 = (int16_t)(c[20] | (c[21] << 8)); bP9 = (int16_t)(c[22] | (c[23] << 8));
    bmpWriteReg(0xF5, 0x00);                                                    // no filter
    return bP1 != 0;
  }
  return false;
}
// One forced measurement (temperature x2, pressure x16). Returns false if the sensor does not answer.
static bool bmpMeasure(float &tC, float &pHpa) {
  if (!bmpWriteReg(0xF4, (2 << 5) | (5 << 2) | 1)) return false;
  delay(45);
  for (int i = 0; i < 10; i++) { uint8_t st; if (!bmpReadRegs(0xF3, &st, 1)) return false; if (!(st & 0x08)) break; delay(5); }
  uint8_t d[6];
  if (!bmpReadRegs(0xF7, d, 6)) return false;
  int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
  if (adcP == 0x80000 || adcT == 0x80000) return false;                         // measurement was skipped
  // Bosch compensation formulas (datasheet, 32-bit temperature / 64-bit pressure)
  int32_t v1 = ((((adcT >> 3) - ((int32_t)bT1 << 1))) * ((int32_t)bT2)) >> 11;
  int32_t v2 = (((((adcT >> 4) - ((int32_t)bT1)) * ((adcT >> 4) - ((int32_t)bT1))) >> 12) * ((int32_t)bT3)) >> 14;
  int32_t tFine = v1 + v2;
  tC = ((tFine * 5 + 128) >> 8) / 100.0f;
  int64_t p1 = (int64_t)tFine - 128000;
  int64_t p2 = p1 * p1 * (int64_t)bP6;
  p2 = p2 + (p1 * (int64_t)bP5) * 131072LL;                                      // (x << 17), written as a multiplication: x may be negative
  p2 = p2 + ((int64_t)bP4) * 34359738368LL;                                      // << 35
  p1 = ((p1 * p1 * (int64_t)bP3) >> 8) + (p1 * (int64_t)bP2) * 4096LL;           // << 12
  p1 = ((140737488355328LL + p1) * ((int64_t)bP1)) >> 33;                        // (1 << 47) + p1
  if (p1 == 0) return false;
  int64_t p = 1048576 - adcP;
  p = ((p * 2147483648LL - p2) * 3125) / p1;                                     // p << 31
  p1 = (((int64_t)bP9) * (p >> 13) * (p >> 13)) >> 25;
  p2 = (((int64_t)bP8) * p) >> 19;
  p = ((p + p1 + p2) >> 8) + ((int64_t)bP7) * 16;                                // << 4
  float pa = p / 256.0f;
  pHpa = pa / 100.0f;
  if (pressAltM != 0.0f) pHpa = pHpa / powf(1.0f - pressAltM / 44330.0f, 5.255f);   // station -> sea-level pressure
  return (pHpa > 300.0f && pHpa < 1100.0f && tC > -41.0f && tC < 86.0f);
}
// Measure now and store the result in the globals (inPress / pressOK). Cheap enough to call every few seconds.
static bool bmpPoll() {
  if (!bmpFound) bmpFound = bmpBegin();
  if (!bmpFound) { pressOK = false; return false; }
  float t, p;
  if (bmpMeasure(t, p)) { bmpTemp = t; inPress = p; pressOK = true; lastBmpMs = millis(); return true; }
  bmpFound = false; pressOK = false;                                            // sensor lost -> look for it again next time
  return false;
}
#endif

void initIndoor() {
#if USE_INDOOR_SENSOR
  ahtFound = ahtBegin();         // false if no sensor responds on the bus
#if HAVE_BARO
  bmpFound = bmpBegin();         // BMP280 / BME280 (pressure)
#endif
#endif
}
void readIndoor() {
  lastInRead = millis();
#if USE_INDOOR_SENSOR
  bool ahtOk = false; float t = 0, h = 0;
  if (!ahtFound) ahtFound = ahtBegin();                    // retry, e.g. sensor plugged in later
  if (ahtFound) { if (ahtMeasure(t, h)) ahtOk = true; else ahtFound = false; }   // re-initialise on the next read if it fails
#if HAVE_BARO
  bool bmpOk = bmpPoll();
#else
  bool bmpOk = false;
#endif
  if (ahtOk)       { inTemp = t; inHum = h; inHumOK = true;  inOK = true; }
  else if (bmpOk)  { inTemp = bmpTemp; inHumOK = false;      inOK = true; }      // pressure-only sensor: temperature from the BMP280
  else             { inOK = false; inHumOK = false; }
#else
  inOK = false;
#endif
}

// ---- Open-Meteo fetch ----
void fetchWeather() {
  lastWxFetch = millis();
  wxOK = false;
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setUserAgent("ESP32-ENZ/1.0");
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(WX_LAT, 4); url += "&longitude="; url += String(WX_LON, 4);
  url += "&current=temperature_2m,relative_humidity_2m,weather_code,is_day";
  http.begin(client, url); http.setTimeout(12000);
  if (http.GET() == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonObject cur = doc["current"];
      if (!cur.isNull()) {
        wxTemp  = cur["temperature_2m"].as<float>();
        wxHum   = cur["relative_humidity_2m"].as<int>();
        wxCode  = cur["weather_code"].as<int>();
        wxIsDay = cur["is_day"].as<int>() == 1;
        wxOK = true;
      }
    }
  }
  http.end();
}

const char* wxText(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow showers";
  if (code >= 95) return "Thunderstorm";
  return "--";
}

// ---- Weather icons (hand-drawn, drawn on card colour icBg) ----
static uint16_t icBg = UI_CARD;
#define IC_SUN   RGB565(255, 200, 60)
#define IC_CLOUD RGB565(206, 216, 236)
#define IC_DROP  RGB565(96, 176, 255)
#define IC_MOON  RGB565(226, 232, 246)
void icSun(int cx, int cy) {
  for (int a = 0; a < 360; a += 45) {
    float r = a * 0.01745329;
    int x1 = cx + cos(r) * 15, y1 = cy + sin(r) * 15, x2 = cx + cos(r) * 21, y2 = cy + sin(r) * 21;
    tft.drawLine(x1, y1, x2, y2, IC_SUN); tft.drawLine(x1 + 1, y1, x2 + 1, y2, IC_SUN);
  }
  tft.fillCircle(cx, cy, 11, IC_SUN);
}
void icMoon(int cx, int cy) {
  tft.fillCircle(cx, cy, 13, IC_MOON);
  tft.fillCircle(cx + 6, cy - 4, 12, icBg);
}
void icCloud(int cx, int cy, uint16_t c) {
  tft.fillCircle(cx - 11, cy + 2, 9, c);
  tft.fillCircle(cx, cy - 5, 12, c);
  tft.fillCircle(cx + 12, cy + 1, 10, c);
  tft.fillRect(cx - 11, cy + 2, 24, 10, c);
}
void icPartly(int cx, int cy, bool day) {
  if (day) {
    for (int a = 180; a < 330; a += 45) {
      float r = a * 0.01745329;
      int x1 = cx - 8 + cos(r) * 11, y1 = cy - 8 + sin(r) * 11, x2 = cx - 8 + cos(r) * 16, y2 = cy - 8 + sin(r) * 16;
      tft.drawLine(x1, y1, x2, y2, IC_SUN); tft.drawLine(x1 + 1, y1, x2 + 1, y2, IC_SUN);
    }
    tft.fillCircle(cx - 8, cy - 8, 8, IC_SUN);
  } else {
    tft.fillCircle(cx - 8, cy - 9, 8, IC_MOON);
    tft.fillCircle(cx - 4, cy - 12, 7, icBg);
  }
  icCloud(cx + 3, cy + 4, IC_CLOUD);
}
void icRain(int cx, int cy) {
  icCloud(cx, cy - 3, IC_CLOUD);
  for (int i = -1; i <= 1; i++) {
    tft.drawLine(cx + i * 9, cy + 14, cx + i * 9 - 2, cy + 22, IC_DROP);
    tft.drawLine(cx + i * 9 + 1, cy + 14, cx + i * 9 - 1, cy + 22, IC_DROP);
  }
}
void icSnow(int cx, int cy) {
  icCloud(cx, cy - 3, IC_CLOUD);
  for (int i = -1; i <= 1; i++) tft.fillCircle(cx + i * 9, cy + 18, 2, TFT_WHITE);
}
void icThunder(int cx, int cy) {
  icCloud(cx, cy - 3, RGB565(160, 172, 200));
  tft.fillTriangle(cx - 2, cy + 12, cx + 5, cy + 12, cx - 3, cy + 22, IC_SUN);
  tft.fillTriangle(cx + 3, cy + 14, cx + 6, cy + 14, cx, cy + 24, IC_SUN);
}
void icFog(int cx, int cy) {
  icCloud(cx, cy - 5, IC_CLOUD);
  for (int i = 0; i < 3; i++) tft.drawFastHLine(cx - 12, cy + 12 + i * 5, 24, UI_DIM);
}

// Short condition names for the cards
static const char* wxShort(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2) return "Partly";
  if (code == 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow";
  if (code >= 95) return "Storm";
  return "--";
}

// "22" + degree ring + "C"  (big value in font 4). Returns the x where it ends.
static int uiTemp(int x, int y, float t, uint16_t col) {
  char b[8];
  snprintf(b, sizeof b, "%d", (int)roundf(t));
  uiText(b, x, y, TL_DATUM, col, 4, true);
  int w = uiW(b, 4) + 2;
  tft.drawCircle(x + w + 4, y + 6, 3, col); tft.drawCircle(x + w + 4, y + 6, 2, col);
  uiText("C", x + w + 10, y + 7, TL_DATUM, UI_DIM, 2);
  return x + w + 10 + uiW("C", 2);
}
static void uiDroplet(int x, int y, uint16_t c) {            // small water drop, (x,y) = centre
  tft.fillCircle(x, y + 2, 3, c);
  tft.fillTriangle(x - 3, y + 1, x + 3, y + 1, x, y - 5, c);
}

// ---- Cards ----
#define CARD_Y 102
#define CARD_H 72
#define CARD_W 148
#define CARD_L 8
#define CARD_R 164

// ---- 5-day forecast (tap the Outside panel on the clock to open) ----

// Centered number + small degree circle (no "C", to fit narrow columns)

// ---- Big 7-segment clock (rounded "pill" segments, ghost segments when unlit) ----
#define CK_DW 25
#define CK_DH 44
#define CK_T  6
#define CK_Y  28
#define CK_X0 8
static const uint8_t CK_SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };   // bits: a b c d e f g
static int ckLast[4] = { -2, -2, -2, -2 };
static int ckLastColon = -1;

static void ckSeg(int x, int y, int w, int h, bool on) { uiRRect(x, y, w, h, (w < h ? w : h) / 2, on ? UI_ACCENT : UI_GHOST, UI_BG); }
static void ckDigit(int x, int d) {              // d = 0..9, or -1 for an empty (all ghost) digit
  uint8_t m = (d >= 0) ? CK_SEG[d] : 0;
  const int w = CK_DW, h = CK_DH, t = CK_T, g = 2, y = CK_Y;
  int half = (h - t) / 2, hl = w - t - 2 * g, vl = half - 2 * g;
  ckSeg(x + t / 2 + g, y,                       hl, t, m & 1);     // a
  ckSeg(x + w - t,     y + t / 2 + g,           t, vl, m & 2);     // b
  ckSeg(x + w - t,     y + half + t / 2 + g,    t, vl, m & 4);     // c
  ckSeg(x + t / 2 + g, y + h - t,               hl, t, m & 8);     // d
  ckSeg(x,             y + half + t / 2 + g,    t, vl, m & 16);    // e
  ckSeg(x,             y + t / 2 + g,           t, vl, m & 32);    // f
  ckSeg(x + t / 2 + g, y + half,                hl, t, m & 64);    // g
}
static void ckColon(bool on) {
  int cx = CK_X0 + 62;
  uint16_t c = on ? UI_ACCENT : UI_GHOST;
  uiRRect(cx - 4, CK_Y + 10, 8, 8, 3, c, UI_BG);
  uiRRect(cx - 4, CK_Y + 26, 8, 8, 3, c, UI_BG);
}
static void drawBigTime(int hh, int mm, bool colon, bool force) {   // hh = -1 -> blank "--:--"
  const int xs[4] = { CK_X0, CK_X0 + 29, CK_X0 + 71, CK_X0 + 100 };
  int d[4] = { hh < 0 ? -1 : hh / 10, hh < 0 ? -1 : hh % 10, mm < 0 ? -1 : mm / 10, mm < 0 ? -1 : mm % 10 };
  for (int i = 0; i < 4; i++) if (force || d[i] != ckLast[i]) { ckDigit(xs[i], d[i]); ckLast[i] = d[i]; }
  if (force || (int)colon != ckLastColon) { ckColon(colon); ckLastColon = colon; }
}

// Rotate button icon (circular arrow), 24x18 box at the top-right of the home screen
static void drawRotateIcon(int x, int y, uint16_t col) {
  int cx = x + 12, cy = y + 9;
  for (int a = 20; a <= 300; a += 7) {                         // open circle
    float r = a * 0.0174533f;
    tft.fillRect(cx + (int)roundf(7 * cosf(r)) - 1, cy - (int)roundf(7 * sinf(r)) - 1, 2, 2, col);
  }
  tft.fillTriangle(cx + 4, cy - 8, cx + 11, cy - 5, cx + 4, cy - 1, col);   // arrow head at the open end
}

// Status bar: date on the left (WiFi icon is drawn by drawWifiIcon on the right)
static void drawClockDate(struct tm* t) {
  char buf[28];
  strftime(buf, sizeof(buf), "%Y/%m/%d (%a)", t);
  tft.fillRect(0, 74, 120, 18, UI_BG);
  uiText(buf, 8, 76, TL_DATUM, UI_DIM, 2);
}

static bool homeNoteShown = false;
static void drawHomeNote() {
  homeNoteShown = musicPlaying && !musicPaused;
  tft.fillRect(140, 0, 30, 24, UI_BG);
  if (homeNoteShown) drawMenuIcon(APP_MUSIC, 144, 0, UI_ACCENT, UI_BG, 1);
}

// =================== HOME (outdoor): weather strip, Sensors/Pressure cards,
// comic-chat preview and Photo / Music / Settings buttons ===================
#define HOME_CARD_Y  100
#define HOME_LOW_Y   228
static String wxMsg = "";                    // feedback line for manual fetches

static void drawWxStrip() {   // weather on the date line (right side); tap the line to fetch
  tft.fillRect(128, 74, SCREEN_W - 128, 18, UI_BG);
  if (wxOK) {
    char b[48];
    snprintf(b, sizeof(b), "%dC %s %d%%", (int)roundf(wxTemp), wxShort(wxCode), wxHum);
    uiText(b, SCREEN_W - 8, 76, TR_DATUM, UI_TEXT, 2);
  } else {
    uiText(wxMsg.length() ? wxMsg.c_str() : "WX: TAP", SCREEN_W - 8, 76, TR_DATUM, UI_MUTE, 2);
  }
}

static void drawWxStripMsg(const char* m) {
  tft.fillRect(128, 74, SCREEN_W - 128, 18, UI_BG);
  uiText(m, SCREEN_W - 8, 76, TR_DATUM, UI_ACCENT, 2);
}

// Manual weather fetch: WiFi only comes up for this when Boot WiFi is off.
static void manualWeatherFetch() {
  drawWxStripMsg("Updating...");
  bool startedHere = (WiFi.status() != WL_CONNECTED);
  if (startedHere) {
    if (!savedSSID.length()) { wxMsg = "No WiFi saved"; drawClock(); return; }
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    fetchWeather();
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
    wxMsg = wxOK ? "" : "Fetch failed";
  } else {
    wxMsg = "No connection";
  }
  if (startedHere && !bootWifi) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
  drawClock();
}

void drawClock() {
  if (clockScreen == 2) { drawPressure(); return; }
  tft.fillScreen(UI_BG);
  drawWifiIcon(SCREEN_W - 32, 2, WiFi.status() == WL_CONNECTED);
  drawRotateIcon(ROT_X, ROT_Y, UI_DIM);                                       // tap = turn the screen upside down
  drawHomeNote();
  lastDateDay = -1;                              // force date redraw
  ckLast[0] = ckLast[1] = ckLast[2] = ckLast[3] = -2; ckLastColon = -1;

  struct tm t;
  bool timeOk = getLocalTime(&t);
  if (timeOk) { drawBigTime(t.tm_hour, t.tm_min, true, true); drawClockDate(&t); lastDateDay = t.tm_mday; }
  else        { drawBigTime(-1, -1, false, true); }
  readIndoor();
  if (timeOk) drawWxStrip();
  else {   // no NTP sync yet: say so on the date line instead of looking frozen
    tft.fillRect(0, 74, SCREEN_W, 18, UI_BG);
    uiText("TIME NOT SYNCED", 8, 76, TL_DATUM, UI_WARN, 2);
  }
  drawSensorsCard();
  drawPressureCard();
  drawHomeChatTerm();
  drawHomeButtons();
}

void updateClock() {
  if (clockScreen == 2 && millis() - lastPrDraw > 60000UL) drawPressure();   // keep the pressure graph fresh while it is open
  if (clockScreen != 0) return;   // forecast / graph are static; don't tick over them
  unsigned long now = millis();
  if ((musicPlaying && !musicPaused) != homeNoteShown) drawHomeNote();
  static unsigned long lastTick = 0;
  if (now - lastTick >= 500) {                   // colon blink; digits only redraw when they change
    lastTick = now;
    struct tm t;
    if (getLocalTime(&t)) {
      drawBigTime(t.tm_hour, t.tm_min, (t.tm_sec % 2) == 0, false);
      if (t.tm_mday != lastDateDay) { lastDateDay = t.tm_mday; drawClockDate(&t); }
    }
  }
  if (now - lastInRead > 5000) { readIndoor(); drawSensorsValues(); drawPressureValues(); }   // numbers every 5 s
  if (now - lastPrDraw > 60000UL) { lastPrDraw = now; drawPressureCard(); }                   // graph every 60 s
}

// Sub-screens (forecast / full pressure graph): any tap returns home.
void handleClock(int tx, int ty) {
  clockScreen = 0; drawClock();
}

// ---------- Sensors card ----------
void drawSensorsValues() {
  const int x = 8, y = HOME_CARD_Y, w = 108, h = 84;
  tft.fillRect(x + 3, y + 20, w - 6, h - 24, UI_CARD);
  if (!inOK) {
    uiText("No sensor", x + w / 2, y + 36, TC_DATUM, UI_DIM, 2);
    uiText("check wiring", x + w / 2, y + 54, TC_DATUM, UI_MUTE, 2);
    return;
  }
  uiTemp(x + 8, y + 24, inTemp, UI_TEXT);
  if (inHumOK) {
    uiDroplet(x + 14, y + 62, UI_ACCENT);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", (int)roundf(inHum));
    uiText(b, x + 24, y + 54, TL_DATUM, UI_ACCENT, 2, true);
    uiText("HUM", x + w - 10, y + 54, TR_DATUM, UI_MUTE, 2);
  }
}
void drawSensorsCard() {
  uiCard(8, HOME_CARD_Y, 108, 84, 10, UI_CARD, UI_BORDER, UI_BG);
  uiText("SENSORS", 16, HOME_CARD_Y + 5, TL_DATUM, UI_DIM, 2);
  drawSensorsValues();
}

// ---------- Photo / Music / Settings buttons --------------------------------
static void drawHomeButton(AppState app, int x, bool active) {   // icon only, top-right row
  uiCard(x, 30, 30, 30, 8, active ? RGB565(10, 46, 20) : UI_CARD, active ? UI_ACCENT : UI_BORDER, UI_BG);
  drawAppTile(app, x + 3, 33, 24, 5, active ? RGB565(10, 46, 20) : UI_CARD);
}
void drawHomeButtons() {
  drawHomeButton(APP_FRAME, 138, false);
  drawHomeButton(APP_MUSIC, 173, musicPlaying);
  drawHomeButton(APP_SETTINGS, 208, false);
}

// =================== LIGHT SENSOR (LDR) ===================
#define LDR_PIN 34          // ENZ photoresistor -> analog input (input-only pin)
#define BL_PIN  21          // screen backlight (active HIGH on the ENZ)

// ENZ LDR has a SMALL swing: roughly ~50 (bright) up to ~400-900 (dark)
// because of the 1M divider resistors. So we AUTO-RANGE: the app learns the
// min/max your board actually produces and maps between them. No hand-calibration.
// These are just starting seeds; they expand automatically as you use it.
int ldrMin = 80;    // brightest (lowest) raw seen
int ldrMax = 700;   // darkest  (highest) raw seen

#define BL_MIN 25           // never let the screen go fully dark on auto

bool autoDim = false;
int  ldrSmooth = 400;       // smoothed raw reading

// On the ENZ: low raw = bright, high raw = dark. Returns 0..100 brightness.
int lightPercent(int raw) {
  if (raw < ldrMin) ldrMin = raw;     // learn the range as we go
  if (raw > ldrMax) ldrMax = raw;
  if (ldrMax - ldrMin < 20) return 50;
  int p = map(raw, ldrMax, ldrMin, 0, 100);  // dark(high)->0, bright(low)->100
  return constrain(p, 0, 100);
}

const char* lightLabel(int pct) {
  if (pct < 15) return "Dark";
  if (pct < 40) return "Dim";
  if (pct < 75) return "Normal";
  return "Bright";
}

// ---- Manual brightness (slider) ----
int  manualPct = 100;            // 0..100, saved in flash
bool brightDirty = false;        // slider moved, not saved yet
unsigned long brightMs = 0;
bool lightRedrawNow = true;
bool invertColors = false;       // panel polarity (set it if photos look like negatives / the background is white)
void saveInvert() { prefs.begin("disp", false); prefs.putBool("inv", invertColors); prefs.end(); }
void saveRotation() { prefs.begin("disp", false); prefs.putBool("flip", screenFlip); prefs.end(); }
void loadRotation() { prefs.begin("disp", true); screenFlip = prefs.getBool("flip", false); prefs.end(); tft.setRotation(screenFlip ? 2 : 0); }
// Turn the screen upside down (display + touch) and redraw the current screen
void rotateScreen() {
  screenFlip = !screenFlip;
  saveRotation();
  tft.setRotation(screenFlip ? 2 : 0);
  needsRedraw = true;
}
void loadInvert() { prefs.begin("disp", true); invertColors = prefs.getBool("inv", false); prefs.end(); tft.invertDisplay(invertColors); }

int blFromPct(int pct) { return BL_MIN + (255 - BL_MIN) * constrain(pct, 0, 100) / 100; }
void applyBrightness() { if (!autoDim) analogWrite(BL_PIN, blFromPct(manualPct)); }
void loadBrightness() {
  prefs.begin("disp", true);
  manualPct = constrain(prefs.getInt("bl", 100), 0, 100);
  prefs.end();
  analogWrite(BL_PIN, blFromPct(manualPct));
}
void saveBrightness() { prefs.begin("disp", false); prefs.putInt("bl", manualPct); prefs.end(); }

// Runs every loop in ANY app when autoDim is on.
void autoBacklight() {
  if (!autoDim) return;
  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();
  int raw = analogRead(LDR_PIN);
  ldrSmooth = (ldrSmooth * 3 + raw) / 4;          // smooth to avoid flicker
  int cap = blFromPct(manualPct);                 // slider = maximum brightness
  int bl = map(lightPercent(ldrSmooth), 0, 100, BL_MIN, cap);
  analogWrite(BL_PIN, constrain(bl, BL_MIN, cap));
}

// Display screen (Setting -> Display): ambient reading, brightness slider, auto-dim, invert colours, rotate
#define LC_Y 82
#define LC_H 56
#define SL_X 32
#define SL_W 176
#define SL_Y 120

bool lightSliderHit(int x, int y) { return y >= 106 && y <= 136 && x >= SL_X - 16 && x <= SL_X + SL_W + 16; }

void drawLightSlider() {
  tft.fillRect(13, LC_Y + 5, SCREEN_W - 26, LC_H - 10, UI_CARD);
  uiText(autoDim ? "MAX BRIGHTNESS" : "BRIGHTNESS", 22, LC_Y + 5, TL_DATUM, UI_DIM, 2);
  char b[8]; snprintf(b, sizeof b, "%d%%", manualPct);
  uiText(b, SCREEN_W - 16, LC_Y + 3, TR_DATUM, UI_WARN, 4, true);
  uiRRect(SL_X, SL_Y, SL_W, 10, 5, RGB565(34, 44, 76), UI_CARD);                       // track
  int fx = SL_X + SL_W * manualPct / 100;
  if (fx - SL_X >= 10) uiRRect(SL_X, SL_Y, fx - SL_X, 10, 5, UI_WARN, UI_CARD);        // filled part
  uiDot(fx, SL_Y + 5, 11, TFT_WHITE, UI_CARD);                                         // knob
  uiDot(fx, SL_Y + 5, 4, UI_WARN, TFT_WHITE);
}

// Compact card (28px): title on the left, on/off switch on the right
static void drawSwitchCard(int y, const char* title, bool on) {
  uiCard(8, y, SCREEN_W - 16, 28, 10, UI_CARD, UI_BORDER, UI_BG);
  uiText(title, 22, y + 6, TL_DATUM, UI_TEXT, 2, true);
  uint16_t tc = on ? UI_GOOD : RGB565(60, 68, 98);
  uiRRect(SCREEN_W - 58, y + 4, 40, 20, 10, tc, UI_CARD);
  uiDot(on ? SCREEN_W - 58 + 40 - 10 : SCREEN_W - 58 + 10, y + 14, 7, TFT_WHITE, tc);
}
static void drawLightSwitch()  { drawSwitchCard(142, "Auto-dim", autoDim); }
static void drawInvertSwitch() { drawSwitchCard(173, "Invert colors", invertColors); }
static void drawRotateCard() {                                    // tap = turn the screen upside down
  uiCard(8, 204, SCREEN_W - 16, 28, 10, UI_CARD, UI_BORDER, UI_BG);
  uiText("Rotate screen", 22, 210, TL_DATUM, UI_TEXT, 2, true);
  uiText(screenFlip ? "180 deg" : "Normal", SCREEN_W - 16, 210, TR_DATUM, UI_ACCENT, 2, true);
}

// Called continuously while the finger is on the slider (no debounce -> smooth dragging)
void lightSliderTouch(int x) {
  int pct = constrain((x - SL_X) * 100 / SL_W, 0, 100);
  if (pct == manualPct) return;
  manualPct = pct;
  applyBrightness();
  drawLightSlider();
  brightDirty = true; brightMs = millis();
}

void drawLight() {
  drawScreenFrame("Display");
  uiCard(8, 38, SCREEN_W - 16, 40, 11, UI_CARD, UI_BORDER, UI_BG);          // ambient card (filled by updateLight)
  uiCard(8, LC_Y, SCREEN_W - 16, LC_H, 11, UI_CARD, UI_BORDER, UI_BG);      // brightness card
  drawLightSlider();
  drawLightSwitch();
  drawInvertSwitch();
  drawRotateCard();
  lightRedrawNow = true;
  updateLight();   // fill in the live numbers
}

void updateLight() {
  if (brightDirty && millis() - brightMs > 800) { saveBrightness(); brightDirty = false; }   // save once the slider settles

  static unsigned long last = 0;
  if (!lightRedrawNow && millis() - last < 300) return;
  lightRedrawNow = false;
  last = millis();

  int raw = analogRead(LDR_PIN);
  ldrSmooth = (ldrSmooth * 3 + raw) / 4;
  int pct = lightPercent(ldrSmooth);

  tft.fillRect(13, 43, SCREEN_W - 26, 31, UI_CARD);
  uiText("AMBIENT", 22, 42, TL_DATUM, UI_DIM, 2);
  char b[24]; snprintf(b, sizeof b, "%s  %d%%", lightLabel(pct), pct);
  uiText(b, SCREEN_W - 16, 42, TR_DATUM, UI_ACCENT, 2, true);
  uiRRect(22, 63, SCREEN_W - 44, 6, 3, RGB565(34, 44, 76), UI_CARD);
  int w = (SCREEN_W - 44) * pct / 100;
  if (w >= 6) uiRRect(22, 63, w, 6, 3, UI_ACCENT, UI_CARD);
}

void handleLight(int tx, int ty) {
  if (tx < 8 || tx > SCREEN_W - 8) return;
  if (ty >= 142 && ty <= 170) {                                   // auto-dim switch
    autoDim = !autoDim;
    if (!autoDim) applyBrightness();                              // back to the slider's brightness
    drawLightSwitch();
    drawLightSlider();                                            // caption changes (Max brightness / Brightness)
  } else if (ty >= 173 && ty <= 201) {                            // invert colours switch
    invertColors = !invertColors;
    tft.invertDisplay(invertColors);
    saveInvert();
    drawInvertSwitch();
  } else if (ty >= 204 && ty <= 232) {                            // rotate 180 degrees (display + touch)
    screenFlip = !screenFlip;
    saveRotation();
    tft.setRotation(screenFlip ? 2 : 0);
    drawLight();
  }
}

// =================== WIFI SETUP APP ===================
// --- Credential storage in flash (NVS) ---
void loadWifiCreds() {
  prefs.begin("wifi", true);
  savedSSID = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
}
void saveWifiCreds(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// Outdoor version: connecting at boot is optional (Setting -> Boot WiFi, default OFF),
// so the unit starts fast and stays off the air until WiFi is actually needed.
bool bootWifi = false;
void loadBootWifi() { prefs.begin("sys", true);  bootWifi = prefs.getBool("bootwifi", false); prefs.end(); }
void saveBootWifi() { prefs.begin("sys", false); prefs.putBool("bootwifi", bootWifi); prefs.end(); }

// Connect only if we actually have saved credentials.
void connectSaved() {
  if (!savedSSID.length()) return;   // nothing stored yet -> don't attempt
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
}

// --- Status icon: WiFi glyph (white = connected, grey + red dot = not) ---
void drawWifiIcon(int x, int y, bool ok) {
  uint16_t c = ok ? UI_TEXT : UI_MUTE;
  tft.fillRect(x, y, 30, 18, UI_BG);
  int cx = x + 15, cy = y + 16;
  for (int r = 7; r <= 15; r += 4) {
    for (int a = -48; a <= 48; a += 4) {                       // upper cone only
      float rad = a * 0.0174533f;
      tft.fillCircle(cx + (int)roundf(r * sinf(rad)), cy - (int)roundf(r * cosf(rad)), 1, c);
    }
  }
  tft.fillCircle(cx, cy - 1, 2, c);
  if (!ok) tft.fillCircle(x + 27, y + 3, 3, UI_BAD);
}

void updateMenuWifiIcon() {
  static unsigned long last = 0; static int lastState = -1;
  if (millis() - last < 1500) return; last = millis();
  int st = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  if (st != lastState) { lastState = st; drawWifiIcon(SCREEN_W - 32, 2, st == 1); }
}

// --- On-screen keyboard data ---
const char* LET_ROWS[4] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
const char* SYM_ROWS[4] = { "1234567890", "!@#$%^&*()", "-_=+[]{}|\\", ";:'\",.<>/?" };
#define KB_Y0   72
#define KB_ROWH 40
#define KB_FUNC_Y 254
#define KB_FUNC_H 36
#define KB_GAP  3

void kbRowLayout(int n, int &keyW, int &startX) {
  keyW = 22;   // portrait: 10 keys must fit 240 px
  int total = n * keyW + (n - 1) * 2;
  startX = (SCREEN_W - total) / 2;
}

void drawCharRow(const char* row, int y, bool upper) {
  int n = strlen(row), keyW, sx; kbRowLayout(n, keyW, sx);
  for (int i = 0; i < n; i++) {
    int x = sx + i * (keyW + 2);
    char ch = row[i];
    if (upper && ch >= 'a' && ch <= 'z') ch -= 32;
    char l[2] = { ch, 0 };
    drawKey(x, y, keyW, KB_ROWH, l, KC_NORMAL);
  }
}

char hitCharRow(int tx, int ty, const char* row, int y, bool upper) {
  if (ty < y || ty > y + KB_ROWH) return 0;
  int n = strlen(row), keyW, sx; kbRowLayout(n, keyW, sx);
  for (int i = 0; i < n; i++) {
    int x = sx + i * (keyW + 2);
    if (tx >= x && tx <= x + keyW) {
      char ch = row[i];
      if (upper && ch >= 'a' && ch <= 'z') ch -= 32;
      return ch;
    }
  }
  return 0;
}

void drawKey(int x, int y, int w, int h, const char* label, uint16_t bg) {
  int r = (h >= 28) ? 7 : 5;
  tft.fillRoundRect(x, y, w, h, r, bg);
  tft.drawFastHLine(x + r, y + 1, w - 2 * r, blend565(bg, TFT_WHITE, 80));              // top highlight
  tft.drawFastHLine(x + r, y + h - 1, w - 2 * r, blend565(bg, TFT_BLACK, 110));         // bottom shade
  int font = 2;
  if (h >= 40) {   // one font per row: test the widest letter, not this key's own letter
    const char* probe = (label[0] && !label[1]) ? "W" : label;
    if (uiW(probe, 4) <= w - 8) font = 4;
  }
  uiText(label, x + w / 2, y + h / 2, MC_DATUM, UI_TEXT, font, font == 2);
}

// ---- Shared pieces for the app screens (lists, pager, messages, dialogs) ----
// Trim a string so it fits maxW pixels (font 2 by default); adds ".." when shortened.
static String uiFit(const String& s, int maxW, int font = 2) {
  String t = s;
  if (uiW(t.c_str(), font) <= maxW) return t;
  while (t.length() > 1 && uiW((t + "..").c_str(), font) > maxW) u8chop(t);
  return t + "..";
}
// A list row card (26px high) with an optional coloured tag chip on the left.
// Prev / Next keys with a centred "1-6 / 20" counter (bottom of list screens)
// Centred message card (empty states, errors)
static void uiMessage(const char* title, const char* sub, uint16_t col) {
  uiCard(20, 110, SCREEN_W - 40, 84, 16, UI_CARD, UI_BORDER, UI_BG);
  uiText(title, SCREEN_W / 2, 128, TC_DATUM, col, 4, true);
  if (sub && sub[0]) uiText(sub, SCREEN_W / 2, 162, TC_DATUM, UI_DIM, 2);
}
// Confirm dialog card; the caller draws the Yes / No keys at (50,130,90,30) and (180,130,90,30)
static void uiConfirmCard(const char* title, const char* name) {
  uiCard(20, 100, SCREEN_W - 40, 100, 16, RGB565(6, 26, 12), UI_ACCENT, UI_BG);
  uiText(title, SCREEN_W / 2, 118, TC_DATUM, UI_TEXT, 4, true);
  String n = uiFit(String(name), 230, 2);
  uiText(n.c_str(), SCREEN_W / 2, 148, TC_DATUM, UI_DIM, 2);
}

void drawFuncRow() {
  int y = KB_FUNC_Y, h = KB_FUNC_H;
  drawKey(4,   y, 36,  h, "aA",  kbUpper ? KC_ACT : KC_NORMAL);
  drawKey(43,  y, 36,  h, kbLayer ? "abc" : "#@", KC_NORMAL);
  drawKey(82,  y, 74,  h, "Space", KC_NORMAL);
  drawKey(159, y, 36,  h, "Del", KC_BAD);
  drawKey(198, y, 38,  h, "OK",  KC_GOOD);
}

// ---------------------------------------------------------------------------------------------
//  Keyboard input field + Japanese input (hiragana / katakana grid, optional kanji conversion)
// ---------------------------------------------------------------------------------------------
int    kbMode = 0;            // 0 = ABC (normal keyboard), 1 = hiragana, 2 = katakana, 3 = conversion candidates
String imeComp;               // kana being composed (shown underlined, not yet in kbInput)
int    kbMaxBytes = 2000;     // size limit for the text being typed (Japanese uses 3 bytes per character)
bool   kbJpActive = false;    // the current keyboard screen is Japanese-capable
String imeCand[24]; int imeCandN = 0, imeCandPage = 0, imePrevMode = 1;
bool   skkMissing = false;
#define KB_DONE 1
#define KB_OK   2

void drawKbInput() {
  tft.fillRect(9, 32, SCREEN_W - 18, 20, UI_CARD);
  if (jpReady && kbJpActive) {                         // everything in the 16px Japanese font
    String shown = kbInput;
    while (shown.length() && jpWidth((shown + imeComp).c_str()) + 12 > SCREEN_W - 28) { const char* p = shown.c_str(); u8next(p); shown.remove(0, p - shown.c_str()); }
    int x = 12;
    x += jpText(shown.c_str(), x, 34, UI_TEXT, UI_CARD);
    if (imeComp.length()) {                              // text being composed: black on an amber block (easy to see)
      int w = jpWidth(imeComp.c_str());
      tft.fillRect(x - 1, 33, w + 2, 18, UI_WARN);
      jpText(imeComp.c_str(), x, 34, TFT_BLACK, UI_WARN);
      x += w;
    }
    jpText("_", x, 34, UI_MUTE, UI_CARD);
    return;
  }
  String shown = kbInput;
  while (shown.length() > 1 && uiW(shown.c_str(), 2) > SCREEN_W - 34) shown.remove(0, 1);   // keep the end visible
  shown += "_";
  uiText(shown.c_str(), 12, 34, TL_DATUM, UI_TEXT, 2);
}

// Keyboard screen background: Back button, caption and the text field (shared by WiFi / Notes / Chat)
void drawKbScreen(const char* caption, bool jp = false) {
  kbJpActive = jp;
  tft.fillScreen(UI_BG);
  drawBack();
  uiText(caption, 64, 6, TL_DATUM, UI_DIM, 2);
  uiCard(4, 28, SCREEN_W - 8, 28, 9, UI_CARD, UI_ACCENT, UI_BG);
  drawKbInput();
}

// ---- kana tables (hiragana; katakana is derived by +0x60) ----
static const char* const KANA_H[5][11] = {
  { "あ","か","さ","た","な","は","ま","や","ら","わ","、" },
  { "い","き","し","ち","に","ひ","み","", "り","を","。" },
  { "う","く","す","つ","ぬ","ふ","む","ゆ","る","ん","！" },
  { "え","け","せ","て","ね","へ","め","", "れ","・","？" },
  { "お","こ","そ","と","の","ほ","も","よ","ろ","…","ー" },
};
static const char* const KANA_DAKU[]  = { "かが","きぎ","くぐ","けげ","こご","さざ","しじ","すず","せぜ","そぞ","ただ","ちぢ","つづ","てで","とど","はばぱ","ひびぴ","ふぶぷ","へべぺ","ほぼぽ","うゔ" };
static const char* const KANA_SMALL[] = { "ぁあ","ぃい","ぅう","ぇえ","ぉお","っつ","ゃや","ゅゆ","ょよ","ゎわ" };

static String toKata(const String& s) {                // hiragana -> katakana
  String r; const char* p = s.c_str();
  while (*p) { uint32_t c = u8next(p); if (c >= 0x3041 && c <= 0x3096) c += 0x60; u8append(r, c); }
  return r;
}
static String toHira(const String& s) {                // katakana -> hiragana
  String r; const char* p = s.c_str();
  while (*p) { uint32_t c = u8next(p); if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60; u8append(r, c); }
  return r;
}
static String kanaCell(int r, int c) {
  String h = KANA_H[r][c];
  return (kbMode == 2) ? toKata(h) : h;
}
static bool kanaIsPunct(int r, int c) { return c == 10 || (c == 9 && r >= 3); }

static void imeCommit() {                              // composition -> text
  if (imeComp.length()) { kbInput += imeComp; imeComp = ""; }
}
static void kbAppend(const String& s) {               // respects the size limit
  if (kbInput.length() + imeComp.length() + s.length() <= (unsigned)kbMaxBytes) kbInput += s;
}
static void imeAppend(const String& s) {
  if (kbInput.length() + imeComp.length() + s.length() <= (unsigned)kbMaxBytes) imeComp += s;
}
static bool imeCycleLast(const char* const* table, int n) {   // ゛゜ and 小 keys
  if (!imeComp.length()) return false;
  int i = imeComp.length() - 1;
  while (i > 0 && ((uint8_t)imeComp[i] & 0xC0) == 0x80) i--;
  String head = imeComp.substring(0, i);
  const char* q = imeComp.c_str() + i; uint32_t cp = u8next(q);
  bool kata = (cp >= 0x30A1 && cp <= 0x30F6);
  uint32_t h = kata ? cp - 0x60 : cp;
  for (int t = 0; t < n; t++) {
    uint32_t cs[4]; int m = 0; const char* e = table[t];
    while (*e && m < 4) cs[m++] = u8next(e);
    for (int k = 0; k < m; k++) if (cs[k] == h) {
      uint32_t nx = cs[(k + 1) % m]; if (kata) nx += 0x60;
      String r = head; u8append(r, nx); imeComp = r;
      return true;
    }
  }
  return false;
}

// ---- optional kanji dictionary: /skk.dic (SKK format, UTF-8, sorted; made by make_skk_dic.py) ----
static bool skkLookup(const String& key, String* out, int& n, int maxN) {
  File f = SD.open("/skk.dic", FILE_READ);
  if (!f) { skkMissing = true; return false; }
  skkMissing = false;
  uint32_t lo = 0, hi = f.size();
  String line;
  while (lo < hi) {
    uint32_t mid = (lo + hi) >> 1, s = mid;
    if (s > lo) {                                      // move to the start of a line
      f.seek(s - 1);
      int c = f.read();
      if (c != '\n') { while (f.available()) { c = f.read(); s++; if (c == '\n') break; } }
    }
    if (s >= hi) { hi = mid; continue; }
    f.seek(s);
    line = "";
    while (f.available() && line.length() < 600) { int c = f.read(); if (c == '\n') break; line += (char)c; }
    uint32_t end = s + line.length() + 1;
    int sp = line.indexOf(' ');
    String k = (sp > 0) ? line.substring(0, sp) : line;
    int cmp = strcmp(k.c_str(), key.c_str());
    if (cmp == 0 && sp > 0) {                          // found: "reading /cand1/cand2;note/..."
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
    if (cmp < 0) lo = end; else hi = s;
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
  String reading = toHira(imeComp);
  imeCandN = 0;
  String tmp[20]; int n = 0;
  skkLookup(reading, tmp, n, 20);
  for (int i = 0; i < n; i++) imeAddCand(tmp[i]);
  imeAddCand(reading);
  imeAddCand(toKata(reading));
  imeCandPage = 0; imePrevMode = kbMode; kbMode = 3;
}

// ---- drawing ----
#define IME_X0 7
#define IME_Y0 62
#define IME_PITCH 21
#define IME_PITCH_Y 38
#define IME_PER 8                                      // candidates per page (2 columns x 4 rows)

static void drawFuncRowJp() {                          // ABC mode function row (with the あ key)
  int y = KB_FUNC_Y, h = KB_FUNC_H;
  drawKey(4,   y, 36, h, "aA",  kbUpper ? KC_ACT : KC_NORMAL);
  drawKey(43,  y, 36, h, kbLayer ? "abc" : "#@", KC_NORMAL);
  drawKey(82,  y, 36, h, "あ",  KC_ACT);
  drawKey(121, y, 52, h, "Space", KC_NORMAL);
  drawKey(176, y, 28, h, "Del", KC_BAD);
  drawKey(207, y, 30, h, "OK",  KC_GOOD);
}
static void drawKanaGrid() {
  for (int r = 0; r < 5; r++) for (int c = 0; c < 11; c++) {
    String k = kanaCell(r, c);
    if (!k.length()) continue;
    drawKey(IME_X0 + c * IME_PITCH, IME_Y0 + r * IME_PITCH_Y, 19, 32, k.c_str(), kanaIsPunct(r, c) ? KC_MUTE : KC_NORMAL);
  }
}
static void drawImeFuncRow() {
  int y = KB_FUNC_Y, h = KB_FUNC_H;
  drawKey(3,   y, 40, h, kbMode == 1 ? "ア" : "ABC", KC_ACT);
  drawKey(46,  y, 38, h, "゛゜", KC_NORMAL);
  drawKey(87,  y, 32, h, "小", KC_NORMAL);
  drawKey(122, y, 44, h, "変換", imeComp.length() ? KC_ACT : KC_MUTE);
  drawKey(169, y, 30, h, "Del", KC_BAD);
  drawKey(202, y, 34, h, "OK", KC_GOOD);
}
static void drawImeCands() {
  int start = imeCandPage * IME_PER;
  for (int i = 0; i < IME_PER && start + i < imeCandN; i++) {
    String t = uiFit(imeCand[start + i], 100, 2);
    drawKey(8 + (i % 2) * 116, IME_Y0 + (i / 2) * IME_PITCH_Y, 108, 34, t.c_str(), KC_NORMAL);
  }
  if (skkMissing) uiText("No /skk.dic - kana only", SCREEN_W / 2, 224, TC_DATUM, UI_MUTE, 2);
  drawKey(3,   KB_FUNC_Y, 74, KB_FUNC_H, "戻る", KC_MUTE);
  drawKey(81,  KB_FUNC_Y, 74, KB_FUNC_H, "前へ", KC_NORMAL);
  drawKey(159, KB_FUNC_Y, 78, KB_FUNC_H, "次へ", KC_NORMAL);
}

// Japanese-capable keyboard screen (caller supplies the caption; redraw callback is the caller's draw function)
void drawKbJp(const char* caption) {
  drawKbScreen(caption, true);
  if (kbMode == 0) {
    const char** rows = (kbLayer == 0) ? LET_ROWS : SYM_ROWS;
    for (int r = 0; r < 4; r++) drawCharRow(rows[r], KB_Y0 + r * (KB_ROWH + KB_GAP), kbUpper && kbLayer == 0);
    drawFuncRowJp();
  } else if (kbMode == 3) {
    drawImeCands();
  } else {
    drawKanaGrid();
    drawImeFuncRow();
  }
}

void kbJpReset(int maxBytes) { kbMode = 0; imeComp = ""; kbMaxBytes = maxBytes; imeCandN = 0; imeCandPage = 0; skkMissing = false; }

// Handle one tap on a Japanese-capable keyboard. Returns KB_OK when OK was pressed (composition already committed).
int kbJpHandle(int tx, int ty, void (*redraw)()) {
  if (kbMode == 0) {                                   // ----- ABC -----
    const char** rows = (kbLayer == 0) ? LET_ROWS : SYM_ROWS;
    for (int r = 0; r < 4; r++) {
      char c = hitCharRow(tx, ty, rows[r], KB_Y0 + r * (KB_ROWH + KB_GAP), kbUpper && kbLayer == 0);
      if (c) { kbAppend(String(c)); drawKbInput(); return KB_DONE; }
    }
    if (ty >= KB_FUNC_Y && ty <= KB_FUNC_Y + KB_FUNC_H) {
      if      (tx >= 4   && tx <= 40)  { kbUpper = !kbUpper; redraw(); }
      else if (tx >= 43  && tx <= 79)  { kbLayer = !kbLayer; redraw(); }
      else if (tx >= 82  && tx <= 118) { kbMode = 1; redraw(); }
      else if (tx >= 121 && tx <= 173) { kbAppend(" "); drawKbInput(); }
      else if (tx >= 176 && tx <= 204) { u8chop(kbInput); drawKbInput(); }
      else if (tx >= 207 && tx <= 237) return KB_OK;
    }
    return KB_DONE;
  }
  if (kbMode == 3) {                                   // ----- candidates -----
    if (ty >= KB_FUNC_Y && ty <= KB_FUNC_Y + KB_FUNC_H) {
      if      (tx < 79)   { kbMode = imePrevMode; redraw(); }
      else if (tx < 157)  { if (imeCandPage > 0) imeCandPage--; redraw(); }
      else                { if ((imeCandPage + 1) * IME_PER < imeCandN) imeCandPage++; redraw(); }
      return KB_DONE;
    }
    for (int i = 0; i < IME_PER; i++) {
      int idx = imeCandPage * IME_PER + i;
      if (idx >= imeCandN) break;
      int x = 8 + (i % 2) * 116, y = IME_Y0 + (i / 2) * IME_PITCH_Y;
      if (tx >= x && tx < x + 108 && ty >= y && ty < y + 34) {
        kbAppend(imeCand[idx]); imeComp = ""; kbMode = imePrevMode; redraw();
        return KB_DONE;
      }
    }
    return KB_DONE;
  }
  // ----- kana (1 = hiragana, 2 = katakana) -----
  if (ty >= KB_FUNC_Y && ty <= KB_FUNC_Y + KB_FUNC_H) {
    if      (tx >= 3   && tx <= 43)  { if (kbMode == 1) kbMode = 2; else { imeCommit(); kbMode = 0; } redraw(); }
    else if (tx >= 46  && tx <= 84)  { if (imeCycleLast(KANA_DAKU,  sizeof(KANA_DAKU)  / sizeof(KANA_DAKU[0])))  drawKbInput(); }
    else if (tx >= 87  && tx <= 119) { if (imeCycleLast(KANA_SMALL, sizeof(KANA_SMALL) / sizeof(KANA_SMALL[0]))) drawKbInput(); }
    else if (tx >= 122 && tx <= 166) { if (imeComp.length()) { imeConvert(); redraw(); } }
    else if (tx >= 169 && tx <= 199) { if (imeComp.length()) u8chop(imeComp); else u8chop(kbInput); drawKbInput(); }
    else if (tx >= 202 && tx <= 236) { imeCommit(); return KB_OK; }
    return KB_DONE;
  }
  for (int r = 0; r < 5; r++) for (int c = 0; c < 11; c++) {
    String k = kanaCell(r, c);
    if (!k.length()) continue;
    int x = IME_X0 + c * IME_PITCH, y = IME_Y0 + r * IME_PITCH_Y;
    if (tx >= x && tx < x + IME_PITCH && ty >= y && ty < y + IME_PITCH_Y) {
      if (kanaIsPunct(r, c)) { imeCommit(); kbAppend(k); }
      else imeAppend(k);
      drawKbInput();
      return KB_DONE;
    }
  }
  return KB_DONE;
}

void drawWifiKeyboard() {
  String c = String("Password: ") + uiFit(kbSSID, 150, 2);
  drawKbScreen(c.c_str());
  const char** rows = (kbLayer == 0) ? LET_ROWS : SYM_ROWS;
  for (int r = 0; r < 4; r++)
    drawCharRow(rows[r], KB_Y0 + r * (KB_ROWH + KB_GAP), kbUpper && kbLayer == 0);
  drawFuncRow();
}

void drawWifiResult(bool ok) {
  drawScreenFrame("Wi-Fi");
  uiCard(20, 86, SCREEN_W - 40, 126, 16, UI_CARD, UI_BORDER, UI_BG);
  uiDot(SCREEN_W / 2, 120, 22, ok ? UI_GOOD : UI_BAD, UI_CARD);
  int cx = SCREEN_W / 2;
  if (ok) { tft.drawLine(cx - 11, 120, cx - 4, 128, TFT_WHITE); tft.drawLine(cx - 4, 128, cx + 11, 112, TFT_WHITE);
            tft.drawLine(cx - 11, 121, cx - 4, 129, TFT_WHITE); tft.drawLine(cx - 4, 129, cx + 11, 113, TFT_WHITE); }
  else    { tft.drawLine(cx - 9, 111, cx + 9, 129, TFT_WHITE); tft.drawLine(cx + 9, 111, cx - 9, 129, TFT_WHITE);
            tft.drawLine(cx - 8, 111, cx + 10, 129, TFT_WHITE); tft.drawLine(cx + 10, 111, cx - 8, 129, TFT_WHITE); }
  uiText(ok ? "Connected" : "Failed", cx, 154, TC_DATUM, ok ? UI_GOOD : UI_BAD, 4, true);
  uiText(ok ? "Saved - tap to go back" : "Tap to try again", cx, 186, TC_DATUM, UI_DIM, 2);
}

void wifiConnect() {
  saveWifiCreds(kbSSID, kbInput);   // persist to flash immediately
  savedSSID = kbSSID; savedPass = kbInput;

  drawScreenFrame("Wi-Fi");
  uiCard(20, 100, SCREEN_W - 40, 96, 16, UI_CARD, UI_BORDER, UI_BG);
  uiText("Connecting...", SCREEN_W / 2, 114, TC_DATUM, UI_TEXT, 4, true);
  { String s = uiFit(kbSSID, 170, 2); uiText(s.c_str(), SCREEN_W / 2, 152, TC_DATUM, UI_DIM, 2); }

  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(kbSSID.c_str(), kbInput.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(150);
    drawSplashProgress((int)((millis() - t0) * 100 / 12000));
  }
  wifiResultOK = (WiFi.status() == WL_CONNECTED);
  if (wifiResultOK) configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);  // sync clock now
  wifiScreen = 2;
  drawWifiResult(wifiResultOK);
}

// Signal strength as 4 small bars (right side of a network row)
static void uiSignalBars(int x, int y, int rssi) {
  int lv = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    tft.fillRect(x + i * 6, y + 13 - h, 4, h, i < lv ? UI_ACCENT : UI_GHOST);
  }
}

void drawWifiList() {
  drawScreenFrame("Wi-Fi");
  uiMessage("Scanning...", "looking for networks", UI_ACCENT);

  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();          // clear any previous scan results
  delay(50);
  wifiNetCount = WiFi.scanNetworks();   // synchronous; takes a few seconds

  drawScreenFrame("Wi-Fi");
  if (wifiNetCount <= 0) {
    uiMessage("No networks", "nothing found nearby", UI_WARN);
    drawKey(50, 200, 140, 34, "Rescan", KC_ACT);
    return;
  }
  int show = min(wifiNetCount, 6);
  for (int i = 0; i < show; i++) {
    int y = 36 + i * 28;
    uiCard(8, y, SCREEN_W - 16, 25, 9, UI_CARD, UI_BORDER, UI_BG);
    String s = uiFit(WiFi.SSID(i), 150, 2);
    uiText(s.c_str(), 18, y + 4, TL_DATUM, UI_TEXT, 2);
    uiSignalBars(SCREEN_W - 40, y + 5, WiFi.RSSI(i));
  }
  drawKey(50, 210, 140, 28, "Rescan", KC_ACT);
}

void drawWifi() {
  if (wifiScreen == 1)      drawWifiKeyboard();
  else if (wifiScreen == 2) drawWifiResult(wifiResultOK);
  else                      drawWifiList();
}

void handleWifiList(int tx, int ty) {
  int show = min(wifiNetCount, 6);
  for (int i = 0; i < show; i++) {
    int y = 36 + i * 28;
    if (ty >= y && ty <= y + 25) {
      kbSSID = WiFi.SSID(i);
      kbInput = ""; kbUpper = false; kbLayer = 0;
      wifiScreen = 1;
      drawWifiKeyboard();
      return;
    }
  }
  if (ty >= 200) drawWifiList();   // Rescan
}

void handleWifiKeyboard(int tx, int ty) {
  const char** rows = (kbLayer == 0) ? LET_ROWS : SYM_ROWS;
  for (int r = 0; r < 4; r++) {
    char c = hitCharRow(tx, ty, rows[r], KB_Y0 + r * (KB_ROWH + KB_GAP), kbUpper && kbLayer == 0);
    if (c) { kbInput += c; drawKbInput(); return; }
  }
  int y = KB_FUNC_Y, h = KB_FUNC_H;
  if (ty >= y && ty <= y + h) {
    if      (tx >= 4   && tx <= 40)  { kbUpper = !kbUpper; drawWifiKeyboard(); }
    else if (tx >= 43  && tx <= 79)  { kbLayer = !kbLayer; drawWifiKeyboard(); }
    else if (tx >= 82  && tx <= 156) { kbInput += ' '; drawKbInput(); }
    else if (tx >= 159 && tx <= 195) { if (kbInput.length()) kbInput.remove(kbInput.length() - 1); drawKbInput(); }
    else if (tx >= 198 && tx <= 236) { wifiConnect(); }
  }
}

void handleWifi(int tx, int ty) {
  if (wifiScreen == 1)      handleWifiKeyboard(tx, ty);
  else if (wifiScreen == 2) {
    if (wifiResultOK) { currentApp = APP_SETTINGS; settingsScreen = 0; needsRedraw = true; }
    else { wifiScreen = 1; drawWifiKeyboard(); }
  }
  else handleWifiList(tx, ty);
}

// =================== AIR PRESSURE HISTORY + GRAPH ===================
// One sample every 10 minutes for 24 hours (kept in flash, survives a restart). Shown as a graph by tapping the Room card.
#if HAVE_BARO
#define PR_N      144
#define PR_STEP_S 600
struct PrSample { uint32_t t; uint16_t p10; };           // time (seconds) and pressure in 0.1 hPa
static PrSample prHist[PR_N];
static int prCount = 0;
static uint32_t prBase = 100000;                         // pseudo clock when the real time is not known (no WiFi yet)
static bool prDirty = false; static uint32_t prLastSave = 0;

static bool prTimeValid() { return time(nullptr) > 1700000000; }
static uint32_t prNow() { return prTimeValid() ? (uint32_t)time(nullptr) : prBase + millis() / 1000; }

void prSave() {
  prefs.begin("baro", false);
  prefs.putBytes("hist", prHist, prCount * sizeof(PrSample));
  prefs.end();
  prDirty = false; prLastSave = prNow();
}
void prLoad() {
  prCount = 0;
  prefs.begin("baro", true);
  size_t len = prefs.getBytesLength("hist");
  if (len > 0 && len <= sizeof(prHist) && len % sizeof(PrSample) == 0) { prefs.getBytes("hist", prHist, len); prCount = len / sizeof(PrSample); }
  prefs.end();
  if (prCount) prBase = prHist[prCount - 1].t + PR_STEP_S;   // keep a sensible time line until the real time arrives
  prLastSave = prNow();
}
static void prAdd(uint32_t t, float hpa) {
  if (prTimeValid() && prCount && prHist[prCount - 1].t < 1700000000) prCount = 0;   // old pseudo-time samples cannot be placed -> start over
  if (prCount >= PR_N) { memmove(prHist, prHist + 1, (PR_N - 1) * sizeof(PrSample)); prCount = PR_N - 1; }
  prHist[prCount].t = t; prHist[prCount].p10 = (uint16_t)lroundf(hpa * 10.0f); prCount++;
  prDirty = true;
}
// Called every loop(): records a sample every 10 minutes (also while other apps are open) and saves every 30 minutes.
void pressureTick() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 20000UL && lastCheck != 0) return;
  lastCheck = now;
  uint32_t t = prNow();
  if (prCount == 0 || t >= prHist[prCount - 1].t + PR_STEP_S) {
    if (lastBmpMs == 0 || now - lastBmpMs > 15000UL) bmpPoll();             // measure unless the clock screen just did
    if (pressOK && now - lastBmpMs < 15000UL) prAdd(t, inPress);
  }
  if (prDirty && t - prLastSave >= 1800) prSave();
}

// Change over the last ~3 hours in hPa. Returns false when there is not enough history.
static bool prTrend3h(float &delta) {
  if (prCount < 2) return false;
  uint32_t tNow = prHist[prCount - 1].t;
  int best = -1; uint32_t bestDiff = 0xFFFFFFFF;
  for (int i = 0; i < prCount - 1; i++) {                                    // sample closest to "3 hours ago"
    uint32_t age = tNow - prHist[i].t;
    uint32_t diff = (age > 10800) ? age - 10800 : 10800 - age;
    if (diff < bestDiff) { bestDiff = diff; best = i; }
  }
  if (best < 0 || tNow - prHist[best].t < 5400) return false;               // need at least 1.5 h of history
  delta = (prHist[prCount - 1].p10 - prHist[best].p10) / 10.0f * (10800.0f / (tNow - prHist[best].t));
  return true;
}
// 0 = rising fast, 1 = rising, 2 = steady, 3 = falling, 4 = falling fast
static int prTrendClass(float d) { return d >= 2.0f ? 0 : d >= 0.5f ? 1 : d > -0.5f ? 2 : d > -2.0f ? 3 : 4; }

static void drawTrendArrow(int cx, int cy, int cls) {                       // 24px arrow icon
  uint16_t col = (cls <= 1) ? UI_GOOD : (cls == 2) ? UI_DIM : (cls == 3) ? UI_WARN : UI_BAD;
  if (cls == 2) {                                                            // steady: right arrow
    tft.fillRect(cx - 10, cy - 2, 14, 5, col); tft.fillTriangle(cx + 2, cy - 8, cx + 2, cy + 8, cx + 11, cy, col);
  } else if (cls <= 1) {                                                     // rising: up arrow (double for fast)
    tft.fillRect(cx - 2, cy - 2, 5, 12, col); tft.fillTriangle(cx - 9, cy - 1, cx + 9, cy - 1, cx, cy - 11, col);
    if (cls == 0) tft.fillTriangle(cx - 9, cy - 9, cx + 9, cy - 9, cx, cy - 19, col);
  } else {                                                                   // falling: down arrow (double for fast)
    tft.fillRect(cx - 2, cy - 10, 5, 12, col); tft.fillTriangle(cx - 9, cy + 1, cx + 9, cy + 1, cx, cy + 11, col);
    if (cls == 4) tft.fillTriangle(cx - 9, cy + 9, cx + 9, cy + 9, cx, cy + 19, col);
  }
}

// ---------- Pressure card: value + mini graph, always on the home screen ----
void drawPressureValues() {
  const int x = 124, y = 100, w = 108, h = 84;
  tft.fillRect(x + 3, y + 56, w - 6, h - 60, UI_CARD);
  if (!pressOK && prCount == 0) { uiText("-- hPa", x + 10, y + 60, TL_DATUM, UI_DIM, 2); return; }
  float cur = pressOK ? inPress : prHist[prCount - 1].p10 / 10.0f;
  char b[16];
  snprintf(b, sizeof(b), "%.1f", cur);
  uiText(b, x + 10, y + 58, TL_DATUM, UI_TEXT, 2, true);
  uiText("hPa", x + 10 + uiW(b, 2) + 4, y + 58, TL_DATUM, UI_DIM, 2);
  float d;
  if (prTrend3h(d)) drawTrendArrow(x + w - 14, y + 66, prTrendClass(d));
}
void drawPressureCard() {
  const int x = 124, y = 100, w = 108, h = 84;
  uiCard(x, y, w, h, 10, UI_CARD, UI_BORDER, UI_BG);
  uiText("PRESSURE", x + 8, y + 5, TL_DATUM, UI_DIM, 2);
  const int gx0 = x + 8, gx1 = x + w - 8, gy0 = y + 22, gy1 = y + 54;
  tft.drawFastHLine(gx0, gy1, gx1 - gx0, UI_GHOST);
  if (prCount >= 2) {
    uint32_t tEnd = prHist[prCount - 1].t;
    uint32_t tBeg = tEnd > 86400UL ? tEnd - 86400UL : 0;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < prCount; i++) {
      if (prHist[i].t < tBeg) continue;
      float p = prHist[i].p10 / 10.0f;
      if (p < lo) lo = p;
      if (p > hi) hi = p;
    }
    if (hi - lo < 2.0f) { float mid = (hi + lo) / 2; lo = mid - 1.0f; hi = mid + 1.0f; }
    int px = -1, py = -1;
    for (int i = 0; i < prCount; i++) {
      if (prHist[i].t < tBeg) continue;
      int sx = gx0 + (int)((uint32_t)(gx1 - gx0) * (prHist[i].t - tBeg) / 86400UL);
      float p = prHist[i].p10 / 10.0f;
      int sy = gy1 - (int)((p - lo) / (hi - lo) * (gy1 - gy0));
      if (px >= 0) tft.drawLine(px, py, sx, sy, UI_ACCENT);
      else tft.drawPixel(sx, sy, UI_ACCENT);
      px = sx; py = sy;
    }
  } else {
    uiText("collecting...", x + w / 2, y + 34, TC_DATUM, UI_MUTE, 2);
  }
  drawPressureValues();
}

void drawPressure() {
  lastPrDraw = millis();
  drawScreenFrame("Pressure");
  if (!pressOK && prCount == 0) {
    uiMessage("No pressure data", "connect a BMP280 sensor", UI_DIM);
    uiText("records 1 point / 10 min for 24 h", SCREEN_W / 2, 190, TC_DATUM, UI_MUTE, 2);
    return;
  }
  // ---- top card: current value + trend ----
  uiCard(8, 38, SCREEN_W - 16, 54, 12, UI_CARD, UI_BORDER, UI_BG);
  float cur = pressOK ? inPress : prHist[prCount - 1].p10 / 10.0f;
  char b[24]; snprintf(b, sizeof b, "%.1f", cur);
  uiText(b, 20, 48, TL_DATUM, UI_TEXT, 4, true);
  uiText("hPa", 24 + uiW(b, 4), 58, TL_DATUM, UI_DIM, 2);
  float d; static const char* NAMES[5] = { "Rising fast", "Rising", "Steady", "Falling", "Falling fast" };
  if (prTrend3h(d)) {
    int cls = prTrendClass(d);
    uint16_t col = (cls <= 1) ? UI_GOOD : (cls == 2) ? UI_DIM : (cls == 3) ? UI_WARN : UI_BAD;
    drawTrendArrow(140, 65, cls);
    uiText(NAMES[cls], SCREEN_W - 16, 44, TR_DATUM, col, 2, true);
    snprintf(b, sizeof b, "%+.1f hPa / 3h", d);
    uiText(b, SCREEN_W - 16, 62, TR_DATUM, UI_DIM, 2);
  } else {
    uiText("collecting data...", SCREEN_W - 16, 54, TR_DATUM, UI_MUTE, 2);
  }
  // ---- graph card ----
  uiCard(8, 98, SCREEN_W - 16, 190, 12, UI_CARD, UI_BORDER, UI_BG);
  const int px0 = 44, px1 = SCREEN_W - 20, py0 = 110, py1 = 256;
  uint32_t tEnd = prCount ? prHist[prCount - 1].t : prNow(), tStart = tEnd - 86400UL;
  float lo = 99999, hi = 0;
  for (int i = 0; i < prCount; i++) if (prHist[i].t >= tStart) { float p = prHist[i].p10 / 10.0f; if (p < lo) lo = p; if (p > hi) hi = p; }
  if (hi < lo) { lo = cur; hi = cur; }
  float span = hi - lo; if (span < 6.0f) { float mid = (hi + lo) / 2; lo = mid - 3.0f; hi = mid + 3.0f; }
  float step = (hi - lo) <= 8 ? 2.0f : ((hi - lo) <= 16 ? 4.0f : 10.0f);
  lo = floorf(lo / step) * step; hi = ceilf(hi / step) * step;
  while (hi - lo < 3 * step) hi += step;
  for (float g = lo; g <= hi + 0.01f; g += step) {                           // grid lines + labels
    int y = py1 - (int)((g - lo) * (py1 - py0) / (hi - lo));
    for (int x = px0; x <= px1; x += 4) tft.drawFastHLine(x, y, 2, UI_BORDER);
    char l[8]; snprintf(l, sizeof l, "%d", (int)lroundf(g));
    uiText(l, px0 - 6, y - 8, TR_DATUM, UI_MUTE, 2);
  }
  uiText("-24h", px0, 264, TL_DATUM, UI_MUTE, 2);
  uiText("-12h", (px0 + px1) / 2, 264, TC_DATUM, UI_MUTE, 2);
  uiText("now", px1, 264, TR_DATUM, UI_MUTE, 2);
  int lastX = -1, lastY = -1; uint32_t lastT = 0;
  for (int i = 0; i < prCount; i++) {
    if (prHist[i].t < tStart) continue;
    int x = px0 + (int)((uint64_t)(prHist[i].t - tStart) * (px1 - px0) / 86400UL);
    int y = py1 - (int)((prHist[i].p10 / 10.0f - lo) * (py1 - py0) / (hi - lo));
    if (y < py0) y = py0; if (y > py1) y = py1;
    if (lastX >= 0 && prHist[i].t - lastT <= 2400) {                         // connect samples (a gap > 40 min leaves a break)
      for (int xx = lastX; xx <= x; xx++) {                                  // area under the line
        int yy = (x == lastX) ? y : lastY + (y - lastY) * (xx - lastX) / (x - lastX);
        tft.drawFastVLine(xx, yy, py1 - yy + 1, blend565(UI_ACCENT, UI_CARD, 205));
      }
      tft.drawLine(lastX, lastY, x, y, UI_ACCENT); tft.drawLine(lastX, lastY + 1, x, y + 1, UI_ACCENT);
    }
    lastX = x; lastY = y; lastT = prHist[i].t;
  }
  if (lastX >= 0) { tft.fillCircle(lastX, lastY, 3, TFT_WHITE); tft.drawCircle(lastX, lastY, 4, UI_ACCENT); }
}
#else
void pressureTick() {}
void prLoad() {}
void drawPressure() { drawScreenFrame("Pressure"); uiMessage("Pressure disabled", "set ENABLE_PRESSURE to 1", UI_DIM); }
#endif

// =================== SETTINGS (WiFi + Time zone) ===================
// settingsScreen: 0 = list, 1 = time zone (declared near the top for the Back handler)
void loadTimezone() {
  prefs.begin("tz", true);
  GMT_OFFSET_SEC = prefs.getLong("gmt", 9 * 3600);
  DST_OFFSET_SEC = prefs.getInt("dst", 0);
  prefs.end();
}
void saveTimezone() {
  prefs.begin("tz", false);
  prefs.putLong("gmt", GMT_OFFSET_SEC);
  prefs.putInt("dst", DST_OFFSET_SEC);
  prefs.end();
}

static void formatTz(char* out, size_t n, bool withDst) {
  long m = GMT_OFFSET_SEC / 60;
  long a = m < 0 ? -m : m;
  snprintf(out, n, "UTC%c%ld:%02ld%s", m < 0 ? '-' : '+', a / 60, a % 60, (withDst && DST_OFFSET_SEC) ? " DST" : "");
}

static const char* tzHint(long mins) {
  struct TzName { int m; const char* name; };
  static const TzName names[] = {
    {-600, "Honolulu"}, {-480, "Los Angeles"}, {-420, "Denver"}, {-360, "Chicago"}, {-300, "New York"},
    {-240, "Halifax"}, {-180, "Sao Paulo"}, {0, "London / UTC"}, {60, "Paris / Berlin"}, {120, "Athens / Cairo"},
    {180, "Moscow"}, {240, "Dubai"}, {330, "India"}, {420, "Bangkok"}, {480, "Beijing / Singapore"},
    {540, "Tokyo / Seoul"}, {570, "Adelaide"}, {600, "Sydney"}, {720, "Auckland"},
  };
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (names[i].m == mins) return names[i].name;
  return "Custom";
}

// One settings row: icon tile | title ........ value | chevron   (6 rows fit on screen)
#define ST_Y0 36
#define ST_PITCH 34
#define ST_H 32
static void drawSettingsRow(int y, AppState icon, const char* title, const char* value) {
  const int x = 8, w = SCREEN_W - 16, h = ST_H;
  uiCard(x, y, w, h, 11, UI_CARD, UI_BORDER, UI_BG);
  drawAppTile(icon, x + 6, y + 2, 28, 8, UI_CARD);
  uiText(title, x + 42, y + 8, TL_DATUM, UI_TEXT, 2, true);
  int cx = x + w - 16, cy = y + h / 2;                          // chevron
  tft.drawLine(cx, cy - 5, cx + 5, cy, UI_MUTE); tft.drawLine(cx + 5, cy, cx, cy + 5, UI_MUTE);
  tft.drawLine(cx + 1, cy - 5, cx + 6, cy, UI_MUTE); tft.drawLine(cx + 6, cy, cx + 1, cy + 5, UI_MUTE);
  uiText(value, cx - 10, y + 8, TR_DATUM, UI_DIM, 2);
}

// Time zone screen geometry
#define TZ_X1 8
#define TZ_X2 124
#define TZ_W  108
#define TZ_H  36
#define TZ_Y1 106
#define TZ_Y2 148
#define TZ_Y3 190

static void drawTzScreen() {
  drawScreenFrame("Time Zone");
  char b[24];
  uiCard(8, 40, SCREEN_W - 16, 58, 12, UI_CARD, UI_BORDER, UI_BG);
  formatTz(b, sizeof b, false);
  uiText(b, SCREEN_W / 2, 45, TC_DATUM, UI_ACCENT, 4, true);
  uiText(tzHint(GMT_OFFSET_SEC / 60), SCREEN_W / 2, 76, TC_DATUM, UI_DIM, 2);
  struct tm t;
  if (getLocalTime(&t, 0)) snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);
  else                     snprintf(b, sizeof b, "--:--");
  uiText(b, SCREEN_W - 20, 46, TR_DATUM, UI_TEXT, 2, true);
  uiText("NOW", 20, 46, TL_DATUM, UI_MUTE, 2);

  drawKey(TZ_X1, TZ_Y1, TZ_W, TZ_H, "-1h",  RGB565(40, 52, 96));
  drawKey(TZ_X2, TZ_Y1, TZ_W, TZ_H, "+1h",  RGB565(28, 96, 180));
  drawKey(TZ_X1, TZ_Y2, TZ_W, TZ_H, "-30m", RGB565(40, 52, 96));
  drawKey(TZ_X2, TZ_Y2, TZ_W, TZ_H, "+30m", RGB565(28, 96, 180));
  drawKey(TZ_X1, TZ_Y3, TZ_W, TZ_H, "Tokyo", RGB565(24, 120, 92));
  drawKey(TZ_X2, TZ_Y3, TZ_W, TZ_H, DST_OFFSET_SEC ? "DST: ON" : "DST: OFF", DST_OFFSET_SEC ? RGB565(205, 120, 30) : RGB565(52, 60, 90));
}

// ---- Launcher name (the "ENZ" in the logo / status bar) ----
void loadBrand() {
  prefs.begin("ui", true);
  String n = prefs.getString("brand", "JohnDoe");
  prefs.end();
  if (n.length() >= 1 && n.length() <= 8) brandName = n;
}
void saveBrand() { prefs.begin("ui", false); prefs.putString("brand", brandName); prefs.end(); }

String nameBuf;                                   // text being edited
static const char* NAME_ROWS[4] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.", "0123456789" };
#define NK_Y0  78
#define NK_H   28
#define NK_GAP 3
#define NK_FY  204        // Del / Save row
#define NK_FH  30
static void nameKeyRect(int r, int c, int &x, int &y, int &w, int &h) {
  int n = strlen(NAME_ROWS[r]);
  w = 21; h = NK_H; y = NK_Y0 + r * (NK_H + NK_GAP);
  x = (SCREEN_W - (n * w + (n - 1) * 2)) / 2 + c * (w + 2);
}

static void drawNameScreen() {
  drawScreenFrame("Name");
  uiCard(8, 38, SCREEN_W - 16, 34, 10, RGB565(24, 38, 70), UI_ACCENT, UI_BG);
  uiText("NAME", 20, 47, TL_DATUM, UI_MUTE, 2);
  String v = nameBuf; if (nameBuf.length() < 8) v += "_";
  uiText(v.c_str(), 72, 4 + 38, TL_DATUM, UI_TEXT, 4, true);
  char cnt[8]; snprintf(cnt, sizeof cnt, "%d/8", (int)nameBuf.length());
  uiText(cnt, SCREEN_W - 20, 47, TR_DATUM, UI_MUTE, 2);
  for (int r = 0; r < 4; r++) {
    int n = strlen(NAME_ROWS[r]);
    for (int c = 0; c < n; c++) {
      int x, y, w, h; nameKeyRect(r, c, x, y, w, h);
      char l[2] = { NAME_ROWS[r][c], 0 };
      drawKey(x, y, w, h, l, RGB565(34, 44, 82));
    }
  }
  drawKey(8,   NK_FY, 108, NK_FH, "Del",  RGB565(150, 50, 60));
  drawKey(124, NK_FY, 108, NK_FH, "Save", nameBuf.length() ? RGB565(24, 120, 92) : RGB565(52, 60, 90));
}

static void handleName(int tx, int ty) {
  if (ty >= NK_FY && ty < NK_FY + NK_FH) {
    if (tx >= 8 && tx < 116) { if (nameBuf.length()) nameBuf.remove(nameBuf.length() - 1); drawNameScreen(); }
    else if (tx >= 124 && tx < SCREEN_W - 8 && nameBuf.length()) {
      brandName = nameBuf; saveBrand();
      settingsScreen = 0; drawSettings();
    }
    return;
  }
  for (int r = 0; r < 4; r++) {
    int n = strlen(NAME_ROWS[r]);
    for (int c = 0; c < n; c++) {
      int x, y, w, h; nameKeyRect(r, c, x, y, w, h);
      if (tx >= x && tx < x + w + 2 && ty >= y && ty < y + h + 2) {
        if (nameBuf.length() < 8) nameBuf += String(NAME_ROWS[r][c]);
        drawNameScreen();
        return;
      }
    }
  }
}

// ---- Location (weather coordinates) ----
void loadLocation() {
  prefs.begin("wx", true);
  WX_LAT = prefs.getFloat("lat", WX_LAT);
  WX_LON = prefs.getFloat("lon", WX_LON);
  prefs.end();
}
void saveLocation() {
  prefs.begin("wx", false);
  prefs.putFloat("lat", WX_LAT);
  prefs.putFloat("lon", WX_LON);
  prefs.end();
}

String locBuf[2];            // text being edited: [0] = latitude, [1] = longitude
int  locField = 0;           // which field is selected
bool locFresh = true;        // first key replaces the old text
bool locErr = false;
bool locSignTouched = false;  // "+/-" pressed before typing -> keep the sign when the new digits replace the old text

#define KP_Y0  108
#define KP_H   29
#define KP_GAP 2
static void locEnter() {
  locBuf[0] = String(WX_LAT, 4); locBuf[1] = String(WX_LON, 4);
  locField = 0; locFresh = true; locErr = false; locSignTouched = false;
}
static void locKeyRect(int r, int c, int &x, int &y, int &w, int &h) {
  y = KP_Y0 + r * (KP_H + KP_GAP); h = KP_H;
  if (r < 3) { x = 8 + c * 58;  w = 52;  }     // 4 keys per row
  else       { x = 8 + c * 116; w = 110; }     // bottom row: "0" and "Save"
}
static const char* locKeyLabel(int r, int c) {
  static const char* L[3][4] = { {"1","2","3","Del"}, {"4","5","6","+/-"}, {"7","8","9","."} };
  if (r < 3) return L[r][c];
  return c == 0 ? "0" : "Save";
}

static void drawLocScreen() {
  drawScreenFrame("Location");
  const char* names[2] = { "LAT", "LON" };
  for (int f = 0; f < 2; f++) {
    int y = 38 + f * 34;
    bool sel = (f == locField);
    uiCard(8, y, SCREEN_W - 16, 30, 10, sel ? RGB565(24, 38, 70) : UI_CARD, sel ? (locErr ? UI_BAD : UI_ACCENT) : UI_BORDER, UI_BG);
    uiText(names[f], 20, y + 7, TL_DATUM, UI_MUTE, 2);
    String v = locBuf[f]; if (sel && !locFresh) v += "_";
    uiText(v.c_str(), 68, y + 2, TL_DATUM, UI_TEXT, 4, true);
  }
  if (locErr) uiText("Out of range", SCREEN_W - 8, 8, TR_DATUM, UI_BAD, 2);
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < (r < 3 ? 4 : 2); c++) {
      int x, y, w, h; locKeyRect(r, c, x, y, w, h);
      const char* l = locKeyLabel(r, c);
      uint16_t col = RGB565(34, 44, 82);
      if (!strcmp(l, "Del")) col = RGB565(150, 50, 60);
      else if (!strcmp(l, "Save")) col = RGB565(24, 120, 92);
      else if (!strcmp(l, "+/-") || !strcmp(l, ".")) col = RGB565(52, 60, 90);
      drawKey(x, y, w, h, l, col);
    }
  }
}

static bool locValid() {
  for (int f = 0; f < 2; f++) {
    bool dig = false;
    for (unsigned i = 0; i < locBuf[f].length(); i++) if (locBuf[f][i] >= '0' && locBuf[f][i] <= '9') dig = true;
    if (!dig) return false;
  }
  float la = locBuf[0].toFloat(), lo = locBuf[1].toFloat();
  return fabsf(la) <= 90.0f && fabsf(lo) <= 180.0f;
}

static void locPress(const char* l) {
  String &b = locBuf[locField];
  if (!strcmp(l, "Save")) {
    if (!locValid()) { locErr = true; drawLocScreen(); return; }
    WX_LAT = locBuf[0].toFloat(); WX_LON = locBuf[1].toFloat();
    saveLocation();
    wxOK = false;                              // weather is manual: the next tap on the home weather strip fetches for the new place
    settingsScreen = 0; drawSettings();
    return;
  }
  if (!strcmp(l, "Del")) {
    if (locFresh) b = ""; else if (b.length()) b.remove(b.length() - 1);
    locFresh = false;
  } else if (!strcmp(l, "+/-")) {
    if (b.startsWith("-")) b.remove(0, 1); else b = "-" + b;
    if (locFresh) locSignTouched = true;
  } else {                                        // digit or '.'
    if (locFresh) { b = (locSignTouched && b.startsWith("-")) ? "-" : ""; locFresh = false; }
    if (l[0] == '.' && b.indexOf('.') >= 0) { /* only one decimal point */ }
    else if (b.length() < 11) b += l;
  }
  locErr = false;
  drawLocScreen();
}

static void handleLoc(int tx, int ty) {
  for (int f = 0; f < 2; f++) {
    if (tx >= 8 && tx < SCREEN_W - 8 && ty >= 38 + f * 34 && ty < 68 + f * 34) { locField = f; locFresh = true; locErr = false; locSignTouched = false; drawLocScreen(); return; }
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < (r < 3 ? 4 : 2); c++) {
      int x, y, w, h; locKeyRect(r, c, x, y, w, h);
      if (tx >= x && tx < x + w && ty >= y && ty < y + h) { locPress(locKeyLabel(r, c)); return; }
    }
  }
}

// ---- Altitude (for the sea-level pressure correction) ----
String altBuf;
bool altFresh = true, altErr = false;
void loadAltitude() { prefs.begin("wx", true);  pressAltM = prefs.getFloat("alt", PRESSURE_ALT_M); prefs.end(); }
void saveAltitude() { prefs.begin("wx", false); prefs.putFloat("alt", pressAltM); prefs.end(); }
static void altEnter() {
  altBuf = (pressAltM == (int)pressAltM) ? String((int)pressAltM) : String(pressAltM, 1);
  altFresh = true; altErr = false;
}
static void drawAltScreen() {
  drawScreenFrame("Altitude");
  uiCard(8, 38, SCREEN_W - 16, 30, 10, RGB565(24, 38, 70), altErr ? UI_BAD : UI_ACCENT, UI_BG);
  uiText("ALT", 20, 45, TL_DATUM, UI_MUTE, 2);
  String v = altBuf; if (!altFresh) v += "_";
  uiText(v.c_str(), 68, 40, TL_DATUM, UI_TEXT, 4, true);
  uiText("m", SCREEN_W - 20, 45, TR_DATUM, UI_MUTE, 2);
  if (altErr) uiText("Out of range", SCREEN_W - 8, 8, TR_DATUM, UI_BAD, 2);
  uiText("Sea-level pressure correction", 8, 78, TL_DATUM, UI_MUTE, 2);
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < (r < 3 ? 4 : 2); c++) {
      int x, y, w, h; locKeyRect(r, c, x, y, w, h);
      const char* l = locKeyLabel(r, c);
      uint16_t col = RGB565(34, 44, 82);
      if (!strcmp(l, "Del")) col = RGB565(150, 50, 60);
      else if (!strcmp(l, "Save")) col = RGB565(24, 120, 92);
      else if (!strcmp(l, "+/-") || !strcmp(l, ".")) col = RGB565(52, 60, 90);
      drawKey(x, y, w, h, l, col);
    }
  }
}
static bool altValid(float& out) {
  bool dig = false;
  for (unsigned i = 0; i < altBuf.length(); i++) if (altBuf[i] >= '0' && altBuf[i] <= '9') dig = true;
  if (!dig) return false;
  out = altBuf.toFloat();
  return out >= -500.0f && out <= 9000.0f;
}
static void altPress(const char* l) {
  if (!strcmp(l, "Save")) {
    float v;
    if (!altValid(v)) { altErr = true; drawAltScreen(); return; }
    pressAltM = v; saveAltitude();
    settingsScreen = 0; drawSettings();
    return;
  }
  if (!strcmp(l, "Del")) {
    if (altFresh) altBuf = ""; else if (altBuf.length()) altBuf.remove(altBuf.length() - 1);
    altFresh = false;
  } else if (!strcmp(l, "+/-")) {
    if (altBuf.startsWith("-")) altBuf.remove(0, 1); else altBuf = "-" + altBuf;
  } else {
    if (altFresh) { altBuf = ""; altFresh = false; }
    if (l[0] == '.' && altBuf.indexOf('.') >= 0) { /* one decimal point only */ }
    else if (altBuf.length() < 8) altBuf += l;
  }
  altErr = false;
  drawAltScreen();
}
static void handleAlt(int tx, int ty) {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < (r < 3 ? 4 : 2); c++) {
      int x, y, w, h; locKeyRect(r, c, x, y, w, h);
      if (tx >= x && tx < x + w && ty >= y && ty < y + h) { altPress(locKeyLabel(r, c)); return; }
    }
  }
}

void drawSettings() {
  if (settingsScreen == 1) { drawTzScreen(); return; }
  if (settingsScreen == 2) { drawLocScreen(); return; }
  if (settingsScreen == 3) { drawNameScreen(); return; }
  if (settingsScreen == 4) { drawAltScreen(); return; }
  if (settingsScreen == 5) { drawEspNowScreen(); return; }
  drawScreenFrame("Setting");
  String sub = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("Not connected");
  if (sub.length() > 10) sub = sub.substring(0, 10);
  drawSettingsRow(ST_Y0 + 0 * ST_PITCH, APP_WIFI, "WiFi", sub.c_str());
  drawSettingsRow(ST_Y0 + 1 * ST_PITCH, APP_WIFI, "Boot WiFi", bootWifi ? "ON" : "OFF");
  char b[24];
  formatTz(b, sizeof b, true);
  drawSettingsRow(ST_Y0 + 2 * ST_PITCH, APP_CLOCK, "Time Zone", b);
  char lc[28]; snprintf(lc, sizeof lc, "%.2f, %.2f", (double)WX_LAT, (double)WX_LON);
  drawSettingsRow(ST_Y0 + 3 * ST_PITCH, APP_LOCATION, "Location", lc);
  char ab[16]; snprintf(ab, sizeof ab, "%d m", (int)pressAltM);
  drawSettingsRow(ST_Y0 + 4 * ST_PITCH, APP_LOCATION, "Altitude", ab);
  char lb[24]; snprintf(lb, sizeof lb, "%d%% Auto %s", manualPct, autoDim ? "ON" : "OFF");
  drawSettingsRow(ST_Y0 + 5 * ST_PITCH, APP_LIGHT, "Display", lb);
  drawSettingsRow(ST_Y0 + 6 * ST_PITCH, APP_NAME, "Name", brandName.c_str());
  char ev[20];
  if (chatManualCh) snprintf(ev, sizeof ev, "%s Ch%d", chatLR ? "LR" : "Normal", chatManualCh);
  else              snprintf(ev, sizeof ev, "%s Auto", chatLR ? "LR" : "Normal");
  drawSettingsRow(ST_Y0 + 7 * ST_PITCH, APP_CHAT, "ESP-NOW", ev);
}

void drawEspNowScreen() {
  drawScreenFrame("ESP-NOW");
  uiText("Mode (both units must match)", 12, 44, TL_DATUM, UI_DIM, 2);
  drawKey(24, 74, 192, 40, "Normal", chatLR ? KC_NORMAL : KC_GOOD);
  drawKey(24, 126, 192, 40, "LongRange", chatLR ? KC_GOOD : KC_NORMAL);
  uiText("LongRange: much longer reach,", 12, 190, TL_DATUM, UI_TEXT, 2);
  uiText("slower link. Normal WiFi", 12, 210, TL_DATUM, UI_TEXT, 2);
  uiText("(weather) always uses Normal.", 12, 230, TL_DATUM, UI_TEXT, 2);
  uiText("Chat channel (match the peer)", 12, 254, TL_DATUM, UI_DIM, 2);
  drawKey(24, 272, 52, 36, "-", KC_NORMAL);
  { char cv[8]; if (chatManualCh == 0) snprintf(cv, sizeof cv, "Auto"); else snprintf(cv, sizeof cv, "Ch%d", chatManualCh);
    drawKey(84, 272, 72, 36, cv, KC_GOOD); }
  drawKey(164, 272, 52, 36, "+", KC_NORMAL);
}
void handleEspNow(int tx, int ty) {
  if (gHit(tx, ty, 24, 272, 52, 36) || gHit(tx, ty, 84, 272, 72, 36) || gHit(tx, ty, 164, 272, 52, 36)) {
    int d = gHit(tx, ty, 24, 272, 52, 36) ? -1 : 1;
    chatManualCh += d;
    if (chatManualCh < 0) chatManualCh = 13;
    if (chatManualCh > 13) chatManualCh = 0;
    saveChatChannel();                       // applied at the next chat start
    drawEspNowScreen();
    return;
  }
  bool v;
  if      (gHit(tx, ty, 24, 74, 192, 40))  v = false;
  else if (gHit(tx, ty, 24, 126, 192, 40)) v = true;
  else return;
  if (v != chatLR) { chatLR = v; saveChatLR(); }   // applied at the next chat start
  drawEspNowScreen();
}
void handleSettings(int tx, int ty) {
  if (settingsScreen == 2) { handleLoc(tx, ty); return; }
  if (settingsScreen == 3) { handleName(tx, ty); return; }
  if (settingsScreen == 4) { handleAlt(tx, ty); return; }
  if (settingsScreen == 5) { handleEspNow(tx, ty); return; }
  if (settingsScreen == 0) {
    if (tx >= 8 && tx < SCREEN_W - 8) {
      int row = (ty - ST_Y0) / ST_PITCH;
      if (ty >= ST_Y0 && row >= 0 && row <= 7 && (ty - ST_Y0) % ST_PITCH < ST_H) {
        if      (row == 0) { currentApp = APP_WIFI; wifiScreen = 0; needsRedraw = true; }
        else if (row == 1) { bootWifi = !bootWifi; saveBootWifi(); drawSettings(); }   // no boot-time connection wait when OFF
        else if (row == 2) { settingsScreen = 1; drawSettings(); }
        else if (row == 3) { locEnter(); settingsScreen = 2; drawSettings(); }
        else if (row == 4) { altEnter(); settingsScreen = 4; drawSettings(); }
        else if (row == 5) { currentApp = APP_LIGHT; needsRedraw = true; }
        else if (row == 6) { nameBuf = brandName; settingsScreen = 3; drawSettings(); }
        else               { settingsScreen = 5; drawSettings(); }
      }
    }
    return;
  }
  // Time zone screen
  auto hit = [&](int x, int y) { return tx >= x && tx < x + TZ_W && ty >= y && ty < y + TZ_H; };
  long m = GMT_OFFSET_SEC / 60;
  bool changed = true;
  if      (hit(TZ_X1, TZ_Y1)) m -= 60;
  else if (hit(TZ_X2, TZ_Y1)) m += 60;
  else if (hit(TZ_X1, TZ_Y2)) m -= 30;
  else if (hit(TZ_X2, TZ_Y2)) m += 30;
  else if (hit(TZ_X1, TZ_Y3)) { m = 9 * 60; DST_OFFSET_SEC = 0; }          // Japan preset
  else if (hit(TZ_X2, TZ_Y3)) { DST_OFFSET_SEC = DST_OFFSET_SEC ? 0 : 3600; }
  else changed = false;
  if (!changed) return;
  if (m < -12 * 60) m = -12 * 60;
  if (m >  14 * 60) m =  14 * 60;
  GMT_OFFSET_SEC = m * 60;
  saveTimezone();
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);   // apply right away
  drawSettings();
}

// =================== SD FILE READER ===================
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS    5
SPIClass sdSPI = SPIClass(VSPI);   // shares VSPI with the display; touch is on HSPI
bool sdReady = false, sdTried = false;

// sdScreen declared earlier

void initSD() {
  sdTried = true;
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, sdSPI) && SD.cardType() != CARD_NONE;
#if ENABLE_JAPANESE
  if (sdReady && bootDone && !jpReady) jpInit();   // late mount: /jpfont16.bin on the card enables Japanese text
#endif
}

String baseName(const String& n) {
  int i = n.lastIndexOf('/');
  return (i >= 0) ? n.substring(i + 1) : n;
}

// ---- Image (JPEG) viewer ----
#if USE_JPEG_VIEWER
bool tjpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bmp);
  return 1;
}
#endif

// =================== CHAT (ESP-NOW direct messenger) ===================
#define CHAT_DIR "/chats"
#define MAX_PEERS 8
#define MAX_CHAT_MSGS 30
#define RX_SLOTS 6
#define HELLO_INTERVAL_MS 5000
#define PEER_TIMEOUT_MS 30000

struct __attribute__((packed)) MsgPacket {
  uint8_t type;          // 1 = HELLO, 2 = CHAT
  uint8_t to_mac[6];     // zeros = broadcast
  char    name[16];
  char    text[200];
};

struct Peer {
  uint8_t mac[6];
  char    name[16];
  unsigned long lastSeen;
  bool    hasUnread;
};

Peer   chatPeers[MAX_PEERS];
int    chatPeerCount = 0;
String myName = "";
String chatLines[MAX_CHAT_MSGS];
int    chatLineCount = 0;
int    composeMode = 0;          // 0 = compose chat, 1 = edit nickname

struct RxItem {
  uint8_t  mac[6];
  uint8_t  data[225];            // ~= sizeof(MsgPacket)
  uint16_t len;
};
volatile RxItem msgRx[RX_SLOTS];
volatile uint8_t msgRxHead = 0, msgRxTail = 0;
unsigned long lastHello = 0;
const uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t GROUP_MAC[6] = {0,0,0,0,0,0};   // pseudo peer: broadcast chat (to_mac zero = everyone)

// ---- helpers ----
String macToStr(const uint8_t* m) {
  char b[13];
  snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(b);
}
bool macEq(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
  return true;
}
bool macIsZero(const uint8_t* m) {
  for (int i = 0; i < 6; i++) if (m[i] != 0) return false;
  return true;
}
int findPeerByMac(const uint8_t* mac) {
  for (int i = 0; i < chatPeerCount; i++) if (macEq(chatPeers[i].mac, mac)) return i;
  return -1;
}
String chatPath(const uint8_t* mac) { return String(CHAT_DIR) + "/" + macToStr(mac) + ".log"; }

void loadMyName() {
  prefs.begin("msg", true);
  String n = prefs.getString("name", "");
  prefs.end();
  if (n.length() == 0) {
    uint8_t m[6]; WiFi.macAddress(m);
    char b[16]; snprintf(b, sizeof(b), "%.6s-%02X%02X", brandName.c_str(), m[4], m[5]);
    n = String(b);
  }
  myName = n;
}
void saveMyName(const String& n) {
  prefs.begin("msg", false); prefs.putString("name", n); prefs.end();
  myName = n;
}

// ---- ESP-NOW receive callback (runs in WiFi task) ----
static void msgRecvCommon(const uint8_t* mac, const uint8_t* data, int len) {
  uint8_t next = (msgRxHead + 1) % RX_SLOTS;
  if (next == msgRxTail) return;        // ring full, drop
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

void ensurePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0; p.encrypt = false;
  esp_now_add_peer(&p);
}

void loadChatLR() { Preferences p; p.begin("chat", true); chatLR = p.getBool("lr", false); p.end(); }
void loadChatChannel() { Preferences p; p.begin("chat", true); chatManualCh = p.getInt("mch", 0); p.end(); if (chatManualCh < 0 || chatManualCh > 13) chatManualCh = 0; }
void saveChatChannel() { Preferences p; p.begin("chat", false); p.putInt("mch", chatManualCh); p.end(); }
void saveChatLR() { Preferences p; p.begin("chat", false); p.putBool("lr", chatLR); p.end(); }
static void applyChatProtocol() {   // LR trades speed for distance; WiFi (weather) needs Normal
  esp_wifi_set_protocol(WIFI_IF_STA, chatLR ? WIFI_PROTOCOL_LR
                                            : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
}
void initChat() {
  loadMyName();
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);    // keep radio fully awake for ESP-NOW
  applyChatProtocol();
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(onMsgRecv);
  ensurePeer(BCAST_MAC);
}

// ---- ESP-NOW on demand (outdoor build): the radio/ESP-NOW run only in Chat -
bool chatOn = false;
void chatStart() {
  if (chatOn) return;
  initChat();
  chatOn = true;
  if (chatManualCh > 0) setChatChannel(chatManualCh);   // fixed channel: talk to non-WiFi peers (e.g. M5Core2 on ch 1)
  lastHello = 0;
  sendHello();
}
void chatStop() {
  if (!chatOn) return;
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);   // back to Normal for WiFi
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  chatOn = false;
  homeTerm = false;
  chatPeerCount = 0;
  curPeer = -1;
  if (bootWifi && savedSSID.length()) connectSaved();   // back to the normal WiFi state
  else if (WiFi.status() != WL_CONNECTED) WiFi.mode(WIFI_OFF);
}

// Switch the ESP-NOW radio channel. 0 = Auto: rejoin WiFi and follow the AP (restores internet).
// 1..13 = hold a fixed channel so a non-WiFi peer can match it (this drops the WiFi link).
void setChatChannel(int ch) {
  chatManualCh = ch;
  chatPeerCount = 0; curPeer = -1;            // rediscover peers on the new channel
  if (ch == 0) {
    WiFi.reconnect();                         // back to AP-driven channel
  } else {
    WiFi.disconnect(false);                   // leave the AP but keep stored creds
    delay(30);
    esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE);
  }
}

void sendHello() {
  MsgPacket m; memset(&m, 0, sizeof(m));
  m.type = 1;
  { String nm = u8cut(myName, 15); strncpy(m.name, nm.c_str(), 15); }
  esp_now_send(BCAST_MAC, (uint8_t*)&m, sizeof(m));
}

bool sendChatMsg(int peerIdx, const String& text) {
  if (peerIdx < 0 || peerIdx >= chatPeerCount) return false;
  MsgPacket m; memset(&m, 0, sizeof(m));
  m.type = 2;
  memcpy(m.to_mac, chatPeers[peerIdx].mac, 6);
  { String nm = u8cut(myName, 15); strncpy(m.name, nm.c_str(), 15); }
  { String tx = u8cut(text, 199); strncpy(m.text, tx.c_str(), 199); }
  const uint8_t* dst = macIsZero(chatPeers[peerIdx].mac) ? BCAST_MAC : chatPeers[peerIdx].mac;
  ensurePeer(dst);
  return esp_now_send(dst, (uint8_t*)&m, sizeof(m)) == ESP_OK;
}

// ---- SD-card chat log ----
void appendChatLog(int peerIdx, bool outgoing, const String& text) {
  if (!sdReady || peerIdx < 0) return;
  if (!SD.exists(CHAT_DIR)) SD.mkdir(CHAT_DIR);
  String path = chatPath(chatPeers[peerIdx].mac);
  { Preferences p; p.begin("chat", false); p.putString("lastpath", path); p.end(); }   // the home terminal shows this conversation
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  String who = outgoing ? "Me" : String(chatPeers[peerIdx].name);
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
static void loadGroupLog() {
  chatLineCount = 0;
  if (!sdReady) return;
  if (!SD.exists("/chats/group.log")) return;
  File f = SD.open("/chats/group.log");
  if (!f) return;
  String ring[MAX_CHAT_MSGS]; int idx = 0;
  String line = "";
  while (f.available()) {
    char c = f.read();
    if (c == '\r') continue;
    if (c == '\n') { ring[idx % MAX_CHAT_MSGS] = line; idx++; line = ""; }
    else line += c;
  }
  if (line.length()) { ring[idx % MAX_CHAT_MSGS] = line; idx++; }
  f.close();
  int total = (idx < MAX_CHAT_MSGS) ? idx : MAX_CHAT_MSGS;
  int start = (idx > MAX_CHAT_MSGS) ? idx % MAX_CHAT_MSGS : 0;
  for (int i = 0; i < total; i++) chatLines[i] = ring[(start + i) % MAX_CHAT_MSGS];
  chatLineCount = total;
}
void loadChatHistory(int peerIdx) {
  chatLineCount = 0;
  if (!sdReady || peerIdx < 0) return;
  String path = chatPath(chatPeers[peerIdx].mac);
  if (!SD.exists(path)) return;
  File f = SD.open(path);
  if (!f) return;
  String ring[MAX_CHAT_MSGS]; int idx = 0;
  String line = "";
  while (f.available()) {
    char c = f.read();
    if (c == '\r') continue;
    if (c == '\n') { ring[idx % MAX_CHAT_MSGS] = line; idx++; line = ""; }
    else line += c;
  }
  if (line.length()) { ring[idx % MAX_CHAT_MSGS] = line; idx++; }
  f.close();
  int total = (idx < MAX_CHAT_MSGS) ? idx : MAX_CHAT_MSGS;
  int start = (idx > MAX_CHAT_MSGS) ? idx % MAX_CHAT_MSGS : 0;
  for (int i = 0; i < total; i++) chatLines[i] = ring[(start + i) % MAX_CHAT_MSGS];
  chatLineCount = total;
}
void deleteChatLog(int peerIdx) {
  if (!sdReady || peerIdx < 0) return;
  String path = chatPath(chatPeers[peerIdx].mac);
  if (SD.exists(path)) SD.remove(path);
  chatLineCount = 0;
}

// ---- drain receive ring (called every loop) ----
void processChatRx() {
  while (msgRxTail != msgRxHead) {
    uint8_t mac[6]; memcpy(mac, (void*)msgRx[msgRxTail].mac, 6);
    MsgPacket m; int len = msgRx[msgRxTail].len;
    if (len > (int)sizeof(m)) len = sizeof(m);
    memcpy(&m, (void*)msgRx[msgRxTail].data, len);
    msgRxTail = (msgRxTail + 1) % RX_SLOTS;
    m.name[15] = 0; m.text[199] = 0;

    int idx = findPeerByMac(mac);
    if (idx < 0 && chatPeerCount < MAX_PEERS) {
      idx = chatPeerCount++;
      memcpy(chatPeers[idx].mac, mac, 6);
      chatPeers[idx].name[0] = 0;
      chatPeers[idx].hasUnread = false;
    }
    if (idx < 0) continue;
    if (m.name[0]) { strncpy(chatPeers[idx].name, m.name, 15); chatPeers[idx].name[15] = 0; }
    chatPeers[idx].lastSeen = millis();

    if (m.type == 1) {                  // HELLO
      if (currentApp == APP_CHAT && msgScreen == 0) drawPeerList();
    } else if (m.type == 2) {           // CHAT
      uint8_t myMac[6]; WiFi.macAddress(myMac);
      bool toMe = macEq(m.to_mac, myMac) || macIsZero(m.to_mac);
      if (!toMe) continue;
      appendChatLog(idx, false, String(m.text));
      if (homeTerm) appendGroupLog(String(chatPeers[idx].name), String(m.text));
      if (homeTerm && currentApp == APP_MENU) {
        String line = String(chatPeers[idx].name) + "|" + String(m.text);
        if (chatLineCount < MAX_CHAT_MSGS) chatLines[chatLineCount++] = line;
        else { for (int i = 0; i < MAX_CHAT_MSGS - 1; i++) chatLines[i] = chatLines[i+1]; chatLines[MAX_CHAT_MSGS-1] = line; }
        needsRedraw = true;
      } else if (currentApp == APP_CHAT && msgScreen == 1 && curPeer == idx) {
        String line = String(chatPeers[idx].name) + "|" + String(m.text);
        if (chatLineCount < MAX_CHAT_MSGS) chatLines[chatLineCount++] = line;
        else { for (int i = 0; i < MAX_CHAT_MSGS - 1; i++) chatLines[i] = chatLines[i+1]; chatLines[MAX_CHAT_MSGS-1] = line; }
        drawChatView();
      } else {
        chatPeers[idx].hasUnread = true;
        if (currentApp == APP_CHAT && msgScreen == 0) drawPeerList();
      }
    }
  }
}

// ---------- Comic-chat terminal (home screen) -------------------------------
// Shows the latest conversation from the SD logs. Tapping the prompt starts
// ESP-NOW and opens the composer; sending returns here, so the whole chat
// happens inside this area. Header tap = next peer, [END] = ESP-NOW off.
static String termPeerName(int i) {
  return chatPeers[i].name[0] ? String(chatPeers[i].name) : macToStr(chatPeers[i].mac);
}
static void termResolvePath() {
  if (termPath.length() && SD.exists(termPath)) return;
  termPath = "";
  if (sdReady && SD.exists("/chats/group.log")) { termPath = "/chats/group.log"; return; }
  { Preferences p; p.begin("chat", true); termPath = p.getString("lastpath", ""); p.end(); }
  if (termPath.length() && SD.exists(termPath)) return;
  termPath = "";
  if (!sdReady) return;
  File dir = SD.open("/chats");
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      String n = String(f.name());
      if (n.endsWith(".log")) { termPath = String("/chats/") + n; break; }
    }
  }
  dir.close();
}
void termOpenComposer() {
  composeMode = 0; kbInput = ""; kbUpper = false; kbLayer = 0; kbJpReset(199);
  msgScreen = 2;
}
bool termEnsurePeer() {
  if (curPeer >= 0) return true;
  for (int i = 0; i < chatPeerCount; i++)           // the partner the terminal was showing
    if (!macIsZero(chatPeers[i].mac) && termPath == chatPath(chatPeers[i].mac)) {
      curPeer = i; loadChatHistory(i); chatPeers[i].hasUnread = false; return true;
    }
  int bi = findPeerByMac(GROUP_MAC);                // else fall back to broadcast: always sendable
  if (bi < 0 && chatPeerCount < MAX_PEERS) {
    bi = chatPeerCount++;
    memset(chatPeers[bi].mac, 0, 6);
    strncpy(chatPeers[bi].name, "Broadcast", 15); chatPeers[bi].name[15] = 0;
    chatPeers[bi].hasUnread = false; chatPeers[bi].lastSeen = millis();
  }
  if (bi < 0) return false;
  curPeer = bi;
  loadGroupLog();
  return true;
}
void termCyclePeer() {
  if (chatPeerCount == 0) return;
  curPeer = (curPeer + 1) % chatPeerCount;
  loadChatHistory(curPeer);
  chatPeers[curPeer].hasUnread = false;
}
static int termLines(String* out, int maxN) {
  if (homeTerm && curPeer >= 0) {                     // live: mirror chatLines
    int n = 0;
    for (int i = (chatLineCount > maxN ? chatLineCount - maxN : 0); i < chatLineCount; i++) out[n++] = chatLines[i];
    return n;
  }
  termResolvePath();
  if (!termPath.length()) return 0;
  File f = SD.open(termPath);
  if (!f) return 0;
  String ring[4]; int idx = 0; String line = "";
  while (f.available()) {
    char c = (char)f.read();
    if (c == '\n') { if (line.length()) { ring[idx % 4] = line; idx++; } line = ""; }
    else if (c != '\r') line += c;
  }
  if (line.length()) { ring[idx % 4] = line; idx++; }
  f.close();
  int total = idx < maxN ? idx : maxN, start = idx > maxN ? idx % maxN : 0, n = 0;
  for (int i = 0; i < total; i++) out[n++] = ring[(start + i) % maxN];
  return n;
}
void drawHomeChatTerm() {
  const int x = 8, y = TERM_Y, w = SCREEN_W - 16, h = 120;
  uiCard(x, y, w, h, 10, UI_CARD, UI_BORDER, UI_BG);
  uiText(chatLR ? "COMIC CHAT [LR]" : "COMIC CHAT", x + 8, y + 5, TL_DATUM, UI_DIM, 2, true);
  String st = homeTerm ? (curPeer >= 0 ? termPeerName(curPeer) : String("SEARCHING...")) : String("ESP-NOW: OFF");
  st = uiFit(st, 118, 2);
  uiText(st.c_str(), x + w - 8, y + 5, TR_DATUM, homeTerm ? UI_ACCENT : UI_MUTE, 2);
  String lines[4];
  int n = termLines(lines, 4);
  if (n == 0) {
    uiText("(no messages yet)", x + 8, y + 26, TL_DATUM, UI_MUTE, 2);
    if (!homeTerm) uiText("Tap the prompt to start", x + 8, y + 44, TL_DATUM, UI_MUTE, 2);
  }
  for (int i = 0; i < n; i++) {
    int p = lines[i].indexOf('|');
    String who = (p >= 0) ? lines[i].substring(0, p) : "";
    String txt = (p >= 0) ? lines[i].substring(p + 1) : lines[i];
    String s = "[" + who + "] " + txt;
    s = uiFit(s, w - 16, 2);
    uiText(s.c_str(), x + 8, y + 24 + i * 17, TL_DATUM, who == "Me" ? UI_ACCENT : UI_TEXT, 2);
  }
  tft.drawFastHLine(x + 8, y + 94, w - 16, UI_BORDER);
  uiText("C:\\> Type message...", x + 8, y + 100, TL_DATUM, homeTerm ? UI_TEXT : UI_DIM, 2);
  if (homeTerm) uiText("[END]", x + w - 8, y + 100, TR_DATUM, UI_BAD, 2, true);
}

// ---- UI: peer list ----
void drawPeerList() {
  drawScreenFrame("Chat");
  { char chl[8]; if (chatManualCh > 0) snprintf(chl, sizeof(chl), "Ch%d", chatManualCh); else snprintf(chl, sizeof(chl), "Auto");
    drawKey(118, 4, 56, 22, chl, KC_NORMAL); }
  drawKey(178, 4, 58, 22, "Name", KC_NORMAL);
  if (chatPeerCount == 0) {
    uiMessage("Waiting for", "nearby devices...", UI_DIM);
  }
  for (int i = 0; i < (chatPeerCount < 6 ? chatPeerCount : 6); i++) {
    int y = 32 + i * 30;
    bool active = (millis() - chatPeers[i].lastSeen) < PEER_TIMEOUT_MS;
    uiCard(8, y, SCREEN_W - 16, 26, 9, UI_CARD, active ? blend565(UI_GOOD, UI_BG, 150) : UI_BORDER, UI_BG);
    uiText(active ? "[ON]" : "[--]", 12, y + 5, TL_DATUM, active ? UI_GOOD : UI_MUTE, 2);
    String n = (chatPeers[i].name[0]) ? String(chatPeers[i].name) : macToStr(chatPeers[i].mac);
    n = uiFit(n, 150, 2);
    uiText(n.c_str(), 58, y + 5, TL_DATUM, UI_TEXT, 2);
    if (chatPeers[i].hasUnread) uiText("[NEW]", SCREEN_W - 16, y + 5, TR_DATUM, UI_WARN, 2, true);
  }
  char info[48];
  if (chatManualCh > 0) snprintf(info, sizeof info, "C:\\> You: %s   ch %d (manual)", myName.c_str(), chatManualCh);
  else                  snprintf(info, sizeof info, "C:\\> You: %s   ch %d", myName.c_str(), WiFi.channel());
  String is = uiFit(String(info), SCREEN_W - 20, 2);
  uiText(is.c_str(), SCREEN_W / 2, SCREEN_H - 24, TC_DATUM, UI_MUTE, 2);
}

void handlePeerList(int tx, int ty) {
  if (gHit(tx, ty, 118, 4, 56, 22)) { msgScreen = 5; drawMsgChannel(); return; }   // channel picker
  if (gHit(tx, ty, 178, 4, 58, 22)) {                     // Name
    composeMode = 1;
    kbInput = myName; kbUpper = false; kbLayer = 0; kbJpReset(15);
    msgScreen = 3; drawMsgKeyboard();
    return;
  }
  for (int i = 0; i < (chatPeerCount < 6 ? chatPeerCount : 6); i++) {
    int y = 32 + i * 30;
    if (ty >= y && ty <= y + 26) {
      curPeer = i;
      chatPeers[i].hasUnread = false;
      loadChatHistory(i);
      msgScreen = 1; drawChatView();
      return;
    }
  }
}

// ---- UI: channel picker ----
void drawMsgChannel() {
  drawScreenFrame("Channel");
  for (int k = 0; k < 14; k++) {                 // 0 = Auto, 1..13 = channel
    int x = 8 + (k % 4) * 58, y = 38 + (k / 4) * 42;
    bool sel = (chatManualCh == k);
    char lbl[8]; if (k == 0) snprintf(lbl, sizeof(lbl), "Auto"); else snprintf(lbl, sizeof(lbl), "%d", k);
    drawKey(x, y, 52, 36, lbl, sel ? KC_GOOD : KC_NORMAL);
  }
  uiText("Auto follows WiFi. Fixed drops WiFi.", SCREEN_W / 2, 218, TC_DATUM, UI_MUTE, 2);
}

void handleMsgChannel(int tx, int ty) {
  for (int k = 0; k < 14; k++) {
    int x = 8 + (k % 4) * 58, y = 34 + (k / 4) * 44;
    if (gHit(tx, ty, x, y, 52, 38)) { setChatChannel(k); msgScreen = 0; drawPeerList(); return; }
  }
}

// ---- UI: chat view ----
// Word-wrap txt into rows: first row gets firstW chars, the rest contW. Breaks on spaces
// where possible, hard-splits over-long words. Fills out[0..n-1] (top to bottom).
void wrapText(const String& txt, int firstW, int contW, String* out, int& n, int maxN) {
  n = 0; int i = 0, L = txt.length(); bool first = true;
  while (i < L && n < maxN) {
    while (i < L && txt[i] == ' ') i++;           // skip leading spaces
    if (i >= L) break;
    int w = first ? firstW : contW; if (w < 6) w = 6;
    int lineStart = i, lastSpace = -1, count = 0, j = i;
    while (j < L && count < w) { if (txt[j] == ' ') lastSpace = j; j++; count++; }
    int lineEnd;
    if (j >= L)                    lineEnd = L;          // remainder fits
    else if (lastSpace > lineStart) lineEnd = lastSpace; // break at last space
    else                           lineEnd = j;          // hard split (long word)
    out[n++] = txt.substring(lineStart, lineEnd);
    i = lineEnd; first = false;
  }
}
// Chat messages with Japanese support: 16px rows, wrapped by pixel width
static void drawChatMessagesJp() {
  const int LH = 16, TOP = 32, MAXR = (SCREEN_H - 56) / LH;     // 13 rows
  String rTxt[MAXR + 8]; bool rFirst[MAXR + 8]; bool rMine[MAXR + 8]; String rWho[MAXR + 8];
  int rN = 0;
  for (int mi = chatLineCount - 1; mi >= 0 && rN < MAXR; mi--) {   // newest first, until the screen is full
    String s = chatLines[mi];
    int p = s.indexOf('|');
    String who = (p >= 0) ? s.substring(0, p) : "";
    String txt = (p >= 0) ? s.substring(p + 1) : s;
    bool mine = (who == "Me");
    int prefixW = jpWidth(("[" + who + "] ").c_str());
    int firstW = (SCREEN_W - 12) - prefixW; if (firstW < 40) firstW = 40;
    String segs[8]; int segN = 0;
    jpWrap(txt, firstW, SCREEN_W - 12, segs, segN, 8);
    if (segN == 0) { segs[0] = ""; segN = 1; }
    for (int k = segN - 1; k >= 0 && rN < MAXR + 8; k--) {
      rTxt[rN] = segs[k]; rFirst[rN] = (k == 0); rMine[rN] = mine; rWho[rN] = who; rN++;
    }
  }
  int show = (rN < MAXR) ? rN : MAXR;
  int y = TOP;
  for (int r = show - 1; r >= 0; r--) {                           // draw top to bottom
    if (rFirst[r]) {
      int w = jpText(("[" + rWho[r] + "] ").c_str(), 6, y, rMine[r] ? UI_ACCENT : UI_DIM, UI_BG);
      jpText(rTxt[r].c_str(), 6 + w, y, UI_TEXT, UI_BG, SCREEN_W - 12 - w);
    } else {
      jpText(rTxt[r].c_str(), 6, y, UI_TEXT, UI_BG, SCREEN_W - 12);
    }
    y += LH;
  }
}

void drawChatView() {
  tft.fillScreen(UI_BG);
  drawBack();
  { String pn = (chatPeers[curPeer].name[0]) ? String(chatPeers[curPeer].name) : macToStr(chatPeers[curPeer].mac);
    pn = uiFit(pn, 124, 2); uiText(pn.c_str(), 64, 6, TL_DATUM, UI_ACCENT, 2, true); }
  drawKey(124, 4, 50, 22, "Del",  KC_BAD);
  drawKey(178, 4, 58, 22, "Type", KC_GOOD);
  tft.drawFastHLine(0, 29, SCREEN_W, UI_BORDER);
  uiText("C:\\> [Type] write   [Del] clear log", 8, SCREEN_H - 16, TL_DATUM, UI_MUTE, 2);
  if (jpReady) { drawChatMessagesJp(); return; }                          // Japanese-capable renderer

  const int MAXROWS = 17, COLS = 52;     // ~52 chars/line at size 1; 17 lines from y=32
  String rTxt[MAXROWS + 8]; bool rFirst[MAXROWS + 8]; bool rMine[MAXROWS + 8]; String rWho[MAXROWS + 8];
  int rN = 0;
  // Gather wrapped rows newest-first (pushed bottom-to-top) until the screen is full
  for (int mi = chatLineCount - 1; mi >= 0 && rN < MAXROWS; mi--) {
    String s = chatLines[mi];
    int p = s.indexOf('|');
    String who = (p >= 0) ? s.substring(0, p) : "";
    String txt = (p >= 0) ? s.substring(p + 1) : s;
    bool mine = (who == "Me");
    int firstW = COLS - ((int)who.length() + 2); if (firstW < 8) firstW = 8;
    String segs[8]; int segN = 0;
    wrapText(txt, firstW, COLS, segs, segN, 8);
    if (segN == 0) { segs[0] = ""; segN = 1; }            // empty text still shows the sender
    for (int k = segN - 1; k >= 0 && rN < MAXROWS + 8; k--) {
      rTxt[rN] = segs[k]; rFirst[rN] = (k == 0); rMine[rN] = mine; rWho[rN] = who; rN++;
    }
  }
  int show = (rN < MAXROWS) ? rN : MAXROWS;
  int y = 32;
  tft.setTextSize(1);
  for (int r = show - 1; r >= 0; r--) {                   // render top to bottom
    if (rFirst[r]) {
      tft.setTextColor(rMine[r] ? UI_ACCENT : UI_DIM);
      tft.setCursor(6, y); tft.print("["); tft.print(rWho[r]); tft.print("] ");
      tft.setTextColor(UI_TEXT); tft.print(rTxt[r]);
    } else {
      tft.setTextColor(UI_TEXT);
      tft.setCursor(6, y); tft.print(rTxt[r]);
    }
    y += 12;
  }
}
void handleChatView(int tx, int ty) {
  if (gHit(tx, ty, 124, 4, 50, 22)) { msgPendingDel = curPeer; msgScreen = 4; drawMsgDelConfirm(); return; }
  if (gHit(tx, ty, 178, 4, 58, 22)) {
    composeMode = 0; kbInput = ""; kbUpper = false; kbLayer = 0; kbJpReset(199);
    msgScreen = 2; drawMsgKeyboard();
  }
}

// ---- UI: keyboard (compose chat or edit nickname) ----
void drawMsgKeyboard() {
  String c;
  if (composeMode == 1) c = "Your nickname";
  else c = String("To ") + uiFit(String((chatPeers[curPeer].name[0]) ? chatPeers[curPeer].name : "peer"), 200, 2);
  if (jpReady) { drawKbJp(c.c_str()); return; }                          // Japanese-capable
  drawKbScreen(c.c_str());
  const char** rows = (kbLayer == 0) ? LET_ROWS : SYM_ROWS;
  for (int r = 0; r < 4; r++) drawCharRow(rows[r], KB_Y0 + r * (KB_ROWH + KB_GAP), kbUpper && kbLayer == 0);
  drawFuncRow();
}
static void msgKbOk() {                                                  // OK key: save nickname / send message
  if (composeMode == 1) {
    String n = kbInput; n.trim(); n = u8cut(n, 15);
    if (n.length() > 0) saveMyName(n);
    msgScreen = 0; drawPeerList();
  } else {
    if (kbInput.length() > 0 && curPeer >= 0) {
      if (sendChatMsg(curPeer, kbInput)) {
        appendChatLog(curPeer, true, kbInput);
        if (homeTerm) appendGroupLog("Me", kbInput);
        String line = "Me|" + kbInput;
        if (chatLineCount < MAX_CHAT_MSGS) chatLines[chatLineCount++] = line;
        else { for (int i = 0; i < MAX_CHAT_MSGS - 1; i++) chatLines[i] = chatLines[i+1]; chatLines[MAX_CHAT_MSGS-1] = line; }
      }
    }
    if (homeTerm) { currentApp = APP_MENU; needsRedraw = true; }
    else { msgScreen = 1; drawChatView(); }
  }
}
void handleMsgKeyboard(int tx, int ty) {
  if (jpReady) { if (kbJpHandle(tx, ty, drawMsgKeyboard) == KB_OK) msgKbOk(); return; }
  const char** rows = (kbLayer == 0) ? LET_ROWS : SYM_ROWS;
  for (int r = 0; r < 4; r++) {
    char c = hitCharRow(tx, ty, rows[r], KB_Y0 + r * (KB_ROWH + KB_GAP), kbUpper && kbLayer == 0);
    if (c) { kbInput += c; drawKbInput(); return; }
  }
  int y = KB_FUNC_Y, h = KB_FUNC_H;
  if (ty >= y && ty <= y + h) {
    if      (tx >= 4   && tx <= 40)  { kbUpper = !kbUpper; drawMsgKeyboard(); }
    else if (tx >= 43  && tx <= 79)  { kbLayer = !kbLayer; drawMsgKeyboard(); }
    else if (tx >= 82  && tx <= 156) { kbInput += ' '; drawKbInput(); }
    else if (tx >= 159 && tx <= 195) { if (kbInput.length()) kbInput.remove(kbInput.length() - 1); drawKbInput(); }
    else if (tx >= 198 && tx <= 236) msgKbOk();
  }
}

// ---- delete-chat confirm ----
void drawMsgDelConfirm() {
  drawChatView();
  String n = (chatPeers[curPeer].name[0]) ? String(chatPeers[curPeer].name) : macToStr(chatPeers[curPeer].mac);
  uiConfirmCard("Delete chat?", n.c_str());
  drawKey(20,  162, 90, 30, "Yes", KC_BAD);
  drawKey(130, 162, 90, 30, "No",  KC_NORMAL);
}
void handleMsgDelConfirm(int tx, int ty) {
  if (gHit(tx, ty, 20, 162, 90, 30)) {
    deleteChatLog(curPeer); msgPendingDel = -1;
    msgScreen = 1; drawChatView();
  } else if (gHit(tx, ty, 130, 162, 90, 30)) {
    msgPendingDel = -1; msgScreen = 1; drawChatView();
  }
}

// ---- dispatchers ----
void drawChat() {
  if      (msgScreen == 0) drawPeerList();
  else if (msgScreen == 1) drawChatView();
  else if (msgScreen == 2) drawMsgKeyboard();
  else if (msgScreen == 3) drawMsgKeyboard();
  else if (msgScreen == 4) drawMsgDelConfirm();
  else if (msgScreen == 5) drawMsgChannel();
}
void handleChat(int tx, int ty) {
  if      (msgScreen == 0) handlePeerList(tx, ty);
  else if (msgScreen == 1) handleChatView(tx, ty);
  else if (msgScreen == 2) handleMsgKeyboard(tx, ty);
  else if (msgScreen == 3) handleMsgKeyboard(tx, ty);
  else if (msgScreen == 4) handleMsgDelConfirm(tx, ty);
  else if (msgScreen == 5) handleMsgChannel(tx, ty);
}

// =================== MUSIC PLAYER (WAV files from /music, ESP32 built-in DAC on GPIO26) ===================
// Wiring: the speaker connects to the board's speaker header (driven from GPIO26 through the on-board amplifier).
// Plays 8/16-bit PCM WAV, mono or stereo (stereo is mixed down), 8-48 kHz. Keeps playing in the background.
#if ENABLE_MUSIC
#define MUSIC_DIR "/music"
#define MUSIC_MAX 64
String musicNames[MUSIC_MAX]; int musicCount = 0, musicTop = 0, musicCur = -1;
bool musicPlaying = false, musicPaused = false;
int  musicVol = 60;                        // 0..100 (saved)
bool musicUiDirty = true;                  // track changed -> redraw the whole player
String musicError = "";
String musicCurName = "";                 // name of the track being played (survives a rescan)
static File musicFile;
static uint32_t musicDataStart = 0, musicDataLeft = 0, musicDataTotal = 0, musicRate = 22050, musicBytesPlayed = 0;
static uint16_t musicCh = 2, musicBits = 16;
static uint8_t musicRaw[1024];             // raw WAV bytes read from the SD card
static int musicRawLen = 0, musicRawPos = 0;
static uint8_t musicOut8[1024];            // resampled 8-bit mono samples for the DAC
static int musicOutLen = 0, musicOutPos = 0;
static int32_t rsPrev = 0, rsCur = 0;      // linear resampler (WAV rate -> MUSIC_OUT_HZ)
static uint32_t rsFrac = 0, rsStep = 0; static int rsOwe = 0;
static bool musicAudioOn = false;
#define MUSIC_OUT_HZ 32000                 // fixed DAC rate (the new DAC driver needs >= ~20 kHz); every file is resampled to it

void loadMusicVol() { prefs.begin("music", true); musicVol = constrain(prefs.getInt("vol", 60), 0, 100); prefs.end(); }

void musicScan() {
  musicCount = 0; musicTop = 0;
  if (!sdReady) return;
  if (!SD.exists(MUSIC_DIR)) SD.mkdir(MUSIC_DIR);
  File d = SD.open(MUSIC_DIR);
  if (!d) return;
  File f = d.openNextFile();
  while (f && musicCount < MUSIC_MAX) {
    if (!f.isDirectory()) {
      String n = baseName(String(f.name())); String l = n; l.toLowerCase();
      if (l.endsWith(".wav") && n[0] != '.') musicNames[musicCount++] = n;
    }
    f = d.openNextFile();
  }
  d.close();
  for (int i = 1; i < musicCount; i++) {                    // sort by name (case-insensitive)
    String key = musicNames[i], kl = key; kl.toLowerCase();
    int j = i - 1;
    while (j >= 0) { String jl = musicNames[j]; jl.toLowerCase(); if (jl.compareTo(kl) <= 0) break; musicNames[j + 1] = musicNames[j]; j--; }
    musicNames[j + 1] = key;
  }
  if (musicCurName.length()) { musicCur = -1; for (int i = 0; i < musicCount; i++) if (musicNames[i] == musicCurName) musicCur = i; }
}

// ---- audio output: 8-bit mono at MUSIC_OUT_HZ on GPIO26 (DAC2). GPIO25 stays the touch clock. ----
// loop() puts samples into a ring buffer; a small task feeds them to the DAC with blocking writes (the DAC's DMA
// paces that task), so the sound does not depend on how often or how quickly loop() runs.
#define MUSIC_RING 8192
static uint8_t musicRing[MUSIC_RING];
static volatile uint32_t ringHead = 0, ringTail = 0;      // free-running counters (producer: head, consumer: tail)
static volatile bool audioRun = false, audioTaskDone = true, ringFlush = false;
static TaskHandle_t audioTaskH = NULL;
static size_t ringAvail() { return (size_t)(ringHead - ringTail); }
static size_t ringSpace() { return MUSIC_RING - ringAvail(); }
static size_t ringWrite(const uint8_t* d, size_t n) {
  size_t sp = ringSpace(); if (n > sp) n = sp;
  for (size_t i = 0; i < n; i++) musicRing[(ringHead + i) % MUSIC_RING] = d[i];
  __sync_synchronize();
  ringHead = ringHead + n;
  return n;
}
static void ringRead(uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) d[i] = musicRing[(ringTail + i) % MUSIC_RING];
  __sync_synchronize();
  ringTail = ringTail + n;
}

#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
static dac_continuous_handle_t musicDac = NULL;
static bool audioDevOpen() {
  dac_continuous_config_t cfg = {};
  cfg.chan_mask = DAC_CHANNEL_MASK_CH1;                     // CH1 = GPIO26
  cfg.desc_num = 6; cfg.buf_size = 1024;
  cfg.freq_hz = MUSIC_OUT_HZ; cfg.offset = 0;
  cfg.clk_src = DAC_DIGI_CLK_SRC_DEFAULT;
  cfg.chan_mode = DAC_CHANNEL_MODE_SIMUL;
  if (dac_continuous_new_channels(&cfg, &musicDac) != ESP_OK) { musicDac = NULL; return false; }
  if (dac_continuous_enable(musicDac) != ESP_OK) { dac_continuous_del_channels(musicDac); musicDac = NULL; return false; }
  return true;
}
static void audioDevClose() {
  if (musicDac) { dac_continuous_disable(musicDac); dac_continuous_del_channels(musicDac); musicDac = NULL; }
}
static void audioDevWrite(const uint8_t* d, size_t n) {      // blocks until the data is loaded into the DMA buffers
  if (musicDac) dac_continuous_write(musicDac, (uint8_t*)d, n, NULL, -1);
}
#else
static bool musicI2sOn = false;
static bool audioDevOpen() {
  i2s_config_t cfg; memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = MUSIC_OUT_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_MSB;
  cfg.dma_buf_count = 8; cfg.dma_buf_len = 512;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
  i2s_set_pin(I2S_NUM_0, NULL);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);                // left channel = DAC2 = GPIO26
  i2s_zero_dma_buffer(I2S_NUM_0);
  musicI2sOn = true;
  return true;
}
static void audioDevClose() {
  if (musicI2sOn) { i2s_zero_dma_buffer(I2S_NUM_0); i2s_driver_uninstall(I2S_NUM_0); musicI2sOn = false; }
}
static void audioDevWrite(const uint8_t* d, size_t n) {
  static int16_t buf[256 * 2];
  size_t done = 0;
  while (done < n) {
    size_t chunk = (n - done) < 256 ? (n - done) : 256;
    for (size_t i = 0; i < chunk; i++) { int16_t v = (int16_t)((uint16_t)d[done + i] << 8); buf[2 * i] = v; buf[2 * i + 1] = v; }
    size_t w = 0;
    i2s_write(I2S_NUM_0, buf, chunk * 4, &w, portMAX_DELAY);
    done += chunk;
  }
}
#endif

static void audioTask(void*) {
  static uint8_t chunk[1024];
  while (audioRun) {
    if (ringFlush) { ringTail = ringHead; ringFlush = false; }          // skip / seek: drop what is queued
    if (ringAvail() >= sizeof(chunk)) ringRead(chunk, sizeof(chunk));
    else memset(chunk, 128, sizeof(chunk));                              // nothing ready: play silence (keeps the DAC fed)
    audioDevWrite(chunk, sizeof(chunk));                                 // ~32 ms per chunk, paced by the DAC
  }
  audioTaskDone = true;
  vTaskDelete(NULL);
}
static bool audioOutStart() {
  if (audioTaskH) return true;
  if (!audioDevOpen()) return false;
  ringHead = ringTail = 0; ringFlush = false;
  audioRun = true; audioTaskDone = false;
  if (xTaskCreatePinnedToCore(audioTask, "enzaudio", 4096, NULL, 5, &audioTaskH, 1) != pdPASS) { audioRun = false; audioTaskDone = true; audioDevClose(); return false; }
  return true;
}
static void audioOutStop() {
  if (!audioTaskH) return;
  audioRun = false;
  for (int i = 0; i < 100 && !audioTaskDone; i++) vTaskDelay(pdMS_TO_TICKS(5));   // the task ends after its current write
  audioTaskH = NULL;
  audioDevClose();
}

// ---- WAV header ----
static bool musicWavParse() {
  uint8_t h[12];
  if (musicFile.read(h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) { musicError = "Not a WAV file"; return false; }
  bool fmtOk = false; uint32_t fsize = musicFile.size();
  while (musicFile.available() >= 8) {
    uint8_t ch[8];
    if (musicFile.read(ch, 8) != 8) break;
    uint32_t sz = ch[4] | (ch[5] << 8) | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t f[16];
      if (sz < 16 || musicFile.read(f, 16) != 16) break;
      uint16_t tag = f[0] | (f[1] << 8);
      musicCh = f[2] | (f[3] << 8);
      musicRate = f[4] | (f[5] << 8) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
      musicBits = f[14] | (f[15] << 8);
      if (sz > 16) musicFile.seek(musicFile.position() + (sz - 16));
      fmtOk = (tag == 1 || tag == 0xFFFE);
    } else if (memcmp(ch, "data", 4) == 0) {
      musicDataStart = musicFile.position();
      if (sz == 0 || sz > fsize - musicDataStart) sz = fsize - musicDataStart;     // streamed / truncated files
      musicDataLeft = musicDataTotal = sz;
      if (!fmtOk) { musicError = "Not PCM audio"; return false; }
      if ((musicCh != 1 && musicCh != 2) || (musicBits != 8 && musicBits != 16)) { musicError = "Use 8/16-bit PCM"; return false; }
      if (musicRate < 8000 || musicRate > 48000) { musicError = "Rate must be 8-48 kHz"; return false; }
      return true;
    } else {
      musicFile.seek(musicFile.position() + sz + (sz & 1));
    }
  }
  musicError = "Bad WAV file";
  return false;
}

static void musicStopFile() { if (musicFile) musicFile.close(); }
void musicStop() {                                          // stop completely (keeps the playlist)
  musicStopFile(); audioOutStop(); musicAudioOn = false;
  musicPlaying = false; musicPaused = false; musicUiDirty = true;
}
bool musicStart(int idx) {
  if (idx < 0 || idx >= musicCount) return false;
  musicStopFile();
  musicError = "";
  musicFile = SD.open(String(MUSIC_DIR) + "/" + musicNames[idx], FILE_READ);
  if (!musicFile) { musicError = "Cannot open file"; return false; }
  if (!musicWavParse()) { musicFile.close(); musicPlaying = false; musicUiDirty = true; return false; }
  if (!audioOutStart()) { musicFile.close(); musicError = "Audio output failed"; musicPlaying = false; musicUiDirty = true; return false; }
  musicAudioOn = true;
  musicCur = idx; musicCurName = musicNames[idx]; musicPlaying = true; musicPaused = false;
  musicBytesPlayed = 0; musicOutLen = musicOutPos = 0; musicUiDirty = true;
  musicRawLen = musicRawPos = 0;                            // restart the resampler
  rsStep = (uint32_t)(((uint64_t)musicRate << 16) / MUSIC_OUT_HZ); rsFrac = 0; rsOwe = 2; rsPrev = rsCur = 0;
  return true;
}
void musicNext(int dir) {                                   // wraps around the playlist; skips files that cannot be played
  if (musicCount == 0) return;
  int base = (musicCur < 0) ? (dir > 0 ? -1 : 0) : musicCur;
  for (int k = 1; k <= musicCount; k++) {
    int n = ((base + dir * k) % musicCount + musicCount) % musicCount;
    if (musicStart(n)) return;
  }
  musicStop();                                              // nothing playable
}

// Next source frame from the raw buffer as a mono 16-bit value; false when the buffer has no complete frame left
static bool musicNextSource(int32_t& v) {
  int step = (musicBits / 8) * musicCh;
  if (musicRawPos + step > musicRawLen) return false;
  const uint8_t* p = musicRaw + musicRawPos; musicRawPos += step;
  if (musicBits == 16) { v = (int16_t)(p[0] | (p[1] << 8)); if (musicCh == 2) v = (v + (int16_t)(p[2] | (p[3] << 8))) / 2; }
  else                 { v = ((int)p[0] - 128) << 8;        if (musicCh == 2) v = (v + (((int)p[1] - 128) << 8)) / 2; }
  return true;
}
// Fill musicOut8 with 8-bit samples at MUSIC_OUT_HZ (linear resampling, volume applied). Returns the count.
static int musicGenerate() {
  int n = 0, gain = musicVol * musicVol / 100;              // quadratic volume = more natural steps
  while (n < (int)sizeof(musicOut8)) {
    while (rsOwe > 0) { int32_t s; if (!musicNextSource(s)) return n; rsPrev = rsCur; rsCur = s; rsOwe--; }
    int32_t v = rsPrev + (int32_t)(((int64_t)(rsCur - rsPrev) * (int32_t)rsFrac) >> 16);
    v = v * gain / 100;
    int u = (v >> 8) + 128; if (u < 0) u = 0; if (u > 255) u = 255;
    musicOut8[n++] = (uint8_t)u;
    rsFrac += rsStep;
    while (rsFrac >= 65536) { rsFrac -= 65536; rsOwe++; }
  }
  return n;
}

// Call every loop(): keeps the DAC's DMA buffers full (non-blocking). The next track starts by itself.
// Outdoor version: the Music button on the main screen is a simple toggle.
// Tap once: scan /music, force 100% volume and play from the first track (auto-advances).
// Tap again: stop. There is no player / playlist / volume screen anymore.
void homeMusicToggle() {
  if (musicPlaying) { musicStop(); return; }
  musicScan();
  if (musicCount == 0) return;
  musicVol = 100;
  musicStart(0);
}

void musicUpdate() {
  if (!musicPlaying || musicPaused || !musicAudioOn) return;
  for (int iter = 0; iter < 6; iter++) {
    if (musicOutPos >= musicOutLen) {
      musicOutLen = musicGenerate(); musicOutPos = 0;
      if (musicOutLen == 0) {                               // out of source data: refill from the SD card
        int keep = musicRawLen - musicRawPos;
        memmove(musicRaw, musicRaw + musicRawPos, keep);
        musicRawLen = keep; musicRawPos = 0;
        uint32_t room = sizeof(musicRaw) - keep, want = musicDataLeft < room ? musicDataLeft : room;
        if (want == 0) { musicNext(+1); return; }           // end of this track
        int got = musicFile.read(musicRaw + keep, want);
        if (got <= 0) { musicDataLeft = 0; continue; }
        musicRawLen += got; musicDataLeft -= got; musicBytesPlayed += got;
        continue;
      }
    }
    size_t w = ringWrite(musicOut8 + musicOutPos, musicOutLen - musicOutPos);
    musicOutPos += w;
    if (musicOutPos < musicOutLen) return;                  // ring buffer is full for now
  }
}

#endif   // ENABLE_MUSIC

// =================== PHOTO FRAME / SLIDESHOW ===================
// Scans the photo folder (default /photos) for JPEGs. Pick which ones to include, set an
// interval (1/2/3 min), then Start. Tap during the show = next picture;
// Back = return to the config screen.
#define MAX_SLIDES 40
String slideNames[MAX_SLIDES];
bool   slideSel[MAX_SLIDES];
int    slideCount = 0;
int    slideCur = 0;
unsigned long lastSlide = 0;

// Folder the slideshow reads its JPEGs from (default /photos, created automatically)
String photoDir = "/picture";
void loadPhotoDir() {
  prefs.begin("frame", true); photoDir = prefs.getString("dir", "/picture"); prefs.end();
  if (photoDir.length() < 1 || photoDir[0] != '/') photoDir = "/picture";
}
static String photoPath(const String& name) { return (photoDir == "/") ? String("/") + name : photoDir + "/" + name; }
static void ensurePhotoDir() { if (sdReady && photoDir != "/" && !SD.exists(photoDir)) SD.mkdir(photoDir); }

void scanSlides() {
  slideCount = 0;
  if (!sdReady) return;
  File root = SD.open(photoDir);
  if (!root) return;
  File f = root.openNextFile();
  while (f && slideCount < MAX_SLIDES) {
    if (!f.isDirectory()) {
      String n = baseName(String(f.name()));
      String low = n; low.toLowerCase();
      if (low.endsWith(".jpg") || low.endsWith(".jpeg")) {
        slideNames[slideCount] = n;
        slideSel[slideCount] = true;   // selected by default
        slideCount++;
      }
    }
    f = root.openNextFile();
  }
  root.close();
}

int firstSelected() { for (int i = 0; i < slideCount; i++) if (slideSel[i]) return i; return -1; }
int nextSelected(int cur) {
  for (int i = cur + 1; i < slideCount; i++) if (slideSel[i]) return i;
  for (int i = 0; i <= cur; i++) if (slideSel[i]) return i;
  return cur;
}

#if USE_JPEG_VIEWER
void drawFrameImage(const String& name) {
  String path = photoPath(name);
  uint16_t w = 0, h = 0;
  if (TJpgDec.getSdJpgSize(&w, &h, path.c_str()) != JDR_OK || w == 0 || h == 0) {
    tft.setTextColor(TFT_RED, TFT_BLACK); tft.setTextSize(2);
    tft.setCursor(20, 110); tft.print("Can't open img");
    return;
  }
  int scale = 1;
  while ((w / scale > SCREEN_W || h / scale > SCREEN_H) && scale < 8) scale *= 2;
  TJpgDec.setJpgScale(scale);
  int dw = w / scale, dh = h / scale;
  int x = (SCREEN_W - dw) / 2, y = (SCREEN_H - dh) / 2;
  if (x < 0) x = 0; if (y < 0) y = 0;
  JRESULT res = TJpgDec.drawSdJpg(x, y, path.c_str());
  // JDR_INTR = image was clipped at the screen edge (too large to shrink further); not an error
  if (res != JDR_OK && res != JDR_INTR) {
    tft.setTextColor(TFT_RED, TFT_BLACK); tft.setTextSize(2);
    tft.setCursor(20, 110); tft.print("Decode error");
  }
}
#else
void drawFrameImage(const String& name) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(2);
  tft.setCursor(20, 110); tft.print("JPEG disabled");
}
#endif

void showSlide() { tft.fillScreen(TFT_BLACK); drawFrameImage(slideNames[slideCur]); }   // no overlay: any tap returns to the main screen

void updateSlideshow() {
  if (frameScreen != 1) return;
  if (millis() - lastSlide > 15000UL) {   // fixed 15-second interval (outdoor version)
    slideCur = nextSelected(slideCur);
    lastSlide = millis();
    showSlide();
  }
}

void handleSlideshow(int tx, int ty) {     // any tap ends the show and returns to the main screen
  frameScreen = 0;
  currentApp = APP_MENU;
  needsRedraw = true;
}

void drawFrame() {
  // Outdoor version: no selection pages. Entering Photo starts the /picture slideshow at once.
  if (!sdTried) initSD();
  loadPhotoDir();
  if (frameScreen == 1) { showSlide(); return; }
  ensurePhotoDir(); scanSlides();
  int f = firstSelected();
  if (f < 0) {
    tft.fillScreen(UI_BG);
    uiMessage("No pictures", "put JPEGs in /picture - tap to go back", UI_DIM);
    frameScreen = 4;   // message state: any tap returns to main
    return;
  }
  frameScreen = 1; slideCur = f; lastSlide = millis();
  showSlide();
}
void handleFrame(int tx, int ty) {
  // Slideshow (1) and the no-pictures message (4): any tap returns to the main screen.
  handleSlideshow(tx, ty);
}

// =================== SETUP / LOOP ===================
static void bootLog(const char* what) { Serial.printf("[ENZ] %s  (free heap %u)\n", what, (unsigned)ESP.getFreeHeap()); }

void setup() {
  Serial.begin(115200);
  bootLog("boot start");

  // Onboard RGB LED off at boot (active LOW -> HIGH = off)
  pinMode(LED_R, OUTPUT); digitalWrite(LED_R, HIGH);
  pinMode(LED_G, OUTPUT); digitalWrite(LED_G, HIGH);
  pinMode(LED_B, OUTPUT); digitalWrite(LED_B, HIGH);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);   // portrait (matches TFT rotation 0/2)
  bootLog("touch ready");

  initIndoor();
  bootLog("sensor checked");
  randomSeed(esp_random());   // varied tiles/mines/sequences for the games
// ESP-NOW chat is NOT started at boot in the outdoor build: it starts when
// Chat is opened and stops again when you leave it (see chatStart/chatStop).
#if USE_JPEG_VIEWER
  tft.setSwapBytes(true);
  TJpgDec.setCallback(tjpgOutput);
#endif

  bootLog("chat on demand");
  tft.init();
  bootLog("display ready");
  loadRotation();               // saved orientation (Setting -> Display -> Rotate screen)
  loadInvert();                 // saved colour polarity (Setting -> Display -> Invert colors)
  loadBrand();                  // launcher name (default JohnDoe)
  drawSplash();
  {
    char bb[48];
    snprintf(bb, sizeof(bb), "Starting %s OS...", brandName.c_str());
    bootLine(bb);
  }
  bootLine("Initializing Kernel...");
  bootLine("Loading Font & SKK Engine...");
  bootLog("splash drawn");
  unsigned long splashT = millis();

  loadWifiCreds();              // pull saved SSID/pass from flash (if any)
  loadTimezone();               // saved time zone (default: Japan)
  loadBrightness();             // saved screen brightness (slider)
  loadLocation();               // saved weather coordinates
  loadAltitude();               // saved altitude for pressure correction
  loadBootWifi();               // saved boot-WiFi preference
  loadChatLR();                 // saved ESP-NOW Normal/LongRange preference
  loadChatChannel();            // saved fixed chat channel (0 = Auto, follows WiFi)
  prLoad();                     // saved pressure history
#if ENABLE_MUSIC
  loadMusicVol();               // saved music volume
#endif
  if (bootWifi && savedSSID.length()) {   // connect at boot only when the user asked for it
    bootLine("Connecting WiFi...");
    WiFi.mode(WIFI_STA);
    drawSplashProgress(20);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
      delay(150);
      drawSplashProgress(20 + (int)((millis() - t0) * 30 / 12000));
    }
    bootLine(WiFi.status() == WL_CONNECTED ? "WiFi Connected." : "WiFi: no answer");
  } else {
    WiFi.mode(WIFI_OFF);          // radio stays off: weather fetches are manual
    bootLine(bootWifi ? "WiFi: no saved network" : "WiFi: manual only");
    drawSplashProgress(50);
  }
  while (millis() - splashT < 1400) delay(30);   // keep the boot log on screen a moment
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

  initSD();          // mount SD now so birthday reminders work from boot
  bootLog(sdReady ? "SD card ready" : "no SD card");
  bootLine(sdReady ? "SD Card Ready." : "SD Card: not found");
  drawSplashProgress(70);
#if ENABLE_BDAY
  loadBirthdays();
#endif

  needsRedraw = true;
#if ENABLE_JAPANESE
  if (sdReady) { jpInit(); bootLog(jpReady ? "Japanese font loaded" : "no Japanese font on the SD card"); }
  bootLine(jpReady ? "Font & SKK Engine Ready." : "Font: ASCII only");
#endif
  bootLine("ESP-NOW: on demand (Chat only)");
  bootLine("System Ready.");
  drawSplashProgress(100);
  delay(400);
  bootDone = true;
  bootLog("setup finished");
}

void loop() {
#if ENABLE_MUSIC
  musicUpdate();                // keep the audio buffers full (music keeps playing in any app)
#endif

  if (needsRedraw) {
    switch (currentApp) {
      case APP_MENU:    drawMenu();    break;
      case APP_CLOCK:   drawClock();   break;
      case APP_LIGHT:   drawLight();   break;
      case APP_WIFI:    drawWifi();    break;
      case APP_SETTINGS: drawSettings(); break;
      case APP_CHAT:    drawChat();    break;
      case APP_FRAME:   drawFrame();   break;
    }
    needsRedraw = false;
  }

  // Per-app updates
  if      (currentApp == APP_MENU)  { updateClock(); if (clockScreen == 0) updateMenuWifiIcon(); }
  else if (currentApp == APP_LIGHT) updateLight();
  else if (currentApp == APP_FRAME) updateSlideshow();

#if ENABLE_CHAT
  // ESP-NOW (outdoor build): runs only while Chat is open
  if (chatOn) {
    processChatRx();
    if (millis() - lastHello > HELLO_INTERVAL_MS) { lastHello = millis(); sendHello(); }
  }
#endif

  // Auto screen dimming (runs in every app when enabled)
  autoBacklight();
  pressureTick();               // records the air pressure every 10 minutes (also while other apps are open)
  if (brightDirty && millis() - brightMs > 800) { saveBrightness(); brightDirty = false; }   // save slider value once it settles (any app)

  // Touch
  // Brightness slider: follow the finger continuously (not debounced) so dragging is smooth
  if (currentApp == APP_LIGHT) {
    TouchPoint lp = readTouch();
    if (lp.valid && lightSliderHit(lp.x, lp.y)) { lightSliderTouch(lp.x); delay(5); return; }
  }
  TouchPoint p = readTouch();
  if (p.valid && millis() - lastTouch > TOUCH_DEBOUNCE) {
    lastTouch = millis();
    if (currentApp != APP_MENU && hitBack(p.x, p.y)) {
      if (currentApp == APP_CLOCK && clockScreen != 0) { clockScreen = 0; drawClock(); }
      else if (currentApp == APP_FRAME) { frameScreen = 0; currentApp = APP_MENU; needsRedraw = true; }
#if ENABLE_CHAT
      else if (currentApp == APP_CHAT && homeTerm) { currentApp = APP_MENU; needsRedraw = true; }   // composer opened from the home terminal
      else if (currentApp == APP_CHAT && (msgScreen != 0 || msgPendingDel >= 0)) {
        msgPendingDel = -1;
        msgScreen = 0; drawPeerList();
      }
#endif
      else if (currentApp == APP_SETTINGS && settingsScreen != 0) { settingsScreen = 0; drawSettings(); }
      else if (currentApp == APP_WIFI || currentApp == APP_LIGHT) { currentApp = APP_SETTINGS; settingsScreen = 0; needsRedraw = true; }   // WiFi + Light live inside Settings
      else {
#if ENABLE_CHAT
        if (currentApp == APP_CHAT) chatStop();   // leaving Chat powers ESP-NOW down
#endif
        currentApp = APP_MENU; needsRedraw = true;
      }
    } else {
      switch (currentApp) {
        case APP_MENU:  handleMenu(p.x, p.y);  break;
        case APP_LIGHT: handleLight(p.x, p.y); break;
        case APP_WIFI:  handleWifi(p.x, p.y);  break;
        case APP_SETTINGS: handleSettings(p.x, p.y); break;
        case APP_FRAME: handleFrame(p.x, p.y); break;
        case APP_CLOCK: handleClock(p.x, p.y); break;
#if ENABLE_CHAT
        case APP_CHAT:  handleChat(p.x, p.y);  break;
#endif
        default: break;
      }
    }
  }
  delay(15);
}
