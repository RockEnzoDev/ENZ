// ============================================================
//  ENZ for M5Stack Core2  --  clock + ESP-NOW chat + music player
//  Chat-companion to the CYD (ESP32 ENZ Launcher): the ESP-NOW wire
//  protocol is byte-identical (MsgPacket, HELLO every 5 s, SD logs in
//  /chats as "who|text"), so the two devices chat with each other.
//  No WiFi is used: the clock runs from the Core2's RTC (set it by
//  hand in Settings) and ESP-NOW runs on a fixed channel (default 1).
//  Music: MP3 + WAV from the SD card folder /music, through the
//  built-in speaker (I2S amp). Japanese text/input uses the same SD
//  files as the CYD: /jpfont16.bin and /skk.dic.
//  Libraries: M5Unified, ESP8266Audio.  Board: M5Stack Core2.
// ============================================================
#include <esp_bt_defs.h>   // esp_bd_addr_t for the A2DP discovery callback
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioFileSourceSD.h>
#include <AudioOutput.h>

// ---------------- palette (DOS green, same family as the CYD) ----------------
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | ((b) >> 3)))
#define UI_BG      RGB565(2, 10, 4)
#define UI_CARD    RGB565(4, 22, 10)
#define UI_BORDER  RGB565(16, 70, 34)
#define UI_TEXT    RGB565(180, 255, 200)
#define UI_DIM     RGB565(64, 200, 110)
#define UI_MUTE    RGB565(24, 110, 58)
#define UI_ACCENT  RGB565(64, 255, 128)
#define UI_ACCENT2 RGB565(24, 160, 80)
#define UI_GHOST   RGB565(8, 26, 14)
#define UI_WARN    RGB565(255, 190, 60)
#define UI_BAD     RGB565(255, 90, 70)
#define KC_NORMAL  RGB565(6, 30, 14)
#define KC_ACT     RGB565(10, 60, 26)

#define SCR_W 320
#define SCR_H 240

static M5GFX& Lcd = M5.Lcd;
Preferences prefs;
bool sdReady = false;
String brandName = "JohnDoe";

// screen ids (kept high in the file: see the note at the chat section)
enum Screen { SC_CLOCK, SC_PEERS, SC_CHAT, SC_MUSIC, SC_TRACKS, SC_SET, SC_BT, SC_BTSCAN, SC_TIMESET, SC_KB };
static Screen screen = SC_CLOCK;
static bool needsRedraw = true;

// ---------------- small draw/text helpers ----------------
static void txt(const char* s, int x, int y, uint16_t color, int size = 1) {
  Lcd.setFont(&fonts::Font2);
  Lcd.setTextSize(size);
  Lcd.setTextDatum(m5gfx::datum_t::top_left);
  Lcd.setTextColor(color);
  Lcd.drawString(s, x, y);
}
static int txtW(const char* s, int size = 1) {
  Lcd.setFont(&fonts::Font2);
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
  Lcd.setFont(&fonts::Font2);
  Lcd.setTextSize(1);
  Lcd.setTextDatum(m5gfx::datum_t::middle_center);
  Lcd.setTextColor(color);
  Lcd.drawString(label, x + w / 2, y + h / 2);
  Lcd.setTextDatum(m5gfx::datum_t::top_left);
}
static String fitText(const String& s, int maxW);   // fwd (JP-aware, defined below)

// ============================================================
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
//  CHAT (ESP-NOW) -- wire-compatible with the CYD ENZ Launcher
// ============================================================
#define CHAT_DIR "/chats"
#define MAX_PEERS 8
#define MAX_CHAT_MSGS 30
#define RX_SLOTS 6
#define HELLO_INTERVAL_MS 5000
#define PEER_TIMEOUT_MS 30000

struct __attribute__((packed)) MsgPacket {   // identical layout to the CYD
  uint8_t type;          // 1 = HELLO, 2 = CHAT
  uint8_t to_mac[6];     // zeros = broadcast (everyone)
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
String chatLines[MAX_CHAT_MSGS];      // "who|text" lines of the open conversation
int    chatLineCount = 0;
int    curPeer = -1;
bool   chatLR = false;
int    chatChannel = 1;               // fixed: no AP to follow on this device

struct RxItem { uint8_t mac[6]; uint8_t data[225]; uint16_t len; };
volatile RxItem msgRx[RX_SLOTS];
volatile uint8_t msgRxHead = 0, msgRxTail = 0;
unsigned long lastHello = 0;
const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t GROUP_MAC[6] = {0, 0, 0, 0, 0, 0};   // pseudo peer: broadcast chat

// notify state (vibration + beep on incoming chat)
static unsigned long vibUntil = 0;
static void notifyIncoming() {
  M5.Power.setVibration(200);
  vibUntil = millis() + 150;
  M5.Speaker.tone(2093.0f, 70);   // short beep on a free speaker channel
}

String macToStr(const uint8_t* m) {
  char b[13];
  snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
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
static String peerName(int i) { return chatPeers[i].name[0] ? String(chatPeers[i].name) : macToStr(chatPeers[i].mac); }

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
void loadChatPrefs() {
  prefs.begin("chat", true);
  chatLR = prefs.getBool("lr", false);
  chatChannel = prefs.getInt("ch", 1);
  prefs.end();
  if (chatChannel < 1) chatChannel = 1;
  if (chatChannel > 13) chatChannel = 13;
}
void saveChatPrefs() {
  prefs.begin("chat", false);
  prefs.putBool("lr", chatLR);
  prefs.putInt("ch", chatChannel);
  prefs.end();
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

void ensurePeer(const uint8_t* mac) {
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
  loadMyName();
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel((uint8_t)chatChannel, WIFI_SECOND_CHAN_NONE);
  applyChatProtocol();
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(onMsgRecv);
  ensurePeer(BCAST_MAC);
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

// ---- SD chat logs (same format as the CYD: "who|text" per line) ----
void appendChatLog(int peerIdx, bool outgoing, const String& text) {
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
static void loadLogTail(const String& path) {   // last MAX_CHAT_MSGS lines -> chatLines
  chatLineCount = 0;
  if (!sdReady) return;
  File f = SD.open(path);
  if (!f) return;
  String ring[MAX_CHAT_MSGS]; int idx = 0;
  String line = "";
  while (f.available()) {
    char c = (char)f.read();
    if (c == '\n') { if (line.length()) { ring[idx % MAX_CHAT_MSGS] = line; idx++; } line = ""; }
    else if (c != '\r') line += c;
  }
  if (line.length()) { ring[idx % MAX_CHAT_MSGS] = line; idx++; }
  f.close();
  int total = idx < MAX_CHAT_MSGS ? idx : MAX_CHAT_MSGS;
  int start = idx > MAX_CHAT_MSGS ? idx % MAX_CHAT_MSGS : 0;
  for (int i = 0; i < total; i++) chatLines[chatLineCount++] = ring[(start + i) % MAX_CHAT_MSGS];
}
void loadChatHistory(int peerIdx) {
  if (peerIdx < 0) { chatLineCount = 0; return; }
  loadLogTail(macIsZero(chatPeers[peerIdx].mac) ? String("/chats/group.log")
                                                : chatPath(chatPeers[peerIdx].mac));
}
static void pushChatLine(const String& line) {
  if (chatLineCount < MAX_CHAT_MSGS) chatLines[chatLineCount++] = line;
  else {
    for (int i = 0; i < MAX_CHAT_MSGS - 1; i++) chatLines[i] = chatLines[i + 1];
    chatLines[MAX_CHAT_MSGS - 1] = line;
  }
}
static int ensureGroupPeer() {   // the Broadcast pseudo-peer always exists
  int bi = findPeerByMac(GROUP_MAC);
  if (bi < 0 && chatPeerCount < MAX_PEERS) {
    bi = chatPeerCount++;
    memset(chatPeers[bi].mac, 0, 6);
    strncpy(chatPeers[bi].name, "Broadcast", 15); chatPeers[bi].name[15] = 0;
    chatPeers[bi].hasUnread = false; chatPeers[bi].lastSeen = 0;
  }
  return bi;
}

// (screen enum lives near the top of the file: Arduino hoists function
// prototypes above it otherwise, and kbOpen() takes a Screen argument.)

static volatile bool clockStatusDirty = false;   // chat event: clock screen repaints only its status card
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

    if (m.type == 1) {                       // HELLO
      if (screen == SC_PEERS) needsRedraw = true;
      if (screen == SC_CLOCK) clockStatusDirty = true;   // online count may change
    } else if (m.type == 2) {                // CHAT
      uint8_t myMac[6]; WiFi.macAddress(myMac);
      bool toMe = macEq(m.to_mac, myMac) || macIsZero(m.to_mac);
      if (!toMe) continue;
      bool group = macIsZero(m.to_mac);
      appendChatLog(idx, false, String(m.text));
      if (group) appendGroupLog(peerName(idx), String(m.text));
      bool viewing = (screen == SC_CHAT && ((group && curPeer >= 0 && macIsZero(chatPeers[curPeer].mac)) || curPeer == idx));
      if (viewing) {
        pushChatLine(peerName(idx) + "|" + String(m.text));
        needsRedraw = true;
      } else {
        chatPeers[idx].hasUnread = true;
        if (group) { int gi = ensureGroupPeer(); if (gi >= 0) chatPeers[gi].hasUnread = true; }
        notifyIncoming();
        if (screen == SC_CLOCK) clockStatusDirty = true; else needsRedraw = true;
      }
    }
  }
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

// ============================================================
//  KEYBOARD (ABC + kana + SKK conversion) -- logic ported from the CYD
// ============================================================
static String kbInput = "";
static String imeComp = "";            // kana composition (not yet committed)
static int  kbMode = 0;                // 0 = ABC, 1 = hiragana, 2 = katakana
static bool kbUpper = false;
static int  kbLayer = 0;               // ABC sub-layer: 0 = letters, 1 = symbols
static int  kbMaxBytes = 199;
static int  kbTarget = 0;              // 0 = chat message, 1 = nickname
static Screen kbReturn = SC_CHAT;
static bool skkMissing = false;
static String imeCand[24];
static int  imeCandN = 0, imeCandPage = 0;

static const char* const KANA_H[5][11] = {
  { "あ","か","さ","た","な","は","ま","や","ら","わ","、" },
  { "い","き","し","ち","に","ひ","み","", "り","を","。" },
  { "う","く","す","つ","ぬ","ふ","む","ゆ","る","ん","！" },
  { "え","け","せ","て","ね","へ","め","", "れ","・","？" },
  { "お","こ","そ","と","の","ほ","も","よ","ろ","…","ー" },
};
static const char* const KANA_DAKU[]  = { "かが","きぎ","くぐ","けげ","こご","さざ","しじ","すず","せぜ","そぞ","ただ","ちぢ","つづ","てで","とど","はばぱ","ひびぴ","ふぶぷ","へべぺ","ほぼぽ","うゔ" };
static const char* const KANA_SMALL[] = { "ぁあ","ぃい","ぅう","ぇえ","ぉお","っつ","ゃや","ゅゆ","ょよ","ゎわ" };

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
static void imeCommit() { if (imeComp.length()) { kbInput += imeComp; imeComp = ""; } imeCandN = 0; }
static void kbAppend(const String& s) {
  if (kbInput.length() + imeComp.length() + s.length() <= (unsigned)kbMaxBytes) kbInput += s;
}
static void imeAppend(const String& s) {
  if (kbInput.length() + imeComp.length() + s.length() <= (unsigned)kbMaxBytes) imeComp += s;
}
static bool imeCycleLast(const char* const* table, int n) {
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
static bool skkLookup(const String& key, String* out, int& n, int maxN) {
  File f = SD.open("/skk.dic", FILE_READ);
  if (!f) { skkMissing = true; return false; }
  skkMissing = false;
  uint32_t lo = 0, hi = f.size();
  String line;
  while (lo < hi) {
    uint32_t mid = (lo + hi) >> 1, s = mid;
    if (s > lo) {
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
  if (!imeComp.length()) return;
  String reading = toHira(imeComp);
  imeCandN = 0;
  String tmp[20]; int n = 0;
  skkLookup(reading, tmp, n, 20);
  for (int i = 0; i < n; i++) imeAddCand(tmp[i]);
  imeAddCand(reading);
  imeAddCand(toKata(reading));
  imeCandPage = 0;
}

// ---- keyboard drawing ----
static void drawKey(int x, int y, int w, int h, const String& label, uint16_t fill) {
  Lcd.fillRoundRect(x, y, w, h, 4, fill);
  Lcd.drawRoundRect(x, y, w, h, 4, UI_BORDER);
  if (hasNonAscii(label.c_str()) && jpReady) {
    int tw = jpWidth(label.c_str());
    jpText(label.c_str(), x + (w - tw) / 2, y + (h - 16) / 2, UI_TEXT, -1);
  } else {
    Lcd.setFont(&fonts::Font2);
    Lcd.setTextSize(1);
    Lcd.setTextDatum(m5gfx::datum_t::middle_center);
    Lcd.setTextColor(UI_TEXT);
    Lcd.drawString(label.c_str(), x + w / 2, y + h / 2);
    Lcd.setTextDatum(m5gfx::datum_t::top_left);
  }
}
static void drawKbInput() {
  card(4, 26, SCR_W - 8, 28, UI_CARD, UI_ACCENT);
  String shown = kbInput;
  int maxW = SCR_W - 24;
  // keep the tail visible: drop leading chars until it fits
  String tail = shown;
  while (tail.length() && textW(tail) + textW(imeComp) > maxW) { const char* p = tail.c_str(); u8next(p); tail.remove(0, p - tail.c_str()); }
  int x = 10;
  x += jpText(tail.c_str(), x, 32, UI_TEXT, -1);
  if (imeComp.length()) {
    int w = jpText(imeComp.c_str(), x, 32, UI_ACCENT, -1);
    Lcd.drawLine(x, 50, x + w, 50, UI_ACCENT);   // composition underline
  }
  txt("_", x + (imeComp.length() ? textW(imeComp) : 0) + 2, 33, UI_DIM, 1);
}
static void drawKb() {
  Lcd.fillScreen(UI_BG);
  hbtn(2, 0, 46, 22, "<", KC_NORMAL, UI_TEXT);
  txt(kbTarget == 1 ? "Name" : "Message", 56, 3, UI_DIM, 1);
  hbtn(266, 0, 52, 22, "OK", UI_ACCENT2, UI_BG);
  drawKbInput();
  if (kbMode == 0) {                                     // ---- ABC ----
    static const char* rows[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const char* srows[3] = { "1234567890", "!@#$%^&*()", "-_=+[];" };
    for (int r = 0; r < 3; r++) {
      const char* row = kbLayer ? srows[r] : rows[r];
      int n = strlen(row);
      int kw = 30, gap = 2;
      int x0 = (r == 1) ? 17 : 2;
      if (r == 2) x0 = 50;
      int y = 84 + r * 38;
      if (r == 2) drawKey(2, y, 46, 34, kbLayer ? "abc" : "aA", kbUpper ? KC_ACT : KC_NORMAL);
      for (int i = 0; i < n; i++) {
        char c = row[i];
        String lbl = String(kbUpper || kbLayer ? c : (char)tolower(c));
        if (!kbLayer && !kbUpper) lbl = String((char)tolower(c));
        if (!kbLayer && kbUpper) lbl = String(c);
        drawKey(x0 + i * (kw + gap), y, kw, 34, lbl, KC_NORMAL);
      }
      if (r == 2) drawKey(272, y, 46, 34, "Del", KC_NORMAL);
    }
    int y = 198;
    if (kbLayer) {
      drawKey(2, y, 52, 34, "ABC", KC_NORMAL);
      drawKey(56, y, 52, 34, "JP", KC_NORMAL);
      drawKey(110, y, 34, 34, "/", KC_NORMAL);
      drawKey(146, y, 34, 34, "?", KC_NORMAL);
      drawKey(182, y, 62, 34, "Space", KC_NORMAL);
      drawKey(246, y, 34, 34, "<", KC_NORMAL);
      drawKey(282, y, 34, 34, ">", KC_NORMAL);
    } else {
      drawKey(2, y, 52, 34, "#@", KC_NORMAL);
      drawKey(56, y, 52, 34, "JP", KC_NORMAL);
      drawKey(110, y, 164, 34, "Space", KC_NORMAL);
      drawKey(276, y, 42, 34, ".", KC_NORMAL);
    }
  } else {                                               // ---- kana ----
    // function / candidate strip
    int sy = 58;
    if (imeCandN > 0) {
      drawKey(2, sy, 40, 24, "X", KC_NORMAL);
      int x = 46;
      int start = imeCandPage * 5;
      for (int i = start; i < imeCandN && i < start + 5; i++) {
        int w = textW(imeCand[i]) + 14;
        if (x + w > 284) break;
        drawKey(x, sy, w, 24, imeCand[i], KC_ACT);
        x += w + 3;
      }
      if (imeCandN > 5) drawKey(288, sy, 30, 24, ">", KC_NORMAL);
    } else {
      drawKey(2,   sy, 48, 24, "ABC", KC_NORMAL);
      drawKey(52,  sy, 40, 24, "Daku", KC_NORMAL);
      drawKey(94,  sy, 44, 24, "Small", KC_NORMAL);
      drawKey(140, sy, 44, 24, kbMode == 2 ? "Hira" : "Kata", KC_NORMAL);
      drawKey(186, sy, 62, 24, "Conv", imeComp.length() ? KC_ACT : KC_NORMAL);
      drawKey(250, sy, 68, 24, "Del", KC_NORMAL);
    }
    for (int r = 0; r < 5; r++) {
      int y = 88 + r * 28;
      for (int c = 0; c < 11; c++) {
        String cell = KANA_H[r][c];
        if (kbMode == 2) cell = toKata(cell);
        if (!cell.length()) continue;
        drawKey(4 + c * 29, y, 27, 25, cell, KC_NORMAL);
      }
    }
  }
}

static void kbOpen(int target, const String& initial, int maxBytes, Screen ret) {
  kbTarget = target;
  kbReturn = ret;
  kbInput = initial;
  imeComp = "";
  imeCandN = 0;
  kbMaxBytes = maxBytes;
  kbMode = 0; kbUpper = false; kbLayer = 0;
  screen = SC_KB;
  needsRedraw = true;
}
static void kbOk() {
  imeCommit();
  if (kbTarget == 0) {                                   // chat message
    if (kbInput.length() && curPeer >= 0) {
      if (sendChatMsg(curPeer, kbInput)) {
        if (macIsZero(chatPeers[curPeer].mac)) appendGroupLog("Me", kbInput);
        else appendChatLog(curPeer, true, kbInput);
        pushChatLine("Me|" + kbInput);
      }
    }
    screen = SC_CHAT;
  } else {                                               // nickname
    String n = u8cut(kbInput, 15);
    if (n.length()) saveMyName(n);
    screen = SC_SET;
  }
  needsRedraw = true;
}
static void kbCancel() { screen = kbReturn; needsRedraw = true; }

static void kbBackspace() {
  if (imeCandN > 0) { imeCandN = 0; return; }
  if (imeComp.length()) u8chop(imeComp);
  else u8chop(kbInput);
  drawKbInput();
  if (kbMode != 0) needsRedraw = true;   // strip state (Conv highlight) may change
}
static void handleKb(int tx, int ty) {
  if (ty < 24) {
    if (tx < 50) { kbCancel(); return; }
    if (tx >= 262) { kbOk(); return; }
    return;
  }
  if (kbMode == 0) {
    static const char* rows[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const char* srows[3] = { "1234567890", "!@#$%^&*()", "-_=+[];" };
    for (int r = 0; r < 3; r++) {
      int y = 84 + r * 38;
      if (ty < y || ty >= y + 34) continue;
      const char* row = kbLayer ? srows[r] : rows[r];
      int n = strlen(row);
      int x0 = (r == 1) ? 17 : (r == 2 ? 50 : 2);
      if (r == 2 && tx < 48) {                            // aA / abc
        if (kbLayer) kbLayer = 0; else kbUpper = !kbUpper;
        needsRedraw = true; return;
      }
      if (r == 2 && tx >= 272) { kbBackspace(); return; }
      for (int i = 0; i < n; i++) {
        int x = x0 + i * 32;
        if (tx >= x && tx < x + 30) {
          char c = row[i];
          if (!kbLayer && !kbUpper) c = tolower(c);
          kbAppend(String(c));
          drawKbInput();
          return;
        }
      }
      return;
    }
    if (ty >= 198 && ty < 232) {
      if (kbLayer) {
        if (tx < 54) { kbLayer = 0; needsRedraw = true; }
        else if (tx < 108) { kbMode = jpReady ? 1 : 0; needsRedraw = true; }
        else if (tx < 144) { kbAppend("/"); drawKbInput(); }
        else if (tx < 180) { kbAppend("?"); drawKbInput(); }
        else if (tx < 244) { kbAppend(" "); drawKbInput(); }
        else if (tx < 280) { kbAppend("<"); drawKbInput(); }
        else { kbAppend(">"); drawKbInput(); }
      } else {
        if (tx < 54) { kbLayer = 1; needsRedraw = true; }
        else if (tx < 108) { kbMode = jpReady ? 1 : 0; needsRedraw = true; }
        else if (tx < 274) { kbAppend(" "); drawKbInput(); }
        else { kbAppend("."); drawKbInput(); }
      }
    }
    return;
  }
  // ---- kana ----
  if (ty >= 58 && ty < 82) {                             // strip
    if (imeCandN > 0) {
      if (tx < 44) { imeCandN = 0; needsRedraw = true; return; }
      int x = 46;
      int start = imeCandPage * 5;
      for (int i = start; i < imeCandN && i < start + 5; i++) {
        int w = textW(imeCand[i]) + 14;
        if (x + w > 284) break;
        if (tx >= x && tx < x + w) {
          kbInput += imeCand[i];
          imeComp = "";
          imeCandN = 0;
          needsRedraw = true;
          return;
        }
        x += w + 3;
      }
      if (tx >= 288 && imeCandN > 5) {
        imeCandPage = (imeCandPage + 1) % ((imeCandN + 4) / 5);
        needsRedraw = true;
      }
      return;
    }
    if (tx < 50) { kbMode = 0; needsRedraw = true; }
    else if (tx < 92) { if (imeCycleLast(KANA_DAKU, sizeof(KANA_DAKU) / sizeof(KANA_DAKU[0]))) needsRedraw = true; }
    else if (tx < 138) { if (imeCycleLast(KANA_SMALL, sizeof(KANA_SMALL) / sizeof(KANA_SMALL[0]))) needsRedraw = true; }
    else if (tx < 184) { kbMode = (kbMode == 2) ? 1 : 2; needsRedraw = true; }
    else if (tx < 248) { imeConvert(); needsRedraw = true; }
    else kbBackspace();
    return;
  }
  if (ty >= 88) {
    int r = (ty - 88) / 28;
    if (r < 0 || r > 4) return;
    if ((ty - 88) % 28 > 25) return;
    int c = (tx - 4) / 29;
    if (c < 0 || c > 10 || (tx - 4) % 29 > 27) return;
    String cell = KANA_H[r][c];
    if (!cell.length()) return;
    if (kbMode == 2) cell = toKata(cell);
    imeAppend(cell);
    drawKbInput();
  }
}

// ============================================================
//  MUSIC PLAYER (MP3 + WAV from /music, ESP8266Audio -> M5.Speaker)
//  v2 design (after v1 froze the UI and crackled): decoding runs in
//  its own FreeRTOS task, and the output class is the M5Unified
//  example's proven design -- triple buffers handed to the speaker
//  task with release callbacks, and ConsumeSample returns false for
//  backpressure instead of dropping samples or blocking the UI.
// ============================================================
#include <atomic>
class AudioOutputM5Speaker : public AudioOutput {
public:
  AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t virtual_sound_channel = 0) {
    _m5sound = m5sound;
    _virtual_ch = virtual_sound_channel;
  }
  virtual ~AudioOutputM5Speaker(void) {}
  void setup(void) { _m5sound->setBufferReleaseCallback(this, bufferReleased); }
  virtual bool begin(void) override { return true; }
  virtual bool ConsumeSample(int16_t sample[2]) override {
    if (_buffer_index < buf_size) {
      _buffer[_index][_buffer_index]     = sample[0];
      _buffer[_index][_buffer_index + 1] = sample[1];
      _buffer_index += 2;
      return true;
    }
    flush();
    return false;   // backpressure: the decoder retries after the flush
  }
  virtual void flush(void) override {
    if (_buffer_index) {
      _busy[_index] = true;
      bool queued = _m5sound->playRaw(_buffer[_index], _buffer_index, hertz, true, 1, _virtual_ch);
      if (queued) { _frames += _buffer_index / 2; }
      else { _busy[_index] = false; }
      if (++_index >= buf_count) { _index = 0; }
      _buffer_index = 0;
      while (_busy[_index]) { vTaskDelay(1); }   // wait for the speaker task (task context only)
    }
  }
  virtual bool stop(void) override {
    flush();
    for (size_t i = 0; i < buf_count; ++i) { while (_busy[i]) { vTaskDelay(1); } }
    return true;
  }
protected:
  m5::Speaker_Class* _m5sound;
  uint8_t _virtual_ch;
  static constexpr size_t buf_count = 3;
  static constexpr size_t buf_size = 1024;      // 512 stereo frames per buffer
  int16_t _buffer[buf_count][buf_size];
  std::atomic<bool> _busy[buf_count] = { {false}, {false}, {false} };
  size_t _buffer_index = 0;
  size_t _index = 0;
  uint32_t _frames = 0;
  static void bufferReleased(void* args, const void* data, uint8_t) {
    auto me = (AudioOutputM5Speaker*)args;
    for (size_t i = 0; i < buf_count; ++i) { if (data == me->_buffer[i]) { me->_busy[i] = false; } }
  }
};

#define MAX_TRACKS 64
static String musicNames[MAX_TRACKS];
static int musicCount = 0;
static volatile int musicCur = -1;
static int musicTop = 0;
static volatile bool musicPlaying = false, musicPaused = false;
static int musicVol = 200;
static volatile unsigned long musicElapsedBase = 0, musicPlayStarted = 0;
static AudioOutputM5Speaker spkOut(&M5.Speaker, 0);
static AudioGenerator* gen = nullptr;
static AudioFileSourceSD* srcFile = nullptr;
static AudioGeneratorMP3* gMp3 = nullptr;
static AudioGeneratorWAV* gWav = nullptr;
static volatile int reqPlay = -1;       // UI -> decode task: track index to start
static volatile bool reqStop = false;   // UI -> decode task: stop playback

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
// ---- Bluetooth A2DP source output (M5 -> BT speaker/headphones) ----
// The decoder writes into a ring buffer; the A2DP data callback drains
// it, resampling to the fixed 44.1 kHz A2DP rate when the file differs.
#include <BluetoothA2DPSource.h>
static AudioOutput* curOut = nullptr;
class AudioOutputRing : public AudioOutput {
public:
  bool begin() override { _head = _tail = 0; _readPos = 0; return true; }
  bool ConsumeSample(int16_t sample[2]) override {
    int spins = 0;
    while (full() && spins < 300) { vTaskDelay(1); spins++; }   // decode-task context: waiting is fine
    if (full()) return false;
    _buf[_head][0] = sample[0];
    _buf[_head][1] = sample[1];
    _head = (_head + 1) % RING_FRAMES;
    return true;
  }
  bool stop() override { _head = _tail = 0; _readPos = 0; return true; }
  int32_t drain(int16_t* dst, int32_t want) {   // called from the BT stack task
    double step = hertz > 0 ? (double)hertz / 44100.0 : 1.0;
    int32_t produced = 0;
    while (produced < want) {
      uint32_t av = avail();
      uint32_t i0 = (uint32_t)_readPos;
      if (av < i0 + 2) {   // underrun: pad the rest with silence
        for (int32_t i = produced; i < want; i++) { dst[i * 2] = 0; dst[i * 2 + 1] = 0; }
        uint32_t drop = av > i0 ? i0 : av;
        _tail = (_tail + drop) % RING_FRAMES;
        _readPos = 0;
        return want;
      }
      double frac = _readPos - i0;
      uint32_t ia = (_tail + i0) % RING_FRAMES;
      uint32_t ib = (_tail + i0 + 1) % RING_FRAMES;
      for (int ch = 0; ch < 2; ch++) {
        double a = _buf[ia][ch], b = _buf[ib][ch];
        dst[produced * 2 + ch] = (int16_t)(a + (b - a) * frac);
      }
      produced++;
      _readPos += step;
      if (_readPos >= 2048.0) { uint32_t drop = (uint32_t)_readPos; _tail = (_tail + drop) % RING_FRAMES; _readPos -= drop; }
    }
    uint32_t drop = (uint32_t)_readPos;
    _tail = (_tail + drop) % RING_FRAMES;
    _readPos -= drop;
    return want;
  }
private:
  static const uint32_t RING_FRAMES = 8192;
  int16_t _buf[RING_FRAMES][2];
  volatile uint32_t _head = 0, _tail = 0;
  double _readPos = 0;
  bool full() { return ((_head + 1) % RING_FRAMES) == _tail; }
  uint32_t avail() { return (_head + RING_FRAMES - _tail) % RING_FRAMES; }
};
static AudioOutputRing btOut;
static BluetoothA2DPSource a2dp_source;
static bool outBT = false;
static bool btOn = false;
static String btTargetName = "";
static String btSavedName = "";
#define BT_MAX_FOUND 12
static String btFound[BT_MAX_FOUND];
static int btFoundRssi[BT_MAX_FOUND];
static volatile int btFoundCount = 0;
static volatile bool btListDirty = false;
static int32_t btDataCb(uint8_t* data, int32_t len) {
  return btOut.drain((int16_t*)data, len / 4) * 4;
}
static bool btSsidCb(const char* ssid, esp_bd_addr_t address, int rssi) {
  (void)address;
  if (ssid && ssid[0]) {
    bool exists = false;
    for (int i = 0; i < btFoundCount; i++) if (btFound[i] == ssid) { exists = true; btFoundRssi[i] = rssi; break; }
    if (!exists && btFoundCount < BT_MAX_FOUND) {
      btFound[btFoundCount] = ssid;
      btFoundRssi[btFoundCount] = rssi;
      btFoundCount++;
      btListDirty = true;
    }
  }
  return btTargetName.length() > 0 && btTargetName == ssid;   // connect only to the chosen device
}
static void btEnsureStarted() {
  if (btOn) return;
  a2dp_source.set_local_name("ENZ-M5");
  a2dp_source.set_ssid_callback(btSsidCb);
  a2dp_source.set_auto_reconnect(false);
  a2dp_source.start_raw(btDataCb);
  btOn = true;
}
static void btStopStack() {
  if (!btOn) return;
  a2dp_source.end();
  btOn = false;
}
void loadBtPrefs() {
  prefs.begin("music", true);
  outBT = prefs.getBool("outbt", false);
  btSavedName = prefs.getString("btname", "");
  prefs.end();
  btTargetName = btSavedName;
}
void saveBtPrefs() {
  prefs.begin("music", false);
  prefs.putBool("outbt", outBT);
  prefs.putString("btname", btSavedName);
  prefs.end();
}
static void btConnect(const String& name) {
  btTargetName = name;
  btSavedName = name;
  saveBtPrefs();
  btStopStack();
  btEnsureStarted();   // discovery matches the target via btSsidCb and connects
}
static void applyVolume() {
  M5.Speaker.setVolume((uint8_t)musicVol);
  if (btOn && a2dp_source.is_connected()) a2dp_source.set_volume((uint8_t)(musicVol * 127 / 255));
}
static void setOutputBT(bool bt) {
  if (bt == outBT) return;
  outBT = bt;
  saveBtPrefs();
  if (bt) { btTargetName = btSavedName; btEnsureStarted(); }
  else btStopStack();
  if (musicPlaying && musicCur >= 0) musicPlay((int)musicCur);   // restart the track on the new output
  needsRedraw = true;
}
static String btStatusText() {
  if (!outBT) return "Speaker output";
  if (!btOn) return "BT off";
  if (a2dp_source.is_connected()) return "Connected: " + (btTargetName.length() ? btTargetName : String("device"));
  return btTargetName.length() ? ("Searching: " + btTargetName) : String("Searching...");
}

// ---- decode-task-only helpers (never call from the UI loop) ----
static void doStop() {
  if (gen) { if (curOut) curOut->stop(); gen->stop(); }
  curOut = nullptr;
  if (gMp3) { delete gMp3; gMp3 = nullptr; }
  if (gWav) { delete gWav; gWav = nullptr; }
  gen = nullptr;
  if (srcFile) { delete srcFile; srcFile = nullptr; }
  musicPlaying = false;
  musicPaused = false;
  if (screen == SC_MUSIC) needsRedraw = true;
}
static bool doPlay(int idx) {
  if (idx < 0 || idx >= musicCount) return false;
  doStop();
  String path = "/music/" + musicNames[idx];
  srcFile = new AudioFileSourceSD(path.c_str());
  String l = musicNames[idx]; l.toLowerCase();
  bool ok = false;
  curOut = outBT ? (AudioOutput*)&btOut : (AudioOutput*)&spkOut;
  if (l.endsWith(".mp3")) { gMp3 = new AudioGeneratorMP3(); gen = gMp3; ok = gMp3->begin(srcFile, curOut); }
  else                    { gWav = new AudioGeneratorWAV(); gen = gWav; ok = gWav->begin(srcFile, curOut); }
  if (!ok) { doStop(); return false; }
  musicCur = idx;
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
      if (!gen->loop()) {                       // track ended: auto-advance
        int n = (int)musicCur + 1;
        if (n < musicCount) doPlay(n); else doStop();
      }
    } else {
      vTaskDelay(10);
    }
  }
}
// ---- UI-facing controls (just set requests; the task does the work) ----
void musicStop() { reqPlay = -1; reqStop = true; }
bool musicPlay(int idx) {
  if (idx < 0 || idx >= musicCount) return false;
  reqStop = false;
  reqPlay = idx;
  return true;
}
void musicTogglePause() {
  if (!musicPlaying) { if (musicCur >= 0) musicPlay((int)musicCur); return; }
  if (musicPaused) {
    musicPaused = false;
    musicPlayStarted = millis();
  } else {
    musicElapsedBase += (millis() - musicPlayStarted) / 1000;
    musicPaused = true;
  }
  if (screen == SC_MUSIC) needsRedraw = true;
}
void musicNext(bool autoAdvance = false) {
  (void)autoAdvance;                            // end-of-track advance lives in the task
  if (musicCount == 0) return;
  int n = (int)musicCur + 1;
  if (n >= musicCount) n = 0;
  musicPlay(n);
}
void musicPrev() {
  if (musicCount == 0) return;
  int n = (int)musicCur - 1;
  if (n < 0) n = musicCount - 1;
  musicPlay(n);
}
static unsigned long musicElapsedSec() {
  if (!musicPlaying) return 0;
  if (musicPaused) return musicElapsedBase;
  return musicElapsedBase + (millis() - musicPlayStarted) / 1000;
}
static float musicProgress() {
  AudioFileSourceSD* f = srcFile;
  if (!f) return 0;
  uint32_t sz = f->getSize();
  if (!sz) return 0;
  return (float)f->getPos() / (float)sz;
}
void loadMusicVol() {
  prefs.begin("music", true);
  musicVol = prefs.getInt("vol", 200);
  prefs.end();
  if (musicVol < 0) musicVol = 0;
  if (musicVol > 255) musicVol = 255;
}
void saveMusicVol() {
  prefs.begin("music", false); prefs.putInt("vol", musicVol); prefs.end();
}
static String trackTitle(int idx) {
  String n = musicNames[idx];
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  return n;
}

// ============================================================
//  SCREENS
// ============================================================
static const char* WDAY[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static int weekdayOf(int y, int m, int d) {
  static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static int brightnessVal = 200;

static void drawHeader(const char* title, bool back = true) {
  if (back) hbtn(2, 0, 46, 22, "<", KC_NORMAL, UI_TEXT);
  txt(title, back ? 56 : 8, 3, UI_DIM, 1);
}

// ---- clock ----
static int lastClockSec = -1, lastClockMin = -1, lastClockDay = -1;
static String lastChatStatus = "\x01", lastMusicStatus = "\x01";
// ---- 7-segment time (drawn as shapes: no font scaling, so no jaggies) ----
#define SEG_X0 69
#define SEG_Y0 64
#define SEG_SEC_X 258
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
  static const uint8_t SEGMAP[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };  // bits: A,B,C,D,E,F,G
  uint8_t m = SEGMAP[d % 10];
  uint16_t off = Lcd.color565(7, 22, 12);
  const int t = 9;
  auto one = [&](int bit, uint16_t c) {
    switch (bit) {
      case 0: segH(x + 1, y, w - 2, t, c); break;                 // A
      case 1: segV(x + w - t, y + 1, h / 2 - 1, t, c); break;     // B
      case 2: segV(x + w - t, y + h / 2, h / 2 - 1, t, c); break; // C
      case 3: segH(x + 1, y + h - t, w - 2, t, c); break;         // D
      case 4: segV(x, y + h / 2, h / 2 - 1, t, c); break;         // E
      case 5: segV(x, y + 1, h / 2 - 1, t, c); break;             // F
      case 6: segH(x + 1, y + (h - t) / 2, w - 2, t, c); break;   // G
    }
  };
  for (int b = 0; b < 7; b++) one(b, off);                          // ghost '8' underlay first
  for (int b = 0; b < 7; b++) if ((m >> b) & 1) one(b, UI_ACCENT);  // lit segments on top (no ghost band cutting them)
}
static void drawSevenSegTime(int hh, int mm) {
  const int w = 36, h = 68;
  segDigit(SEG_X0, SEG_Y0, w, h, hh / 10);
  segDigit(SEG_X0 + 44, SEG_Y0, w, h, hh % 10);
  Lcd.fillRect(SEG_X0 + 88, SEG_Y0 + 18, 9, 9, UI_ACCENT);
  Lcd.fillRect(SEG_X0 + 88, SEG_Y0 + 44, 9, 9, UI_ACCENT);
  segDigit(SEG_X0 + 106, SEG_Y0, w, h, mm / 10);
  segDigit(SEG_X0 + 150, SEG_Y0, w, h, mm % 10);
}

static void drawClockDateTime() {   // name/battery + date + big time + seconds (minute/day change only)
  m5::rtc_datetime_t dt = M5.Rtc.getDateTime();
  Lcd.fillRect(0, 0, SCR_W, 146, UI_BG);
  char db[40];
  snprintf(db, sizeof(db), "%04d-%02d-%02d %s", dt.date.year, dt.date.month, dt.date.date,
           WDAY[weekdayOf(dt.date.year, dt.date.month, dt.date.date) % 7]);
  Lcd.setFont(&fonts::Font2);
  Lcd.setTextSize(2);
  Lcd.setTextDatum(m5gfx::datum_t::top_center);
  Lcd.setTextColor(UI_TEXT, UI_BG);
  Lcd.drawString(db, SCR_W / 2, 14);
  int bat = M5.Power.getBatteryLevel();
  char bb[16]; snprintf(bb, sizeof(bb), "BAT %d%%", bat);
  Lcd.setTextSize(1);
  Lcd.setTextDatum(m5gfx::datum_t::top_right);
  Lcd.setTextColor(bat <= 20 ? UI_WARN : UI_MUTE, UI_BG);
  Lcd.drawString(bb, SCR_W - 6, 2);
  Lcd.setTextDatum(m5gfx::datum_t::top_left);
  txt(myName.c_str(), 6, 2, UI_MUTE, 1);
  drawSevenSegTime(dt.time.hours, dt.time.minutes);
  char sb[8]; snprintf(sb, sizeof(sb), "%02d", dt.time.seconds);
  Lcd.setFont(&fonts::Font2);
  Lcd.setTextSize(1);
  Lcd.setTextDatum(m5gfx::datum_t::top_left);
  Lcd.setTextColor(UI_DIM, UI_BG);
  Lcd.drawString(sb, SEG_SEC_X, 118);
}
static void drawClockSeconds() {   // seconds patch only (~60x22 px: no visible blink)
  m5::rtc_datetime_t dt = M5.Rtc.getDateTime();
  Lcd.fillRect(SEG_SEC_X - 1, 112, 55, 24, UI_BG);   // starts right of the last digit (ends x=255): never nibbles it
  char sb[8]; snprintf(sb, sizeof(sb), "%02d", dt.time.seconds);
  Lcd.setFont(&fonts::Font2);
  Lcd.setTextSize(1);
  Lcd.setTextDatum(m5gfx::datum_t::top_left);
  Lcd.setTextColor(UI_DIM, UI_BG);
  Lcd.drawString(sb, SEG_SEC_X, 118);
}
static void drawClockStatus() {   // status card contents, repainted only when the text changes
  char st[64];
  snprintf(st, sizeof(st), "CHAT  ch%d%s  Online: %d%s", chatChannel, chatLR ? " [LR]" : "",
           onlinePeerCount(), anyUnread() ? "  [NEW]" : "");
  String ms;
  if (musicPlaying && musicCur >= 0) ms = String(musicPaused ? "| | " : "> ") + fitText(trackTitle((int)musicCur), SCR_W - 40);
  else ms = "MUSIC  stopped";
  if (!clockStatusDirty && String(st) == lastChatStatus && ms == lastMusicStatus) return;
  clockStatusDirty = false;
  lastChatStatus = st;
  lastMusicStatus = ms;
  Lcd.fillRect(9, 149, SCR_W - 18, 42, UI_CARD);
  txt(st, 16, 154, UI_DIM, 1);
  if (musicPlaying && musicCur >= 0) jpText(ms.c_str(), 16, 172, UI_TEXT, -1);
  else txt("MUSIC  stopped", 16, 174, UI_MUTE, 1);
}
static void clockRefresh(bool force) {
  m5::rtc_datetime_t dt = M5.Rtc.getDateTime();
  if (force || dt.date.date != lastClockDay || dt.time.minutes != lastClockMin) {
    drawClockDateTime();
    lastClockDay = dt.date.date;
    lastClockMin = dt.time.minutes;
    lastClockSec = dt.time.seconds;
  } else if (dt.time.seconds != lastClockSec) {
    drawClockSeconds();
    lastClockSec = dt.time.seconds;
  }
  drawClockStatus();
}
static void drawClockScreen() {   // full draw (screen entry / explicit redraws)
  Lcd.fillScreen(UI_BG);
  card(8, 148, SCR_W - 16, 44);
  hbtn(8, 202, 96, 32, "CHAT", KC_ACT, UI_TEXT);
  if (anyUnread()) Lcd.fillCircle(96, 208, 5, UI_WARN);
  hbtn(112, 202, 96, 32, "MUSIC", KC_NORMAL, UI_TEXT);
  hbtn(216, 202, 96, 32, "SET", KC_NORMAL, UI_TEXT);
  lastClockMin = -1; lastClockDay = -1;
  lastChatStatus = "\x01"; lastMusicStatus = "\x01";
  clockStatusDirty = false;
  clockRefresh(true);
}

// ---- peer list ----
static void drawPeers() {
  Lcd.fillScreen(UI_BG);
  drawHeader("COMIC CHAT");
  txt(myName.c_str(), 190, 3, UI_MUTE, 1);
  int y = 30;
  for (int i = 0; i < chatPeerCount && y < 210; i++) {
    bool online = macIsZero(chatPeers[i].mac) || (chatPeers[i].lastSeen && millis() - chatPeers[i].lastSeen < PEER_TIMEOUT_MS);
    card(6, y, SCR_W - 12, 32, i == curPeer ? KC_ACT : UI_CARD);
    txt(macIsZero(chatPeers[i].mac) ? "[ALL]" : (online ? "[ON] " : "[--] "), 14, y + 8, online ? UI_ACCENT : UI_MUTE, 1);
    jpText(fitText(peerName(i), 170).c_str(), 62, y + 8, UI_TEXT, -1);
    if (chatPeers[i].hasUnread) txt("[NEW]", 262, y + 8, UI_WARN, 1);
    y += 38;
  }
  if (chatPeerCount <= 1) txt("No peers yet. HELLO every 5 s...", 10, 76, UI_MUTE, 1);
  txt("Tap a peer to open the conversation.", 10, 222, UI_MUTE, 1);
}

// ---- conversation ----
static void drawChatView() {
  Lcd.fillScreen(UI_BG);
  drawHeader("");
  if (curPeer >= 0) {
    String h = peerName(curPeer);
    bool online = macIsZero(chatPeers[curPeer].mac) || (chatPeers[curPeer].lastSeen && millis() - chatPeers[curPeer].lastSeen < PEER_TIMEOUT_MS);
    h += online ? "  [ON]" : "  [--]";
    jpText(fitText(h, 240).c_str(), 56, 3, UI_TEXT, -1);
  }
  // build display lines (wrapped), show the last ones
  String disp[36]; int dn = 0;
  for (int i = 0; i < chatLineCount; i++) {
    int bar = chatLines[i].indexOf('|');
    String who = bar >= 0 ? chatLines[i].substring(0, bar) : "?";
    String body = bar >= 0 ? chatLines[i].substring(bar + 1) : chatLines[i];
    String wtmp[3]; int wn = wrapText(body, SCR_W - 24 - textW("[" + who + "] "), wtmp, 3);
    for (int k = 0; k < wn && dn < 36; k++) {
      disp[dn++] = (k == 0 ? ("[" + who + "] ") : "    ") + wtmp[k];
    }
  }
  int show = dn < 9 ? dn : 9;
  for (int i = 0; i < show; i++) {
    const String& ln = disp[dn - show + i];
    bool mine = ln.startsWith("[Me]");
    jpText(ln.c_str(), 10, 30 + i * 19, mine ? UI_ACCENT : UI_TEXT, -1, SCR_W - 18);
  }
  hbtn(6, 204, SCR_W - 12, 30, "C:\\> Type message...", KC_NORMAL, UI_DIM);
}

// ---- music ----
static int tracksTop = 0;
static void drawMusicScreen() {
  Lcd.fillScreen(UI_BG);
  drawHeader("MUSIC");
  txt(outBT ? (btOn && a2dp_source.is_connected() ? "BT" : "BT..") : "SPK", 286, 4, UI_DIM, 1);
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
  // volume (big buttons)
  txt("VOL", 10, 157, UI_DIM, 1);
  hbtn(48, 148, 48, 34, "-", KC_NORMAL, UI_TEXT);
  Lcd.drawRect(108, 158, 132, 12, UI_BORDER);
  Lcd.fillRect(109, 159, (int)(130 * musicVol / 255.0f), 10, UI_ACCENT2);
  hbtn(252, 148, 48, 34, "+", KC_NORMAL, UI_TEXT);
  // output toggle + track list
  hbtn(6, 192, 150, 40, outBT ? "OUT: BT" : "OUT: SPK", KC_NORMAL, UI_TEXT);
  hbtn(164, 192, 150, 40, "TRACKS >", KC_NORMAL, UI_TEXT);
}
static void drawTracks() {
  Lcd.fillScreen(UI_BG);
  drawHeader("TRACKS");
  if (musicCount == 0) txt(sdReady ? "No .mp3/.wav in /music" : "No SD card", 10, 44, UI_MUTE, 1);
  for (int r = 0; r < 4; r++) {
    int idx = tracksTop + r;
    if (idx >= musicCount) break;
    int y = 34 + r * 42;
    Lcd.fillRoundRect(6, y, SCR_W - 12, 38, 6, idx == musicCur ? KC_ACT : UI_CARD);
    Lcd.drawRoundRect(6, y, SCR_W - 12, 38, 6, UI_BORDER);
    String line = String(idx == musicCur && musicPlaying ? "> " : "  ") + trackTitle(idx);
    jpText(fitText(line, SCR_W - 44).c_str(), 14, y + 11, idx == musicCur ? UI_ACCENT : UI_TEXT, -1);
  }
  if (tracksTop > 0) hbtn(6, 208, 150, 28, "^ PAGE", KC_NORMAL, UI_TEXT);
  if (tracksTop + 4 < musicCount) hbtn(164, 208, 150, 28, "PAGE v", KC_NORMAL, UI_TEXT);
}
static void drawBt() {
  Lcd.fillScreen(UI_BG);
  drawHeader("Bluetooth");
  const int x = 8, w = SCR_W - 16;
  int y = 30;
  auto row = [&](const char* label, const String& value) {
    card(x, y, w, 34);
    txt(label, x + 10, y + 9, UI_DIM, 1);
    jpText(fitText(value, 190).c_str(), x + 118, y + 9, UI_TEXT, -1);
    y += 40;
  };
  row("Output", outBT ? "Bluetooth" : "Speaker");
  row("Device", btSavedName.length() ? btSavedName : String("(none)"));
  row("Status", btStatusText());
  txt("Tap Device to scan & pick a speaker.", 10, 160, UI_MUTE, 1);
  txt("BT audio and ESP-NOW chat share the", 10, 184, UI_MUTE, 1);
  txt("radio: chat may slow while streaming.", 10, 200, UI_MUTE, 1);
}
static void drawBtScan() {
  Lcd.fillScreen(UI_BG);
  drawHeader("BT Devices");
  txt(btOn ? "Scanning..." : "Starting BT...", 10, 28, UI_DIM, 1);
  for (int i = 0; i < btFoundCount && i < 5; i++) {
    int y = 50 + i * 36;
    Lcd.fillRoundRect(6, y, SCR_W - 12, 32, 6, (btSavedName == btFound[i]) ? KC_ACT : UI_CARD);
    Lcd.drawRoundRect(6, y, SCR_W - 12, 32, 6, UI_BORDER);
    jpText(fitText(btFound[i], 240).c_str(), 14, y + 8, UI_TEXT, -1);
    char rb[12]; snprintf(rb, sizeof(rb), "%d dBm", btFoundRssi[i]);
    txt(rb, 262, y + 8, UI_MUTE, 1);
  }
  if (btFoundCount == 0) txt("No devices yet...", 10, 56, UI_MUTE, 1);
  hbtn(6, 208, 150, 28, "RESCAN", KC_NORMAL, UI_TEXT);
  hbtn(164, 208, 150, 28, "BACK", KC_NORMAL, UI_TEXT);
}

static void updateMusicScreen() {   // 500 ms tick: elapsed + progress patch only (fixes the 2x/s blink)
  if (musicCur < 0) return;
  Lcd.fillRect(12, 56, 106, 18, UI_CARD);
  unsigned long el = musicElapsedSec();
  char eb[24]; snprintf(eb, sizeof(eb), "%s  %lu:%02lu", musicPaused ? "PAUSE" : (musicPlaying ? "PLAY" : "STOP"), el / 60, el % 60);
  txt(eb, 14, 58, UI_DIM, 1);
  Lcd.drawRect(120, 62, 180, 8, UI_BORDER);
  Lcd.fillRect(121, 63, 178, 6, UI_CARD);
  Lcd.fillRect(121, 63, (int)(178 * musicProgress()), 6, UI_ACCENT);
}

// ---- settings ----
static void drawSettings() {
  Lcd.fillScreen(UI_BG);
  drawHeader("Settings");
  const int x = 8, w = SCR_W - 16;
  int y = 26;
  auto row = [&](const char* label, const String& value) {
    card(x, y, w, 30);
    txt(label, x + 10, y + 7, UI_DIM, 1);
    jpText(fitText(value, 190).c_str(), x + 118, y + 7, UI_TEXT, -1);
    y += 35;
  };
  row("Name", myName);
  row("ESP-NOW Ch", String(chatChannel));
  row("ESP-NOW", chatLR ? "LongRange" : "Normal");
  {
    m5::rtc_datetime_t dt = M5.Rtc.getDateTime();
    char cb[32];
    snprintf(cb, sizeof(cb), "%04d-%02d-%02d %02d:%02d", dt.date.year, dt.date.month, dt.date.date, dt.time.hours, dt.time.minutes);
    row("Clock", cb);
  }
  row("Bright", String(brightnessVal));
  row("Bluetooth", outBT ? "Bluetooth" : "Speaker");
}

// ---- time set ----
static m5::rtc_datetime_t tsDt;
static void drawTimeSet() {
  Lcd.fillScreen(UI_BG);
  drawHeader("Set clock", false);
  struct F { const char* lbl; int val; } fs[6] = {
    { "Year", tsDt.date.year }, { "Month", tsDt.date.month }, { "Day", tsDt.date.date },
    { "Hour", tsDt.time.hours }, { "Min", tsDt.time.minutes }, { "Sec", tsDt.time.seconds },
  };
  for (int i = 0; i < 6; i++) {
    int y = 28 + i * 30;
    txt(fs[i].lbl, 14, y + 5, UI_DIM, 1);
    hbtn(90, y, 44, 26, "-", KC_NORMAL, UI_TEXT);
    char vb[10]; snprintf(vb, sizeof(vb), "%d", fs[i].val);
    Lcd.setFont(&fonts::Font2);
    Lcd.setTextSize(1);
    Lcd.setTextDatum(m5gfx::datum_t::middle_center);
    Lcd.setTextColor(UI_TEXT);
    Lcd.drawString(vb, 160, y + 13);
    Lcd.setTextDatum(m5gfx::datum_t::top_left);
    hbtn(186, y, 44, 26, "+", KC_NORMAL, UI_TEXT);
  }
  hbtn(250, 28, 62, 34, "SET", UI_ACCENT2, UI_BG);
  hbtn(250, 68, 62, 34, "BACK", KC_NORMAL, UI_TEXT);
  txt("Weekday is set automatically.", 14, 216, UI_MUTE, 1);
}
static void tsAdjust(int field, int delta) {
  int v[6] = { tsDt.date.year, tsDt.date.month, tsDt.date.date, tsDt.time.hours, tsDt.time.minutes, tsDt.time.seconds };
  static const int lo[6] = { 2024, 1, 1, 0, 0, 0 };
  static const int hi[6] = { 2099, 12, 31, 23, 59, 59 };
  v[field] += delta;
  if (v[field] < lo[field]) v[field] = hi[field];
  if (v[field] > hi[field]) v[field] = lo[field];
  tsDt.date.year = v[0]; tsDt.date.month = v[1]; tsDt.date.date = v[2];
  tsDt.time.hours = v[3]; tsDt.time.minutes = v[4]; tsDt.time.seconds = v[5];
  needsRedraw = true;
}

static void drawCurrent() {
  switch (screen) {
    case SC_CLOCK: drawClockScreen(); break;
    case SC_PEERS: drawPeers(); break;
    case SC_CHAT: drawChatView(); break;
    case SC_MUSIC: drawMusicScreen(); break;
    case SC_TRACKS: drawTracks(); break;
    case SC_BT: drawBt(); break;
    case SC_BTSCAN: drawBtScan(); break;
    case SC_SET: drawSettings(); break;
    case SC_TIMESET: drawTimeSet(); break;
    case SC_KB: drawKb(); break;
  }
}

// ---- touch dispatch ----
static void goHome() { screen = SC_CLOCK; needsRedraw = true; }
static void openPeer(int i) {
  curPeer = i;
  chatPeers[i].hasUnread = false;
  loadChatHistory(i);
  screen = SC_CHAT;
  needsRedraw = true;
}
static void handleTouch(int tx, int ty) {
  switch (screen) {
    case SC_CLOCK:
      if (ty >= 200) {
        if (tx < 108) { screen = SC_PEERS; needsRedraw = true; }
        else if (tx < 212) { screen = SC_MUSIC; needsRedraw = true; }
        else { screen = SC_SET; needsRedraw = true; }
      }
      break;
    case SC_PEERS:
      if (ty < 24 && tx < 50) { goHome(); break; }
      {
        int y = 30;
        for (int i = 0; i < chatPeerCount && y < 210; i++, y += 38)
          if (ty >= y && ty < y + 32 && tx >= 6) { openPeer(i); break; }
      }
      break;
    case SC_CHAT:
      if (ty < 24 && tx < 50) { screen = SC_PEERS; needsRedraw = true; }
      else if (ty >= 202) kbOpen(0, "", 199, SC_CHAT);
      break;
    case SC_MUSIC:
      if (ty < 24 && tx < 50) { goHome(); break; }
      if (ty >= 96 && ty < 140) {
        if (tx < 108) musicPrev();
        else if (tx < 213) musicTogglePause();
        else musicNext();
        needsRedraw = true;
        break;
      }
      if (ty >= 148 && ty < 182) {
        if (tx >= 48 && tx < 96) { musicVol = max(0, musicVol - 16); saveMusicVol(); applyVolume(); needsRedraw = true; }
        else if (tx >= 252 && tx < 300) { musicVol = min(255, musicVol + 16); saveMusicVol(); applyVolume(); needsRedraw = true; }
        break;
      }
      if (ty >= 192 && ty < 232) {
        if (tx < 160) setOutputBT(!outBT);
        else {
          tracksTop = musicCur >= 4 ? (int)musicCur - 3 : 0;
          if (tracksTop + 4 > musicCount) tracksTop = max(0, musicCount - 4);
          screen = SC_TRACKS;
        }
        needsRedraw = true;
        break;
      }
      break;
    case SC_TRACKS:
      if (ty < 24 && tx < 50) { screen = SC_MUSIC; needsRedraw = true; break; }
      if (ty >= 208) {
        if (tx < 160 && tracksTop > 0) { tracksTop = max(0, tracksTop - 4); needsRedraw = true; }
        else if (tx >= 160 && tracksTop + 4 < musicCount) { tracksTop += 4; needsRedraw = true; }
        break;
      }
      if (ty >= 34) {
        int off = ty - 34;
        int r = off / 42;
        if (r >= 0 && r < 4 && (off % 42) < 38) {
          int idx = tracksTop + r;
          if (idx < musicCount) { musicPlay(idx); screen = SC_MUSIC; needsRedraw = true; }
        }
      }
      break;
    case SC_BT:
      if (ty < 24 && tx < 50) { screen = SC_SET; needsRedraw = true; break; }
      {
        int rowIdx = (ty - 30) / 40;
        if (ty >= 30 && rowIdx >= 0 && rowIdx < 3) {
          if (rowIdx == 0) setOutputBT(!outBT);
          else if (rowIdx == 1) {
            if (!outBT) setOutputBT(true); else btEnsureStarted();
            screen = SC_BTSCAN;
          }
          needsRedraw = true;
        }
      }
      break;
    case SC_BTSCAN:
      if (ty < 24 && tx < 50) { screen = SC_BT; needsRedraw = true; break; }
      if (ty >= 208) {
        if (tx < 160) { btFoundCount = 0; btStopStack(); btEnsureStarted(); }
        else screen = SC_BT;
        needsRedraw = true;
        break;
      }
      if (ty >= 50) {
        int i = (ty - 50) / 36;
        if (i >= 0 && i < 5 && i < btFoundCount && ((ty - 50) % 36) < 32) {
          btConnect(btFound[i]);
          screen = SC_BT;
          needsRedraw = true;
        }
      }
      break;
    case SC_SET:
      if (ty < 24 && tx < 50) { goHome(); break; }
      {
        int rowIdx = (ty - 26) / 35;
        if (ty >= 26 && rowIdx >= 0 && rowIdx < 6) {
          if (rowIdx == 0) kbOpen(1, myName, 15, SC_SET);
          else if (rowIdx == 1) { chatChannel = chatChannel % 13 + 1; saveChatPrefs(); esp_wifi_set_channel((uint8_t)chatChannel, WIFI_SECOND_CHAN_NONE); needsRedraw = true; }
          else if (rowIdx == 2) { chatLR = !chatLR; saveChatPrefs(); applyChatProtocol(); needsRedraw = true; }
          else if (rowIdx == 3) { tsDt = M5.Rtc.getDateTime(); screen = SC_TIMESET; needsRedraw = true; }
          else if (rowIdx == 4) {
            brightnessVal += 40; if (brightnessVal > 255) brightnessVal = 40;
            Lcd.setBrightness((uint8_t)brightnessVal);
            prefs.begin("sys", false); prefs.putInt("bright", brightnessVal); prefs.end();
            needsRedraw = true;
          }
          else if (rowIdx == 5) { screen = SC_BT; needsRedraw = true; }
        }
      }
      break;
    case SC_TIMESET:
      if (ty >= 28 && ty < 208) {
        int f = (ty - 28) / 30;
        if (f >= 0 && f < 6) {
          if (tx >= 90 && tx < 134) tsAdjust(f, -1);
          else if (tx >= 186 && tx < 230) tsAdjust(f, +1);
        }
      }
      if (tx >= 250 && ty >= 28 && ty < 62) {
        tsDt.date.weekDay = weekdayOf(tsDt.date.year, tsDt.date.month, tsDt.date.date);
        M5.Rtc.setDateTime(tsDt);
        screen = SC_SET; needsRedraw = true;
      } else if (tx >= 250 && ty >= 68 && ty < 102) {
        screen = SC_SET; needsRedraw = true;
      }
      break;
    case SC_KB:
      handleKb(tx, ty);
      break;
  }
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Lcd.setRotation(1);
  prefs.begin("sys", true);
  brightnessVal = prefs.getInt("bright", 200);
  prefs.end();
  Lcd.setBrightness((uint8_t)brightnessVal);
  loadMusicVol();
  M5.Speaker.begin();
  M5.Speaker.setVolume((uint8_t)musicVol);
  spkOut.setup();

  if (SD.begin(GPIO_NUM_4)) sdReady = true;
  if (sdReady) jpInit();

  loadChatPrefs();
  chatStart();
  ensureGroupPeer();
  sendHello();
  lastHello = millis();

  musicScan();
  xTaskCreatePinnedToCore(musicTask, "enzdec", 4096, nullptr, 2, nullptr, 1);
  loadBtPrefs();
  if (outBT) btEnsureStarted();   // Bluetooth output was on: start the stack and look for the saved device
  screen = SC_CLOCK;
  needsRedraw = true;
}

void loop() {
  M5.update();
  if (vibUntil && millis() > vibUntil) { M5.Power.setVibration(0); vibUntil = 0; }
  processChatRx();
  if (millis() - lastHello > HELLO_INTERVAL_MS) { lastHello = millis(); sendHello(); }

  auto td = M5.Touch.getDetail();
  if (td.wasPressed()) handleTouch(td.x, td.y);

  if (M5.BtnA.wasClicked()) {
    if (screen == SC_KB) kbCancel();
    else if (screen != SC_CLOCK) goHome();
  }
  if (M5.BtnB.wasClicked() && screen != SC_KB) { screen = SC_PEERS; needsRedraw = true; }
  if (M5.BtnC.wasClicked() && screen != SC_KB) { screen = SC_MUSIC; needsRedraw = true; }

  if (screen == SC_CLOCK) clockRefresh(false);   // partial patches only: fixes the 1 s full-screen blink
  if (screen == SC_MUSIC && musicPlaying) {
    static unsigned long lastUi = 0;
    if (millis() - lastUi > 500) { lastUi = millis(); updateMusicScreen(); }   // elapsed + progress patch only
  }
  if (screen == SC_BTSCAN && btListDirty) { btListDirty = false; needsRedraw = true; }
  if (screen == SC_BT) {
    static bool lastBtConn = false;
    bool c = btOn && a2dp_source.is_connected();
    if (c != lastBtConn) { lastBtConn = c; needsRedraw = true; }
  }
  if (needsRedraw) { drawCurrent(); needsRedraw = false; }
  delay(4);
}
