/* LogMyGrow Display firmware 3.1 for the ESP32-2432S028R "Cheap Yellow Display" (2.8" 320x240, resistive touch).
 * Open source (MIT). Works with logmygrow.com or a self-hosted LogMyGrow server.
 *
 * Tabs along the bottom: Home (every space at a glance), Spaces (one space in detail, swipe for the next),
 * To do (the most urgent actions), Controls (switch plugs, when you allow it in the app), and Settings (gear).
 *
 * First start: 1) join the Wi-Fi "LogMyGrow-Display" from your phone (scan the QR on screen) and pick your home Wi-Fi,
 * 2) tap the two crosshairs to calibrate touch, 3) type the 6-letter code the screen shows in LogMyGrow > Equipment >
 * Desk display (or scan its QR). Done: no links to paste.
 *
 * BOOT button (back of the board): tap = next page / wake the screen; hold 2 s = Settings; hold 5 s = calibrate touch;
 * hold 10 s = erase everything (Wi-Fi, pairing, settings) and start over. RST = restart, keeps everything.
 *
 * Everything that talks to the network runs in its own task, so the screen never freezes while it fetches.
 * If Wi-Fi drops or the server stops answering, the screen says so and offers the fix (Wi-Fi setup opens by itself
 * after 10 minutes). Updates install over Wi-Fi from the server (Settings > Firmware), checked every 6 hours.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include "qrcode.h"   // the QR generator that ships inside the ESP32 core (esp_qrcode_*)
#include <Update.h>
#include "mbedtls/sha256.h"

#define FW_VERSION "3.3.0"
#ifdef LMG_ST7789
#define LMG_BOARD "st7789"
#else
#define LMG_BOARD "cyd"
#endif
#define POLL_SECONDS 60
#define DEFAULT_HOST "logmygrow.com"

// CYD pins
#define T_IRQ 36
#define T_MOSI 32
#define T_MISO 39
#define T_CLK 25
#define T_CS 33
#define PIN_BL 21
#define PIN_LDR 34
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17
#define PIN_SPK 26
#define PIN_BOOT 0
#define CH_BL 0
#define CH_R 1
#define CH_G 2
#define CH_B 3
#define CH_SPK 4

XPT2046_Touchscreen ts(T_CS, T_IRQ);
TFT_eSPI tft;
TFT_eSprite bar(&tft);     // header / rows / nav are drawn here first, then pushed: no flicker
Preferences prefs;
SemaphoreHandle_t mtx;

/* ── settings ───────────────────────────────────────────────────────────────────────────────────────── */
struct Settings {
  uint8_t theme = 0;        // 0 dark, 1 light
  uint8_t bri = 4;          // 0 auto, 1..4 fixed; starts at full brightness
  uint8_t dim = 0;          // index into DIM_MIN; never dims until the grower asks
  uint8_t off = 0;          // index into OFF_MIN
  uint8_t cycle = 0;        // index into CYCLE_S
  bool alertWake = true, led = true, beep = false, flip = false, autoUpd = true, vivid = true, idleDial = true;
  int16_t cx0 = 3700, cx1 = 300, cy0 = 3750, cy1 = 350;   // raw touch at screen x=20/300, y=20/220
  bool cal = false;
  String host = DEFAULT_HOST, link = "";
} S;
const uint8_t DIM_MIN[] = { 0, 1, 2, 5, 15 };      // 0 = never
const uint8_t OFF_MIN[] = { 0, 15, 30, 60 };
const uint8_t CYCLE_S[] = { 0, 10, 20, 30 };
const uint8_t BRI_PCT[] = { 0, 20, 45, 75, 100 };

struct Theme { uint16_t bg, surf, edge, text, muted, accent, ideal, ok, risk, danger, none; };
struct Band { float lo = NAN, hi = NAN; };
struct Space {
  String name, stage, tier = "none", tT = "none", tH = "none", tV = "none", mold = "none", act[3];
  float temp = NAN, rh = NAN, vpd = NAN; Band iT, iH, iV, oT, oH, oV, rT, rH; long age = -1; int plants = 0, nAct = 0;
  float moldL[3] = { NAN, NAN, NAN }; String mprof; bool guardOn = false, guardRun = false; int guardLine = 0;
};
struct Device { String id, name, kind, space, garden; int8_t on = -1; };
struct Feed {
  Space sp[8]; int nSp = 0;
  String aSpace[5], aText[5]; int nAct = 0;
  Device dev[12]; int nDev = 0; bool control = false;
  String unit = "F";
};
enum Page { P_HOME, P_SPACE, P_TODO, P_CTRL, P_SET };
enum Scr { S_AWAKE, S_DIM, S_OFF };
struct Row { const char* label; String (*val)(); void (*tap)(); };
struct Toggle { bool pending; String garden, id; bool on; };

void saveSettings() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  prefs.begin("lmg", false);
  prefs.putUChar("theme", S.theme); prefs.putUChar("bri2", S.bri); prefs.putUChar("dim2", S.dim); prefs.putUChar("off", S.off);
  prefs.putUChar("cycle", S.cycle); prefs.putBool("aw", S.alertWake); prefs.putBool("led", S.led); prefs.putBool("beep", S.beep);
  prefs.putBool("flip", S.flip); prefs.putBool("au", S.autoUpd); prefs.putBool("vivid", S.vivid); prefs.putBool("idial", S.idleDial);
  prefs.putShort("cx0", S.cx0); prefs.putShort("cx1", S.cx1); prefs.putShort("cy0", S.cy0); prefs.putShort("cy1", S.cy1);
  prefs.putBool("cal", S.cal); prefs.putString("host", S.host); prefs.putString("link", S.link);
  prefs.end();
  xSemaphoreGive(mtx);
}
void loadSettings() {
  prefs.begin("lmg", true);
  // bri2/dim2: 3.0 stored auto-brightness and a 2-minute dim; 3.1 starts everyone at full brightness, never dimming
  S.theme = prefs.getUChar("theme", 0); S.bri = prefs.getUChar("bri2", 4); S.dim = prefs.getUChar("dim2", 0); S.off = prefs.getUChar("off", 0);
  S.cycle = prefs.getUChar("cycle", 0); S.alertWake = prefs.getBool("aw", true); S.led = prefs.getBool("led", true); S.beep = prefs.getBool("beep", false);
  S.flip = prefs.getBool("flip", false); S.autoUpd = prefs.getBool("au", true); S.vivid = prefs.getBool("vivid", true); S.idleDial = prefs.getBool("idial", true);
  S.cx0 = prefs.getShort("cx0", 3700); S.cx1 = prefs.getShort("cx1", 300); S.cy0 = prefs.getShort("cy0", 3750); S.cy1 = prefs.getShort("cy1", 350);
  S.cal = prefs.getBool("cal", false); S.host = prefs.getString("host", DEFAULT_HOST); S.link = prefs.getString("link", "");
  prefs.end();
  if (S.dim >= sizeof(DIM_MIN)) S.dim = 0; if (S.off >= sizeof(OFF_MIN)) S.off = 0; if (S.cycle >= sizeof(CYCLE_S)) S.cycle = 0; if (S.bri > 4) S.bri = 4;
  if (!S.host.length()) S.host = DEFAULT_HOST;
}

/* ── colours ────────────────────────────────────────────────────────────────────────────────────────── */
constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const Theme DARK  = { C(0, 0, 0), C(24, 28, 26), C(64, 72, 68), C(255, 255, 255), C(178, 188, 183), C(70, 235, 130), C(40, 235, 100), C(255, 210, 0), C(255, 128, 0), C(255, 40, 40), C(120, 124, 122) };
const Theme LIGHT = { C(255, 255, 255), C(236, 240, 237), C(180, 190, 184), C(0, 0, 0), C(70, 80, 75), C(0, 140, 60), C(0, 150, 60), C(176, 120, 0), C(210, 84, 0), C(205, 0, 0), C(120, 124, 122) };
const Theme* T = &DARK;
/* panel gamma: "Vivid" is a steeper curve that fixes the washed-out look of the CYD's ILI9341 viewed off-axis;
   "Standard" is the curve TFT_eSPI's ILI9341_2 driver sets at power-on */
void applyGamma() {
#ifndef LMG_ST7789
  static const uint8_t VP[] = { 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00 };
  static const uint8_t VN[] = { 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F };
  static const uint8_t SP[] = { 0x0F, 0x22, 0x1C, 0x1B, 0x08, 0x0F, 0x48, 0xB8, 0x34, 0x05, 0x0C, 0x09, 0x0F, 0x07, 0x00 };
  static const uint8_t SN[] = { 0x00, 0x23, 0x24, 0x07, 0x10, 0x07, 0x38, 0x47, 0x4B, 0x0A, 0x13, 0x06, 0x30, 0x38, 0x0F };
  const uint8_t* P = S.vivid ? VP : SP; const uint8_t* N = S.vivid ? VN : SN;
  tft.startWrite();
  tft.writecommand(0x26); tft.writedata(0x01);
  tft.writecommand(0xE0); for (int i = 0; i < 15; i++) tft.writedata(P[i]);
  tft.writecommand(0xE1); for (int i = 0; i < 15; i++) tft.writedata(N[i]);
  tft.endWrite();
#endif
}
uint16_t tierCol(const String& t) { return t == "ideal" ? T->ideal : t == "ok" ? T->ok : t == "risk" ? T->risk : t == "danger" ? T->danger : T->none; }
int tierRank(const String& t) { return t == "ideal" ? 0 : t == "ok" ? 1 : t == "risk" ? 2 : t == "danger" ? 3 : -1; }

/* ── the feed ───────────────────────────────────────────────────────────────────────────────────────── */
Feed feed;                       // what the screen draws; only the loop touches it
Feed incoming; volatile bool haveIncoming = false;   // the net task leaves new data here, under the mutex
volatile bool feedNew = false;
volatile unsigned long lastOk = 0;                   // millis of the last good fetch
char netErr[48] = "";                                 // fixed buffers: the two tasks share them without a String freeing under the reader
char pairCode[8] = ""; volatile bool pairing = false, paired = false;
Toggle toggleReq = { false, "", "", false };
char toggleMsg[40] = ""; unsigned long toggleMsgAt = 0;
volatile bool pollNow = true;
volatile unsigned long wifiDownSince = 0, lastTouch = 0;   // 0 = Wi-Fi is up
volatile int failCount = 0;                                // feed fetches failed in a row
volatile int otaPct = -1; volatile bool otaCheckNow = false, otaInstallNow = false;
char otaAvail[16] = "", otaMsg[40] = "";

float fv(JsonVariantConst v) { return v.isNull() ? NAN : v.as<float>(); }
Band band(JsonVariantConst v) { Band b; if (!v.isNull()) { b.lo = fv(v[0]); b.hi = fv(v[1]); } return b; }
String num(float v, int dp) { return isnan(v) ? String("--") : String(v, dp); }

String baseUrl() { return (S.host.startsWith("http") ? S.host : "https://" + S.host); }
void setErr(const String& e) { strlcpy(netErr, e.c_str(), sizeof(netErr)); }
String linkNow() { xSemaphoreTake(mtx, portMAX_DELAY); String l = S.link; xSemaphoreGive(mtx); return l; }
void setLink(const String& l) { xSemaphoreTake(mtx, portMAX_DELAY); S.link = l; xSemaphoreGive(mtx); paired = l.length() >= 20; saveSettings(); }
int httpDo(const String& url, const char* method, const String& body, String& out) {
  HTTPClient http; http.setTimeout(12000); http.setReuse(false);
  WiFiClientSecure sc; WiFiClient pc; bool ok;
  if (url.startsWith("https")) { sc.setInsecure(); ok = http.begin(sc, url); } else ok = http.begin(pc, url);  // the token in the URL is the secret
  if (!ok) return -1;
  http.addHeader("Content-Type", "application/json");
  int code = strcmp(method, "POST") == 0 ? http.POST(body) : http.GET();
  if (code > 0) out = http.getString();
  http.end();
  return code;
}

bool parseFeed(const String& body, Feed& f) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  f.unit = (const char*)(doc["unit"] | "F"); f.control = doc["control"] | false;
  f.nSp = 0;
  for (JsonObjectConst s : doc["spaces"].as<JsonArrayConst>()) {
    if (f.nSp >= 8) break;
    Space& x = f.sp[f.nSp++];
    x.name = (const char*)(s["name"] | "Space"); x.stage = (const char*)(s["stage"] | "");
    x.tier = (const char*)(s["tier"] | "none"); x.mold = (const char*)(s["mold"] | "none");
    x.tT = (const char*)(s["tiers"]["temp"] | "none"); x.tH = (const char*)(s["tiers"]["rh"] | "none"); x.tV = (const char*)(s["tiers"]["vpd"] | "none");
    x.temp = fv(s["temp"]); x.rh = fv(s["rh"]); x.vpd = fv(s["vpd"]); x.plants = s["plants"] | 0;
    x.age = s["ageMin"].isNull() ? -1 : s["ageMin"].as<long>();
    x.iT = band(s["ideal"]["temp"]); x.iH = band(s["ideal"]["rh"]); x.iV = band(s["ideal"]["vpd"]);
    x.oT = band(s["ok"]["temp"]); x.oH = band(s["ok"]["rh"]); x.oV = band(s["ok"]["vpd"]);
    x.rT = band(s["risk"]["temp"]); x.rH = band(s["risk"]["rh"]);
    for (int k = 0; k < 3; k++) x.moldL[k] = fv(s["moldLines"][k]);
    x.mprof = (const char*)(s["moldProfile"] | "");
    x.guardOn = !s["guard"].isNull(); x.guardRun = s["guard"]["running"] | false; x.guardLine = s["guard"]["line"] | 0;
    x.nAct = 0; for (JsonVariantConst a : s["actions"].as<JsonArrayConst>()) if (x.nAct < 3) x.act[x.nAct++] = (const char*)(a | "");
  }
  f.nAct = 0;
  for (JsonObjectConst a : doc["actions"].as<JsonArrayConst>()) { if (f.nAct >= 5) break; f.aSpace[f.nAct] = (const char*)(a["space"] | ""); f.aText[f.nAct++] = (const char*)(a["text"] | ""); }
  f.nDev = 0;
  for (JsonObjectConst d : doc["devices"].as<JsonArrayConst>()) {
    if (f.nDev >= 12) break;
    Device& v = f.dev[f.nDev++];
    v.id = (const char*)(d["id"] | ""); v.name = (const char*)(d["name"] | "Plug"); v.kind = (const char*)(d["kind"] | "other");
    v.space = (const char*)(d["space"] | ""); v.garden = (const char*)(d["gardenId"] | "");
    v.on = d["on"].isNull() ? -1 : (d["on"].as<bool>() ? 1 : 0);
  }
  return true;
}

/* ── over-the-air updates: the server publishes /firmware/display-<board>.json { version, url, sha256, size } ─── */
int verCmp(const char* a, const char* b) {
  int A[3] = { 0, 0, 0 }, B[3] = { 0, 0, 0 };
  sscanf(a, "%d.%d.%d", &A[0], &A[1], &A[2]); sscanf(b, "%d.%d.%d", &B[0], &B[1], &B[2]);
  for (int i = 0; i < 3; i++) if (A[i] != B[i]) return A[i] < B[i] ? -1 : 1;
  return 0;
}
String otaUrl, otaSha; size_t otaSize = 0;
bool otaCheck() {
  String out; int c = httpDo(baseUrl() + "/firmware/display-" LMG_BOARD ".json", "GET", "", out);
  JsonDocument d;
  if (c != 200 || deserializeJson(d, out) || !d["version"].is<const char*>()) { strlcpy(otaMsg, "No update info yet", sizeof(otaMsg)); return false; }
  const char* v = d["version"];
  otaUrl = (const char*)(d["url"] | ""); otaSha = (const char*)(d["sha256"] | ""); otaSize = d["size"] | 0;
  if (verCmp(v, FW_VERSION) > 0 && otaUrl.length() && otaSha.length() == 64) { strlcpy(otaAvail, v, sizeof(otaAvail)); snprintf(otaMsg, sizeof(otaMsg), "Install %s", v); }
  else { otaAvail[0] = 0; strlcpy(otaMsg, "Up to date", sizeof(otaMsg)); }
  return true;
}
bool otaInstall() {
  String url = otaUrl.startsWith("http") ? otaUrl : baseUrl() + otaUrl;
  HTTPClient http; http.setTimeout(20000);
  WiFiClientSecure sc; WiFiClient pc; bool ok;
  if (url.startsWith("https")) { sc.setInsecure(); ok = http.begin(sc, url); } else ok = http.begin(pc, url);
  if (!ok) { strlcpy(otaMsg, "Update: bad address", sizeof(otaMsg)); return false; }
  int c = http.GET(); int len = http.getSize();
  if (c != 200 || len <= 0 || (otaSize && (size_t)len != otaSize)) { http.end(); snprintf(otaMsg, sizeof(otaMsg), "Update failed (%d)", c); return false; }
  if (!Update.begin(len)) { http.end(); strlcpy(otaMsg, "Update: no room", sizeof(otaMsg)); return false; }
  otaPct = 0;
  WiFiClient* st = http.getStreamPtr();
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts_ret(&ctx, 0);
  static uint8_t buf[1024]; int got = 0; unsigned long t0 = millis(), lastData = millis();
  while (got < len && millis() - t0 < 180000 && millis() - lastData < 20000) {
    size_t av = st->available();
    if (!av) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    int n = st->readBytes(buf, av < sizeof(buf) ? av : sizeof(buf));
    if (n <= 0) continue;
    lastData = millis();
    mbedtls_sha256_update_ret(&ctx, buf, n);
    if (Update.write(buf, n) != (size_t)n) break;
    got += n; otaPct = (int)((long)got * 100 / len);
  }
  uint8_t h[32]; mbedtls_sha256_finish_ret(&ctx, h); mbedtls_sha256_free(&ctx); http.end();
  char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);
  if (got != len || !otaSha.equalsIgnoreCase(hex)) { Update.abort(); otaPct = -1; strlcpy(otaMsg, "Update didn't verify, kept old", sizeof(otaMsg)); return false; }
  if (!Update.end(true)) { otaPct = -1; strlcpy(otaMsg, "Update didn't finish", sizeof(otaMsg)); return false; }
  otaPct = 100; vTaskDelay(pdMS_TO_TICKS(800)); ESP.restart();
  return true;
}

/* ── network task (core 0): Wi-Fi stays up, pairing, the feed every minute, plug switches, updates ─────── */
void netTask(void*) {
  unsigned long lastPoll = 0, lastPairPoll = 0, pairExp = 0, lastOta = 0;
  String pairSecret;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (!wifiDownSince) wifiDownSince = millis();
      setErr("Wi-Fi down"); feedNew = true;
      static unsigned long lastKick = 0;
      if (millis() - lastKick > 20000) { lastKick = millis(); WiFi.reconnect(); }
      vTaskDelay(pdMS_TO_TICKS(1000)); continue;
    }
    if (wifiDownSince) { wifiDownSince = 0; pollNow = true; feedNew = true; }
    // updates: 30 s after boot, every 6 h, or when asked from Settings
    if (otaCheckNow || (!lastOta && millis() > 30000) || (lastOta && millis() - lastOta > 6 * 3600000UL)) {
      otaCheckNow = false; lastOta = millis(); otaCheck(); feedNew = true;
    }
    if (otaAvail[0] && (otaInstallNow || (S.autoUpd && millis() - lastTouch > 120000))) { otaInstallNow = false; otaInstall(); feedNew = true; }
    String link = linkNow();
    // pairing: no link yet, so ask for a code and wait for the grower to type it in the app
    if (link.length() < 20) {
      pairing = true;
      if (!pairCode[0] || millis() > pairExp) {
        String out; int c = httpDo(baseUrl() + "/display-pair/start", "POST", "{}", out);
        JsonDocument d;
        if (c == 200 && !deserializeJson(d, out) && d["code"].is<const char*>()) { strlcpy(pairCode, d["code"] | "", sizeof(pairCode)); pairSecret = (const char*)(d["secret"] | ""); pairExp = millis() + ((d["expiresIn"] | 600) - 20) * 1000UL; setErr(""); feedNew = true; }
        else {   // 200 with a web page, or 404: the server is older than this firmware (no pairing route yet)
          setErr(c == 200 || c == 404 ? String("Server needs an update. Retrying...") : c < 0 ? String("No internet. Check Wi-Fi. Retrying...") : "Server error " + String(c) + ". Retrying...");
          feedNew = true; vTaskDelay(pdMS_TO_TICKS(10000)); continue;
        }
      }
      if (millis() - lastPairPoll > 3000) {
        lastPairPoll = millis();
        String out; int c = httpDo(baseUrl() + "/display-pair/poll/" + String(pairCode) + "/" + pairSecret, "GET", "", out);
        JsonDocument d;
        if (c == 200 && !deserializeJson(d, out) && d["link"].is<const char*>()) { setLink((const char*)d["link"]); pairCode[0] = 0; pairing = false; failCount = 0; pollNow = true; feedNew = true; }
        else if (c == 404) { pairCode[0] = 0; }   // expired: get a fresh one
      }
      vTaskDelay(pdMS_TO_TICKS(250)); continue;
    }
    pairing = false;
    // a plug switch from the Controls page
    xSemaphoreTake(mtx, portMAX_DELAY); Toggle tg = toggleReq; toggleReq.pending = false; xSemaphoreGive(mtx);
    if (tg.pending) {
      String out, body = "{\"gardenId\":\"" + tg.garden + "\",\"deviceId\":\"" + tg.id + "\",\"on\":" + (tg.on ? "true" : "false") + "}";
      int c = httpDo(link + "/device", "POST", body, out);
      String m = c == 200 ? String(tg.on ? "Switched on" : "Switched off") : c == 403 ? String("Control is off in the app") : "Switch failed (" + String(c) + ")";
      strlcpy(toggleMsg, m.c_str(), sizeof(toggleMsg)); toggleMsgAt = millis(); pollNow = true; feedNew = true;
      vTaskDelay(pdMS_TO_TICKS(1500));      // give Kasa / Home Assistant a moment before reading the state back
    }
    if (pollNow || millis() - lastPoll > (failCount ? 20000UL : POLL_SECONDS * 1000UL)) {   // retry faster while failing
      pollNow = false; lastPoll = millis();
      String out; int c = httpDo(link + (link.indexOf('?') >= 0 ? "&fw=" : "?fw=") + FW_VERSION, "GET", "", out);
      if (c == 200) {
        Feed* f = new Feed();
        if (parseFeed(out, *f)) { xSemaphoreTake(mtx, portMAX_DELAY); incoming = *f; haveIncoming = true; xSemaphoreGive(mtx); lastOk = millis(); setErr(""); failCount = 0; }
        else { setErr("The server sent something unreadable"); failCount++; }
        delete f;
      } else if (c == 403) { setErr("unpaired"); setLink(""); failCount = 0; }   // the grower made a new link: pair again
      else { setErr(c < 0 ? String("No answer from ") + S.host : "Server error " + String(c)); failCount++; }
      feedNew = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

/* ── backlight, LED, beeper ─────────────────────────────────────────────────────────────────────────── */
uint8_t blNow = 255; float ldr = 1000; Scr scr = S_AWAKE;
void backlight() {
  unsigned long idle = (millis() - lastTouch) / 60000UL;
  int pct;
  if (S.bri == 0) {                               // auto: the light sensor on the front reads lower in brighter light
    ldr = ldr * 0.9 + analogRead(PIN_LDR) * 0.1;
    pct = constrain(map((long)ldr, 0, 1200, 100, 30), 30, 100);
  } else pct = BRI_PCT[S.bri];
  // the screen's state is decided here and nowhere else; touch reads it, it never guesses from the brightness
  if (OFF_MIN[S.off] && idle >= OFF_MIN[S.off]) scr = S_OFF;
  else if (DIM_MIN[S.dim] && idle >= DIM_MIN[S.dim]) scr = S_DIM;
  else scr = S_AWAKE;
  if (scr == S_OFF) pct = 0; else if (scr == S_DIM) pct = min(pct, 10);
  uint8_t want = pct * 255 / 100;
  if (want != blNow) { blNow = blNow + (want > blNow ? min(12, want - blNow) : -min(12, blNow - want)); ledcWrite(CH_BL, blNow); }   // fade
}
void led(uint8_t r, uint8_t g, uint8_t b) { ledcWrite(CH_R, 255 - r); ledcWrite(CH_G, 255 - g); ledcWrite(CH_B, 255 - b); }   // active low
int worst() { int w = -1; for (int i = 0; i < feed.nSp; i++) w = max(w, tierRank(feed.sp[i].tier)); return w; }
void statusLed() {
  if (!S.led) { led(0, 0, 0); return; }
  bool blink = (millis() / 500) % 2;
  int w = worst();
  if (wifiDownSince || failCount >= 3) led(0, 0, blink ? 40 : 0);                                // offline: slow blue
  else if (w == 3) led(blink ? 120 : 0, 0, 0);                                                      // danger: red blink
  else if (w == 2) led(((millis() / 1500) % 2) ? 90 : 20, 30, 0);                                   // risk: amber pulse
  else led(0, 0, 0);                                                                                // fine: dark, it sits by a tent at night
}
void beep(int hz, int ms) { if (!S.beep) return; ledcWriteTone(CH_SPK, hz); delay(ms); ledcWriteTone(CH_SPK, 0); }

/* ── drawing ────────────────────────────────────────────────────────────────────────────────────────── */
Page page = P_HOME; int spaceIdx = 0, homeOff = 0, setPage = 0, trouble = 0;
const int HDR = 30, NAV_Y = 206, NAV_H = 34;
const GFXfont* F_XL = &FreeSansBold24pt7b;
const GFXfont* F_L = &FreeSansBold18pt7b;
const GFXfont* F_M = &FreeSansBold12pt7b;
const GFXfont* F_S = &FreeSansBold9pt7b;
void txt(TFT_eSPI& g, const String& s, int x, int y, uint8_t datum, const GFXfont* f, uint16_t col) {
  g.setFreeFont(f); g.setTextDatum(datum); g.setTextColor(col); g.drawString(s, x, y); g.setTextFont(1);
}
String tierWord(const String& t) { return t == "ideal" ? "IDEAL" : t == "ok" ? "OK" : t == "risk" ? "RISK" : t == "danger" ? "DANGER" : "NO DATA"; }
String fmtT(float v) { return num(v, feed.unit == "F" ? 0 : 1); }

void wifiIcon(TFT_eSPI& g, int x, int y) {
  int r = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -200;
  int bars = r > -60 ? 4 : r > -70 ? 3 : r > -80 ? 2 : r > -90 ? 1 : 0;
  for (int i = 0; i < 4; i++) { int h = 3 + i * 3; g.fillRect(x + i * 4, y + 12 - h, 3, h, i < bars ? T->text : T->edge); }
}
String ago(unsigned long ms) { unsigned long s = ms / 1000; return s < 60 ? String(s) + "s" : s < 3600 ? String(s / 60) + "m" : String(s / 3600) + "h"; }
void header(const String& title) {
  bar.createSprite(320, HDR); bar.fillSprite(T->bg);
  txt(bar, title, 10, HDR / 2 + 1, ML_DATUM, F_M, T->text);
  bar.setTextDatum(MR_DATUM); bar.setTextColor(T->muted, T->bg);
  String st = netErr[0] ? (lastOk ? "stale " + ago(millis() - lastOk) : String("offline")) : lastOk ? ago(millis() - lastOk) + " ago" : String("");
  if (otaAvail[0] && !S.autoUpd) st = "update ready";
  bar.drawString(st, 292, HDR / 2, 2);
  wifiIcon(bar, 298, 8);
  bar.drawFastHLine(0, HDR - 1, 320, T->edge);
  bar.pushSprite(0, 0); bar.deleteSprite();
}
int navSlots() { return (feed.control ? 4 : 3) + 1; }   // tabs + the gear, all the same width
void nav() {
  const char* L[] = { "Home", "Spaces", "To do", "Plugs" };
  int n = navSlots() - 1, w = 320 / navSlots();
  bar.createSprite(320, NAV_H); bar.fillSprite(T->surf); bar.drawFastHLine(0, 0, 320, T->edge);
  for (int i = 0; i <= n; i++) {
    bool on = i < n ? (int)page == i : page == P_SET;
    int x = i * w;
    if (on) bar.fillRoundRect(x + 3, 4, w - 6, NAV_H - 8, 8, T->accent);
    else if (i) bar.drawFastVLine(x, 8, NAV_H - 16, T->edge);
    uint16_t c = on ? T->bg : T->text;
    if (i < n) {
      txt(bar, L[i], x + w / 2, NAV_H / 2 + 1, MC_DATUM, F_S, c);
      if (i == 2 && feed.nAct && !on) bar.fillCircle(x + w - 9, 9, 4, T->risk);
    } else {
      int gx = x + w / 2, gy = NAV_H / 2;
      for (int a = 0; a < 8; a++) { float t = a * PI / 4; bar.fillCircle(gx + cos(t) * 8, gy + sin(t) * 8, 2, c); }
      bar.fillCircle(gx, gy, 7, c); bar.fillCircle(gx, gy, 3, on ? T->accent : T->surf);
    }
  }
  bar.pushSprite(0, NAV_Y); bar.deleteSprite();
}
void clearBody() { tft.fillRect(0, HDR, 320, NAV_Y - HDR, T->bg); }

/* one big number in its own tile: label, value in its tier colour, the ideal range, the tier word */
void tile(int x, int y, int w, int h, const char* label, const String& val, const char* unit, const String& tier, Band ideal, int dp) {
  uint16_t c = tierCol(tier);
  tft.fillRoundRect(x, y, w, h, 10, T->surf);
  tft.fillRoundRect(x, y, w, 6, 3, c);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(T->muted, T->surf); tft.drawString(label, x + 8, y + 10, 2);
  tft.setTextDatum(TR_DATUM); tft.drawString(unit, x + w - 8, y + 10, 2);
  txt(tft, val, x + w / 2, y + h / 2 + 6, MC_DATUM, F_XL, c);
  tft.setTextDatum(BL_DATUM); tft.setTextColor(c, T->surf); tft.drawString(tierWord(tier), x + 8, y + h - 5, 2);
  if (!isnan(ideal.lo)) { tft.setTextDatum(BR_DATUM); tft.setTextColor(T->muted, T->surf); tft.drawString(num(ideal.lo, dp) + "-" + num(ideal.hi, dp), x + w - 8, y + h - 6, 1); }
}
String ageText(long m) { return m < 0 ? String("no reading yet") : m < 2 ? String("reading just now") : m < 120 ? "reading " + String(m) + " min ago" : "reading " + String(m / 60) + " h ago"; }
/* one space, as big as the screen allows: the Home page when there is one space, and every Spaces page */
void hero(Space& s, bool showPos) {
  int y = HDR + 4, h = 104;
  tile(4, y, 102, h, "TEMP", fmtT(s.temp), feed.unit == "F" ? "F" : "C", s.tT, s.iT, feed.unit == "F" ? 0 : 1);
  tile(109, y, 102, h, "RH", num(s.rh, 0), "%", s.tH, s.iH, 0);
  tile(214, y, 102, h, "VPD", num(s.vpd, 2), "kPa", s.tV, s.iV, 2);
  int ay = y + h + 6;
  tft.setTextDatum(TL_DATUM); tft.setTextColor(T->muted, T->bg);
  String sub = (s.stage.length() ? s.stage : String("Room")) + "  -  " + String(s.plants) + (s.plants == 1 ? " plant" : " plants") + "  -  " + ageText(s.age);
  if (s.mold != "none") sub = "Mold risk " + s.mold + "  -  " + ageText(s.age);
  if (s.guardOn) sub = String(s.guardRun ? "Mold guard running" : "Mold guard on") + " (" + String(s.guardLine) + "%)  -  " + ageText(s.age);
  tft.setTextColor(s.mold == "severe" ? T->danger : s.mold == "high" ? T->risk : T->muted, T->bg); tft.drawString(sub, 8, ay, 2);
  ay += 20;
  if (!s.nAct) txt(tft, s.tier == "none" ? "No reading to grade yet" : "On target. Nothing to do.", 8, ay + 2, TL_DATUM, F_S, s.tier == "none" ? T->muted : T->ideal);
  for (int k = 0; k < s.nAct && k < 2; k++) {
    tft.fillCircle(13, ay + 9 + k * 24, 5, k == 0 ? tierCol(s.tier) : T->muted);
    txt(tft, s.act[k].substring(0, 32), 24, ay + 1 + k * 24, TL_DATUM, F_S, T->text);
  }
  if (showPos && feed.nSp > 1) { tft.setTextDatum(BR_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString(String(spaceIdx + 1) + " / " + String(feed.nSp) + "   swipe >", 314, NAV_Y - 2, 1); }
}

void drawHome() {
  header(feed.nSp == 1 ? feed.sp[0].name.substring(0, 18) : feed.nSp ? String(feed.nSp) + " spaces" : String("LogMyGrow"));
  clearBody();
  if (!feed.nSp) { txt(tft, lastOk ? "No spaces yet" : "Loading...", 160, 105, MC_DATUM, F_M, T->muted); if (lastOk) { tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("Add a space in the app and it shows up here.", 160, 135, 2); } nav(); return; }
  if (feed.nSp == 1) { hero(feed.sp[0], false); nav(); return; }
  bool tall = feed.nSp == 2;
  int rh = tall ? 86 : 43, per = tall ? 2 : 4, rows = min(per, feed.nSp - homeOff);
  for (int i = 0; i < rows; i++) {
    Space& s = feed.sp[homeOff + i];
    bar.createSprite(320, rh); bar.fillSprite(T->bg);
    bar.fillRoundRect(4, 2, 312, rh - 4, 10, T->surf);
    bar.fillRoundRect(4, 2, 7, rh - 4, 3, tierCol(s.tier));
    if (tall) {
      txt(bar, s.name.substring(0, 11), 18, 10, TL_DATUM, F_M, T->text);
      bar.setTextDatum(TL_DATUM); bar.setTextColor(s.mold == "severe" || s.mold == "high" ? T->risk : T->muted, T->surf);
      bar.drawString(s.mold != "none" ? "mold " + s.mold : (s.stage.length() ? s.stage : String("room")), 18, 38, 2);
      bar.setTextColor(tierCol(s.tier), T->surf); bar.drawString(tierWord(s.tier), 18, 58, 2);
      txt(bar, fmtT(s.temp), 172, 34, MC_DATUM, F_L, tierCol(s.tT));
      txt(bar, num(s.rh, 0), 232, 34, MC_DATUM, F_L, tierCol(s.tH));
      txt(bar, num(s.vpd, 1), 290, 34, MC_DATUM, F_L, tierCol(s.tV));
      bar.setTextDatum(MC_DATUM); bar.setTextColor(T->muted, T->surf);
      bar.drawString(feed.unit == "F" ? "TEMP F" : "TEMP C", 172, 66, 1); bar.drawString("RH %", 232, 66, 1); bar.drawString("VPD", 290, 66, 1);
    } else {
      txt(bar, s.name.substring(0, 12), 18, 6, TL_DATUM, F_S, T->text);
      bar.setTextDatum(TL_DATUM); bar.setTextColor(s.mold == "severe" || s.mold == "high" ? T->risk : T->muted, T->surf);
      bar.drawString(s.mold != "none" ? "mold " + s.mold : (s.stage.length() ? s.stage : String("room")), 18, 26, 1);
      txt(bar, fmtT(s.temp), 172, 20, MC_DATUM, F_M, tierCol(s.tT));
      txt(bar, num(s.rh, 0) + "%", 234, 20, MC_DATUM, F_M, tierCol(s.tH));
      txt(bar, num(s.vpd, 1), 292, 20, MC_DATUM, F_M, tierCol(s.tV));
    }
    bar.pushSprite(0, HDR + 2 + i * rh); bar.deleteSprite();
  }
  if (feed.nSp > per) { tft.setTextDatum(BC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString(String(homeOff + 1) + "-" + String(homeOff + rows) + " of " + String(feed.nSp) + "  swipe up for more", 160, NAV_Y - 1, 1); }
  nav();
}
void drawSpace() {
  if (!feed.nSp) { page = P_HOME; drawHome(); return; }
  spaceIdx = constrain(spaceIdx, 0, feed.nSp - 1);
  header(feed.sp[spaceIdx].name.substring(0, 18));
  clearBody();
  hero(feed.sp[spaceIdx], true);
  nav();
}
void drawTodo() {
  header("To do");
  clearBody();
  if (!feed.nAct) { txt(tft, "All on target", 160, 100, MC_DATUM, F_L, T->ideal); tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("Nothing needs you right now.", 160, 138, 2); nav(); return; }
  for (int i = 0; i < feed.nAct; i++) {
    bar.createSprite(320, 34); bar.fillSprite(T->bg); bar.fillRoundRect(4, 1, 312, 32, 8, T->surf);
    bar.fillRoundRect(4, 1, 6, 32, 2, i == 0 ? T->danger : i < 3 ? T->risk : T->ok);
    bar.setTextDatum(TL_DATUM); bar.setTextColor(T->muted, T->surf); bar.drawString(feed.aSpace[i].substring(0, 30), 18, 3, 1);
    txt(bar, feed.aText[i].substring(0, 34), 18, 13, TL_DATUM, F_S, T->text);
    bar.pushSprite(0, HDR + 2 + i * 34); bar.deleteSprite();
  }
  nav();
}
const char* kindIcon(const String& k) { return k == "light" ? "LIGHT" : k == "fan" || k == "exhaust" ? "FAN" : k == "heater" ? "HEAT" : k == "humidifier" ? "HUMID" : k == "dehumidifier" ? "DEHUM" : k == "pump" ? "PUMP" : k == "ac" ? "A/C" : "PLUG"; }
void drawCtrl() {
  header("Controls");
  clearBody();
  if (!feed.control) { tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("Turn on \"Let the display switch my plugs\"", 160, 100, 2); tft.drawString("in LogMyGrow > Equipment > Desk display.", 160, 122, 2); nav(); return; }
  if (!feed.nDev) { txt(tft, "No plugs yet", 160, 100, MC_DATUM, F_M, T->muted); tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("Add plugs to a space in the app.", 160, 130, 2); nav(); return; }
  for (int i = 0; i < min(feed.nDev, 6); i++) {
    Device& d = feed.dev[i]; int x = 4 + (i % 2) * 158, y = HDR + 4 + (i / 2) * 57;
    uint16_t bg = d.on == 1 ? T->accent : T->surf, fg = d.on == 1 ? T->bg : T->text;
    tft.fillRoundRect(x, y, 154, 53, 10, bg); tft.drawRoundRect(x, y, 154, 53, 10, T->edge);
    txt(tft, d.name.substring(0, 12), x + 10, y + 8, TL_DATUM, F_S, fg);
    tft.setTextDatum(BL_DATUM); tft.setTextColor(d.on == 1 ? T->bg : T->muted, bg); tft.drawString(String(kindIcon(d.kind)) + " " + d.space.substring(0, 10), x + 10, y + 47, 1);
    txt(tft, d.on == 1 ? "ON" : d.on == 0 ? "OFF" : "?", x + 146, y + 40, BR_DATUM, F_M, fg);
  }
  if (toggleMsg[0] && millis() - toggleMsgAt < 4000) { tft.setTextDatum(BC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString(toggleMsg, 160, NAV_Y - 1, 1); }
  nav();
}

/* ── THE CLIMATE DIAL (the website's crossed-needle dial, 2026-10-14) ─────────────────────────────────────────────
   Same geometry as src/components/ClimateDial.jsx, scaled to the 2.8" screen: the temperature needle from the left
   pivot, humidity from the right, and where they cross is the tent. Green ideal box, yellow ok outline, red where the
   air harms the plant or grows mold for the stage. Shown as the idle screen; any touch returns to the pages. */
const float DAX = 50, DAY = 258, DBX = 350, DBY = 258, DR = 258, DT0 = 10, DT1 = 35, DH0 = 20, DH1 = 100, DA0 = 8, DA1 = 48, DB0 = 172, DB1 = 128;
const float DK = 0.78, DOX = 4, DOY = 6;
float dAngT(float c) { c = constrain(c, DT0, DT1); return DA0 + (c - DT0) / (DT1 - DT0) * (DA1 - DA0); }
float dAngH(float h) { h = constrain(h, DH0, DH1); return DB0 + (h - DH0) / (DH1 - DH0) * (DB1 - DB0); }
void dCross(float tC, float rh, int& x, int& y) {
  float a = dAngT(tC) * DEG_TO_RAD, b = dAngH(rh) * DEG_TO_RAD;
  float dax = cos(a), day = -sin(a), dbx = cos(b), dby = -sin(b), den = dax * dby - day * dbx;
  float s = ((DBX - DAX) * dby - (DBY - DAY) * dbx) / den;
  x = (int)((DAX + s * dax) * DK + DOX); y = (int)((DAY + s * day) * DK + DOY);
}
void dQuad(float t0, float t1, float h0, float h1, uint16_t fill, bool outline, uint16_t line) {
  int x[4], y[4];
  dCross(t0, h0, x[0], y[0]); dCross(t0, h1, x[1], y[1]); dCross(t1, h1, x[2], y[2]); dCross(t1, h0, x[3], y[3]);
  if (fill != 0x0001) { tft.fillTriangle(x[0], y[0], x[1], y[1], x[2], y[2], fill); tft.fillTriangle(x[0], y[0], x[2], y[2], x[3], y[3], fill); }
  if (outline) for (int i = 0; i < 4; i++) tft.drawLine(x[i], y[i], x[(i + 1) % 4], y[(i + 1) % 4], line);
}
uint16_t dim(uint16_t c, float f) { uint8_t r = (c >> 11) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3; return C(r * f, g * f, b * f); }
float toC(float v) { return feed.unit == "F" ? (v - 32) * 5 / 9 : v; }
int dialIdx = 0; bool dialMode = false; unsigned long dialSince = 0;
void drawDial() {
  if (!feed.nSp) return;
  dialIdx %= feed.nSp; Space& s = feed.sp[dialIdx];
  tft.fillScreen(T->bg);
  float lo = DT0, hi = DT1;
  dQuad(DT0, DT1, DH0, DH1, T->surf, true, T->edge);                                       // every possible crossing
  if (!isnan(s.rT.lo)) { dQuad(DT0, DT1, DH0, DH1, dim(T->danger, 0.33), false, 0);         // harm everywhere...
    dQuad(max(lo, toC(s.rT.lo)), min(hi, toC(s.rT.hi)), max((float)DH0, s.rH.lo), min((float)DH1, s.rH.hi), T->surf, false, 0); }   // ...except inside the risk band
  if (!isnan(s.moldL[0])) {
    float mt0 = 13, mt1 = 29;
    if (s.mprof == "veg") { mt0 = 16; mt1 = 32; } else if (s.mprof == "young" || s.mprof == "room") { mt0 = 10; mt1 = 35; } else if (s.mprof == "cure") mt0 = 10;
    dQuad(mt0, mt1, s.moldL[0], s.moldL[1], dim(T->risk, 0.45), false, 0);                    // watch band
    dQuad(mt0, mt1, s.moldL[1], DH1, dim(T->danger, 0.6), true, T->danger);                   // high and above
  }
  if (!isnan(s.oT.lo)) dQuad(toC(s.oT.lo), toC(s.oT.hi), s.oH.lo, s.oH.hi, 0x0001, true, T->ok);
  if (!isnan(s.iT.lo)) dQuad(toC(s.iT.lo), toC(s.iT.hi), s.iH.lo, s.iH.hi, dim(T->ideal, 0.45), true, T->ideal);
  // the scale tracks, ticked every 5, numbered every 10
  tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted);
  bool F = feed.unit == "F";
  for (int v = F ? 50 : 10; v <= (F ? 95 : 35); v += F ? 5 : 2) {
    float c = F ? (v - 32) * 5.0 / 9 : v, a = dAngT(c) * DEG_TO_RAD; bool major = F ? v % 10 == 0 : v % 4 == 0 || v == 35;
    int x1 = (DAX + (DR + 1) * cos(a)) * DK + DOX, y1 = (DAY - (DR + 1) * sin(a)) * DK + DOY, x2 = (DAX + (DR + (major ? 11 : 6)) * cos(a)) * DK + DOX, y2 = (DAY - (DR + (major ? 11 : 6)) * sin(a)) * DK + DOY;
    tft.drawLine(x1, y1, x2, y2, T->text);
    if (major) tft.drawString(String(v), (DAX + (DR + 20) * cos(a)) * DK + DOX, (DAY - (DR + 20) * sin(a)) * DK + DOY, 1);
  }
  for (int v = 20; v <= 100; v += 5) {
    float a = dAngH(v) * DEG_TO_RAD; bool major = v % 10 == 0;
    int x1 = (DBX + (DR + 1) * cos(a)) * DK + DOX, y1 = (DBY - (DR + 1) * sin(a)) * DK + DOY, x2 = (DBX + (DR + (major ? 11 : 6)) * cos(a)) * DK + DOX, y2 = (DBY - (DR + (major ? 11 : 6)) * sin(a)) * DK + DOY;
    tft.drawLine(x1, y1, x2, y2, T->text);
    if (major) tft.drawString(String(v), (DBX + (DR + 20) * cos(a)) * DK + DOX, (DBY - (DR + 20) * sin(a)) * DK + DOY, 1);
  }
  // needles and the crossing
  uint16_t tc = tierCol(s.tier);
  if (!isnan(s.temp) && !isnan(s.rh)) {
    float tC = toC(s.temp), a = dAngT(tC) * DEG_TO_RAD, b = dAngH(s.rh) * DEG_TO_RAD;
    int ax = DAX * DK + DOX, ay = DAY * DK + DOY, bx = DBX * DK + DOX, by = DBY * DK + DOY;
    tft.drawWideLine(ax, ay, ax + (DR - 4) * DK * cos(a), ay - (DR - 4) * DK * sin(a), 2.2, T->text, T->bg);
    tft.drawWideLine(bx, by, bx + (DR - 4) * DK * cos(b), by - (DR - 4) * DK * sin(b), 2.2, T->text, T->bg);
    int px, py; dCross(tC, s.rh, px, py);
    tft.fillCircle(px, py, 9, dim(tc, 0.4)); tft.drawCircle(px, py, 9, tc); tft.fillCircle(px, py, 5, tc); tft.fillCircle(px, py, 1, TFT_WHITE);
  }
  for (int i = 0; i < 2; i++) { int x = (i ? DBX : DAX) * DK + DOX, y = (i ? DBY : DAY) * DK + DOY; tft.fillCircle(x, y, 7, T->text); tft.fillCircle(x, y, 2, C(192, 38, 63)); }
  // the words: which space, its numbers in their tier colours, and the guard
  txt(tft, s.name.substring(0, 16), 6, 4, TL_DATUM, F_S, T->text);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString(s.stage.length() ? s.stage : String("room"), 6, 24, 1);
  if (s.guardOn) { uint16_t gc = s.guardRun ? T->accent : T->edge; tft.fillRoundRect(226, 4, 90, 18, 9, gc); tft.setTextDatum(MC_DATUM); tft.setTextColor(s.guardRun ? T->bg : T->text, gc); tft.drawString(s.guardRun ? "GUARD RUNNING" : "MOLD GUARD", 271, 13, 1); }
  int y = 214;
  txt(tft, fmtT(s.temp) + (F ? "F" : "C"), 80, y, MC_DATUM, F_M, tierCol(s.tT));
  txt(tft, num(s.rh, 0) + "%", 160, y, MC_DATUM, F_M, tierCol(s.tH));
  txt(tft, num(s.vpd, 2), 240, y, MC_DATUM, F_M, tierCol(s.tV));
  tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("kPa", 240, y + 15, 1);
  if (feed.nSp > 1) { tft.setTextDatum(BR_DATUM); tft.drawString(String(dialIdx + 1) + "/" + String(feed.nSp), 316, 238, 1); }
}

/* settings: two pages of rows; tap a row to change it */
String onOff(bool b) { return b ? "On" : "Off"; }
void reqPortal(); void calibrate(); void factoryReset(); void applyTheme();
Row ROWS[] = {
  { "Brightness", [] { const char* v[] = { "Auto", "Low", "Medium", "High", "Full" }; return String(v[S.bri]); }, [] { S.bri = (S.bri + 4) % 5; } },
  { "Dim after", [] { return DIM_MIN[S.dim] ? String(DIM_MIN[S.dim]) + " min" : String("Never"); }, [] { S.dim = (S.dim + 1) % sizeof(DIM_MIN); } },
  { "Screen off after", [] { return OFF_MIN[S.off] ? String(OFF_MIN[S.off]) + " min" : String("Never"); }, [] { S.off = (S.off + 1) % sizeof(OFF_MIN); } },
  { "Contrast", [] { return String(S.vivid ? "Vivid" : "Standard"); }, [] { S.vivid = !S.vivid; applyGamma(); } },
  { "Theme", [] { return String(S.theme ? "Light" : "Dark"); }, [] { S.theme ^= 1; applyTheme(); } },
  { "Idle screen", [] { return String(S.idleDial ? "Climate dial" : "Pages"); }, [] { S.idleDial = !S.idleDial; } },
  { "Wake on alert", [] { return onOff(S.alertWake); }, [] { S.alertWake = !S.alertWake; } },
  { "Cycle pages", [] { return CYCLE_S[S.cycle] ? "every " + String(CYCLE_S[S.cycle]) + " s" : String("Off"); }, [] { S.cycle = (S.cycle + 1) % sizeof(CYCLE_S); } },
  { "Status light", [] { return onOff(S.led); }, [] { S.led = !S.led; } },
  { "Alert beep", [] { return onOff(S.beep); }, [] { S.beep = !S.beep; beep(2200, 60); } },
  { "Firmware " FW_VERSION, [] { return otaPct >= 0 ? String("installing ") + String(otaPct) + "%" : String(otaMsg[0] ? otaMsg : "Check"); }, [] { if (otaAvail[0]) otaInstallNow = true; else { otaCheckNow = true; strlcpy(otaMsg, "Checking...", sizeof(otaMsg)); } } },
  { "Update by itself", [] { return onOff(S.autoUpd); }, [] { S.autoUpd = !S.autoUpd; } },
  { "Turn screen 180", [] { return onOff(S.flip); }, [] { S.flip = !S.flip; tft.setRotation(S.flip ? 3 : 1); ts.setRotation(S.flip ? 3 : 1); } },
  { "Calibrate touch", [] { return String(">"); }, [] { calibrate(); } },
  { "Pair again", [] { return String(">"); }, [] { setLink(""); pollNow = true; } },
  { "Wi-Fi and server", [] { return String(">"); }, [] { reqPortal(); } },
  { "Network", [] { return WiFi.localIP().toString(); }, [] {} },
};
const int NROWS = sizeof(ROWS) / sizeof(ROWS[0]), PER = 5, RH = 34, SPAGES = (NROWS + PER - 1) / PER;
void setHeader() {
  bar.createSprite(320, HDR); bar.fillSprite(T->bg);
  txt(bar, "Settings", 10, HDR / 2 + 1, ML_DATUM, F_M, T->text);
  bar.setTextDatum(MC_DATUM); bar.setTextColor(T->muted, T->bg); bar.drawString(String(setPage + 1) + " / " + String(SPAGES), 196, HDR / 2, 2);
  // big page buttons in the header, clear of the tab bar: 44 px wide each
  bar.fillRoundRect(228, 2, 42, HDR - 4, 6, setPage > 0 ? T->surf : T->bg); bar.fillRoundRect(274, 2, 44, HDR - 4, 6, setPage < SPAGES - 1 ? T->surf : T->bg);
  uint16_t a = setPage > 0 ? T->text : T->edge, b = setPage < SPAGES - 1 ? T->text : T->edge;
  bar.fillTriangle(243, HDR / 2, 255, HDR / 2 - 7, 255, HDR / 2 + 7, a);
  bar.fillTriangle(302, HDR / 2, 290, HDR / 2 - 7, 290, HDR / 2 + 7, b);
  bar.drawFastHLine(0, HDR - 1, 320, T->edge);
  bar.pushSprite(0, 0); bar.deleteSprite();
}
void setRow(int i, bool pressed) {
  int r = setPage * PER + i; if (r >= NROWS) return;
  int y = HDR + 2 + i * RH;
  uint16_t bg = pressed ? T->accent : T->surf;
  bar.createSprite(320, RH); bar.fillSprite(T->bg);
  bar.fillRoundRect(4, 2, 312, RH - 4, 8, bg);
  txt(bar, ROWS[r].label, 14, RH / 2, ML_DATUM, F_S, pressed ? T->bg : T->text);
  txt(bar, ROWS[r].val(), 306, RH / 2, MR_DATUM, &FreeSans9pt7b, pressed ? T->bg : T->accent);
  bar.pushSprite(0, y); bar.deleteSprite();
}
void drawSet() {
  setHeader();
  clearBody();
  for (int i = 0; i < PER; i++) setRow(i, false);
  nav();
}
/* when something is wrong, say so and offer the fix, instead of quietly showing old numbers */
void drawTrouble() {
  tft.fillScreen(T->bg);
  bool wifi = trouble == 1;
  txt(tft, wifi ? "Wi-Fi lost" : "Can't reach LogMyGrow", 12, 14, TL_DATUM, F_M, T->risk);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(T->text, T->bg);
  if (wifi) {
    tft.drawString("Trying to reconnect to " + WiFi.SSID() + ".", 12, 54, 2);
    unsigned long left = wifiDownSince ? (600000UL - min(600000UL, millis() - wifiDownSince)) / 60000UL : 10;
    tft.setTextColor(T->muted, T->bg); tft.drawString("Wi-Fi setup opens by itself in " + String(left + 1) + " min.", 12, 76, 2);
  } else {
    tft.drawString(String(netErr), 12, 54, 2);
    tft.setTextColor(T->muted, T->bg); tft.drawString("Retrying every 20 s. " + String(lastOk ? "Last good data " + ago(millis() - lastOk) + " ago." : ""), 12, 76, 2);
  }
  const char* L[] = { "Retry", wifi ? "Wi-Fi setup" : "Pair again", "Wi-Fi setup" };
  int n = wifi ? 2 : 3, w = (312 - (n - 1) * 8) / n;
  for (int i = 0; i < n; i++) { int x = 4 + i * (w + 8); tft.fillRoundRect(x, 130, w, 56, 10, i == 0 ? T->accent : T->surf); txt(tft, L[i], x + w / 2, 158, MC_DATUM, F_S, i == 0 ? T->bg : T->text); }
  tft.setTextDatum(BL_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("This goes back to your spaces as soon as it works again.", 8, 232, 1);
}
void drawOta() {
  static int shown = -2; if (otaPct == shown) return; shown = otaPct;
  if (otaPct <= 0) { tft.fillScreen(T->bg); txt(tft, "Updating", 160, 80, MC_DATUM, F_L, T->text); tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString(String("Installing ") + otaAvail + ". Keep it plugged in.", 160, 118, 2); }
  tft.fillRoundRect(30, 150, 260, 18, 9, T->edge);
  tft.fillRoundRect(30, 150, max(18, 260 * max(0, (int)otaPct) / 100), 18, 9, T->accent);
}

/* QR codes (Wi-Fi join, pairing) with the generator built into the ESP32 core */
int qrX, qrY, qrPx;
void qrDraw(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q); int px = max(1, qrPx / (n + 4)); int off = (qrPx - px * n) / 2;
  tft.fillRect(qrX, qrY, qrPx, qrPx, TFT_WHITE);
  for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) if (esp_qrcode_get_module(q, x, y)) tft.fillRect(qrX + off + x * px, qrY + off + y * px, px, px, TFT_BLACK);
}
void qr(const String& text, int x, int y, int size) {
  qrX = x; qrY = y; qrPx = size;
  esp_qrcode_config_t cfg; cfg.display_func = qrDraw; cfg.max_qrcode_version = 8; cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&cfg, text.c_str());
}
void drawPair() {
  tft.fillScreen(T->bg);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(T->text, T->bg); tft.drawString("Pair this display", 12, 10, 4);
  tft.setTextColor(T->muted, T->bg);
  tft.drawString("In LogMyGrow open Equipment >", 12, 46, 2);
  tft.drawString("Desk display and type this code,", 12, 64, 2);
  tft.drawString("or scan the QR with your phone.", 12, 82, 2);
  if (pairCode[0]) {
    tft.fillRoundRect(12, 112, 186, 56, 10, T->surf);
    tft.setTextDatum(MC_DATUM); tft.setTextColor(T->accent, T->surf);
    String pc = String(pairCode);
    tft.drawString(pc.substring(0, 3) + " " + pc.substring(3), 105, 141, 4);
    qr(baseUrl() + "/#/equipment?pair=" + pc, 212, 46, 100);
    tft.setTextDatum(TL_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("The code changes every 10 minutes.", 12, 180, 1);
  } else { tft.setTextDatum(TL_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString(netErr[0] ? String(netErr) : String("Getting a code..."), 12, 124, 2); }
  tft.drawString("v" FW_VERSION "  " + S.host + "   hold BOOT 2 s for settings", 12, 222, 1);
}
void draw() {
  if (trouble) { drawTrouble(); return; }
  if ((pairing || !paired) && page != P_SET) { drawPair(); return; }
  switch (page) { case P_HOME: drawHome(); break; case P_SPACE: drawSpace(); break; case P_TODO: drawTodo(); break; case P_CTRL: drawCtrl(); break; case P_SET: drawSet(); break; }
}
void applyTheme() { T = S.theme ? &LIGHT : &DARK; }

/* ── touch ──────────────────────────────────────────────────────────────────────────────────────────── */
bool readTouch(int& x, int& y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  x = constrain(map(p.x, S.cx0, S.cx1, 20, 300), 0, 319);
  y = constrain(map(p.y, S.cy0, S.cy1, 20, 220), 0, 239);
  return true;
}
TS_Point rawWait() {      // one firm tap, raw coordinates, averaged
  while (ts.touched()) delay(10);
  while (!ts.touched()) delay(10);
  long sx = 0, sy = 0; int n = 0; unsigned long t0 = millis();
  while (ts.touched() && millis() - t0 < 600) { TS_Point p = ts.getPoint(); if (p.z > 400) { sx += p.x; sy += p.y; n++; } delay(15); }
  while (ts.touched()) delay(10);
  TS_Point r; r.x = n ? sx / n : 0; r.y = n ? sy / n : 0; r.z = n; return r;
}
void cross(int x, int y, uint16_t c) { tft.drawFastHLine(x - 12, y, 25, c); tft.drawFastVLine(x, y - 12, 25, c); tft.drawCircle(x, y, 6, c); }
void calibrate() {
  ledcWrite(CH_BL, 255);
  tft.fillScreen(T->bg); tft.setTextDatum(MC_DATUM); tft.setTextColor(T->text, T->bg);
  tft.drawString("Touch calibration", 160, 100, 4); tft.setTextColor(T->muted, T->bg); tft.drawString("Tap the centre of each cross, firmly", 160, 134, 2);
  cross(20, 20, T->accent); TS_Point a = rawWait(); cross(20, 20, T->bg);
  cross(300, 220, T->accent); TS_Point b = rawWait();
  if (a.z && b.z && abs(a.x - b.x) > 1000 && abs(a.y - b.y) > 1000) { S.cx0 = a.x; S.cy0 = a.y; S.cx1 = b.x; S.cy1 = b.y; S.cal = true; saveSettings(); }
  tft.fillScreen(T->bg); tft.drawString(S.cal ? "Saved" : "Didn't catch that, using defaults", 160, 120, 2); delay(700);
  lastTouch = millis();
}

/* ── Wi-Fi setup portal ─────────────────────────────────────────────────────────────────────────────── */
void portalScreen() {
  tft.fillScreen(T->bg); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(T->text, T->bg); tft.drawString("Connect to Wi-Fi", 12, 10, 4);
  tft.setTextColor(T->muted, T->bg);
  tft.drawString("1. Scan the QR, or join the", 12, 50, 2);
  tft.setTextColor(T->accent, T->bg); tft.drawString("LogMyGrow-Display", 12, 70, 2);
  tft.setTextColor(T->muted, T->bg); tft.drawString("   Wi-Fi from your phone", 12, 90, 2);
  tft.drawString("2. A page opens (or go to", 12, 116, 2);
  tft.drawString("   192.168.4.1). Pick your Wi-Fi.", 12, 136, 2);
  tft.drawString("3. Save. This screen restarts.", 12, 162, 2);
  qr("WIFI:T:nopass;S:LogMyGrow-Display;;", 212, 50, 100);
  tft.drawString("v" FW_VERSION, 12, 222, 1);
}
bool portal(bool force) {
  WiFiManager wm;
  WiFiManagerParameter pHost("host", "Server (leave as logmygrow.com unless you self-host)", S.host.c_str(), 80);
  WiFiManagerParameter pLink("link", "Display link (optional; pairing with a code is easier)", "", 160);
  wm.addParameter(&pHost); wm.addParameter(&pLink);
  wm.setTitle("LogMyGrow Display"); wm.setConfigPortalTimeout(600); wm.setConnectTimeout(20);
  wm.setAPCallback([](WiFiManager*) { portalScreen(); });
  bool saved = false; wm.setSaveParamsCallback([&]() { saved = true; }); wm.setSaveConfigCallback([&]() { saved = true; });
  bool ok = force ? wm.startConfigPortal("LogMyGrow-Display") : wm.autoConnect("LogMyGrow-Display");
  String h = String(pHost.getValue()); h.trim(); String l = String(pLink.getValue()); l.trim();
  if (h.length()) S.host = h;
  if (l.length() >= 20) S.link = l;
  if (saved) saveSettings();
  return ok;
}
volatile bool portalWanted = false;
void reqPortal() { portalWanted = true; }
void factoryReset() {
  tft.fillScreen(T->bg); tft.setTextDatum(MC_DATUM); tft.setTextColor(T->danger, T->bg); tft.drawString("Erasing...", 160, 120, 4);
  prefs.begin("lmg", false); prefs.clear(); prefs.end();
  WiFiManager wm; wm.resetSettings();
  delay(800); ESP.restart();
}

/* ── main ───────────────────────────────────────────────────────────────────────────────────────────── */
void goPage(Page p) { page = p; homeOff = 0; draw(); }
void nextPage() {
  if (page == P_HOME) { page = P_SPACE; spaceIdx = 0; }
  else if (page == P_SPACE && spaceIdx < feed.nSp - 1) spaceIdx++;
  else if (page == P_SPACE) page = P_TODO;
  else if (page == P_TODO && feed.control) page = P_CTRL;
  else page = P_HOME;
  draw();
}
void onTroubleTap(int x, int y) {
  if (y < 125 || y > 195) return;
  bool wifi = trouble == 1; int n = wifi ? 2 : 3, w = (312 - (n - 1) * 8) / n, i = (x - 4) / (w + 8);
  if (i == 0) { pollNow = true; WiFi.reconnect(); tft.fillRoundRect(4, 130, w, 56, 10, T->edge); txt(tft, "Retrying...", 4 + w / 2, 158, MC_DATUM, F_S, T->text); }
  else if (i == 1 && !wifi) { setLink(""); pollNow = true; trouble = 0; draw(); }
  else reqPortal();
}
void onTap(int x, int y) {
  Serial.printf("tap %d,%d page %d\n", x, y, (int)page);
  if (trouble) { onTroubleTap(x, y); return; }
  if (y >= NAV_Y - 4) {                        // the tab bar (a few px of slack above it: fingers land low)
    int n = navSlots() - 1, i = constrain(x / (320 / navSlots()), 0, n);
    if (i == n) { setPage = 0; goPage(P_SET); } else goPage((Page)i);
    return;
  }
  if (page == P_SET) {
    if (y < HDR + 4) {                          // the page buttons in the header
      if (x >= 272 && setPage < SPAGES - 1) { setPage++; drawSet(); }
      else if (x >= 224 && x < 272 && setPage > 0) { setPage--; drawSet(); }
      return;
    }
    int i = (y - HDR - 2) / RH, r = setPage * PER + i;
    if (i >= 0 && i < PER && r < NROWS) { setRow(i, true); delay(90); ROWS[r].tap(); saveSettings(); draw(); }
    return;
  }
  if (page == P_CTRL && feed.control) {
    int col = x < 160 ? 0 : 1, row = (y - HDR - 4) / 57, i = row * 2 + col;
    if (y >= HDR + 4 && row >= 0 && row < 3 && i < feed.nDev) {
      Device& d = feed.dev[i];
      xSemaphoreTake(mtx, portMAX_DELAY); toggleReq.pending = true; toggleReq.garden = d.garden; toggleReq.id = d.id; toggleReq.on = d.on != 1; xSemaphoreGive(mtx);
      d.on = d.on != 1 ? 1 : 0;            // show it at once; the next feed confirms
      strlcpy(toggleMsg, "Sending...", sizeof(toggleMsg)); toggleMsgAt = millis(); beep(1800, 25); draw();
    }
    return;
  }
  if (page == P_HOME && feed.nSp > 1 && y >= HDR + 2) { int rh = feed.nSp == 2 ? 86 : 43, i = homeOff + (y - HDR - 2) / rh; if (i < feed.nSp) { spaceIdx = i; page = P_SPACE; draw(); } return; }
  if (page == P_HOME && feed.nSp == 1) { spaceIdx = 0; page = P_SPACE; draw(); return; }
  if (page == P_SPACE && feed.nSp > 1) { if (x < 100 && spaceIdx > 0) spaceIdx--; else if (x >= 100) spaceIdx = (spaceIdx + 1) % feed.nSp; draw(); return; }
}
void onSwipe(int dx, int dy) {
  Serial.printf("swipe %d,%d page %d\n", dx, dy, (int)page);
  if (trouble) return;
  int per = feed.nSp == 2 ? 2 : 4;
  if (abs(dy) > abs(dx)) { if (page == P_HOME && feed.nSp > per) { homeOff = constrain(homeOff + (dy < 0 ? per : -per), 0, max(0, feed.nSp - per)); draw(); } return; }
  if (page == P_SPACE) { spaceIdx = constrain(spaceIdx + (dx < 0 ? 1 : -1), 0, max(0, feed.nSp - 1)); draw(); }
  else if (page == P_SET) { setPage = constrain(setPage + (dx < 0 ? 1 : -1), 0, SPAGES - 1); draw(); }
  else if (dx < 0) nextPage();
  else { page = page == P_HOME ? (feed.control ? P_CTRL : P_TODO) : (Page)((int)page - 1); draw(); }
}

void setup() {
  Serial.begin(115200);
  mtx = xSemaphoreCreateMutex();
  loadSettings(); applyTheme();
  tft.init(); applyGamma(); tft.setRotation(S.flip ? 3 : 1); tft.fillScreen(T->bg);
  ledcSetup(CH_BL, 5000, 8); ledcAttachPin(PIN_BL, CH_BL); ledcWrite(CH_BL, 255);
  ledcSetup(CH_R, 5000, 8); ledcAttachPin(PIN_LED_R, CH_R); ledcSetup(CH_G, 5000, 8); ledcAttachPin(PIN_LED_G, CH_G); ledcSetup(CH_B, 5000, 8); ledcAttachPin(PIN_LED_B, CH_B); led(0, 0, 0);
  ledcSetup(CH_SPK, 2000, 8); ledcAttachPin(PIN_SPK, CH_SPK);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  analogSetPinAttenuation(PIN_LDR, ADC_0db);
  SPI.begin(T_CLK, T_MISO, T_MOSI, T_CS); ts.begin(); ts.setRotation(S.flip ? 3 : 1);
  txt(tft, "LogMyGrow", 160, 100, MC_DATUM, F_L, T->accent);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(T->muted, T->bg); tft.drawString("Display " FW_VERSION "  -  joining Wi-Fi", 160, 140, 2);
  WiFi.setAutoReconnect(true);
  if (!portal(false)) ESP.restart();          // no Wi-Fi saved, or it timed out: start over
  paired = S.link.length() >= 20;
  if (!S.cal) calibrate();
  lastTouch = millis();
  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 0);
  draw();
}

void loop() {
  static int lastWorst = -1; static unsigned long lastDraw = 0, lastCycle = 0, bootDown = 0; static bool wasPairing = false;
  if (otaPct >= 0) { ledcWrite(CH_BL, 255); drawOta(); delay(50); return; }      // an update is installing
  // BOOT button: tap = next / wake, 2 s = settings, 5 s = calibrate, 10 s = erase
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!bootDown) bootDown = millis();
    unsigned long held = millis() - bootDown;
    if (held > 2000) { ledcWrite(CH_BL, 255); tft.fillRect(0, 0, 320, 18, T->danger); tft.setTextColor(TFT_WHITE, T->danger); tft.setTextDatum(ML_DATUM);
      tft.drawString(held > 10000 ? "Release to erase everything" : held > 5000 ? "Release: calibrate touch  (keep holding: erase)" : "Release: settings  (keep holding: calibrate)", 6, 9, 2); }
  } else if (bootDown) {
    unsigned long held = millis() - bootDown; bootDown = 0;
    bool wasAsleep = scr != S_AWAKE || dialMode; dialMode = false; lastTouch = millis();
    if (held > 10000) factoryReset();
    else if (held > 5000) { calibrate(); draw(); }
    else if (held > 2000) { trouble = 0; goPage(P_SET); }
    else if (!wasAsleep) nextPage();
    else draw();
  }
  // the idle screen: the climate dial after two minutes untouched, one space every 20 seconds
  if (S.idleDial && !dialMode && !trouble && !pairing && paired && feed.nSp && scr != S_OFF && millis() - lastTouch > 120000UL) { dialMode = true; dialIdx = spaceIdx; dialSince = millis(); drawDial(); }
  if (dialMode && feed.nSp > 1 && millis() - dialSince > 20000UL) { dialSince = millis(); dialIdx = (dialIdx + 1) % feed.nSp; drawDial(); }
  // touch: tap or swipe; the first touch on a dimmed or dark screen only wakes it
  int x, y;
  if (readTouch(x, y)) {
    bool wasAsleep = scr != S_AWAKE || dialMode;
    if (dialMode) { dialMode = false; }
    int sx = x, sy = y, ex = x, ey = y;
    while (readTouch(x, y)) { ex = x; ey = y; delay(12); }
    lastTouch = millis(); scr = S_AWAKE;
    if (wasAsleep) draw();
    else if ((pairing || !paired) && !trouble && page != P_SET) { /* the pairing screen has nothing to tap */ }
    else { int dx = ex - sx, dy = ey - sy; if (abs(dx) > 60 || abs(dy) > 50) onSwipe(dx, dy); else onTap(sx, sy); lastCycle = millis(); }
  }
  if (portalWanted) { portalWanted = false; tft.fillScreen(T->bg); portal(true); ESP.restart(); }
  // Wi-Fi gone for 10 minutes: open the setup portal by itself (it gives up after 5 minutes and tries again)
  if (wifiDownSince && millis() - wifiDownSince > 600000UL) { wifiDownSince = millis(); tft.fillScreen(T->bg); portal(true); ESP.restart(); }
  // trouble screens: Wi-Fi lost for a minute, or three failed fetches in a row
  int tr = (wifiDownSince && millis() - wifiDownSince > 60000UL) ? 1 : (paired && !pairing && failCount >= 3) ? 2 : 0;
  if (tr != trouble) { trouble = tr; dialMode = false; lastTouch = millis(); draw(); }
  // new data from the network task
  if (feedNew) {
    feedNew = false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (haveIncoming) { feed = incoming; haveIncoming = false; }
    int w = worst();
    if (w == 3 && lastWorst < 3) { if (S.alertWake) lastTouch = millis(); beep(2400, 120); delay(80); beep(2400, 120); }
    lastWorst = w;
    if (page == P_CTRL && !feed.control) page = P_HOME;
    xSemaphoreGive(mtx);
    if (scr != S_OFF && !trouble) { if (dialMode) drawDial(); else draw(); }
  }
  if (pairing != wasPairing) { wasPairing = pairing; draw(); }
  if (scr != S_OFF && millis() - lastDraw > 30000) { lastDraw = millis(); if (trouble) drawTrouble(); else if (dialMode) { /* the dial has no clock to refresh */ } else if (!pairing && page == P_SET) setHeader(); else if (!pairing) header(page == P_SET ? String("Settings") : page == P_TODO ? "To do" : page == P_CTRL ? "Controls" : page == P_SPACE && feed.nSp ? feed.sp[spaceIdx].name.substring(0, 18) : feed.nSp == 1 ? feed.sp[0].name.substring(0, 18) : feed.nSp ? String(feed.nSp) + " spaces" : String("LogMyGrow")); }
  if (CYCLE_S[S.cycle] && !pairing && !trouble && page != P_SET && millis() - lastCycle > CYCLE_S[S.cycle] * 1000UL && millis() - lastTouch > 30000) { lastCycle = millis(); nextPage(); }
  backlight(); statusLed();
  delay(10);
}
