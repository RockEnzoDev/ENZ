// ============================================================
//  ENZ T-DECK v16 "RADIO BANCHO" -- LILYGO T-Deck companion
//  The all-radio terminal of the ENZ CoT family: ESP-NOW chat
//  (byte-compatible with CYD / M5Core2), LoRa text + MeshCore
//  monitor, Si4703 FM radio (RDS), and on the T-Deck Plus an
//  onboard-GPS position beacon + cached GSI map viewer.
//  No clock UI (GPS time quietly sets the system clock), no
//  music, no onboard sensors -- radios only.
//  v15 adds: RF band scanner (920.6-923.4 RSSI sweep), SOS
//  beacon on the GPS screen (both radios, 10x every 30 s),
//  peer proximity from ESP-NOW RSSI, and MeshCore RAW capture
//  logging to SD (/meshcore_raw.log) for later PC decoding.
//  v16 adds: group voice PTT (MsgPacket type 3, IMA-ADPCM
//  8 kHz over ESP-NOW, ES7210 mic in, MAX98357A speaker out).
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
#include <TJpg_Decoder.h>
#include <driver/i2s.h>
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
// ES7210 mic codec (I2C control on the shared bus, own I2S bus for data)
#define PIN_ES_MCLK   48
#define PIN_ES_SCK    47
#define PIN_ES_LRCK   21
#define PIN_ES_DIN    14
#define ES7210_ADDR   0x40
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

enum Screen { SC_HOME, SC_CHAT, SC_PEERS, SC_SET, SC_RAW, SC_RADIO, SC_GPS, SC_MAP, SC_SCAN, SC_VOICE };
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
struct ChatPeer { uint8_t mac[6]; char name[16]; unsigned long lastSeen; bool hasUnread;
                  int32_t latE6; int32_t lonE6; int16_t altM; unsigned long posAt;
                  int rssi; unsigned long rssiAt; };   // ESP-NOW RSSI (v15 proximity)
static ChatPeer chatPeers[MAX_PEERS];
static int chatPeerCount = 0;
static uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static String chatLines[MAX_CHAT_MSGS];
static int chatLineCount = 0;
static int curPeer = -1;             // conversation shown on the chat screen
static int chatChannel = 1;
static bool chatLR = false;
static int loraProf = 0;   // LoRa modem profile: 0 = CoT Std, 1 = Narrow, 2 = MeshCore listen (pref "chat"/"lprof")
static bool chatOn = false;
static unsigned long lastHello = 0;
struct RxSlot { uint8_t mac[6]; uint8_t data[sizeof(MsgPacket)]; int len; int8_t rssi; };
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
  loraProf = prefs.getInt("lprof", 0);
  if (loraProf < 0 || loraProf > 2) loraProf = 0;
  prefs.end();
  if (chatChannel < 1 || chatChannel > 13) chatChannel = 1;
}
static void saveChatPrefs() { prefs.begin("chat", false); prefs.putInt("ch", chatChannel); prefs.putBool("lr", chatLR); prefs.putInt("lprof", loraProf); prefs.end(); }
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
  chatPeers[idx].latE6 = 0; chatPeers[idx].lonE6 = 0; chatPeers[idx].altM = 0; chatPeers[idx].posAt = 0;
  chatPeers[idx].rssi = 0; chatPeers[idx].rssiAt = 0;
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
static void msgRecvCommon(const uint8_t* mac, const uint8_t* data, int len, int rssi) {
  uint8_t next = (msgRxHead + 1) % RX_SLOTS;
  if (next == msgRxTail) return;
  if (len > (int)sizeof(MsgPacket)) len = sizeof(MsgPacket);
  memcpy((void*)msgRx[msgRxHead].mac, mac, 6);
  memcpy((void*)msgRx[msgRxHead].data, data, len);
  msgRx[msgRxHead].len = len;
  msgRx[msgRxHead].rssi = (int8_t)rssi;
  msgRxHead = next;
}
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onMsgRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  msgRecvCommon(info->src_addr, data, len, info->rx_ctrl ? (int)info->rx_ctrl->rssi : 0);
}
#else
void onMsgRecv(const uint8_t* mac, const uint8_t* data, int len) { msgRecvCommon(mac, data, len, 0); }
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
static void voiceRx(const uint8_t* data, int len, int peerIdx);
void processChatRx() {
  while (msgRxTail != msgRxHead) {
    RxSlot slot;
    memcpy(slot.mac, (const void*)msgRx[msgRxTail].mac, 6);
    slot.len = msgRx[msgRxTail].len;
    slot.rssi = msgRx[msgRxTail].rssi;
    memcpy(slot.data, (const void*)msgRx[msgRxTail].data, slot.len);
    msgRxTail = (msgRxTail + 1) % RX_SLOTS;
    if (slot.len < 8) continue;
    MsgPacket m;
    memcpy(&m, slot.data, slot.len < (int)sizeof(m) ? slot.len : (int)sizeof(m));
    uint8_t myMac[6]; WiFi.macAddress(myMac);
    if (macEq(slot.mac, myMac)) continue;
    int idx = findOrAddPeer(slot.mac);
    if (idx < 0) continue;
    if (slot.rssi) { chatPeers[idx].rssi = slot.rssi; chatPeers[idx].rssiAt = millis(); }
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
    } else if (m.type == 4) {                // POS beacon: "latE6,lonE6,altM"
      long la = 0, lo = 0; int al = 0;
      if (sscanf(m.text, "%ld,%ld,%d", &la, &lo, &al) == 3) {
        chatPeers[idx].latE6 = (int32_t)la; chatPeers[idx].lonE6 = (int32_t)lo;
        chatPeers[idx].altM = (int16_t)al; chatPeers[idx].posAt = millis();
        if (screen == SC_PEERS || screen == SC_MAP) needsRedraw = true;
      }
    } else if (m.type == 3) {                // VOICE (v16): group PTT audio
      if (macIsZero(m.to_mac)) voiceRx(slot.data, slot.len, idx);
    }
  }
  if (chatOn && millis() - lastHello > HELLO_INTERVAL_MS) { lastHello = millis(); sendHello(); }
}

// ============================================================
//  VOICE PTT (v16) -- group voice over ESP-NOW, MsgPacket type 3.
//  IMA-ADPCM at 8 kHz (4 KB/s), 49 ms per packet, half-duplex:
//  hold the on-screen PTT zone (or the T key) to talk; incoming
//  streams play on the speaker. LoRa stays text-only (airtime).
//  Mic = ES7210 codec (I2C control, dedicated I2S bus I2S_NUM_1,
//  captured at 16 kHz and decimated to 8 kHz). Speaker = MAX98357A
//  on I2S_NUM_0 at 8 kHz. Old firmware ignores type 3 (PROTOCOL.md).
// ============================================================
#define V_SAMPLES   392                  // samples per packet (49 ms @ 8 kHz)
#define V_ADPCM     (V_SAMPLES / 2)      // 196 bytes of ADPCM per packet
#define V_FLAG_START 0x01
#define V_FLAG_END   0x02

#pragma pack(push, 1)
struct VoicePkt {                        // mirrors MsgPacket's head layout
  uint8_t type;                          // 3
  uint8_t to_mac[6];                     // all zero = group
  char    name[16];
  uint8_t seq;
  uint8_t flags;
  uint8_t len;                           // ADPCM bytes used (0..V_ADPCM)
  uint8_t data[V_ADPCM];
};
#pragma pack(pop)

// ---- IMA-ADPCM codec (standard tables) ----
static const int8_t ADPCM_IDX[16] = { -1,-1,-1,-1, 2, 4, 6, 8, -1,-1,-1,-1, 2, 4, 6, 8 };
static const int16_t ADPCM_STEP[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
  107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,
  876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,
  4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
  22385,24623,27086,29794,32767
};
// Codec state lives in globals: struct-typed function parameters break
// Arduino's auto-generated prototypes (same trap as RawPkt in v13/v15).
static int32_t adpcmEncPred = 0; static int adpcmEncIdx = 0;
static int32_t adpcmDecPred = 0; static int adpcmDecIdx = 0;
static void adpcmEncReset() { adpcmEncPred = 0; adpcmEncIdx = 0; }
static void adpcmDecReset() { adpcmDecPred = 0; adpcmDecIdx = 0; }
static uint8_t adpcmEncodeSample(int16_t in) {
  int32_t step = ADPCM_STEP[adpcmEncIdx];
  int32_t diff = (int32_t)in - adpcmEncPred;
  uint8_t code = 0;
  if (diff < 0) { code = 8; diff = -diff; }
  int32_t vpdiff = step >> 3;
  if (diff >= step)      { code |= 4; diff -= step;      vpdiff += step; }
  if (diff >= (step >> 1)) { code |= 2; diff -= (step >> 1); vpdiff += (step >> 1); }
  if (diff >= (step >> 2)) { code |= 1; vpdiff += (step >> 2); }
  adpcmEncPred += (code & 8) ? -vpdiff : vpdiff;
  if (adpcmEncPred > 32767) adpcmEncPred = 32767; else if (adpcmEncPred < -32768) adpcmEncPred = -32768;
  adpcmEncIdx += ADPCM_IDX[code];
  if (adpcmEncIdx < 0) adpcmEncIdx = 0; else if (adpcmEncIdx > 88) adpcmEncIdx = 88;
  return code;
}
static int16_t adpcmDecodeSample(uint8_t code) {
  int32_t step = ADPCM_STEP[adpcmDecIdx];
  int32_t vpdiff = step >> 3;
  if (code & 4) vpdiff += step;
  if (code & 2) vpdiff += (step >> 1);
  if (code & 1) vpdiff += (step >> 2);
  adpcmDecPred += (code & 8) ? -vpdiff : vpdiff;
  if (adpcmDecPred > 32767) adpcmDecPred = 32767; else if (adpcmDecPred < -32768) adpcmDecPred = -32768;
  adpcmDecIdx += ADPCM_IDX[code & 0x0F];
  if (adpcmDecIdx < 0) adpcmDecIdx = 0; else if (adpcmDecIdx > 88) adpcmDecIdx = 88;
  return (int16_t)adpcmDecPred;
}

// ---- voice state (UI reads snapshots only; the play task owns I2S) ----
static QueueHandle_t vRxQueue = nullptr;
struct VoiceBlk { uint8_t len; uint8_t data[V_ADPCM]; };
static volatile bool vRxPlaying = false;
static bool     vRxOpen = false;         // a stream is being received
static char     vRxName[16] = "";
static unsigned long vRxStartAt = 0, vRxLastBlkAt = 0;
static uint32_t vRxBlocks = 0;
static uint8_t  vRxExpectSeq = 0;
static bool     vTxActive = false;
static unsigned long vTxStartAt = 0, vLastTKeyAt = 0;
static uint8_t  vTxSeq = 0;
static bool     vMicReady = false, vSpkReady = false, vCodecFound = false;
static int16_t  vAcc[V_SAMPLES];
static int      vAccN = 0;
static int32_t  vDecimPrev = 0;
static bool     vDecimHave = false;

static bool spkInit() {
  if (vSpkReady) return true;
  i2s_config_t c = {};
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  c.sample_rate = 8000;
  c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count = 8;
  c.dma_buf_len = 256;
  i2s_pin_config_t p = {};
  p.mck_io_num = I2S_PIN_NO_CHANGE;
  p.bck_io_num = PIN_I2S_BCK;
  p.ws_io_num = PIN_I2S_WS;
  p.data_out_num = PIN_I2S_DOUT;
  p.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_driver_install(I2S_NUM_0, &c, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &p) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);
  vSpkReady = true;
  return true;
}

static void voicePlayTask(void*) {
  static int16_t pcm[V_SAMPLES];
  static int16_t st[V_SAMPLES * 2];
  VoiceBlk blk;
  for (;;) {
    if (xQueueReceive(vRxQueue, &blk, pdMS_TO_TICKS(120)) == pdTRUE) {
      if (!spkInit()) continue;
      vRxPlaying = true;
      for (int i = 0; i < V_SAMPLES; i++) {
        uint8_t byte = (i >> 1) < blk.len ? blk.data[i >> 1] : 0;
        uint8_t code = (i & 1) ? (byte & 0x0F) : (byte >> 4);
        pcm[i] = adpcmDecodeSample(code);
      }
      for (int i = 0; i < V_SAMPLES; i++) { st[2 * i] = pcm[i]; st[2 * i + 1] = pcm[i]; }
      size_t wr = 0;
      i2s_write(I2S_NUM_0, st, sizeof(st), &wr, pdMS_TO_TICKS(250));
    } else if (vRxPlaying) {
      i2s_zero_dma_buffer(I2S_NUM_0);
      vRxPlaying = false;
    }
  }
}

static void voiceEnsureInit() {
  if (!vRxQueue) {
    vRxQueue = xQueueCreate(24, sizeof(VoiceBlk));
    xTaskCreate(voicePlayTask, "voicePlay", 4096, nullptr, 3, nullptr);
  }
}

static void voiceRx(const uint8_t* data, int len, int peerIdx) {
  (void)peerIdx;
  if (vTxActive) return;                          // half-duplex: own TX wins
  if (len < 26) return;
  const VoicePkt* vp = (const VoicePkt*)data;
  if (vp->len > V_ADPCM) return;
  voiceEnsureInit();
  unsigned long now = millis();
  if ((vp->flags & V_FLAG_START) || !vRxOpen || vp->seq != vRxExpectSeq) {
    adpcmDecReset();                               // (re)sync decoder
    strncpy(vRxName, vp->name, 15); vRxName[15] = 0;
    if (!vRxOpen || (vp->flags & V_FLAG_START)) { vRxStartAt = now; vRxBlocks = 0; }
    vRxOpen = true;
  }
  vRxExpectSeq = vp->seq + 1;
  vRxLastBlkAt = now;
  if (vp->len > 0) {
    VoiceBlk blk; blk.len = vp->len;
    memcpy(blk.data, vp->data, vp->len);
    if (xQueueSend(vRxQueue, &blk, 0) == pdTRUE) vRxBlocks++;
  }
  if (screen == SC_VOICE) needsRedraw = true;
}

// ---- ES7210 mic codec ----
static void es7210Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
static uint8_t es7210Read(uint8_t reg) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)ES7210_ADDR, (size_t)1);
  return Wire.available() ? (uint8_t)Wire.read() : 0;
}
static void es7210Update(uint8_t reg, uint8_t mask, uint8_t data) {
  uint8_t v = es7210Read(reg);
  es7210Write(reg, (uint8_t)((v & ~mask) | (mask & data)));
}
static void es7210MicSelectAll() {          // driver's mic_select, all 4 mics
  for (int i = 0; i < 4; i++) es7210Update(0x43 + i, 0x10, 0x00);
  es7210Write(0x4B, 0xFF);
  es7210Write(0x4C, 0xFF);
  es7210Update(0x01, 0x0B, 0x00); es7210Write(0x4B, 0x00); es7210Update(0x43, 0x10, 0x10);  // MIC1
  es7210Update(0x01, 0x0B, 0x00); es7210Write(0x4B, 0x00); es7210Update(0x44, 0x10, 0x10);  // MIC2
  es7210Update(0x01, 0x15, 0x00); es7210Write(0x4C, 0x00); es7210Update(0x45, 0x10, 0x10);  // MIC3
  es7210Update(0x01, 0x15, 0x00); es7210Write(0x4C, 0x00); es7210Update(0x46, 0x10, 0x10);  // MIC4
}
// Init sequence follows LilyGo's official es7210 driver (es7210.cpp) for
// SLAVE mode at 16 kHz: ESP32 supplies MCLK = 16k x 256 = 4.096 MHz on
// GPIO48, so the clock coefficients are the {4096000, 16000} table row.
static bool es7210Init() {
  Wire.beginTransmission(ES7210_ADDR);
  if (Wire.endTransmission() != 0) { vCodecFound = false; return false; }
  vCodecFound = true;
  es7210Write(0x00, 0xFF);                    // reset
  es7210Write(0x00, 0x41);
  es7210Write(0x01, 0x1F);                    // clocks off
  es7210Write(0x09, 0x30);                    // state cycle timing
  es7210Write(0x0A, 0x30);
  // (codec is I2S slave: no master-mode writes to 0x08 / 0x03)
  es7210Write(0x40, 0xC3);                    // analog
  es7210Write(0x41, 0x70);                    // mic bias 2.87 V
  es7210Write(0x42, 0x70);
  es7210Write(0x07, 0x20);                    // OSR
  es7210Write(0x02, 0xC1);                    // mainclk (16k row)
  es7210Write(0x04, 0x01);                    // LRCK div H
  es7210Write(0x05, 0x00);                    // LRCK div L
  es7210MicSelectAll();
  for (int i = 0; i < 4; i++) es7210Update(0x43 + i, 0x0F, 0x00);   // gain 0 dB first
  // config_i2s: 16-bit, normal I2S format, SDOUT routing, sample again
  es7210Write(0x11, (uint8_t)((es7210Read(0x11) & 0x1F) | 0x60));
  es7210Write(0x11, (uint8_t)(es7210Read(0x11) & 0xFC));
  es7210Write(0x12, 0x00);
  es7210Write(0x02, 0xC1);
  es7210Write(0x07, 0x20);
  es7210Write(0x04, 0x01);
  es7210Write(0x05, 0x00);
  // mic gains: 24 dB on all channels (tune after hardware test)
  for (int i = 0; i < 4; i++) es7210Update(0x43 + i, 0x0F, 8);
  // start: restore clock register, power up, select mics again
  es7210Write(0x01, es7210Read(0x01));
  es7210Write(0x06, 0x00);
  es7210Write(0x47, 0x00);
  es7210Write(0x48, 0x00);
  es7210Write(0x49, 0x00);
  es7210Write(0x4A, 0x00);
  es7210MicSelectAll();
  return true;
}

static bool micInit() {
  if (vMicReady) return true;
  if (!es7210Init()) return false;
  i2s_config_t c = {};
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  c.sample_rate = 16000;
  c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  c.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count = 8;
  c.dma_buf_len = 256;
  c.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  i2s_pin_config_t p = {};
  p.mck_io_num = PIN_ES_MCLK;
  p.bck_io_num = PIN_ES_SCK;
  p.ws_io_num = PIN_ES_LRCK;
  p.data_out_num = I2S_PIN_NO_CHANGE;
  p.data_in_num = PIN_ES_DIN;
  if (i2s_driver_install(I2S_NUM_1, &c, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_1, &p) != ESP_OK) return false;
  vMicReady = true;
  return true;
}

static void voiceSendPkt(uint8_t flags, const uint8_t* adpcm, uint8_t n) {
  VoicePkt vp; memset(&vp, 0, sizeof(vp));
  vp.type = 3;
  { String nm = u8cut(myName, 15); strncpy(vp.name, nm.c_str(), 15); }
  vp.seq = vTxSeq++;
  vp.flags = flags;
  vp.len = n;
  if (adpcm && n) memcpy(vp.data, adpcm, n);
  esp_now_send(BCAST_MAC, (uint8_t*)&vp, sizeof(vp));
}

static String voiceDurStr(unsigned long ms) {
  int s = (int)((ms + 500) / 1000);
  if (s < 1) s = 1;
  return String("(voice ") + String(s) + String("s)");
}

static void voiceTick() {
  unsigned long now = millis();
  // ---- RX stream finalisation -> chat log line ----
  if (vRxOpen && !vRxPlaying && now - vRxLastBlkAt > 900) {
    String line = voiceDurStr(vRxLastBlkAt - vRxStartAt + 49);
    appendGroupLog(String(vRxName), line);
    termPush(String(vRxName), line);
    int gi = ensureGroupPeer();
    if (gi >= 0 && screen != SC_HOME && screen != SC_CHAT) chatPeers[gi].hasUnread = true;
    vRxOpen = false;
    if (screen == SC_VOICE) needsRedraw = true;
  }
  // ---- PTT state ----
  bool wantTx = false;
  if (screen == SC_VOICE && chatOn && !vRxPlaying && !vRxOpen) {
    if (now - vLastTKeyAt < 400 && vLastTKeyAt) wantTx = true;
    int tx, ty;
    if (gtTouch(&tx, &ty) && tx >= 25 && tx <= 295 && ty >= 78 && ty <= 188) wantTx = true;
  }
  if (wantTx && !vTxActive) {
    if (micInit()) {
      vTxActive = true;
      vTxStartAt = now;
      vTxSeq = 0;
      adpcmEncReset();
      vAccN = 0; vDecimHave = false;
      voiceSendPkt(V_FLAG_START, nullptr, 0);
      if (screen == SC_VOICE) needsRedraw = true;
    }
  } else if (!wantTx && vTxActive) {
    voiceSendPkt(V_FLAG_END, nullptr, 0);
    vTxActive = false;
    String line = voiceDurStr(now - vTxStartAt);
    appendGroupLog("Me", line);
    termPush("Me", line);
    if (screen == SC_VOICE) needsRedraw = true;
  }
  // ---- TX pump: mic 16 kHz -> decimate -> ADPCM -> packets ----
  if (vTxActive) {
    static int16_t fr[512 * 2];
    static uint8_t out[V_ADPCM];
    for (int iter = 0; iter < 6; iter++) {
      size_t br = 0;
      if (i2s_read(I2S_NUM_1, fr, sizeof(fr), &br, 0) != ESP_OK || br == 0) break;
      int frames = (int)(br / 4);
      for (int i = 0; i < frames; i++) {
        int32_t s = ((int32_t)fr[2 * i] + fr[2 * i + 1]) / 2;   // ADC1+ADC2 slots averaged
        if (!vDecimHave) { vDecimPrev = s; vDecimHave = true; continue; }
        vAcc[vAccN++] = (int16_t)((vDecimPrev + s) / 2);
        vDecimHave = false;
        if (vAccN >= V_SAMPLES) {
          for (int j = 0; j < V_SAMPLES; j += 2) {
            uint8_t hi = adpcmEncodeSample(vAcc[j]);
            uint8_t lo = adpcmEncodeSample(vAcc[j + 1]);
            out[j / 2] = (uint8_t)((hi << 4) | lo);
          }
          voiceSendPkt(0, out, V_ADPCM);
          vAccN = 0;
        }
      }
      if (now - vTxStartAt > 60000) break;           // 60 s hard cap per burst
    }
  }
}

static void vTxtC(const char* s, int cx, int y, uint16_t color, int size) {
  txt(s, cx - txtW(s, size) / 2, y, color, size);
}
static void drawVoice() {
  drawHeader("VOICE PTT");
  if (!vCodecFound && !vMicReady) txt("Mic: ES7210 (checked on first TX)", 14, 34, UI_MUTE, 1);
  else if (vCodecFound) txt("Mic: ES7210 OK", 14, 34, UI_MUTE, 1);
  else txt("Mic: ES7210 NOT FOUND", 14, 34, UI_BAD, 1);
  char buf[48];
  snprintf(buf, sizeof(buf), "Online peers: %d", onlinePeerCount());
  txt(buf, 14, 50, UI_MUTE, 1);
  bool txing = vTxActive;
  Lcd.fillRoundRect(25, 78, 270, 110, 10, txing ? UI_BAD : UI_CARD);
  Lcd.drawRoundRect(25, 78, 270, 110, 10, txing ? UI_BAD : UI_ACCENT);
  if (txing) {
    vTxtC("TX - TALK NOW", 160, 105, UI_TEXT, 2);
    snprintf(buf, sizeof(buf), "%lu s", (unsigned long)((millis() - vTxStartAt) / 1000));
    vTxtC(buf, 160, 140, UI_TEXT, 2);
  } else if (vRxPlaying || vRxOpen) {
    vTxtC("RX", 160, 105, UI_ACCENT, 2);
    vTxtC(vRxName, 160, 140, UI_TEXT, 2);
  } else {
    vTxtC("HOLD TO TALK", 160, 105, UI_TEXT, 2);
    vTxtC("(touch here or hold T key)", 160, 145, UI_MUTE, 1);
  }
  txt("Group voice: everyone hears everyone.", 14, 205, UI_MUTE, 1);
  txt("Esc: back", 14, 221, UI_MUTE, 1);
  if (txing || vRxPlaying) needsRedraw = true;   // live timer / state
}

// ============================================================
//  LoRa TEXT (SX1262 via RadioLib) -- 920 MHz JP low-power plan:
//  920.8 MHz, TX 13 dBm, LBT (CAD) before TX. Modem profile (Settings):
//  Std = BW125 / SF8 / CR 4/5 (CoT standard), Narrow = BW62.5 / SF8 /
//  CR 4/6 (Meshtastic NarrowSlow experiment). MeshCore = BW62.5 / SF8 /
//  CR 4/5, listen-only for now: raw packets go to the RAW log screen and
//  CoT LoRa TX (HELLO + chat relay) stays off in that profile.
//  v13: the RAW screen also decodes MeshCore ADVERTs (Stage 2: pubkey,
//  timestamp, node name; signature not verified yet).
//  Frame: [type][nameLen][name][text]  HELLO every 90 s.
// ============================================================
#define LORA_FREQ 920.8f
static SX1262 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
static bool loraReady = false;
static volatile bool loraRxFlag = false;
static unsigned long loraLastHello = 0, loraHeardAt = 0;
static String loraHeardName = "";
static int loraHeardRssi = 0;
static uint16_t loraMsgId = 1;
struct RawPkt { uint32_t at; int rssi; uint8_t len; uint8_t data[160]; };
#define RAW_N 8
static RawPkt rawLog[RAW_N];
static int rawCount = 0;
static uint32_t rawTotal = 0;
// v15: every captured MeshCore packet is also appended to SD as
// /meshcore_raw.log (one line: ISO time or ms, len, rssi, full hex)
// so a capture session can be decoded later on a PC. The file is
// restarted when it passes 1 MB. Format per line:
//   2026-10-30T01:02:03Z len=111 rssi=-40 data=1200E9...
static uint32_t rawSdCount = 0;
// NOTE: primitive params only -- a sketch-defined struct type in the
// signature breaks Arduino's auto-generated prototypes (v13 lesson).
static void rawSdAppend(uint32_t atMs, int rssi, int len, const uint8_t* data) {
  if (!sdReady) return;
  {
    File chk = SD.open("/meshcore_raw.log", FILE_READ);
    if (chk) { uint32_t sz = chk.size(); chk.close(); if (sz > 1048576UL) SD.remove("/meshcore_raw.log"); }
  }
  File f = SD.open("/meshcore_raw.log", FILE_APPEND);
  if (!f) return;
  time_t tt = time(nullptr);
  char ts[28];
  if (tt > 1700000000) {
    struct tm t; gmtime_r(&tt, &t);
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  } else snprintf(ts, sizeof(ts), "ms%lu", (unsigned long)atMs);
  f.print(ts); f.print(" len="); f.print(len); f.print(" rssi="); f.print(rssi); f.print(" data=");
  for (int i = 0; i < len; i++) { char hx[3]; snprintf(hx, sizeof(hx), "%02X", data[i]); f.print(hx); }
  f.print('\n');
  f.close();
  rawSdCount++;
}
// MeshCore ADVERT decode (Stage 2). Packet: [header][path_len][path...]
// then payload: pubkey[32], timestamp LE32, signature[64], flags[1],
// optional lat/lon (flags bit0, 8 bytes), then the node name to the end.
// Header byte: bits 7-6 version, bits 5-2 payload type (4 = ADVERT),
// bits 1-0 route. Signature is NOT verified at this stage.
static bool mcParseAdvert(const uint8_t* d, int dlen, char* nameOut, size_t nameCap,
                          char* metaOut, size_t metaCap) {
  if (dlen < 2) return false;
  if (((d[0] >> 2) & 0x0F) != 4) return false;
  int off = 2 + (d[1] & 0x3F);
  if (dlen < off + 32 + 4 + 64 + 1) return false;
  const uint8_t* pk = d + off;
  uint32_t ts = (uint32_t)d[off + 32] | ((uint32_t)d[off + 33] << 8) |
                ((uint32_t)d[off + 34] << 16) | ((uint32_t)d[off + 35] << 24);
  uint8_t flags = d[off + 100];
  int cur = off + 101;
  if ((flags & 0x01) && (dlen - cur) > 8) cur += 8;  // lat/lon present
  int nl = dlen - cur;
  while (nl > 0 && d[cur + nl - 1] == 0) nl--;
  if (nl <= 0) {
    snprintf(nameOut, nameCap, "(no name)");
  } else {
    int w = 0;
    for (int i = 0; i < nl && w < (int)nameCap - 1; i++) {
      uint8_t c = d[cur + i];
      nameOut[w++] = (c >= 0x20) ? (char)c : '.';
    }
    nameOut[w] = 0;
  }
  time_t t = (time_t)ts;
  struct tm g; gmtime_r(&t, &g);
  snprintf(metaOut, metaCap, "PK %02X%02X%02X%02X %02d:%02d:%02dZ",
           pk[0], pk[1], pk[2], pk[3], g.tm_hour, g.tm_min, g.tm_sec);
  return true;
}
static void loraIsr() { loraRxFlag = true; }
static bool loraInit() {
  int st = radio.begin(LORA_FREQ, loraProf == 0 ? 125.0f : 62.5f, 8, loraProf == 1 ? 6 : 5, 0x12, 13, 8);
  if (st != RADIOLIB_ERR_NONE) return false;
  radio.setDio1Action(loraIsr);
  st = radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) return false;
  loraReady = true;
  return true;
}
static bool loraSendFrame(uint8_t type, const String& text) {
  if (!loraReady || loraProf == 2) return false;
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
// ============================================================
//  RF SCANNER (v15): sweeps 920.6-923.4 MHz in 0.2 MHz steps and
//  draws live RSSI bars so the band's occupancy is visible. While
//  the scanner screen is up, CoT LoRa RX/TX is paused (the SX1262
//  is busy sweeping); leaving the screen restores 920.8 RX.
// ============================================================
#define SCAN_N 15
static bool scanRunning = false;
static int scanIdx = 0;
static int scanRssi[SCAN_N];
static float scanFreqMhz(int i) { return 920.6f + 0.2f * i; }   // index 1 = 920.8 (CoT)
static void scanEnter() {
  if (!loraReady) return;
  for (int i = 0; i < SCAN_N; i++) scanRssi[i] = -120;
  scanIdx = 0;
  scanRunning = true;
}
static void scanExit() {
  if (!scanRunning) return;
  scanRunning = false;
  if (loraReady) { radio.setFrequency(LORA_FREQ); radio.startReceive(); }
}
static void scanTick() {
  static bool dwell = false;
  static unsigned long dueAt = 0;
  if (!scanRunning || !loraReady) return;
  unsigned long now = millis();
  if (!dwell) {
    radio.setFrequency(scanFreqMhz(scanIdx));
    radio.startReceive();
    dueAt = now + 9;
    dwell = true;
    return;
  }
  if (now >= dueAt) {
    int r = (int)radio.getRSSI();
    if (r < -120) r = -120;
    if (r > 0) r = 0;
    scanRssi[scanIdx] = r;
    scanIdx++;
    dwell = false;
    if (scanIdx >= SCAN_N) { scanIdx = 0; needsRedraw = true; }
  }
}
static void loraPoll() {
  if (!loraReady) return;
  if (scanRunning) { loraRxFlag = false; return; }   // scanner owns the radio
  if (loraRxFlag) {
    loraRxFlag = false;
    uint8_t buf[255];
    int len = radio.getPacketLength();
    int st = (len > 0 && len <= (int)sizeof(buf)) ? radio.readData(buf, len) : RADIOLIB_ERR_NONE;
    if (st == RADIOLIB_ERR_NONE && len >= 1) {
      if (loraProf == 2) {
        RawPkt rp; rp.at = millis(); rp.rssi = (int)radio.getRSSI(); rp.len = (uint8_t)min(len, 160);
        memcpy(rp.data, buf, min(len, 160));
        memmove(&rawLog[1], &rawLog[0], sizeof(RawPkt) * (RAW_N - 1));
        rawLog[0] = rp; if (rawCount < RAW_N) rawCount++;
        rawTotal++;
        rawSdAppend(rp.at, rp.rssi, rp.len, rp.data);
        if (screen == SC_RAW) needsRedraw = true;
      } else if (len >= 4) {
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
        } else if (type == 3) {   // LoRa POS beacon, attributed by name
          String text = String((const char*)buf + 4 + nl).substring(0, len - 4 - nl);
          long la = 0, lo = 0; int al = 0;
          if (sscanf(text.c_str(), "%ld,%ld,%d", &la, &lo, &al) == 3) {
            for (int i = 0; i < chatPeerCount; i++) {
              if (name == peerName(i)) {
                chatPeers[i].latE6 = (int32_t)la; chatPeers[i].lonE6 = (int32_t)lo;
                chatPeers[i].altM = (int16_t)al; chatPeers[i].posAt = millis();
                if (screen == SC_PEERS || screen == SC_MAP) needsRedraw = true;
                break;
              }
            }
          }
        }
      }
      }
    }
  }
  if (loraProf != 2 && millis() - loraLastHello > 90000UL) {
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
//  FM RADIO (Si4703 on the shared I2C bus, addr 0x10) -- RDS
//  station name + radiotext. Audio leaves the module on its
//  own 3.5 mm jack (earphones double as the antenna); nothing
//  is routed through the ESP32. Register values follow the
//  Si4703 datasheet / Linux si470x driver. The module needs
//  SDA/SCL/3.3V/GND (Grove port); RST is assumed pulled up on
//  the breakout, so init is bus-only.
// ============================================================
#define FM_ADDR 0x10
static bool fmPresent = false;
static uint16_t fmR[16];              // register shadow, indexed by register number
static int fmKhz = 81300;              // 81.3 MHz default (Tokyo-ish FM)
static int fmVol = 10;                 // SYSCONFIG2 volume 0..15
static bool fmStereo = false;
static int fmRssi = 0;
static String fmPS = "";               // RDS program service name
static String fmRT = "";               // RDS radiotext
static char fmPsBuf[9] = "";
static uint8_t fmPsMask = 0;
static char fmRtBuf[65] = "";
static int fmRtAB = -1;
static void fmReadRegs() {
  if (Wire.requestFrom(FM_ADDR, 32) != 32) return;
  uint16_t order[16] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
  for (int i = 0; i < 16; i++) {
    int hi = Wire.read(), lo = Wire.read();
    if (hi < 0 || lo < 0) return;
    fmR[order[i]] = (uint16_t)((hi << 8) | lo);
  }
}
static void fmWriteRegs() {            // writes registers 0x02..0x07
  Wire.beginTransmission(FM_ADDR);
  for (int r = 0x02; r <= 0x07; r++) { Wire.write((uint8_t)(fmR[r] >> 8)); Wire.write((uint8_t)(fmR[r] & 0xFF)); }
  Wire.endTransmission();
}
static void fmSavePrefs() { prefs.begin("fm", false); prefs.putInt("khz", fmKhz); prefs.putInt("vol", fmVol); prefs.end(); }
static void loadFmPrefs() {
  prefs.begin("fm", true);
  fmKhz = prefs.getInt("khz", 81300);
  fmVol = prefs.getInt("vol", 10);
  prefs.end();
  if (fmKhz < 76000 || fmKhz > 108000) fmKhz = 81300;
  if (fmVol < 0 || fmVol > 15) fmVol = 10;
}
static bool fmWaitStc() {
  for (int i = 0; i < 60; i++) { fmReadRegs(); if (fmR[0x0A] & 0x4000) return true; delay(10); }
  return false;
}
static void fmTune(int khz) {
  if (!fmPresent) return;
  if (khz < 76000) khz = 76000;
  if (khz > 108000) khz = 108000;
  uint16_t chan = (uint16_t)((khz - 76000) / 100);
  fmR[0x03] = (uint16_t)(0x8000 | chan);          // TUNE | CHAN
  fmWriteRegs();
  fmWaitStc();
  fmR[0x03] &= (uint16_t)~0x8000;                  // clear TUNE
  fmWriteRegs();
  fmReadRegs();
  fmKhz = 76000 + (int)(fmR[0x0B] & 0x03FF) * 100; // READCHAN
  fmPsMask = 0; fmPS = ""; fmRT = ""; fmRtAB = -1;
  memset(fmRtBuf, 0, sizeof(fmRtBuf));
  fmSavePrefs();
}
static bool fmInit() {
  Wire.beginTransmission(FM_ADDR);
  if (Wire.endTransmission() != 0) { fmPresent = false; return false; }
  fmPresent = true;
  fmReadRegs();
  fmR[0x07] = 0x8100;                              // TEST1: crystal oscillator on
  fmWriteRegs();
  fmR[0x02] |= 0x4001;                             // POWERCFG: DMUTE | ENABLE
  fmWriteRegs();
  delay(110);                                      // oscillator start-up
  fmReadRegs();
  fmR[0x04] |= 0x1000;                             // SYSCONFIG1: RDS on
  fmR[0x05] = (uint16_t)((fmR[0x05] & 0xFF00) | 0x0040 | 0x0010 | (fmVol & 0x0F));  // BAND 76-108, SPACE 100 kHz, volume
  fmWriteRegs();
  fmTune(fmKhz);
  return true;
}
static void fmSeek(bool up) {
  if (!fmPresent) return;
  fmReadRegs();
  fmR[0x02] |= (uint16_t)(0x0200 | (up ? 0x0400 : 0));   // SEEK | SEEKUP
  fmWriteRegs();
  fmWaitStc();
  fmReadRegs();
  bool edge = (fmR[0x0A] & 0x2000) != 0;                  // SF/BL
  fmR[0x02] &= (uint16_t)~0x0200;                          // clear SEEK
  fmWriteRegs();
  fmKhz = 76000 + (int)(fmR[0x0B] & 0x03FF) * 100;
  if (edge) { /* wrapped at the band edge: READCHAN already reflects it */ }
  fmPsMask = 0; fmPS = ""; fmRT = ""; fmRtAB = -1;
  memset(fmRtBuf, 0, sizeof(fmRtBuf));
  fmSavePrefs();
}
static void fmVolSet(int v) {
  if (v < 0) v = 0;
  if (v > 15) v = 15;
  fmVol = v;
  if (fmPresent) {
    fmReadRegs();
    fmR[0x05] = (uint16_t)((fmR[0x05] & 0xFFF0) | (fmVol & 0x0F));
    fmWriteRegs();
  }
  fmSavePrefs();
}
static void fmPoll() {                     // called from loop() on the radio screen
  if (!fmPresent) return;
  static unsigned long last = 0;
  if (millis() - last < 150) return;
  last = millis();
  fmReadRegs();
  fmStereo = (fmR[0x0A] & 0x0100) != 0;
  fmRssi = fmR[0x0A] & 0x00FF;
  if (!(fmR[0x0A] & 0x8000)) return;       // RDSR: no new RDS group
  uint16_t b = fmR[0x0D];                  // RDSB
  int gtype = (b >> 12) & 0x0F;
  int gver = (b >> 11) & 0x01;
  bool dirty = false;
  if (gtype == 0) {                        // PS name: 2 chars per segment in RDSD
    int seg = b & 0x03;
    char c1 = (char)(fmR[0x0F] >> 8), c2 = (char)(fmR[0x0F] & 0xFF);
    if (fmPsBuf[seg * 2] != c1 || fmPsBuf[seg * 2 + 1] != c2) {
      fmPsBuf[seg * 2] = c1; fmPsBuf[seg * 2 + 1] = c2;
      fmPsMask |= (uint8_t)(1 << seg);
      dirty = true;
    }
    if (fmPsMask == 0x0F) { String s = String(fmPsBuf); s.trim(); if (s != fmPS) { fmPS = s; dirty = true; } }
  } else if (gtype == 2 && gver == 0) {    // radiotext (2A): 4 chars per segment
    int ab = (b >> 4) & 0x01;
    if (ab != fmRtAB) { fmRtAB = ab; memset(fmRtBuf, 0, sizeof(fmRtBuf)); }
    int seg = b & 0x0F;
    char cs[4] = { (char)(fmR[0x0E] >> 8), (char)(fmR[0x0E] & 0xFF), (char)(fmR[0x0F] >> 8), (char)(fmR[0x0F] & 0xFF) };
    bool ch = false;
    for (int i = 0; i < 4; i++) {
      char c = cs[i] == '\r' ? 0 : cs[i];
      if (fmRtBuf[seg * 4 + i] != c) { fmRtBuf[seg * 4 + i] = c; ch = true; }
    }
    if (ch) { String s = String(fmRtBuf); s.trim(); if (s != fmRT) { fmRT = s; dirty = true; } }
  }
  if (dirty && screen == SC_RADIO) needsRedraw = true;
}
static void drawRadio() {
  Lcd.fillScreen(UI_BG);
  drawHeader("FM RADIO");
  if (!fmPresent) {
    txt("Si4703 not found (I2C 0x10)", 12, 44, UI_WARN, 1);
    txt("Wire the module to the Grove I2C port:", 12, 74, UI_DIM, 1);
    txt("SDA / SCL / 3.3V / GND, RST pulled up.", 12, 90, UI_DIM, 1);
    txt("Audio: earphones on the module jack", 12, 116, UI_TEXT, 1);
    txt("(the earphone cable is the antenna).", 12, 132, UI_DIM, 1);
    return;
  }
  char fb[16];
  snprintf(fb, sizeof(fb), "%d.%d", fmKhz / 1000, (fmKhz % 1000) / 100);
  Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(3); Lcd.setTextColor(UI_ACCENT, UI_BG);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
  Lcd.drawString(fb, 16, 36);
  Lcd.setTextSize(1); Lcd.setTextColor(UI_DIM, UI_BG);
  Lcd.drawString("MHz", 150, 58);
  if (fmPS.length()) jpText(fitText(fmPS, 296).c_str(), 16, 92, UI_TEXT, -1);
  else txt("... RDS ...", 16, 92, UI_MUTE, 1);
  if (fmRT.length()) {
    String rest = fmRT;
    String l1 = fitText(rest, 296);
    jpText(l1.c_str(), 16, 116, UI_DIM, -1);
    if (l1.length() < rest.length()) {
      String l2 = rest.substring(l1.length());
      l2.trim();
      jpText(fitText(l2, 296).c_str(), 16, 134, UI_DIM, -1);
    }
  }
  char sb[40];
  snprintf(sb, sizeof(sb), "%s  RSSI %d  VOL %d", fmStereo ? "STEREO" : "MONO", fmRssi, fmVol);
  txt(sb, 16, 160, UI_MUTE, 1);
  hbtn(6, 186, 70, 40, "SEEK-", KC_NORMAL, UI_TEXT);
  hbtn(82, 186, 70, 40, "SEEK+", KC_NORMAL, UI_TEXT);
  hbtn(158, 186, 70, 40, "VOL-", KC_NORMAL, UI_TEXT);
  hbtn(234, 186, 74, 40, "VOL+", KC_NORMAL, UI_TEXT);
}

// ============================================================
//  GPS (T-Deck Plus onboard, UART1) -- NMEA GGA/RMC parsed by
//  hand. Auto-detected: original T-Decks simply never receive
//  a sentence, so every GPS feature stays hidden there. The
//  RX/TX order differs between board revs, so if nothing has
//  arrived 20 s after boot the pins are swapped once.
//  A valid RMC also sets the system clock (quiet corner clock
//  on the home screen) -- GPS is this family's RTC.
// ============================================================
#define GPS_RX_PIN 44
#define GPS_TX_PIN 43
static bool gpsPresent = false;      // any valid NMEA seen
static bool gpsFix = false;
static int32_t gpsLatE6 = 0, gpsLonE6 = 0;
static float gpsAltM = 0, gpsSpeedKmh = 0;
static int gpsSats = 0;
static unsigned long gpsFixAt = 0, lastBeaconAt = 0;
static bool gpsSwapped = false, gpsTimeSet = false;
static void gpsInit() { Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN); }
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
static int32_t nmeaCoordE6(const char* v, char hemi) {
  float f = (float)atof(v);
  int deg = (int)(f / 100.0f);
  float mn = f - deg * 100.0f;
  double dec = deg + mn / 60.0;
  if (hemi == 'S' || hemi == 'W') dec = -dec;
  return (int32_t)lround(dec * 1000000.0);
}
static int nmeaField(char* line, int idx, char* out, int cap) {   // 0-based field copy, returns length
  int f = 0; const char* p = line;
  while (f < idx && *p) { if (*p == ',') f++; p++; }
  int n = 0;
  while (*p && *p != ',' && *p != '*' && n < cap - 1) out[n++] = *p++;
  out[n] = 0;
  return n;
}
static void gpsHandleLine(char* line) {
  char fld[24];
  if (strstr(line, "GGA") == line + 3) {           // $??GGA
    nmeaField(line, 6, fld, sizeof(fld));
    int fixq = atoi(fld);
    nmeaField(line, 7, fld, sizeof(fld)); gpsSats = atoi(fld);
    if (fixq > 0) {
      char ns, ew, la[20], lo[20];
      nmeaField(line, 2, la, sizeof(la)); nmeaField(line, 3, fld, sizeof(fld)); ns = fld[0];
      nmeaField(line, 4, lo, sizeof(lo)); nmeaField(line, 5, fld, sizeof(fld)); ew = fld[0];
      gpsLatE6 = nmeaCoordE6(la, ns);
      gpsLonE6 = nmeaCoordE6(lo, ew);
      nmeaField(line, 9, fld, sizeof(fld)); gpsAltM = (float)atof(fld);
      gpsFix = true; gpsPresent = true; gpsFixAt = millis();
    } else gpsFix = false;
  } else if (strstr(line, "RMC") == line + 3) {    // $??RMC
    nmeaField(line, 2, fld, sizeof(fld));
    if (fld[0] == 'A') {
      gpsPresent = true;
      nmeaField(line, 7, fld, sizeof(fld)); gpsSpeedKmh = (float)(atof(fld) * 1.852);
      if (!gpsTimeSet) {
        char tv[16], dv[16];
        nmeaField(line, 1, tv, sizeof(tv));
        nmeaField(line, 9, dv, sizeof(dv));
        if (strlen(tv) >= 6 && strlen(dv) >= 6) {
          int hh = (tv[0] - '0') * 10 + tv[1] - '0', mm = (tv[2] - '0') * 10 + tv[3] - '0', ss = (tv[4] - '0') * 10 + tv[5] - '0';
          int dd = (dv[0] - '0') * 10 + dv[1] - '0', mo = (dv[2] - '0') * 10 + dv[3] - '0', yy = 2000 + (dv[4] - '0') * 10 + dv[5] - '0';
          int64_t ep = daysFromCivil(yy, mo, dd) * 86400 + hh * 3600 + mm * 60 + ss;
          struct timeval now = { (time_t)ep, 0 };
          settimeofday(&now, nullptr);
          gpsTimeSet = true;
        }
      }
    }
  }
}
static void gpsPoll() {
  static char lb[110];
  static int ln = 0;
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') { lb[ln] = 0; if (ln > 6) gpsHandleLine(lb); ln = 0; }
    else if (c != '\r' && ln < (int)sizeof(lb) - 1) lb[ln++] = c;
  }
  if (!gpsSwapped && !gpsPresent && millis() > 20000) {
    gpsSwapped = true;
    Serial1.end();
    Serial1.begin(9600, SERIAL_8N1, GPS_TX_PIN, GPS_RX_PIN);   // swapped order
  }
  if (gpsFix && millis() - gpsFixAt > 10000) gpsFix = false;
}
static void sendPosBeacon() {
  if (!gpsFix) { termPush("GPS", "no fix yet"); return; }
  char tb[44];
  snprintf(tb, sizeof(tb), "%ld,%ld,%d", (long)gpsLatE6, (long)gpsLonE6, (int)lroundf(gpsAltM));
  if (chatOn) {
    MsgPacket m; memset(&m, 0, sizeof(m));
    m.type = 4;
    { String nm = u8cut(myName, 15); strncpy(m.name, nm.c_str(), 15); }
    strncpy(m.text, tb, sizeof(m.text) - 1);
    esp_now_send(BCAST_MAC, (uint8_t*)&m, sizeof(m));
  }
  loraSendFrame(3, String(tb));
  lastBeaconAt = millis();
  termPush("GPS", String("POS ") + tb);
  if (screen == SC_GPS) needsRedraw = true;
}
// ============================================================
//  SOS BEACON (v15): an armed two-tap action on the GPS screen.
//  Sends a POS beacon (when a fix exists) plus an "SOS <name>
//  <latE6,lonE6,altM | NOFIX>" CHAT line over BOTH radios, then
//  repeats every 30 s, 10 times total, or until stopped. Any
//  received chat line whose text starts with "SOS " is drawn in
//  alarm red on the home terminal and the chat screen.
// ============================================================
static bool sosActive = false;
static int sosLeft = 0;
static unsigned long sosNextAt = 0;
static bool sosArm = false;
static unsigned long sosArmAt = 0;
static void sosSendOnce() {
  if (gpsFix) sendPosBeacon();
  char tb[72];
  if (gpsFix) snprintf(tb, sizeof(tb), "SOS %s %ld,%ld,%d", myName.c_str(), (long)gpsLatE6, (long)gpsLonE6, (int)lroundf(gpsAltM));
  else snprintf(tb, sizeof(tb), "SOS %s NOFIX", myName.c_str());
  int gi = ensureGroupPeer();
  if (gi >= 0 && chatOn) sendChatMsg(gi, String(tb));
  loraSendFrame(2, String(tb));
  appendGroupLog("Me", String(tb));
  termPush("Me", String(tb));
}
static void sosButtonTap() {
  if (sosActive) { sosActive = false; termPush("SOS", "beacon stopped"); needsRedraw = true; return; }
  if (sosArm && millis() - sosArmAt < 3000) {
    sosArm = false;
    sosActive = true;
    sosLeft = 10;
    sosSendOnce();
    sosLeft--;
    sosNextAt = millis() + 30000;
    termPush("SOS", "beacon started (10x, every 30 s)");
  } else { sosArm = true; sosArmAt = millis(); }
  needsRedraw = true;
}
static const char* sosBtnLabel() {
  if (sosActive) return "SOS ACTIVE - TAP: STOP";
  if (sosArm && millis() - sosArmAt < 3000) return "TAP AGAIN: SEND SOS";
  return "SOS BEACON";
}
static void sosTick() {
  if (sosArm && millis() - sosArmAt >= 3000) { sosArm = false; if (screen == SC_GPS) needsRedraw = true; }
  if (!sosActive) return;
  if (millis() >= sosNextAt) {
    sosSendOnce();
    if (--sosLeft <= 0) { sosActive = false; termPush("SOS", "beacon finished"); }
    else sosNextAt = millis() + 30000;
    if (screen == SC_GPS) needsRedraw = true;
  }
}
static void distBrg(int32_t la1, int32_t lo1, int32_t la2, int32_t lo2, float* dm, float* brg) {
  float dLa = (la2 - la1) * 1e-6f, dLo = (lo2 - lo1) * 1e-6f;
  float mLa = (la1 + la2) * 0.5e-6f * 0.01745329f;
  float x = dLo * 0.01745329f * cosf(mLa) * 6371000.0f;
  float y = dLa * 0.01745329f * 6371000.0f;
  *dm = sqrtf(x * x + y * y);
  float b = atan2f(x, y) * 57.29578f;
  if (b < 0) b += 360.0f;
  *brg = b;
}
static const char* compass8(float b) {
  static const char* D[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return D[((int)((b + 22.5f) / 45.0f)) % 8];
}
static void drawGps() {
  Lcd.fillScreen(UI_BG);
  drawHeader("GPS");
  if (!gpsPresent) {
    txt("No GPS data.", 12, 44, UI_WARN, 1);
    txt("Onboard GPS is a T-Deck Plus feature;", 12, 74, UI_DIM, 1);
    txt("on the Plus it needs a view of the sky.", 12, 90, UI_DIM, 1);
    txt("RX/TX auto-swap is tried at 20 s.", 12, 116, UI_MUTE, 1);
    txt("SOS still works: it sends your name", 12, 140, UI_DIM, 1);
    txt("with NOFIX over both radios.", 12, 156, UI_DIM, 1);
    hbtn(60, 190, 200, 40, sosBtnLabel(), KC_NORMAL, UI_BAD);
    return;
  }
  char b[56];
  if (gpsFix) {
    snprintf(b, sizeof(b), "LAT %ld.%06ld", (long)(gpsLatE6 / 1000000), (long)labs(gpsLatE6 % 1000000));
    txt(b, 12, 40, UI_TEXT, 1);
    snprintf(b, sizeof(b), "LON %ld.%06ld", (long)(gpsLonE6 / 1000000), (long)labs(gpsLonE6 % 1000000));
    txt(b, 12, 58, UI_TEXT, 1);
    snprintf(b, sizeof(b), "ALT %.0f m   SAT %d   %.0f km/h", gpsAltM, gpsSats, gpsSpeedKmh);
    txt(b, 12, 76, UI_DIM, 1);
    txt(gpsTimeSet ? "Clock set from GPS" : "Clock: waiting for RMC", 12, 94, UI_MUTE, 1);
    if (lastBeaconAt) {
      snprintf(b, sizeof(b), "Last beacon %lus ago", (millis() - lastBeaconAt) / 1000);
      txt(b, 12, 112, UI_MUTE, 1);
    }
    if (sosActive) {
      snprintf(b, sizeof(b), "SOS ACTIVE: %d sends left", sosLeft);
      txt(b, 12, 126, UI_BAD, 1);
    }
    hbtn(60, 140, 200, 40, "SEND POS", KC_ACT, UI_TEXT);
    hbtn(60, 190, 200, 40, sosBtnLabel(), KC_NORMAL, UI_BAD);
  } else {
    snprintf(b, sizeof(b), "Searching... SAT %d", gpsSats);
    txt(b, 12, 40, UI_TEXT, 1);
    txt("A fix can take a minute under open sky.", 12, 66, UI_DIM, 1);
    if (sosActive) {
      snprintf(b, sizeof(b), "SOS ACTIVE: %d sends left", sosLeft);
      txt(b, 12, 92, UI_BAD, 1);
    }
    hbtn(60, 190, 200, 40, sosBtnLabel(), KC_NORMAL, UI_BAD);
  }
}

// ============================================================
//  MAP (cached GSI tiles) -- tiles live on SD as JPG:
//  /maps/<z>/<x>/<y>.jpg (converted from the GSI standard
//  tiles on a PC; see tools/gsi_cache.py in the repo).
//  Source: Geospatial Information Authority of Japan.
//  No tiles for an area = honest dark placeholder squares.
// ============================================================
static int32_t mapLatE6 = 35681000, mapLonE6 = 139691700;   // Tokyo fallback
static int mapZ = 15;
static bool mapCenterSet = false;
static void loadMapPrefs() {
  prefs.begin("map", true);
  mapLatE6 = prefs.getInt("lat", mapLatE6);
  mapLonE6 = prefs.getInt("lon", mapLonE6);
  mapZ = prefs.getInt("z", 15);
  prefs.end();
  if (mapZ < 5 || mapZ > 18) mapZ = 15;
  mapCenterSet = true;
}
static void saveMapPrefs() { prefs.begin("map", false); prefs.putInt("lat", mapLatE6); prefs.putInt("lon", mapLonE6); prefs.putInt("z", mapZ); prefs.end(); }
static void mapRecenterGps() {
  if (gpsFix) { mapLatE6 = gpsLatE6; mapLonE6 = gpsLonE6; saveMapPrefs(); }
}
static void latLonToPx(double lat, double lon, int z, double* px, double* py) {
  double n = pow(2.0, z);
  *px = (lon + 180.0) / 360.0 * n * 256.0;
  double r = lat * 3.141592653589793 / 180.0;
  *py = (1.0 - log(tan(r) + 1.0 / cos(r)) / 3.141592653589793) / 2.0 * n * 256.0;
}
static void pxToLatLon(double px, double py, int z, int32_t* laE6, int32_t* loE6) {
  double n = pow(2.0, z);
  double lon = px / (n * 256.0) * 360.0 - 180.0;
  double lat = atan(sinh(3.141592653589793 * (1.0 - 2.0 * py / (n * 256.0)))) * 180.0 / 3.141592653589793;
  *laE6 = (int32_t)lround(lat * 1000000.0);
  *loE6 = (int32_t)lround(lon * 1000000.0);
}
static void mapPan(int dxPx, int dyPx) {
  double cpx, cpy;
  latLonToPx(mapLatE6 * 1e-6, mapLonE6 * 1e-6, mapZ, &cpx, &cpy);
  pxToLatLon(cpx + dxPx, cpy + dyPx, mapZ, &mapLatE6, &mapLonE6);
}
static void mapTapCenter(int tx, int ty) {
  double cpx, cpy;
  latLonToPx(mapLatE6 * 1e-6, mapLonE6 * 1e-6, mapZ, &cpx, &cpy);
  pxToLatLon(cpx + (tx - SCR_W / 2), cpy + (ty - SCR_H / 2), mapZ, &mapLatE6, &mapLonE6);
  saveMapPrefs();
}
static bool mapTjpgInit = false;
static bool mapTjpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  Lcd.pushImage(x, y, w, h, bmp);
  return true;
}
static void drawMap() {
  if (!mapTjpgInit) { mapTjpgInit = true; Lcd.setSwapBytes(true); TJpgDec.setJpgScale(1); TJpgDec.setCallback(mapTjpgOut); }
  double cpx, cpy;
  latLonToPx(mapLatE6 * 1e-6, mapLonE6 * 1e-6, mapZ, &cpx, &cpy);
  int tlx = (int)lround(cpx) - SCR_W / 2, tly = (int)lround(cpy) - SCR_H / 2;
  int tx0 = tlx / 256, ty0 = tly / 256;
  if (tlx < 0) tx0--;
  if (tly < 0) ty0--;
  Lcd.fillScreen(UI_BG);
  for (int tyi = ty0; tyi * 256 < tly + SCR_H; tyi++) {
    for (int txi = tx0; txi * 256 < tlx + SCR_W; txi++) {
      int sx = txi * 256 - tlx, sy = tyi * 256 - tly;
      char path[40];
      snprintf(path, sizeof(path), "/maps/%d/%d/%d.jpg", mapZ, txi, tyi);
      bool drawn = false;
      if (sdReady && SD.exists(path)) drawn = TJpgDec.drawSdJpg(sx, sy, path) == JDR_OK;
      if (!drawn) {
        Lcd.fillRect(sx, sy, 256, 256, UI_GHOST);
        Lcd.drawRect(sx, sy, 256, 256, UI_BORDER);
      }
    }
  }
  auto marker = [&](int32_t la, int32_t lo, uint16_t col, const char* tag) {
    double px, py;
    latLonToPx(la * 1e-6, lo * 1e-6, mapZ, &px, &py);
    int mx = (int)lround(px) - tlx, my = (int)lround(py) - tly;
    if (mx < -8 || my < -8 || mx > SCR_W + 8 || my > SCR_H + 8) return;
    Lcd.fillCircle(mx, my, 5, col);
    Lcd.drawCircle(mx, my, 7, col);
    if (tag) txt(tag, mx + 9, my - 4, col, 1);
  };
  for (int i = 0; i < chatPeerCount; i++)
    if (chatPeers[i].posAt && millis() - chatPeers[i].posAt < 1800000UL)
      marker(chatPeers[i].latE6, chatPeers[i].lonE6, UI_WARN, peerName(i).substring(0, 4).c_str());
  if (gpsFix) marker(gpsLatE6, gpsLonE6, UI_ACCENT, "ME");
  Lcd.fillRect(0, 0, 44, 20, UI_BG);
  Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(2); Lcd.setTextColor(UI_DIM, UI_BG);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
  Lcd.drawString("<", 4, 0);
  Lcd.fillRect(0, SCR_H - 16, SCR_W, 16, UI_BG);
  char hb[64];
  snprintf(hb, sizeof(hb), "Z%d  %ld.%04ld,%ld.%04ld  GSI tiles", mapZ,
           (long)(mapLatE6 / 1000000), (long)labs((mapLatE6 % 1000000) / 100),
           (long)(mapLonE6 / 1000000), (long)labs((mapLonE6 % 1000000) / 100));
  txt(hb, 4, SCR_H - 14, UI_MUTE, 1);
  hbtn(162, 196, 70, 26, "-", KC_NORMAL, UI_TEXT);
  hbtn(240, 196, 70, 26, "+", KC_NORMAL, UI_TEXT);
}


// ============================================================
//  SCREENS
// ============================================================
static int brightnessVal = 200;
static int batteryPct() {
  int raw = analogRead(PIN_BAT_ADC);
  float v = raw / 4095.0f * 3.3f * 2.0f;
  int pct = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}
// ---- home terminal stream (group conversation, both radios) ----
#define TERM_N 10
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
// ---- home screen: slim header (name / quiet GPS clock / battery),
// radio status card, and the group terminal filling the rest ----
static unsigned long lastStatusDraw = 0;
static int hdrMinShown = -2;
static void drawHomeClock() {
  Lcd.fillRect(178, 0, 76, 16, UI_BG);
  time_t tt = time(nullptr);
  if (tt <= 1700000000 && !gpsPresent) return;   // no clock source on this unit: leave the corner empty
  char cb[8];
  if (tt > 1700000000) { struct tm t; localtime_r(&tt, &t); snprintf(cb, sizeof(cb), "%02d:%02d", t.tm_hour, t.tm_min); }
  else snprintf(cb, sizeof(cb), "--:--");
  Lcd.setFont(&lgfx::fonts::Font2); Lcd.setTextSize(1); Lcd.setTextColor(UI_DIM, UI_BG);
  Lcd.setTextDatum(lgfx::textdatum_t::top_left);
  Lcd.drawString(cb, 196, 1);
}
static void drawHomeStatus() {
  Lcd.fillRect(9, 59, SCR_W - 18, 30, UI_CARD);
  char b1[64], b2[64];
  snprintf(b1, sizeof(b1), "ESP-NOW %s ch%d%s Online:%d%s%s", chatOn ? "ON" : "OFF", chatChannel, chatLR ? " LR" : "", onlinePeerCount(), anyUnread() ? " [NEW]" : "", sosActive ? " [SOS]" : "");
  txt(b1, 16, 61, UI_TEXT, 1);
  if (loraReady) {
    if (loraProf == 2) snprintf(b2, sizeof(b2), "LoRa 920.8 MESHCR  RX %lu pkts", rawTotal);
    else if (loraHeardAt && millis() - loraHeardAt < 3600000UL) snprintf(b2, sizeof(b2), "LoRa 920.8 %s  %s  %ddBm", loraProf ? "NARROW" : "STD", loraHeardName.c_str(), loraHeardRssi);
    else snprintf(b2, sizeof(b2), "LoRa 920.8 %s  listening", loraProf ? "NARROW" : "STD");
  } else snprintf(b2, sizeof(b2), "LoRa -- (not found)");
  txt(b2, 16, 77, UI_DIM, 1);
}
static void drawHomeTerminal() {
  Lcd.fillRect(9, 114, SCR_W - 18, 96, UI_BG);
  if (termScroll > termCount - 5) termScroll = termCount > 5 ? termCount - 5 : 0;
  if (termScroll < 0) termScroll = 0;
  int showTo = termCount - termScroll;
  int start = showTo - 5; if (start < 0) start = 0;
  int y = 116;
  for (int i = start; i < showTo; i++) {
    int bar = termLines[i].indexOf('|');
    String who = bar >= 0 ? termLines[i].substring(0, bar) : "?";
    String body = bar >= 0 ? termLines[i].substring(bar + 1) : termLines[i];
    String ln = "[" + who + "] " + body;
    bool mine = who == "Me";
    bool sos = body.startsWith("SOS ");
    jpText(fitText(ln, SCR_W - 26).c_str(), 12, y, sos ? UI_BAD : (mine ? UI_ACCENT : UI_TEXT), -1);
    y += 18;
  }
  drawComposerLine(12, 216, UI_ACCENT);
  termDirty = false;
}
static void drawHome() {
  Lcd.fillScreen(UI_BG);
  char bb[24]; snprintf(bb, sizeof(bb), "BAT %d%%", batteryPct());
  txt(bb, SCR_W - 6 - txtW(bb), 1, UI_MUTE, 1);
  txt(fitText(myName, 150).c_str(), 8, 1, UI_MUTE, 1);
  hdrMinShown = -2;
  drawHomeClock();
  hbtn(6, 20, 74, 30, "CHAT", KC_ACT, UI_TEXT);
  if (anyUnread()) Lcd.fillCircle(76, 24, 4, UI_WARN);
  hbtn(84, 20, 74, 30, "MAP", KC_NORMAL, UI_TEXT);
  hbtn(162, 20, 74, 30, "FM", KC_NORMAL, UI_TEXT);
  hbtn(240, 20, 74, 30, "SET", KC_NORMAL, UI_TEXT);
  card(8, 56, SCR_W - 16, 36);
  card(8, 96, SCR_W - 16, 140);
  txt(chatOn ? "COMIC CHAT" : "COMIC CHAT (off)", 14, 100, UI_MUTE, 1);
  lastStatusDraw = 0;
  drawHomeStatus();
  drawHomeTerminal();
}
static void homeTick() {
  time_t tt = time(nullptr);
  int mn = tt > 1700000000 ? (int)(tt / 60) : -1;
  if (mn != hdrMinShown) { hdrMinShown = mn; drawHomeClock(); }
  if (millis() - lastStatusDraw > 5000) { lastStatusDraw = millis(); drawHomeStatus(); }
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
    bool sos = ln.indexOf("] SOS ") >= 0;
    jpText(ln.c_str(), 10, 30 + i * 19, sos ? UI_BAD : (ln.startsWith("[Me]") ? UI_ACCENT : UI_TEXT), -1, SCR_W - 18);
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
    if (on && chatPeers[i].rssiAt) {   // v15: proximity from ESP-NOW RSSI
      char rb[20];
      const char* prox = chatPeers[i].rssi >= -55 ? "NEAR" : (chatPeers[i].rssi >= -75 ? "MID" : "FAR");
      snprintf(rb, sizeof(rb), " %ddBm %s", chatPeers[i].rssi, prox);
      ln += rb;
    }
    if (chatPeers[i].posAt && millis() - chatPeers[i].posAt < 1800000UL) {
      if (gpsFix) {
        float dm, br;
        distBrg(gpsLatE6, gpsLonE6, chatPeers[i].latE6, chatPeers[i].lonE6, &dm, &br);
        char pb[24];
        if (dm >= 1000.0f) snprintf(pb, sizeof(pb), "  %.1fkm %s", dm / 1000.0f, compass8(br));
        else snprintf(pb, sizeof(pb), "  %.0fm %s", dm, compass8(br));
        ln += pb;
      } else ln += "  [POS]";
    }
    jpText(fitText(ln, SCR_W - 24).c_str(), 10, y, UI_TEXT, -1);
    y += 22;
  }
  if (chatPeerCount == 0) txt("No peers yet (HELLO every 5 s)", 10, 34, UI_MUTE, 1);
  char lb[64];
  if (loraReady && loraHeardAt) snprintf(lb, sizeof(lb), "LoRa last: %s (%ddBm, %lus ago)", loraHeardName.c_str(), loraHeardRssi, (millis() - loraHeardAt) / 1000);
  else snprintf(lb, sizeof(lb), "LoRa: %s", loraReady ? "listening" : "not found");
  txt(lb, 10, 204, UI_MUTE, 1);
  txt("tap peer to open - bottom bar: RAW LOG", 10, 220, UI_MUTE, 1);
}
static void drawRaw() {
  Lcd.fillScreen(UI_BG);
  drawHeader("LoRa RAW");
  txt("920.8 BW62.5 SF8 CR5 - MeshCore listen", 10, 26, UI_DIM, 1);
  char hb[48];
  snprintf(hb, sizeof(hb), "packets: %lu (latest %d shown)", rawTotal, min(rawCount, 4));
  txt(hb, 10, 38, UI_MUTE, 1);
  if (rawCount == 0) txt("No packets yet - is the peer on air?", 10, 62, UI_MUTE, 1);
  int y = 58;
  for (int i = 0; i < rawCount && i < 4; i++) {
    RawPkt& rp = rawLog[i];
    char lb[48];
    snprintf(lb, sizeof(lb), "%dB  %ddBm  %lus ago", rp.len, rp.rssi, (millis() - rp.at) / 1000);
    txt(lb, 10, y, UI_TEXT, 1);
    int n = min((int)rp.len, 32);
    for (int r = 0; r < 2; r++) {
      char hx[52]; int pos = 0;
      for (int j = r * 16; j < n && j < r * 16 + 16; j++) pos += snprintf(hx + pos, sizeof(hx) - pos, "%02X ", rp.data[j]);
      if (pos) txt(hx, 10, y + 11 + r * 11, UI_DIM, 1);
    }
    char nm[40], meta[40];
    if (mcParseAdvert(rp.data, rp.len, nm, sizeof(nm), meta, sizeof(meta))) {
      char al[96];
      snprintf(al, sizeof(al), "ADV %s  %s", nm, meta);
      jpText(al, 10, y + 33, UI_ACCENT, -1);
      y += 49;
    } else {
      y += 38;
    }
    if (y > 200) break;
  }
  if (sdReady) {
    char sb[56];
    snprintf(sb, sizeof(sb), "SD log: %lu pkts in /meshcore_raw.log", (unsigned long)rawSdCount);
    txt(sb, 10, 212, UI_MUTE, 1);
  }
  txt("ESC / tap top-left: back to peers", 10, 224, UI_MUTE, 1);
}
// ---- RF scanner screen ----
static void drawScan() {
  Lcd.fillScreen(UI_BG);
  drawHeader("RF SCAN");
  txt("920.6-923.4 MHz  step 0.2  (LoRa RX paused)", 10, 26, UI_DIM, 1);
  if (!loraReady) { txt("LoRa radio not found.", 10, 62, UI_WARN, 1); return; }
  const int base = 190;
  Lcd.drawLine(10, base, SCR_W - 10, base, UI_BORDER);
  int peak = 0;
  for (int i = 0; i < SCAN_N; i++) if (scanRssi[i] > scanRssi[peak]) peak = i;
  for (int i = 0; i < SCAN_N; i++) {
    int x = 14 + i * 19;
    int h = (scanRssi[i] + 120) * 110 / 90;   // -120..-30 dBm -> 0..110 px
    if (h < 1) h = 1;
    if (h > 118) h = 118;
    uint16_t col = (i == peak) ? UI_WARN : (i == 1 ? UI_ACCENT : UI_DIM);
    Lcd.fillRect(x, base - h, 13, h, col);
  }
  txt("920.6", 12, base + 4, UI_MUTE, 1);
  txt("920.8=CoT", 52, base + 4, UI_ACCENT, 1);
  txt("923.4", SCR_W - 44, base + 4, UI_MUTE, 1);
  char pb[56];
  snprintf(pb, sizeof(pb), "PEAK %.1f MHz  %d dBm", scanFreqMhz(peak), scanRssi[peak]);
  txt(pb, 10, 222, UI_TEXT, 1);
}
// ---- settings ----
static bool editingName = false;
static String editBuf = "";
static void drawSettings() {
  Lcd.fillScreen(UI_BG);
  drawHeader("Settings");
  const int x = 8, w = SCR_W - 16;
  int y = 28;
  auto row = [&](const char* label, const String& value) {
    card(x, y, w, 20);
    txt(label, x + 10, y + 3, UI_DIM, 1);
    jpText(fitText(value, 190).c_str(), x + 118, y + 3, UI_TEXT, -1);
    y += 22;
  };
  if (editingName) row("Name*", editBuf + "_");
  else row("Name", myName);
  row("ESP-NOW", chatOn ? "ON" : "OFF");
  row("ESP-NOW Ch", String(chatChannel));
  row("ESP-NOW", chatLR ? "LongRange" : "Normal");
  row("Bright", String(brightnessVal));
  row("Cursor", cursorMode == 0 ? "Auto" : (cursorMode == 1 ? "ON" : "OFF"));
  row("LoRa", loraProf == 1 ? "Narrow" : (loraProf == 2 ? "MeshCore" : "Std"));
}
static void drawCurrent() {
  switch (screen) {
    case SC_HOME: drawHome(); break;
    case SC_CHAT: drawChat(); break;
    case SC_PEERS: drawPeers(); break;
    case SC_RAW: drawRaw(); break;
    case SC_SET: drawSettings(); break;
    case SC_RADIO: drawRadio(); break;
    case SC_GPS: drawGps(); break;
    case SC_MAP: drawMap(); break;
    case SC_SCAN: drawScan(); break;
    case SC_VOICE: drawVoice(); break;
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
  if (editingName) {
    if (c == 0x0D) {
      if (editBuf.length()) { myName = editBuf; saveMyName(); }
      editingName = false;
      needsRedraw = true;
    } else if (c == 0x1B) { editingName = false; needsRedraw = true; }
    else if (c == 0x08) { if (editBuf.length()) editBuf.remove(editBuf.length() - 1); needsRedraw = true; }
    else if (c >= 0x20 && c < 0x7F) {
      if (editBuf.length() < 15) editBuf += (char)c;
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
    case SC_PEERS:
      if (c == 0x0D && peerSel >= 0 && peerSel < chatPeerCount) {
        curPeer = peerSel;
        chatPeers[curPeer].hasUnread = false;
        loadChatLines(curPeer);
        screen = SC_CHAT;
        needsRedraw = true;
      } else if (c == 'r' || c == 'R') { screen = SC_RAW; needsRedraw = true; }
      else if (c == 'f' || c == 'F') { screen = SC_RADIO; needsRedraw = true; }
      else if (c == 'g' || c == 'G') { screen = SC_GPS; needsRedraw = true; }
      else if (c == 'm' || c == 'M') { mapRecenterGps(); screen = SC_MAP; needsRedraw = true; }
      else if (c == 's' || c == 'S') { scanEnter(); screen = SC_SCAN; needsRedraw = true; }
      else if (c == 'v' || c == 'V') { voiceEnsureInit(); screen = SC_VOICE; needsRedraw = true; }
      else if (c == 0x1B) goHome();
      break;
    case SC_VOICE:
      if (c == 't' || c == 'T') { vLastTKeyAt = millis(); }
      else if (c == 0x1B || c == 0x0D) { screen = SC_PEERS; needsRedraw = true; }
      break;
    case SC_RAW:
      if (c == 0x1B || c == 0x0D) { screen = SC_PEERS; needsRedraw = true; }
      break;
    case SC_SCAN:
      if (c == 0x1B || c == 0x0D) { scanExit(); screen = SC_PEERS; needsRedraw = true; }
      break;
    case SC_SET:
      if (c == 0x1B) goHome();
      break;
    case SC_RADIO:
      if (c == ',') { fmSeek(false); needsRedraw = true; }
      else if (c == '.') { fmSeek(true); needsRedraw = true; }
      else if (c == '-') { fmVolSet(fmVol - 1); needsRedraw = true; }
      else if (c == '=' || c == '+') { fmVolSet(fmVol + 1); needsRedraw = true; }
      else if (c == 0x1B || c == 0x0D) goHome();
      break;
    case SC_GPS:
      if (c == 0x0D || c == 'b' || c == 'B') { sendPosBeacon(); needsRedraw = true; }
      else if (c == 's' || c == 'S') { sosButtonTap(); }
      else if (c == 0x1B) goHome();
      break;
    case SC_MAP:
      if (c == '+' || c == '=') { if (mapZ < 18) { mapZ++; needsRedraw = true; } }
      else if (c == '-') { if (mapZ > 5) { mapZ--; needsRedraw = true; } }
      else if (c == 'c' || c == 'C') { mapRecenterGps(); needsRedraw = true; }
      else if (c == 0x1B || c == 0x0D) goHome();
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
      if (ty >= 20 && ty < 50) {
        if (tx < 80) { int gi = ensureGroupPeer(); curPeer = gi; if (gi >= 0) { chatPeers[gi].hasUnread = false; loadChatLines(gi); } screen = SC_PEERS; needsRedraw = true; }
        else if (tx < 158) { mapRecenterGps(); screen = SC_MAP; needsRedraw = true; }
        else if (tx < 236) { screen = SC_RADIO; needsRedraw = true; }
        else { screen = SC_SET; needsRedraw = true; }
      } else if (ty >= 56 && ty < 96) { scanEnter(); screen = SC_SCAN; needsRedraw = true; }
      else if (ty >= 96) { screen = SC_CHAT; if (curPeer < 0) { curPeer = ensureGroupPeer(); if (curPeer >= 0) loadChatLines(curPeer); } needsRedraw = true; }
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
      if (ty >= 196) { screen = SC_RAW; needsRedraw = true; break; }
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
    case SC_RAW:
      if (ty < 24 && tx < 50) { screen = SC_PEERS; needsRedraw = true; }
      break;
    case SC_SCAN:
      if (ty < 24 && tx < 50) { scanExit(); screen = SC_PEERS; needsRedraw = true; }
      break;
    case SC_VOICE:
      if (ty < 24 && tx < 50) { screen = SC_PEERS; needsRedraw = true; }
      break;
    case SC_RADIO:
      if (ty < 24 && tx < 50) { goHome(); break; }
      if (ty >= 186 && ty < 226) {
        if (tx < 76) { fmSeek(false); needsRedraw = true; }
        else if (tx < 154) { fmSeek(true); needsRedraw = true; }
        else if (tx < 232) { fmVolSet(fmVol - 1); needsRedraw = true; }
        else { fmVolSet(fmVol + 1); needsRedraw = true; }
      }
      break;
    case SC_GPS:
      if (ty < 24 && tx < 50) { goHome(); break; }
      if (ty >= 140 && ty < 180 && gpsFix) { sendPosBeacon(); needsRedraw = true; }
      else if (ty >= 190 && ty < 230) { sosButtonTap(); }
      break;
    case SC_MAP:
      if (ty < 24 && tx < 50) { goHome(); break; }
      if (ty >= 196 && tx >= 236) { if (mapZ < 18) mapZ++; needsRedraw = true; break; }
      if (ty >= 196 && tx >= 158 && tx < 236) { if (mapZ > 5) mapZ--; needsRedraw = true; break; }
      mapTapCenter(tx, ty);
      needsRedraw = true;
      break;
    case SC_SET: {
      if (editingName) {
        if (ty >= 28 && (ty - 28) / 22 == 0) { editingName = false; needsRedraw = true; }
        break;
      }
      if (ty < 24 && tx < 50) { goHome(); break; }
      int rowIdx = (ty - 28) / 22;
      if (ty >= 28 && rowIdx >= 0 && rowIdx < 7) {
        if (rowIdx == 0) { editingName = true; editBuf = myName; }
        else if (rowIdx == 1) { if (chatOn) chatStop(); else chatStart(); }
        else if (rowIdx == 2) { chatChannel = chatChannel % 13 + 1; saveChatPrefs(); if (chatOn) { chatStop(); chatStart(); } }
        else if (rowIdx == 3) { chatLR = !chatLR; saveChatPrefs(); if (chatOn) applyChatProtocol(); }
        else if (rowIdx == 4) {
          brightnessVal += 40; if (brightnessVal > 255) brightnessVal = 40;
          Lcd.setBrightness((uint8_t)brightnessVal);
          prefs.begin("sys", false); prefs.putInt("bright", brightnessVal); prefs.end();
        }
        else if (rowIdx == 5) {
          cursorMode = (cursorMode + 1) % 3;
          prefs.begin("sys", false); prefs.putInt("cur", cursorMode); prefs.end();
        }
        else if (rowIdx == 6) {
          loraProf = (loraProf + 1) % 3;
          saveChatPrefs();
          loraInit();
        }
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
  prefs.begin("sys", true); brightnessVal = prefs.getInt("bright", 200); prefs.end();
  loadFmPrefs();
  loadMapPrefs();
  setenv("TZ", "JST-9", 1); tzset();
  Lcd.begin(1, (uint8_t)brightnessVal);   // official init: CS pre-deselect + rotation 1
  Lcd.invertDisplay(true);               // this panel batch needs INVON, else colours are inverted
  Lcd.fillScreen(UI_BG);
  bootType("C:\\> BOOT  ENZ T-DECK v16", 12, 8, UI_ACCENT);
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  sdReady = SD.begin(PIN_SD_CS, SPI, 20000000);
  bootLine(sdReady ? "SD card ............ OK" : "SD card ............ NONE");
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);   // keyboard + touch + sensors all share this one bus/controller
  bool jp = jpInit();
  bootLine(jp ? "Japanese font ...... OK" : "Font: ASCII only");
  bool fm = fmInit();
  bootLine(fm ? "FM radio ........... OK (Si4703)" : "FM radio ........... none");
  gpsInit();
  bootLine("GPS ................ auto (Plus)");
  loadMyName();
  chatStart();
  bootLine(chatOn ? "ESP-NOW ............ ON" : "ESP-NOW ............ FAIL");
  bool lr = loraInit();
  bootLine(lr ? "LoRa 920.8MHz ..... RX" : "LoRa ............... FAIL");
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
      int mx = termCount > 5 ? termCount - 5 : 0;
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
    else if (!cursorActive() && screen == SC_MAP && (tbDx != 0 || tbDy != 0)) { mapPan(tbDx * 16, tbDy * 16); needsRedraw = true; }
    else if (!cursorActive() && screen == SC_RADIO && tbDy != 0) { fmVolSet(fmVol - tbDy); needsRedraw = true; }
    else if (!cursorActive() && screen == SC_PEERS) {
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
    else if (screen == SC_RAW) { screen = SC_PEERS; needsRedraw = true; }
    else if (screen == SC_SCAN) { scanExit(); screen = SC_PEERS; needsRedraw = true; }
    else if (screen == SC_HOME) { screen = SC_CHAT; needsRedraw = true; }
  }
  wasTbPress = tbp;
  processChatRx();
  loraPoll();
  gpsPoll();
  sosTick();
  voiceTick();
  if (screen == SC_SCAN) scanTick();
  if (screen == SC_RADIO) fmPoll();
  if (screen == SC_HOME) { homeTick(); cursorShow(); }
  if (screen == SC_CHAT && chatDirty) { chatDirty = false; needsRedraw = true; }
  if (needsRedraw) { drawCurrent(); needsRedraw = false; cursorShow(); }
  if (curMoved && cursorActive() && (millis() - lastCurPaint >= 40 || millis() - lastTbMoveAt >= 30)) {
    drawCurrent();
    cursorShow();
    curMoved = false;
    lastCurPaint = millis();
  }
  delay(4);
}
