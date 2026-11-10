// Mazda 2 Smart Gauge - ESP32 CYD (ESP32-2432S028R), 320x240 landscape
// OBD-II over ELM327 (Bluetooth Classic or BLE), demo mode, Thai/English UI.
#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <BluetoothSerial.h>
#include <BLEDevice.h>
#include <Preferences.h>
#include <string>
#include "ui_strings.h"

#ifndef CRP_PIN
#define CRP_PIN 35          // input-only ADC1 pin on the CYD P3 connector
#endif
#ifndef CRP_RATIO
#define CRP_RATIO 3.0f      // divider ratio: (Rtop+Rbottom)/Rbottom, keep pin <= 3.3 V
#endif

TFT_eSPI tft;
SPIClass tsSPI(VSPI);
XPT2046_Touchscreen ts(33, 36);
BluetoothSerial BT;
Preferences prefs;

constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
// Themes (black background in both): bg, panel, panel2, line, dark, accent, accent-light, dim, text, hot
// 0 = black-blue, 1 = black-red
static const int NTHEME = 2;
static const uint16_t THEMES[NTHEME][10] = {
  {C(0, 0, 0), C(9, 15, 28), C(14, 24, 44), C(26, 52, 98), C(17, 32, 66), C(47, 140, 255), C(155, 216, 255), C(111, 136, 179), C(232, 241, 255), C(255, 77, 94)},
  {C(0, 0, 0), C(26, 9, 12), C(40, 13, 18), C(96, 28, 38), C(70, 20, 28), C(255, 59, 70), C(255, 150, 150), C(179, 120, 125), C(255, 238, 238), C(255, 176, 32)},
};
static uint16_t cBG, cPANEL, cPANEL2, cLINE, cDARK, cBLUE, cICE, cDIM, cTEXT, cHOT;
static const uint16_t cWARN = C(255, 176, 32), cBAD = C(255, 77, 94), cWBG = C(30, 8, 16);
static int theme = 0;
static void applyTheme(int t) {
  const uint16_t* p = THEMES[t];
  cBG = p[0]; cPANEL = p[1]; cPANEL2 = p[2]; cLINE = p[3]; cDARK = p[4]; cBLUE = p[5]; cICE = p[6]; cDIM = p[7]; cTEXT = p[8]; cHOT = p[9];
}

enum Screen { SC_DASH, SC_DATA, SC_GRAPH, SC_TRIP, SC_CODES, SC_SETUP };
static Screen scr = SC_DASH;
static bool redraw = true;

// ---------- settings ----------
static bool thai = false, demoMode = true, warnOn = true;
static int shiftRpm = 6000, bright = 80;
static uint8_t slots[6] = {0, 1, 2, 3, 4, 5};
static bool crpMain = false;                     // dash centre shows CRP volts (true) or speed (false)
static int setupTab = 0;                         // 0 = General, 1 = Bluetooth

// ---------- PID table ----------
struct Pid { uint8_t id; const char* name; const char* unit; uint8_t dec; float mn, mx; char drv; };
static const Pid PIDS[] = {
  {0x04, "Calculated engine load", "%", 0, 0, 100, 'l'}, {0x05, "Coolant temperature", "C", 0, 40, 88, 't'},
  {0x06, "Short fuel trim B1", "%", 1, -6, 6, 'o'}, {0x07, "Long fuel trim B1", "%", 1, 1, 3, 'c'},
  {0x08, "Short fuel trim B2", "%", 1, -6, 6, 'o'}, {0x09, "Long fuel trim B2", "%", 1, 1, 3, 'c'},
  {0x0A, "Fuel pressure", "kPa", 0, 250, 380, 'l'}, {0x0B, "Intake manifold pressure", "kPa", 0, 32, 98, 'l'},
  {0x0C, "Engine speed", "rpm", 0, 0, 7000, 'r'}, {0x0D, "Vehicle speed", "km/h", 0, 0, 110, 's'},
  {0x0E, "Timing advance", "deg", 0, 8, 34, 'l'}, {0x0F, "Intake air temp", "C", 0, 30, 38, 't'},
  {0x10, "Mass air flow", "g/s", 1, 2, 24, 'l'}, {0x11, "Throttle position", "%", 0, 8, 50, 'l'},
  {0x14, "O2 B1S1 voltage", "V", 2, 0.1f, 0.9f, 'o'}, {0x15, "O2 B1S2 voltage", "V", 2, 0.55f, 0.75f, 'c'},
  {0x1F, "Run time since start", "s", 0, 0, 3600, 'k'}, {0x21, "Distance with MIL on", "km", 0, 0, 0, 'c'},
  {0x22, "Fuel rail pressure (rel)", "kPa", 0, 300, 420, 'c'}, {0x23, "Fuel rail pressure (gauge)", "kPa", 0, 6000, 6500, 'c'},
  {0x2C, "Commanded EGR", "%", 0, 0, 18, 'l'}, {0x2D, "EGR error", "%", 1, -3, 3, 'o'},
  {0x2E, "Commanded evap purge", "%", 0, 0, 40, 'l'}, {0x2F, "Fuel tank level", "%", 0, 62, 62, 'c'},
  {0x30, "Warm-ups since clear", "", 0, 41, 41, 'c'}, {0x31, "Distance since clear", "km", 0, 312, 312, 'c'},
  {0x32, "Evap vapor pressure", "Pa", 0, -300, 300, 'o'}, {0x33, "Barometric pressure", "kPa", 0, 100, 100, 'c'},
  {0x34, "O2 B1S1 current", "mA", 2, -2, 2, 'o'}, {0x3C, "Catalyst temp B1S1", "C", 0, 200, 612, 't'},
  {0x3E, "Catalyst temp B1S2", "C", 0, 180, 540, 't'}, {0x42, "Control module voltage", "V", 1, 13.8f, 14.3f, 'o'},
  {0x43, "Absolute load", "%", 0, 15, 85, 'l'}, {0x44, "Commanded lambda", "", 2, 0.97f, 1.03f, 'o'},
  {0x45, "Relative throttle", "%", 0, 0, 45, 'l'}, {0x46, "Ambient air temp", "C", 0, 32, 32, 'c'},
  {0x47, "Absolute throttle B", "%", 0, 8, 50, 'l'}, {0x49, "Accelerator pedal D", "%", 0, 5, 55, 'l'},
  {0x4A, "Accelerator pedal E", "%", 0, 5, 55, 'l'}, {0x4C, "Commanded throttle", "%", 0, 8, 50, 'l'},
  {0x4D, "Time with MIL on", "min", 0, 0, 0, 'c'}, {0x4E, "Time since codes cleared", "min", 0, 420, 420, 'c'},
  {0x52, "Ethanol fuel", "%", 0, 0, 0, 'c'}, {0x5A, "Relative accel pedal", "%", 0, 5, 55, 'l'},
  {0x5C, "Engine oil temp", "C", 0, 35, 92, 't'}, {0x5D, "Fuel injection timing", "deg", 1, -2, 8, 'l'},
  {0x5E, "Engine fuel rate", "L/h", 1, 0.8f, 9, 'l'}, {0x61, "Driver demand torque", "%", 0, 5, 60, 'l'},
  {0x62, "Actual engine torque", "%", 0, 5, 60, 'l'}, {0x63, "Engine reference torque", "Nm", 0, 130, 130, 'c'},
};
static const int NPID = sizeof(PIDS) / sizeof(PIDS[0]);

static float pv[256];
static bool psup[256], want[256];
static uint32_t page[256];
static volatile int linkState = 0;        // 0 demo, 1 connecting, 2 connected
static volatile bool mil = false;
static volatile int dtcCount = 0;
static char carBuf[24] = "";
static uint32_t testUntil = 0;

// fault codes shared between UI and OBD task
static char codeList[8][6];
static volatile int codeCount = 0, codeState = 0, codeVer = 0, req = 0;

static float decodePid(uint8_t id, const uint8_t* b, int n) {
  float A = b[0], B = n > 1 ? b[1] : 0, Cc = n > 2 ? b[2] : 0, D = n > 3 ? b[3] : 0, AB = A * 256 + B;
  switch (id) {
    case 0x04: case 0x11: case 0x2C: case 0x2E: case 0x2F: case 0x45: case 0x47: case 0x49: case 0x4A:
    case 0x4C: case 0x52: case 0x5A: return A * 100 / 255;
    case 0x05: case 0x0F: case 0x46: case 0x5C: return A - 40;
    case 0x06: case 0x07: case 0x08: case 0x09: return (A - 128) * 100 / 128;
    case 0x0A: return A * 3;
    case 0x0C: return AB / 4;
    case 0x0E: return A / 2 - 64;
    case 0x10: return AB / 100;
    case 0x14: case 0x15: return A / 200;
    case 0x1F: case 0x21: case 0x31: case 0x4D: case 0x4E: case 0x63: return AB;
    case 0x22: return AB * 0.079f;
    case 0x23: return AB * 10;
    case 0x2D: return A * 100 / 128 - 100;
    case 0x32: return (float)(int16_t)(uint16_t)AB / 4;
    case 0x34: return (Cc * 256 + D) / 256 - 128;
    case 0x3C: case 0x3E: return AB / 10 - 40;
    case 0x42: return AB / 1000;
    case 0x43: return AB * 100 / 255;
    case 0x44: return AB * 2 / 65536;
    case 0x5D: return AB / 128 - 210;
    case 0x5E: return AB * 0.05f;
    case 0x61: case 0x62: return A - 125;
    default: return A;
  }
}

static float getVal(uint8_t pid, bool& ok) {
  if (pid == 0x05 && millis() < testUntil) { ok = true; return 108; }
  if (demoMode) { ok = true; return pv[pid]; }
  ok = (linkState == 2) && psup[pid] && (millis() - page[pid] < 5000);
  return pv[pid];
}

// ---------- ELM327 communication (runs in obdTask) ----------
static int linkAvail(); static int linkRead(); static void linkSend(const char* cmd);   // Classic or BLE, see the Bluetooth section
static String elmQuery(const char* cmd, uint32_t tmo = 1500) {
  while (linkAvail()) linkRead();
  linkSend(cmd);
  String r; uint32_t t0 = millis();
  while (millis() - t0 < tmo) {
    while (linkAvail()) { char c = (char)linkRead(); if (c == '>') return r; r += c; }
    vTaskDelay(1);
  }
  return r;
}

static bool allHex(const String& s) {
  for (unsigned i = 0; i < s.length(); i++) if (!isxdigit((unsigned char)s[i])) return false;
  return s.length() > 0;
}

// Joins the hex payload lines of a reply (drops echoes, SEARCHING..., NO DATA, length lines, "0:" prefixes).
static String cleanPayload(const String& raw, bool firstOnly) {
  String out; int i = 0, n = raw.length();
  while (i < n) {
    int e = raw.indexOf('\r', i); if (e < 0) e = n;
    String ln = raw.substring(i, e); i = e + 1;
    ln.replace(" ", ""); ln.replace("\n", ""); ln.trim();
    if (!ln.length()) continue;
    int c = ln.indexOf(':');
    if (c >= 0) ln = ln.substring(c + 1);
    else if (ln.length() == 3 && allHex(ln)) continue;
    if (!allHex(ln)) continue;
    out += ln;
    if (firstOnly) break;
  }
  return out;
}

static int hexBytes(const String& d, uint8_t* b, int maxn) {
  int n = 0;
  for (int i = 0; i + 1 < (int)d.length() && n < maxn; i += 2) b[n++] = (uint8_t)strtol(d.substring(i, i + 2).c_str(), nullptr, 16);
  return n;
}

static bool queryPid(uint8_t pid, uint8_t* b, int& n, uint32_t tmo = 1200) {
  char cmd[8], hdr[8];
  snprintf(cmd, sizeof cmd, "01%02X", pid);
  snprintf(hdr, sizeof hdr, "41%02X", pid);
  String p = cleanPayload(elmQuery(cmd, tmo), true);
  int k = p.indexOf(hdr);
  if (k < 0) return false;
  n = hexBytes(p.substring(k + 4), b, 4);
  return n > 0;
}

static void discoverPids() {
  memset(psup, 0, sizeof psup);
  for (int base = 0; base < 0xE0; base += 0x20) {
    uint8_t b[4]; int n = 0;
    if (!queryPid(base, b, n, base == 0 ? 8000 : 1500) || n < 4) break;
    for (int i = 0; i < 32; i++) if (b[i / 8] & (0x80 >> (i % 8))) psup[base + i + 1] = true;
    if (!(b[3] & 1)) break;
  }
}

static bool elmAlive = false;                // set by elmInit: the adapter answered like an ELM327 (so the link itself is fine, even if the car is off)
static bool elmInit() {
  String z = elmQuery("ATZ", 2500), e = elmQuery("ATE0");
  elmAlive = z.indexOf("ELM") >= 0 || e.indexOf("OK") >= 0;
  elmQuery("ATL0"); elmQuery("ATS0"); elmQuery("ATH0"); elmQuery("ATSP0", 1000);
  String p = cleanPayload(elmQuery("0100", 9000), true);
  return p.indexOf("4100") >= 0;
}

static String carFromVin(const String& v) {
  if (v.length() < 11) return "";
  String wmi = v.substring(0, 3), ch = v.substring(3, 5);
  bool mazda = wmi == "MM8" || wmi == "JM1" || wmi == "JM3" || wmi == "JMZ" || wmi == "3MZ" || wmi == "3MV";
  if (!mazda) return "VIN " + wmi;
  if (ch == "DJ" || ch == "DE" || ch == "DW") return "MAZDA 2";
  if (ch == "BM" || ch == "BN" || ch == "BP" || ch == "BL" || ch == "BK") return "MAZDA 3";
  if (ch == "GJ" || ch == "GH" || ch == "GL") return "MAZDA 6";
  if (ch == "KE" || ch == "KF") return "CX-5";
  if (ch == "DK") return "CX-3";
  if (ch == "DM") return "CX-30";
  if (ch == "ND") return "MX-5";
  return "MAZDA";
}

static void readVin() {
  carBuf[0] = 0;
  String p = cleanPayload(elmQuery("0902", 4000), false);
  int k = p.indexOf("4902");
  if (k < 0) return;
  String d = p.substring(k + 6);          // skip 4902 + count byte
  uint8_t b[24]; int n = hexBytes(d, b, 17);
  String vin;
  for (int i = 0; i < n; i++) if (b[i] >= 0x30 && b[i] <= 0x5A) vin += (char)b[i];
  String name = carFromVin(vin);
  strncpy(carBuf, name.c_str(), sizeof carBuf - 1); carBuf[sizeof carBuf - 1] = 0;
}

static void doReadCodes() {
  codeState = 1; codeVer++;
  String p = cleanPayload(elmQuery("03", 3000), false);
  int k = p.indexOf("43");
  codeCount = 0;
  if (k < 0) { codeState = 4; codeVer++; return; }
  uint8_t b[32]; int n = hexBytes(p.substring(k + 2), b, 32), s = (n % 2) ? 1 : 0;
  for (int i = s; i + 1 < n && codeCount < 8; i += 2) {
    if (b[i] == 0 && b[i + 1] == 0) continue;
    snprintf(codeList[codeCount++], 6, "%c%u%X%02X", "PCBU"[b[i] >> 6], (b[i] >> 4) & 3, b[i] & 0xF, b[i + 1]);
  }
  codeState = 2; codeVer++;
}

static void doClearCodes() {
  String p = cleanPayload(elmQuery("04", 3000), false);
  if (p.indexOf("44") >= 0) { codeCount = 0; codeState = 3; } else codeState = 4;
  codeVer++;
}

static void pollPid(uint8_t pid, int& fails) {
  uint8_t b[4]; int n = 0;
  if (queryPid(pid, b, n)) {
    pv[pid] = decodePid(pid, b, n); page[pid] = millis(); fails = 0;
    if (pid == 0x01) { mil = (b[0] & 0x80) != 0; dtcCount = b[0] & 0x7F; }
  } else fails++;
}

// ---------- Bluetooth: nearby-device scan (Classic + BLE), chosen adapter, background link ----------
#ifndef OBD_PIN
#define OBD_PIN "1234"                       // first pairing PIN tried for Classic adapters (some use 0000 or 6789)
#endif
static const char* const PINS[] = {OBD_PIN, "0000", "6789", "1111"};
static const int NPINS = 4;
static volatile int pinIdx = 0, pinSaved = 0;     // PIN that works for the chosen adapter (saved); the next one is tried after a failed connect
static volatile bool pinDirty = false;            // PIN state changed: save it from the UI loop
static volatile bool pinOk = false;               // this adapter has connected with pinIdx at least once: stop guessing PINs

struct BtDev { char addr[18]; char name[26]; int8_t rssi; bool obd; bool ble; uint8_t atype; };
static const int MAXBT = 24;
static BtDev btList[MAXBT];
static volatile int btCount = 0, btVer = 0;
static volatile int scanState = 0;           // 0 idle, 1 scanning, 2 done, 3 failed
static volatile bool scanReq = false, scanStop = false, reconnectReq = false;
static char selAddr[18] = "", selName[26] = "";   // adapter picked by the user (saved in flash)
static bool selBle = false; static uint8_t selAType = 0;   // ... its radio (Classic or BLE) and BLE address type
static volatile uint32_t selGen = 0;              // counts picks, so a connect attempt that was already running for the old pick cannot change the new pick's PIN / GATT choice
static portMUX_TYPE btMux = portMUX_INITIALIZER_UNLOCKED;
static bool btReady = false, bleReady = false, linkActiveBle = false;
static BLEClient* bleClient = nullptr;             // the one BLE client (created with the BLE stack)
static BLERemoteCharacteristic* bleTx = nullptr;   // characteristic that takes our commands

static void cleanName(char* d, size_t n, const char* s) {     // printable ASCII only
  size_t k = 0; bool gap = false;
  for (; *s && k + 1 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if (c >= 32 && c < 127) { d[k++] = (char)c; gap = false; }
    else if (!gap) { d[k++] = '?'; gap = true; }
  }
  d[k] = 0;
}
static bool looksObd(const char* nm) {                        // names that look like an OBD-II adapter are listed first
  static const char* const K[] = {"OBD", "ELM", "V-LINK", "VLINK", "VGATE", "VEEPEAK", "ICAR", "VIECAR", "KONNWEI", "KW90", "CARISTA", "LELINK", "FIXD", "BAFX"};
  char u[26]; size_t i = 0;
  for (; nm[i] && i + 1 < sizeof u; i++) u[i] = (char)toupper((unsigned char)nm[i]);
  u[i] = 0;
  for (const char* k : K) if (strstr(u, k)) return true;
  return false;
}
static bool parseAddr(const char* s, uint8_t* a) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) { if (v[i] > 255) return false; a[i] = (uint8_t)v[i]; }
  return true;
}
static bool btBetter(const BtDev& a, const BtDev& b, bool byRssi) {   // OBD-looking names first, then strongest signal
  if (a.obd != b.obd) return a.obd;
  if (!byRssi) return false;
  return (a.rssi ? a.rssi : -127) > (b.rssi ? b.rssi : -127);
}
// Add or refresh one device (same address on the same radio = same entry). When the list is full a better device replaces the weakest.
static void btInsert(const BtDev& t) {                        // caller holds btMux
  for (int i = 0; i < btCount; i++) {
    if (btList[i].ble != t.ble || strcmp(btList[i].addr, t.addr)) continue;
    if (t.name[0]) { memcpy(btList[i].name, t.name, sizeof t.name); btList[i].obd = btList[i].obd || t.obd; }
    if (t.rssi) btList[i].rssi = t.rssi;
    if (t.ble) btList[i].atype = t.atype;
    return;
  }
  if (btCount < MAXBT) { btList[btCount] = t; btCount = btCount + 1; return; }
  int w = 0;
  for (int i = 1; i < btCount; i++) if (btBetter(btList[w], btList[i], true)) w = i;
  if (btBetter(t, btList[w], true)) btList[w] = t;
}

// Called by the Bluetooth stack for every Classic inquiry result (not on the UI core): copy under a lock, never draw here.
static void onBtFound(BTAdvertisedDevice* d) {
  auto as = d->getAddress().toString();       // Arduino String on this core (std::string on others): auto + c_str() fits both
  auto ns = d->getName();
  const char* ac = as.c_str();
  if (strlen(ac) != 17) return;
  BtDev t; memset(&t, 0, sizeof t);
  for (int i = 0; i < 17; i++) t.addr[i] = (char)toupper((unsigned char)ac[i]);
  cleanName(t.name, sizeof t.name, ns.c_str());
  t.rssi = (int8_t)d->getRSSI(); t.obd = looksObd(t.name);
  portENTER_CRITICAL(&btMux); btInsert(t); portEXIT_CRITICAL(&btMux);
  btVer = btVer + 1;
}

static void btUp() { if (!btReady) { BT.begin("MAZDA-GAUGE", true); BT.setPin(PINS[pinIdx]); btReady = true; } }

// forget the library's list of earlier scans (only if this core has discoverClear(); otherwise nothing happens)
template <class T> static auto btForget(T& b, int) -> decltype(b.discoverClear(), void()) { b.discoverClear(); }
template <class T> static void btForget(T&, long) {}

// BLEDevice::deinit() if this core has it (it lets BLEDevice::init() run again after the shared Bluetooth stack was restarted)
template <class T> static auto bleDeinit(int) -> decltype(T::deinit(false), void()) { T::deinit(false); }
template <class T> static void bleDeinit(long) {}

// Start a Classic inquiry. Some cores refuse a new one after an earlier connect attempt, so retry with a cleaner state each time.
static bool startDiscovery() {
  btForget(BT, 0);
  if (BT.discoverAsync(onBtFound)) return true;
  BT.discoverAsyncStop(); vTaskDelay(400);                 // an unfinished earlier inquiry can block a new one
  if (BT.discoverAsync(onBtFound)) return true;
  if (bleReady) { bleDeinit<BLEDevice>(0); bleReady = false; bleClient = nullptr; bleTx = nullptr; }   // the restart below takes BLE down too; it is set up again when needed
  BT.end(); btReady = false; vTaskDelay(400);              // last resort: restart the Bluetooth stack
  btUp(); vTaskDelay(400);
  btForget(BT, 0);
  return BT.discoverAsync(onBtFound);
}

static bool classicScan() {
  btUp();
  if (!startDiscovery()) return false;
  uint32_t t0 = millis();
  while (millis() - t0 < 10500 && !scanStop) vTaskDelay(50);
  BT.discoverAsyncStop();
  vTaskDelay(500);                            // let the last results arrive
  BTScanResults* res = BT.getScanResults();   // final names / signal of everyone heard during the scan
  if (res) for (int i = 0, n = res->getCount(); i < n; i++) { BTAdvertisedDevice* d = res->getDevice(i); if (d) onBtFound(d); }
  return true;
}

// ---- BLE (Bluetooth Low Energy): many "Bluetooth 4.0 / 5.0, iOS + Android" adapters (KONNWEI KW90x, ...) are BLE, not Classic ----
static bool bleTxResp = true;                      // write with response (only if it has no write-without-response)
static int bleTry = 0;                             // which GATT UART candidate to use; advances after a failed ELM handshake
static uint8_t bleRx[1024]; static uint16_t bleHead = 0, bleTail = 0;   // notification bytes -> ELM reader (guarded by btMux)

static void bleUp() {
  if (bleReady) return;
  BLEDevice::init("MAZDA-GAUGE");
  bleClient = BLEDevice::createClient();           // one client for the whole run (each createClient() takes a GATT slot)
  bleReady = true;
}
static void bleNotify(BLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {   // BLE task: just store the bytes
  portENTER_CRITICAL(&btMux);
  for (size_t i = 0; i < n; i++) { uint16_t nx = (uint16_t)((bleHead + 1) & 1023); if (nx == bleTail) break; bleRx[bleHead] = d[i]; bleHead = nx; }
  portEXIT_CRITICAL(&btMux);
}
static int bleAvail() { portENTER_CRITICAL(&btMux); int n = (bleHead - bleTail) & 1023; portEXIT_CRITICAL(&btMux); return n; }
static int bleReadByte() {
  int c = -1;
  portENTER_CRITICAL(&btMux);
  if (bleHead != bleTail) { c = bleRx[bleTail]; bleTail = (uint16_t)((bleTail + 1) & 1023); }
  portEXIT_CRITICAL(&btMux);
  return c;
}
static void bleFlush() { portENTER_CRITICAL(&btMux); bleTail = bleHead; portEXIT_CRITICAL(&btMux); }

// scan results: the library returns either an object or a pointer depending on its version - these helpers take both
template <class R> static int bleCount(R& r) { return r.getCount(); }
template <class R> static int bleCount(R* r) { return r ? r->getCount() : 0; }
template <class R> static auto bleDevAt(R& r, int i) -> decltype(r.getDevice(i)) { return r.getDevice(i); }
template <class R> static auto bleDevAt(R* r, int i) -> decltype(r->getDevice(i)) { return r->getDevice(i); }

static bool bleScan() {
  bleUp();
  BLEScan* sc = bleReady ? BLEDevice::getScan() : nullptr;
  if (!sc) return false;
  sc->setActiveScan(true);                         // scan responses carry the names
  sc->setInterval(100); sc->setWindow(99);
  auto found = sc->start(5, false);                // blocks for 5 s
  for (int i = 0, n = bleCount(found); i < n; i++) {
    auto d = bleDevAt(found, i);
    auto as = d.getAddress().toString();
    auto ns = d.getName();
    const char* ac = as.c_str();
    if (strlen(ac) != 17) continue;
    BtDev t; memset(&t, 0, sizeof t);
    for (int k = 0; k < 17; k++) t.addr[k] = (char)toupper((unsigned char)ac[k]);
    cleanName(t.name, sizeof t.name, ns.c_str());
    bool uart = d.isAdvertisingService(BLEUUID((uint16_t)0xFFF0)) || d.isAdvertisingService(BLEUUID((uint16_t)0xFFE0)) || d.isAdvertisingService(BLEUUID((uint16_t)0x18F0));
    if (!t.name[0] && !uart) continue;             // anonymous BLE beacons are just noise
    t.rssi = (int8_t)d.getRSSI(); t.ble = true; t.atype = (uint8_t)d.getAddressType(); t.obd = uart || looksObd(t.name);
    portENTER_CRITICAL(&btMux); btInsert(t); portEXIT_CRITICAL(&btMux);
  }
  btVer = btVer + 1;
  sc->clearResults();
  return true;
}

static bool svcSkip(const char* u) {               // firmware-update services also look like "notify + write"
  return strstr(u, "0000fe59") || strstr(u, "00010203-0405-0607-0809-0a0b0c0d1912") || strstr(u, "8d53dc1d");
}
static int svcPrio(const char* u) {                // typical ELM327 BLE adapters: FFF0, FFE0, 18F0, Nordic UART, "BLE serial"
  static const char* const K[] = {"0000fff0", "0000ffe0", "000018f0", "6e400001", "e7810a71"};
  for (const char* k : K) if (strstr(u, k)) return 2;
  return 1;
}
static bool chCanWrite(BLERemoteCharacteristic* c) { return c->canWrite() || c->canWriteNoResponse(); }
static bool chCanNotify(BLERemoteCharacteristic* c) { return c->canNotify() || c->canIndicate(); }

// Find the serial-over-GATT pair (one characteristic that notifies, one that takes writes) and subscribe to the notifications.
static bool bleSetupUart() {
  bleTx = nullptr; bleFlush();
  auto* svcs = bleClient->getServices();           // discovers the services on first use
  if (!svcs) return false;
  struct Cand { BLERemoteCharacteristic* nf; BLERemoteCharacteristic* wr; int prio; };
  Cand cand[8]; int nc = 0;
  for (auto& sv : *svcs) {
    BLERemoteService* s = sv.second; if (!s) continue;
    auto su = s->getUUID().toString();
    char us[48]; { size_t k = 0; for (const char* q = su.c_str(); *q && k + 1 < sizeof us; q++) us[k++] = (char)tolower((unsigned char)*q); us[k] = 0; }
    if (svcSkip(us)) continue;
    auto* chs = s->getCharacteristics(); if (!chs) continue;
    BLERemoteCharacteristic *nf = nullptr, *wr = nullptr;
    for (auto& cv : *chs) { BLERemoteCharacteristic* c = cv.second; if (c && !nf && chCanNotify(c)) nf = c; }
    for (auto& cv : *chs) { BLERemoteCharacteristic* c = cv.second; if (c && c != nf && chCanWrite(c)) { wr = c; break; } }
    if (!wr && nf && chCanWrite(nf)) wr = nf;      // one characteristic does both (HM-10 style FFE1)
    if (nf && wr && nc < 8) { cand[nc].nf = nf; cand[nc].wr = wr; cand[nc].prio = svcPrio(us); nc++; }
  }
  if (!nc) return false;
  Cand ord[8]; int no = 0;                         // known UART services first; a failed handshake moves on to the next one
  for (int p = 2; p >= 1; p--) for (int i = 0; i < nc; i++) if (cand[i].prio == p) ord[no++] = cand[i];
  const Cand& pick = ord[bleTry % no];
  bleTx = pick.wr; bleTxResp = !bleTx->canWriteNoResponse();
  pick.nf->registerForNotify(bleNotify, pick.nf->canNotify());
  return true;
}

// ---- one interface for the ELM327 code: Classic SPP or BLE, whichever radio the chosen adapter uses ----
static int linkAvail() { return linkActiveBle ? bleAvail() : BT.available(); }
static int linkRead() { return linkActiveBle ? bleReadByte() : BT.read(); }
static void linkSend(const char* cmd) {            // command text + carriage return
  if (!linkActiveBle) { BT.print(cmd); BT.print('\r'); return; }
  if (!bleTx) return;
  uint8_t b[40]; size_t n = strlen(cmd); if (n > 30) n = 30;
  memcpy(b, cmd, n); b[n++] = '\r';
  for (size_t o = 0; o < n; o += 20) bleTx->writeValue(b + o, (n - o) > 20 ? 20 : (n - o), bleTxResp);   // default BLE payload is 20 bytes
}
static bool linkIsUp() { return linkActiveBle ? (bleClient && bleClient->isConnected()) : (btReady && BT.connected()); }
static void linkDown() {
  if (linkActiveBle) { if (bleClient && bleClient->isConnected()) bleClient->disconnect(); bleTx = nullptr; bleFlush(); }
  else if (btReady && BT.connected()) BT.disconnect();
}
static bool linkConnect(const char* sa, bool ble, uint8_t atype) {
  uint8_t a[6];
  if (!parseAddr(sa, a)) return false;
  linkDown();                                      // the chosen adapter may use the other radio than the last link
  linkActiveBle = ble;
  if (!ble) { btUp(); BT.setPin(PINS[pinIdx]); return BT.connect(a); }
  bleUp();
  BLEAddress ad(a);
  if (!bleClient->connect(ad, (esp_ble_addr_type_t)atype)) return false;
  if (!bleSetupUart()) { bleClient->disconnect(); return false; }
  return true;
}

static void doScan() {
  portENTER_CRITICAL(&btMux); btCount = 0; portEXIT_CRITICAL(&btMux);
  scanStop = false; scanState = 1; btVer = btVer + 1;
  bool was = linkIsUp(); linkDown(); if (was) vTaskDelay(200);   // no inquiry while a link is up
  bool okC = classicScan();
  bool okB = scanStop ? false : bleScan();         // a tap on a device during the Classic part skips the BLE part
  scanState = (okC || scanStop || (okB && btCount > 0)) ? 2 : 3; btVer = btVer + 1;   // failed = nothing could be scanned and nothing was found
}

void obdTask(void*) {
  int rr = 0, fails = 0; uint32_t lastMil = 0;
  for (;;) {
    if (scanReq) { scanReq = false; doScan(); continue; }
    if (demoMode || !selAddr[0]) {            // demo, or OBD mode with no adapter chosen yet
      linkDown();
      linkState = demoMode ? 0 : 3; vTaskDelay(200); continue;
    }
    if (reconnectReq) { reconnectReq = false; linkDown(); }
    if (!linkIsUp()) {
      linkState = 1;
      char sa[18]; bool sb; uint8_t st; uint32_t gen;
      portENTER_CRITICAL(&btMux); memcpy(sa, selAddr, sizeof sa); sb = selBle; st = selAType; gen = selGen; portEXIT_CRITICAL(&btMux);
      bool ok = linkConnect(sa, sb, st);
      if (!ok) {
        if (!sb && !pinOk && gen == selGen) pinIdx = (pinIdx + 1) % NPINS;   // wrong PIN? the next common one is tried on the next attempt
        if (!scanReq && !reconnectReq) vTaskDelay(3000);
        continue;
      }
      if (demoMode || scanReq || reconnectReq) continue;
      if (!elmInit()) { if (sb && !elmAlive && gen == selGen) bleTry = bleTry + 1; linkDown(); vTaskDelay(2000); continue; }   // silent: try the next GATT candidate (an ELM that talks but has no car yet is not a reason)
      if (!sb && gen == selGen && (!pinOk || pinIdx != pinSaved)) { pinOk = true; pinDirty = true; }   // this PIN works: remember it
      discoverPids(); readVin(); fails = 0; linkState = 2; lastMil = 0;
    }
    while (!demoMode && linkIsUp() && fails < 8 && !scanReq && !reconnectReq) {
      if (req == 1) { req = 0; doReadCodes(); }
      else if (req == 2) { req = 0; doClearCodes(); }
      pollPid(0x0C, fails); pollPid(0x0D, fails);
      for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 255; i++) {
          rr = (rr % 255) + 1;
          if (psup[rr] && want[rr] && rr != 0x0C && rr != 0x0D) { pollPid(rr, fails); break; }
        }
      }
      if (millis() - lastMil > 5000) { pollPid(0x01, fails); lastMil = millis(); }
      vTaskDelay(1);
    }
    linkDown();
    linkState = 1; if (!scanReq && !reconnectReq) vTaskDelay(1000);
  }
}

// ---------- demo simulation ----------
static float simT = 0, crpV = 0;
static void simStep(float dt) {
  simT += dt;
  float ph = sinf(simT / 4) * .5f + .5f, spd = roundf(ph * 110);
  int g = spd < 15 ? 1 : spd < 35 ? 2 : spd < 55 ? 3 : spd < 80 ? 4 : 5;
  static const float K[6] = {0, 90, 55, 40, 32, 27};
  float rpm = fmaxf(850, spd * K[g]) + 700 * fabsf(sinf(simT * 1.3f)), q = spd / 110, w = fminf(1, simT / 60);
  for (int i = 0; i < NPID; i++) {
    const Pid& p = PIDS[i]; float f;
    switch (p.drv) {
      case 'l': f = q * .8f + .1f * fabsf(sinf(simT)); break;
      case 's': f = q; break;
      case 'r': f = fminf(rpm / 7000, 1); break;
      case 't': f = w; break;
      case 'o': f = .5f + .5f * sinf(simT * 3 + p.id); break;
      case 'k': f = simT / 3600; break;
      default: f = .5f;
    }
    pv[p.id] = p.mn + (p.mx - p.mn) * f; page[p.id] = millis(); psup[p.id] = true;
  }
  pv[0x0C] = rpm; pv[0x0D] = spd;
  crpV = fmaxf(0, .8f + fminf(rpm / 7000, 1) * 3.6f + sinf(simT * 40) * .15f);
}

static float readCrp() {
  uint32_t s = 0;
  for (int i = 0; i < 32; i++) s += analogReadMilliVolts(CRP_PIN);
  return (s / 32.0f) / 1000.0f * CRP_RATIO;
}

// ---------- metrics for dash tiles ----------
struct Met { StrId lab; const char* unit; uint8_t dec; float w, b, lo; uint8_t pid; float mul; };
#define NOW_ 1e9f
static const Met MET[] = {
  {S_COOLANT, "C", 0, 100, 108, -NOW_, 0x05, 1}, {S_OIL, "C", 0, 115, 130, -NOW_, 0x5C, 1},
  {S_ATF, "C", 0, 110, 125, -NOW_, 0x00, 1},     {S_INTAKE, "C", 0, 60, 75, -NOW_, 0x0F, 1},
  {S_BATT, "V", 1, NOW_, NOW_, 12, 0x42, 1},     {S_AFR, "", 1, NOW_, NOW_, -NOW_, 0x44, 14.7f},
  {S_LOAD, "%", 0, NOW_, NOW_, -NOW_, 0x04, 1},  {S_THR, "%", 0, NOW_, NOW_, -NOW_, 0x11, 1},
  {S_MAP, "kPa", 0, NOW_, NOW_, -NOW_, 0x0B, 1}, {S_MAF, "g/s", 1, NOW_, NOW_, -NOW_, 0x10, 1},
  {S_TIMING, "deg", 0, NOW_, NOW_, -NOW_, 0x0E, 1}, {S_STFT, "%", 1, NOW_, NOW_, -NOW_, 0x06, 1},
  {S_LTFT, "%", 1, NOW_, NOW_, -NOW_, 0x07, 1},  {S_FUEL, "%", 0, NOW_, NOW_, -NOW_, 0x2F, 1},
  {S_RPM, "rpm", 0, NOW_, NOW_, -NOW_, 0x0C, 1}, {S_SPEED, "km/h", 0, NOW_, NOW_, -NOW_, 0x0D, 1},
};
static const int NMET = sizeof(MET) / sizeof(MET[0]);

struct DI { StrId lab; uint8_t pid; const char* unit; uint8_t dec; float mul; };
static const DI DIS[12] = {
  {S_LAMBDA, 0x44, "", 2, 1}, {S_LOAD, 0x04, "%", 0, 1}, {S_STFT, 0x06, "%", 1, 1}, {S_LTFT, 0x07, "%", 1, 1},
  {S_TIMING, 0x0E, "deg", 0, 1}, {S_MAF, 0x10, "g/s", 1, 1}, {S_MAP, 0x0B, "kPa", 0, 1}, {S_THR, 0x11, "%", 0, 1},
  {S_RAIL, 0x23, "MPa", 1, 0.001f}, {S_BARO, 0x33, "kPa", 0, 1}, {S_CATT, 0x3C, "C", 0, 1}, {S_FUEL, 0x2F, "%", 0, 1},
};

static bool allMode = false;
static void updateWants() {
  memset(want, 0, sizeof want);
  want[0x0C] = want[0x0D] = want[0x05] = want[0x5C] = want[0x42] = want[0x10] = want[0x5E] = true;
  for (int i = 0; i < 6; i++) if (MET[slots[i]].pid) want[MET[slots[i]].pid] = true;
  if (scr == SC_DATA) {
    if (allMode) { for (int i = 0; i < NPID; i++) want[PIDS[i].id] = true; }
    else for (int i = 0; i < 12; i++) want[DIS[i].pid] = true;
  }
}

// ---------- persistence ----------
static void loadPrefs() {
  prefs.begin("gauge", true);
  thai = prefs.getBool("thai", false); demoMode = prefs.getBool("demo", true); warnOn = prefs.getBool("warn", true);
  shiftRpm = prefs.getInt("shift", 6000); bright = prefs.getInt("bright", 80); theme = prefs.getInt("theme", 0);
  crpMain = prefs.getBool("crpm", false);
  String ba = prefs.getString("btaddr", ""), bn = prefs.getString("btname", "");
  int bk = prefs.getInt("btkind", 0), bp = prefs.getInt("btpin", 0); bool bo = prefs.getBool("btpinok", false);
  uint8_t s[6]; if (prefs.getBytes("slots", s, 6) == 6) { bool good = true; for (int i = 0; i < 6; i++) if (s[i] >= NMET) good = false; if (good) memcpy(slots, s, 6); }
  prefs.end();
  if (theme < 0 || theme >= NTHEME) theme = 0;
  applyTheme(theme);
  uint8_t tmp[6];
  pinIdx = pinSaved = (bp >= 0 && bp < NPINS) ? bp : 0; pinOk = bo;
  if (ba.length() == 17 && parseAddr(ba.c_str(), tmp)) {
    strncpy(selAddr, ba.c_str(), sizeof selAddr - 1); cleanName(selName, sizeof selName, bn.c_str());
    selBle = bk >= 1 && bk <= 5; selAType = selBle ? (uint8_t)(bk - 1) : 0;
  }
}
static void savePrefs() {
  prefs.begin("gauge", false);
  prefs.putBool("thai", thai); prefs.putBool("demo", demoMode); prefs.putBool("warn", warnOn); prefs.putBool("crpm", crpMain);
  prefs.putInt("shift", shiftRpm); prefs.putInt("theme", theme); prefs.putInt("bright", bright); prefs.putBytes("slots", slots, 6);
  prefs.putString("btaddr", selAddr); prefs.putString("btname", selName);
  prefs.putInt("btkind", selBle ? 1 + selAType : 0); prefs.putInt("btpin", pinIdx); prefs.putBool("btpinok", pinOk);
  prefs.end();
  pinSaved = pinIdx;
}
static void applyBright() { ledcWrite(0, map(bright, 0, 100, 5, 255)); }

// ---------- drawing helpers ----------
static bool hit(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
static void card(int x, int y, int w, int h, uint16_t fill = cPANEL, uint16_t border = cLINE) {
  tft.fillRoundRect(x, y, w, h, 6, fill); tft.drawRoundRect(x, y, w, h, 6, border);
}
static int strW(StrId id) { return (thai && THB[id].d) ? THB[id].w : tft.textWidth(EN[id], 2); }
static void label(StrId id, int x, int y, uint16_t fg, uint16_t bg) {
  if (thai && THB[id].d) tft.drawBitmap(x, y - 2, THB[id].d, THB[id].w, THB[id].h, fg);   // transparent: keeps card borders intact
  else { tft.setTextColor(fg, bg); tft.setTextDatum(TL_DATUM); tft.setTextPadding(0); tft.drawString(EN[id], x, y, 2); }
}
static void labelC(StrId id, int cx, int y, uint16_t fg, uint16_t bg) { label(id, cx - strW(id) / 2, y, fg, bg); }
static void labelThai(StrId id, int cx, int y, uint16_t fg, uint16_t bg) {  // always Thai (language button)
  tft.drawBitmap(cx - THB[id].w / 2, y - 2, THB[id].d, THB[id].w, THB[id].h, fg);
}
static void txt(const char* s, int x, int y, int font, uint16_t fg, uint16_t bg, uint8_t datum = TL_DATUM, int pad = 0) {
  tft.setTextColor(fg, bg); tft.setTextDatum(datum); tft.setTextPadding(pad);
  tft.drawString(s, x, y, font); tft.setTextPadding(0); tft.setTextDatum(TL_DATUM);
}
static void btn(int x, int y, int w, int h, StrId id, bool on) {
  uint16_t bg = on ? cBLUE : cPANEL; card(x, y, w, h, bg, on ? cICE : cLINE);
  labelC(id, x + w / 2, y + (h - 16) / 2, on ? cBG : cTEXT, bg);
}
static void btnS(int x, int y, int w, int h, const char* s, bool on) {
  uint16_t bg = on ? cBLUE : cPANEL; card(x, y, w, h, bg, on ? cICE : cLINE);
  txt(s, x + w / 2, y + h / 2, 2, on ? cBG : cTEXT, bg, MC_DATUM);
}
static void fmtv(char* b, size_t n, float v, int dec) { snprintf(b, n, "%.*f", dec, v); }

static const StrId NAVS[6] = {S_DASH, S_DATA, S_GRAPH, S_TRIP, S_CODES, S_SETUP};
static void drawNav() {
  for (int i = 0; i < 6; i++) {
    int x = 4 + i * 52; bool on = (int)scr == i;
    tft.fillRoundRect(x, 212, 50, 26, 6, on ? cPANEL2 : cBG); tft.drawRoundRect(x, 212, 50, 26, 6, on ? cBLUE : cLINE);
    labelC(NAVS[i], x + 25, 218, on ? cTEXT : cDIM, on ? cPANEL2 : cBG);
  }
}

static void fitText(char* d, size_t n, const char* s, int font, int maxw) {   // shortens with ".." to fit maxw pixels
  strncpy(d, s, n - 1); d[n - 1] = 0;
  if (tft.textWidth(d, font) <= maxw) return;
  int L = (int)strlen(d);
  while (L > 1) {
    L--; char t[40]; snprintf(t, sizeof t, "%.*s..", L, d);
    if (tft.textWidth(t, font) <= maxw) { strncpy(d, t, n - 1); d[n - 1] = 0; return; }
  }
}

static void drawTop() {
  tft.fillRect(0, 0, 320, 19, cBG);
  uint16_t dot = linkState == 2 ? C(53, 224, 176) : (linkState == 1 || linkState == 3) ? cWARN : cBLUE;
  tft.fillCircle(8, 9, 3, dot);
  label(linkState == 2 ? S_CONNECTED : linkState == 1 ? S_CONNECTING : linkState == 3 ? S_SELDEV : S_DEMO, 16, 2, cDIM, cBG);
  const char* nm = demoMode ? "MAZDA 2" : carBuf;
  if (nm[0]) txt(nm, 316, 2, 2, cICE, cBG, TR_DATUM);
  else if (!demoMode && linkState == 2) label(S_UNKNOWN, 316 - strW(S_UNKNOWN), 2, cICE, cBG);
  else if (!demoMode && linkState == 1 && selName[0]) { char t[32]; fitText(t, sizeof t, selName, 2, 130); txt(t, 316, 2, 2, cDIM, cBG, TR_DATUM); }
}

// ---------- dashboard ----------
static const int TILE_X[6] = {4, 4, 4, 228, 228, 228}, TILE_Y[6] = {28, 83, 138, 28, 83, 138};
static char lastTile[6][12], lastSpd[8], lastRpm[14], lastStrip[10];
static uint16_t lastTileCol[6], lastArcCol;
static int lastArc, lastLit, lastCrpPx;

static bool unitLow(const Met& m) {          // long (Thai) label: put the unit small at the bottom right instead of beside the label
  return m.unit[0] && 6 + strW(m.lab) + 3 + tft.textWidth(m.unit, 1) > 86;
}
static void tileStatic(int i) {
  const Met& m = MET[slots[i]];
  card(TILE_X[i], TILE_Y[i], 88, 50);
  label(m.lab, TILE_X[i] + 6, TILE_Y[i] + 3, cDIM, cPANEL);
  txt(m.unit, TILE_X[i] + 83, TILE_Y[i] + (unitLow(m) ? 38 : 6), 1, cDIM, cPANEL, TR_DATUM);
  lastTile[i][0] = 0;
}
static uint16_t valCol(const Met& m, float v) { return v >= m.b ? cBAD : v >= m.w ? cWARN : v < m.lo ? cWARN : cTEXT; }
static void tileDyn(int i) {
  const Met& m = MET[slots[i]]; char b[12]; uint16_t col = cDIM; bool ok = false; float v = 0;
  if (m.pid) v = getVal(m.pid, ok) * m.mul;
  if (ok) { fmtv(b, sizeof b, v, m.dec); col = valCol(m, v); } else strcpy(b, "--");
  if (!strcmp(b, lastTile[i]) && col == lastTileCol[i]) return;
  txt(b, TILE_X[i] + 8, TILE_Y[i] + 22, 4, col, cPANEL, TL_DATUM, unitLow(m) ? 56 : 72);
  strcpy(lastTile[i], b); lastTileCol[i] = col;
}
static void centerLabel() {                  // small caption under the big number
  if (crpMain) txt("CRP (V)", 160, 142, 1, cICE, cBG, TC_DATUM);
  else txt("km/h", 160, 142, 1, cDIM, cBG, TC_DATUM);
}
static void stripStatic() {                  // bottom strip shows the value that is NOT in the centre
  card(4, 190, 312, 18); label(crpMain ? S_SPEED : S_CRP, 10, 191, cDIM, cPANEL);
  txt(crpMain ? "km/h" : "V", 150, 195, 1, cDIM, cPANEL, TL_DATUM);
  lastStrip[0] = 0; lastCrpPx = -1;
}
static void dashStatic() {
  for (int i = 0; i < 6; i++) tileStatic(i);
  tft.drawSmoothArc(160, 108, 64, 56, 30, 330, cDARK, cBG, true);
  centerLabel(); stripStatic();
  lastSpd[0] = lastRpm[0] = 0; lastArc = -100; lastLit = -1; lastArcCol = 0;
}
static void dashSwap() {                     // tap on the gauge: speed <-> CRP (centre and bottom strip trade places)
  tft.fillCircle(160, 108, 54, cBG);
  lastSpd[0] = lastRpm[0] = 0;
  centerLabel(); stripStatic();
}
static void dashDyn() {
  bool ok; float rpm = getVal(0x0C, ok); if (!ok) rpm = 0;
  float spd = getVal(0x0D, ok); if (!ok) spd = 0;
  int ang = 30 + (int)(300.0f * constrain(rpm / 7000.0f, 0.0f, 1.0f)); uint16_t col = rpm >= shiftRpm ? cHOT : cBLUE;
  if (abs(ang - lastArc) >= 2 || col != lastArcCol) {
    if (ang < 330) tft.drawSmoothArc(160, 108, 64, 56, ang, 330, cDARK, cBG, true);
    if (ang > 30) tft.drawSmoothArc(160, 108, 64, 56, 30, ang, col, cDARK, true);
    lastArc = ang; lastArcCol = col;
  }
  char b[14]; int cv = (int)lroundf(constrain(crpV, 0.0f, 9.99f) * 100.0f);
  if (crpMain) snprintf(b, sizeof b, "%d.%02d", cv / 100, cv % 100); else snprintf(b, sizeof b, "%d", (int)spd);
  if (strcmp(b, lastSpd)) {
    if (crpMain) {                           // volts: big digit + smaller decimals (always the same width, never touches the arc)
      char ip[12], dp[12]; snprintf(ip, sizeof ip, "%d", cv / 100); snprintf(dp, sizeof dp, ".%02d", cv % 100);
      int iw = tft.textWidth("0", 6), dw = tft.textWidth(".00", 4), x0 = 160 - (iw + dw) / 2;
      txt(ip, x0, 112, 6, cICE, cBG, ML_DATUM);
      txt(dp, x0 + iw, 136, 4, cICE, cBG, BL_DATUM, dw);
    } else txt(b, 160, 112, 6, cTEXT, cBG, MC_DATUM, 96);
    strcpy(lastSpd, b);
  }
  snprintf(b, sizeof b, "%d rpm", (int)rpm);
  if (strcmp(b, lastRpm)) { txt(b, 160, 72, 2, cICE, cBG, TC_DATUM, 84); strcpy(lastRpm, b); }
  int lit = rpm < 3000 ? 0 : min(14, (int)((rpm - 3000) * 14.0f / max(500, shiftRpm - 3000)) + 1);
  if (lit != lastLit) {
    for (int i = 0; i < 14; i++) tft.fillRoundRect(4 + i * 22, 20, 19, 4, 2, i < lit ? (i >= 11 ? cHOT : cBLUE) : cDARK);
    lastLit = lit;
  }
  for (int i = 0; i < 6; i++) tileDyn(i);
  char c[10]; int px;
  if (crpMain) { snprintf(c, sizeof c, "%d", (int)spd); px = (int)(constrain(spd / 180.0f, 0.0f, 1.0f) * 132); }
  else { snprintf(c, sizeof c, "%.2f", crpV); px = (int)(constrain(crpV / 5.0f, 0.0f, 1.0f) * 132); }
  if (strcmp(c, lastStrip)) { txt(c, 112, 191, 2, cTEXT, cPANEL, TL_DATUM, 36); strcpy(lastStrip, c); }
  if (px != lastCrpPx) { tft.fillRect(176, 195, px, 8, cBLUE); tft.fillRect(176 + px, 195, 132 - px, 8, cDARK); lastCrpPx = px; }
}

// ---------- data screen ----------
static int listOff = 0, listIds[NPID], nList = 0;
static char dcache[12][14], rcache[8][28];
static void buildList() { nList = 0; for (int i = 0; i < NPID; i++) if (demoMode || psup[PIDS[i].id]) listIds[nList++] = i; if (listOff > max(0, nList - 8)) listOff = max(0, nList - 8); }
static int unitY(int i) {                    // unit sits beside the label if it fits there, else small at the bottom right
  return (5 + strW(DIS[i].lab) + 4 + tft.textWidth(DIS[i].unit, 1) <= 72) ? 6 : 38;
}
static void dataStatic() {
  btn(4, 28, 70, 18, S_MAINV, !allMode); btn(78, 28, 90, 18, S_ALLPID, allMode);
  memset(dcache, 0, sizeof dcache); memset(rcache, 0, sizeof rcache);
  if (!allMode) {
    for (int i = 0; i < 12; i++) {
      int x = 4 + (i % 4) * 78, y = 50 + (i / 4) * 52; card(x, y, 75, 48);
      label(DIS[i].lab, x + 5, y + 3, cDIM, cPANEL);
      if (DIS[i].unit[0] && unitY(i) == 6) txt(DIS[i].unit, x + 71, y + 6, 1, cDIM, cPANEL, TR_DATUM);
    }
  } else {
    buildList(); tft.fillRect(4, 50, 284, 156, cBG);
    card(292, 50, 24, 74); card(292, 128, 24, 74);
    tft.fillTriangle(304, 76, 297, 96, 311, 96, cICE); tft.fillTriangle(304, 176, 297, 156, 311, 156, cICE);
  }
}
static void dataDyn() {
  bool ok;
  if (!allMode) {
    for (int i = 0; i < 12; i++) {
      int x = 4 + (i % 4) * 78, y = 50 + (i / 4) * 52; char b[14]; uint16_t col = cTEXT;
      float v = getVal(DIS[i].pid, ok) * DIS[i].mul;
      if (ok) fmtv(b, sizeof b, v, DIS[i].dec); else { strcpy(b, "--"); col = cDIM; }
      if (strcmp(b, dcache[i])) {
        bool low = DIS[i].unit[0] && unitY(i) == 38;
        txt(b, x + 6, y + 22, 4, col, cPANEL, TL_DATUM, low ? 75 - 12 - tft.textWidth(DIS[i].unit, 1) : 63);
        if (low) txt(DIS[i].unit, x + 71, y + 38, 1, cDIM, cPANEL, TR_DATUM);
        strcpy(dcache[i], b);
      }
    }
  } else {
    static uint32_t lastBuild = 0; if (millis() - lastBuild > 3000) { lastBuild = millis(); int n0 = nList; buildList(); if (n0 != nList) rcache[0][0] = 0; }
    for (int r = 0; r < 8; r++) {
      int y = 50 + r * 19; char b[22];
      if (listOff + r >= nList) { if (rcache[r][0] != '-') { tft.fillRect(4, y, 284, 19, cBG); strcpy(rcache[r], "-"); } continue; }
      const Pid& p = PIDS[listIds[listOff + r]];
      float v = getVal(p.id, ok); if (ok) snprintf(b, sizeof b, "%.*f %s", p.dec, v, p.unit); else strcpy(b, "--");
      char key[28]; snprintf(key, sizeof key, "%02X%s", p.id, b);
      if (strcmp(key, rcache[r])) {
        tft.fillRect(4, y, 284, 18, cBG); tft.drawFastHLine(4, y + 18, 284, cDARK);
        char h[4]; snprintf(h, sizeof h, "%02X", p.id);
        txt(h, 6, y + 1, 2, cICE, cBG); txt(p.name, 34, y + 5, 1, cDIM, cBG);
        txt(b, 286, y + 1, 2, cTEXT, cBG, TR_DATUM); strcpy(rcache[r], key);
      }
    }
  }
}

// ---------- graph ----------
static const int GN = 120;
static float gb[4][GN]; static int gn = 0, gsel = 0;
static const uint8_t GP[4] = {0x0C, 0x0D, 0x05, 0x42};
static const float GMIN[4] = {0, 0, 20, 10}, GMAX[4] = {7000, 180, 120, 16};
static const StrId GL[4] = {S_RPM, S_SPEED, S_COOLANT, S_BATT};
static void graphSample() {
  for (int c = 0; c < 4; c++) {
    bool ok; float v = getVal(GP[c], ok); if (!ok) v = GMIN[c];
    if (gn < GN) gb[c][gn] = v; else { memmove(gb[c], gb[c] + 1, (GN - 1) * sizeof(float)); gb[c][GN - 1] = v; }
  }
  if (gn < GN) gn++;
}
static void graphStatic() { for (int i = 0; i < 4; i++) btn(4 + i * 78, 28, 76, 18, GL[i], gsel == i); }
static void graphDyn() {
  static uint32_t last = 0; if (millis() - last < 500) return; last = millis();
  tft.fillRect(4, 50, 312, 156, cPANEL); tft.drawRect(4, 50, 312, 156, cLINE);
  for (int i = 0; i < 5; i++) {
    int y = 56 + i * 36; tft.drawFastHLine(34, y, 278, cDARK);
    char b[8]; snprintf(b, sizeof b, "%d", (int)(GMAX[gsel] - (GMAX[gsel] - GMIN[gsel]) * i / 4)); txt(b, 8, y - 4, 1, cDIM, cPANEL);
  }
  int px = 0, py = 0;
  for (int i = 0; i < gn; i++) {
    int x = 34 + (i + GN - gn) * 278 / (GN - 1);
    float f = constrain((gb[gsel][i] - GMIN[gsel]) / (GMAX[gsel] - GMIN[gsel]), 0.0f, 1.0f);
    int y = 56 + (int)(144 * (1 - f));
    if (i) { tft.drawLine(px, py, x, y, cBLUE); tft.drawLine(px, py + 1, x, y + 1, cBLUE); }
    px = x; py = y;
  }
  if (gn) { char b[12]; fmtv(b, sizeof b, gb[gsel][gn - 1], gsel == 3 ? 1 : 0); txt(b, 310, 54, 2, cICE, cPANEL, TR_DATUM); }
}

// ---------- trip ----------
static float tDist = 0, tSec = 0, tTop = 0, tFuel = 0, tNow = 0;
static void tripStep(float dt) {
  bool ok; float spd = getVal(0x0D, ok); if (!ok) return;
  tDist += spd / 3600 * dt; tSec += dt; tTop = fmaxf(tTop, spd);
  bool ok2; float lph = getVal(0x5E, ok2);
  if (!ok2) { float maf = getVal(0x10, ok2); if (ok2) lph = maf * 3600 / (14.7f * 745); }
  if (ok2) { tFuel += lph / 3600 * dt; tNow = spd > 3 ? lph / spd * 100 : 0; }
}
static const StrId TL[6] = {S_NOW, S_TAVG, S_DIST, S_DTIME, S_ASPD, S_TOP};
static const char* const TU[6] = {"L/100km", "L/100km", "km", "min", "km/h", "km/h"};
static char tcache[6][12];
static void tripStatic() {
  for (int i = 0; i < 6; i++) {
    int x = 4 + (i % 3) * 106, y = 30 + (i / 3) * 66; card(x, y, 100, 60); label(TL[i], x + 6, y + 4, cDIM, cPANEL);
    txt(TU[i], x + 94, y + 48, 1, cDIM, cPANEL, TR_DATUM);
  }
  btn(4, 166, 150, 26, S_RESET, false); memset(tcache, 0, sizeof tcache);
}
static void tripDyn() {
  char v[6][12];
  fmtv(v[0], 12, tNow, 1); if (tDist > 0.05f) fmtv(v[1], 12, tFuel / tDist * 100, 1); else strcpy(v[1], "--");
  fmtv(v[2], 12, tDist, 1); snprintf(v[3], 12, "%d:%02d", (int)(tSec / 60), (int)tSec % 60);
  fmtv(v[4], 12, tSec > 1 ? tDist / (tSec / 3600) : 0, 0); fmtv(v[5], 12, tTop, 0);
  for (int i = 0; i < 6; i++) if (strcmp(v[i], tcache[i])) {
    int x = 4 + (i % 3) * 106, y = 30 + (i / 3) * 66;
    txt(v[i], x + 8, y + 24, 4, cTEXT, cPANEL, TL_DATUM, 84); strcpy(tcache[i], v[i]);
  }
}

// ---------- fault codes ----------
struct CodeDesc { const char* c; const char* d; };
static const CodeDesc CDESC[] = {
  {"P0101", "MAF range/performance"}, {"P0102", "MAF circuit low"}, {"P0113", "IAT sensor high"}, {"P0117", "ECT sensor low"},
  {"P0118", "ECT sensor high"}, {"P0133", "O2 sensor slow B1S1"}, {"P0134", "O2 no activity B1S1"}, {"P0135", "O2 heater circuit B1S1"},
  {"P0171", "System too lean B1"}, {"P0172", "System too rich B1"}, {"P0174", "System too lean B2"}, {"P0175", "System too rich B2"},
  {"P0300", "Random/multiple misfire"}, {"P0301", "Cylinder 1 misfire"}, {"P0302", "Cylinder 2 misfire"}, {"P0303", "Cylinder 3 misfire"},
  {"P0304", "Cylinder 4 misfire"}, {"P0335", "Crankshaft position circuit"}, {"P0336", "Crankshaft position range"},
  {"P0340", "Camshaft position circuit"}, {"P0420", "Catalyst efficiency low B1"}, {"P0430", "Catalyst efficiency low B2"},
  {"P0455", "EVAP large leak"}, {"P0505", "Idle control system"}, {"P0562", "System voltage low"}, {"P0563", "System voltage high"},
  {"U0100", "Lost comm with ECM/PCM"},
};
static int lastCodeVer = -1; static bool lastMil = false;
static void codesList() {
  tft.fillRect(4, 28, 312, 134, cBG);
  label(S_FAULT, 8, 30, cTEXT, cBG);
  StrId li = mil ? S_LAMPON : S_LAMPOFF; label(li, 312 - strW(li), 30, mil ? cBAD : cDIM, cBG);
  if (codeCount == 0) {
    StrId m = codeState == 1 ? S_READING : codeState == 3 ? S_CLEARED : codeState == 4 ? S_NOLINK : S_NOCODE;
    labelC(m, 160, 90, cDIM, cBG);
  }
  for (int i = 0; i < codeCount && i < 6; i++) {
    int y = 50 + i * 18; const char* d = "(see service manual)";
    for (const CodeDesc& cd : CDESC) if (!strcmp(cd.c, codeList[i])) d = cd.d;
    card(4, y, 312, 17, cPANEL); txt(codeList[i], 10, y + 1, 2, cICE, cPANEL); txt(d, 70, y + 5, 1, cTEXT, cPANEL);
  }
  lastCodeVer = codeVer; lastMil = mil;
}
static void codesStatic() { btn(4, 166, 150, 26, S_READ, false); btn(160, 166, 150, 26, S_CLEAR, true); codesList(); }
static void codesDyn() { if (lastCodeVer != codeVer || lastMil != mil) codesList(); }

// ---------- setup (two tabs: General | Bluetooth) ----------
static const int SU_Y = 50, SU_P = 26;       // first row y and row pitch
static void setupTabs() { btn(4, 28, 100, 18, S_GENERAL, setupTab == 0); btn(108, 28, 100, 18, S_BLUETOOTH, setupTab == 1); }

static void themeChip(int k, int x, int by) {   // each chip previews its own colours
  const uint16_t* p = THEMES[k]; bool sel = theme == k;
  tft.fillRoundRect(x, by, 100, 20, 6, p[2]);
  tft.drawRoundRect(x, by, 100, 20, 6, sel ? p[5] : p[3]);
  if (sel) tft.drawRoundRect(x + 1, by + 1, 98, 18, 5, p[5]);
  tft.fillCircle(x + 11, by + 10, 4, p[5]);
  labelC(k ? S_THRED : S_THBLUE, x + 57, by + 2, p[8], p[2]);
}
static void genRow(int i) {
  int y = SU_Y + i * SU_P, by = y + 2; card(4, y, 312, 24);
  static const StrId L[6] = {S_LANG, S_THEME, S_SHIFT, S_WARN, S_BRIGHT, S_TEST};
  label(L[i], 10, y + 4, cTEXT, cPANEL);
  char b[12];
  switch (i) {
    case 0: card(196, by, 56, 20, thai ? cBLUE : cPANEL2, thai ? cICE : cLINE); labelThai(S_THAI, 224, by + 2, thai ? cBG : cTEXT, thai ? cBLUE : cPANEL2); btnS(258, by, 54, 20, "EN", !thai); break;
    case 1: themeChip(0, 108, by); themeChip(1, 212, by); break;
    case 2: btnS(196, by, 24, 20, "-", false); snprintf(b, sizeof b, "%d", shiftRpm); txt(b, 254, by + 2, 2, cICE, cPANEL, TC_DATUM, 60); btnS(288, by, 24, 20, "+", false); break;
    case 3: btn(196, by, 56, 20, S_ON, warnOn); btn(258, by, 54, 20, S_OFF, !warnOn); break;
    case 4: btnS(196, by, 24, 20, "-", false); snprintf(b, sizeof b, "%d%%", bright); txt(b, 254, by + 2, 2, cICE, cPANEL, TC_DATUM, 60); btnS(288, by, 24, 20, "+", false); break;
    case 5: btn(258, by, 54, 20, S_TESTB, false); break;
  }
}

// --- Bluetooth tab: data source, chosen adapter, list of nearby devices ---
static BtDev btView[MAXBT];
static int nBtView = 0, btOff = 0, btViewVer = -1, btViewScan = -1;
static uint32_t lastBtDraw = 0;

static void btSrcRow() {
  int y = 50, by = y + 2; card(4, y, 312, 24); label(S_SRC, 10, y + 4, cTEXT, cPANEL);
  btn(196, by, 56, 20, S_DEMO, demoMode); btnS(258, by, 54, 20, "OBD BT", !demoMode);
}
static void btDevRow() {
  int y = 77, by = y + 2; card(4, y, 312, 24); label(S_BTDEV, 10, y + 4, cTEXT, cPANEL);
  const char* nm = selName[0] ? selName : selAddr;
  if (nm[0]) {
    int x = 92;
    if (selBle) { tft.drawRoundRect(92, y + 5, 26, 13, 3, cICE); txt("BLE", 105, y + 11, 1, cICE, cPANEL, MC_DATUM); x = 122; }
    char t[32]; fitText(t, sizeof t, nm, 2, 246 - x); txt(t, x, y + 4, 2, cICE, cPANEL, TL_DATUM);
  }
  else label(S_NOSEL, 92, y + 4, cDIM, cPANEL);
  btn(250, by, 62, 20, S_SCAN, scanState == 1);
}
static void btBuildView() {
  portENTER_CRITICAL(&btMux); nBtView = btCount; memcpy(btView, btList, sizeof(BtDev) * nBtView); portEXIT_CRITICAL(&btMux);
  bool byRssi = scanState != 1;                 // keep rows still while the scan is running
  for (int i = 1; i < nBtView; i++) {
    BtDev t = btView[i]; int j = i - 1;
    while (j >= 0 && btBetter(t, btView[j], byRssi)) { btView[j + 1] = btView[j]; j--; }
    btView[j + 1] = t;
  }
  if (btOff > max(0, nBtView - 4)) btOff = max(0, nBtView - 4);
}
static void btRow(int r) {
  int y = 122 + r * 21, i = btOff + r;
  tft.fillRect(4, y, 270, 21, cBG);
  if (i >= nBtView) return;
  const BtDev& d = btView[i]; bool sel = d.ble == selBle && !strcmp(d.addr, selAddr); uint16_t bg = sel ? cPANEL2 : cPANEL;
  card(4, y, 270, 19, bg, sel ? cBLUE : cLINE);
  int x = 10;
  if (d.obd) { tft.fillRoundRect(9, y + 3, 28, 13, 3, cBLUE); txt("OBD", 23, y + 9, 1, cBG, cBLUE, MC_DATUM); x = 42; }
  char t[32]; fitText(t, sizeof t, d.name[0] ? d.name : d.addr, 2, (d.ble ? 190 : 200) - x);
  txt(t, x, y + 2, 2, d.name[0] ? (sel ? cTEXT : cICE) : cDIM, bg, TL_DATUM);
  if (d.ble) { tft.drawRoundRect(194, y + 3, 26, 13, 3, cICE); txt("BLE", 207, y + 9, 1, cICE, bg, MC_DATUM); }
  if (sel) { tft.drawLine(224, y + 10, 228, y + 14, cICE); tft.drawLine(225, y + 10, 229, y + 14, cICE); tft.drawLine(229, y + 14, 237, y + 4, cICE); tft.drawLine(230, y + 14, 238, y + 4, cICE); }
  int lv = !d.rssi ? 0 : d.rssi >= -55 ? 4 : d.rssi >= -67 ? 3 : d.rssi >= -78 ? 2 : 1;
  for (int k = 0; k < 4; k++) { int h = 3 + k * 2; tft.fillRect(246 + k * 5, y + 16 - h, 3, h, k < lv ? cICE : cDARK); }
}
static void btArrows() {
  bool up = btOff > 0, dn = btOff + 4 < nBtView;
  card(278, 122, 38, 40); card(278, 165, 38, 40);
  tft.fillTriangle(297, 130, 289, 152, 305, 152, up ? cICE : cDARK);
  tft.fillTriangle(297, 197, 289, 175, 305, 175, dn ? cICE : cDARK);
}
static void btListDraw() {
  btBuildView(); btViewVer = btVer; btViewScan = scanState; lastBtDraw = millis();
  int st = scanState, n = nBtView;
  tft.fillRect(4, 104, 312, 17, cBG);
  label(n > 0 && st != 1 ? S_TAPPICK : S_NEARBY, 8, 104, cDIM, cBG);
  if (n > 0 && st == 1) label(S_SCANNING, 316 - strW(S_SCANNING), 104, cWARN, cBG);
  else if (n > 0) {
    char c[12]; snprintf(c, sizeof c, "%d", n);
    int w = strW(S_FOUND), cw = tft.textWidth(c, 2);
    txt(c, 316 - w - 4 - cw, 104, 2, cICE, cBG); label(S_FOUND, 316 - w, 104, cDIM, cBG);
  }
  for (int r = 0; r < 4; r++) btRow(r);
  if (n == 0) {
    StrId m = st == 1 ? S_SCANNING : st == 2 ? S_NOTFOUND : st == 3 ? S_SCANFAIL : S_PRESSSCAN;
    labelC(m, 140, 158, st == 3 ? cBAD : cDIM, cBG);
  }
  btArrows();
}
static void btDyn() {
  bool stateChg = btViewScan != scanState;
  if (btViewVer == btVer && !stateChg) return;
  if (!stateChg && millis() - lastBtDraw < 250) return;
  btListDraw();
  if (stateChg) btDevRow();
}
static void btPick(int i) {                  // user tapped a device: remember it, switch to OBD mode and connect
  BtDev d = btView[i];
  portENTER_CRITICAL(&btMux);
  bool same = !strcmp(d.addr, selAddr);                         // tapping the adapter that is already chosen (either radio) keeps what we learned about its PIN
  memcpy(selAddr, d.addr, sizeof selAddr); strcpy(selName, d.name[0] ? d.name : d.addr);
  selBle = d.ble; selAType = d.atype; bleTry = 0; selGen = selGen + 1;
  if (!same) { pinIdx = 0; pinOk = false; }
  portEXIT_CRITICAL(&btMux);
  demoMode = false; carBuf[0] = 0; linkState = 1;
  scanStop = true; reconnectReq = true;
  savePrefs(); updateWants();
  drawTop(); btSrcRow(); btDevRow(); btListDraw();
}

static void setupStatic() {
  setupTabs();
  if (setupTab == 0) { for (int i = 0; i < 6; i++) genRow(i); }
  else { btSrcRow(); btDevRow(); btListDraw(); }
}
static void setupSwitch(int t) {
  if (t == setupTab) return;
  setupTab = t; tft.fillRect(0, 22, 320, 188, cBG); setupStatic();
}
static void genTouch(int x, int y) {
  if (y < SU_Y) return;
  int r = (y - SU_Y) / SU_P, off = (y - SU_Y) % SU_P;
  if (r > 5 || off < 2 || off > 21) return;
  int by = SU_Y + r * SU_P + 2; bool ch = false;
  switch (r) {
    case 0:
      if (hit(x, y, 196, by, 56, 20)) { thai = true; ch = true; } else if (hit(x, y, 258, by, 54, 20)) { thai = false; ch = true; }
      if (ch) { savePrefs(); redraw = true; }
      break;
    case 1:
      for (int k = 0; k < NTHEME; k++) if (hit(x, y, k ? 212 : 108, by, 100, 20) && theme != k) { theme = k; applyTheme(k); savePrefs(); redraw = true; }
      break;
    case 2:
      if (hit(x, y, 196, by, 24, 20)) { shiftRpm = max(4000, shiftRpm - 250); ch = true; }
      else if (hit(x, y, 288, by, 24, 20)) { shiftRpm = min(7000, shiftRpm + 250); ch = true; }
      if (ch) { savePrefs(); genRow(2); }
      break;
    case 3:
      if (hit(x, y, 196, by, 56, 20)) { warnOn = true; ch = true; } else if (hit(x, y, 258, by, 54, 20)) { warnOn = false; ch = true; }
      if (ch) { savePrefs(); genRow(3); }
      break;
    case 4:
      if (hit(x, y, 196, by, 24, 20)) { bright = max(10, bright - 10); ch = true; }
      else if (hit(x, y, 288, by, 24, 20)) { bright = min(100, bright + 10); ch = true; }
      if (ch) { applyBright(); savePrefs(); genRow(4); }
      break;
    case 5:
      if (hit(x, y, 258, by, 54, 20)) testUntil = millis() + 8000;
      break;
  }
}
static void btTouch(int x, int y) {
  if (hit(x, y, 196, 52, 56, 20)) {                                            // Demo
    demoMode = true; linkState = 0; carBuf[0] = 0; savePrefs(); btSrcRow();
  } else if (hit(x, y, 258, 52, 54, 20)) {                                      // OBD BT
    demoMode = false; carBuf[0] = 0; linkState = selAddr[0] ? 1 : 3;
    if (!selAddr[0] && scanState != 1) { scanState = 1; btVer = btVer + 1; scanReq = true; btOff = 0; }   // nothing chosen yet: look around
    savePrefs(); btSrcRow();
  } else if (hit(x, y, 250, 79, 62, 20)) {                                      // Scan
    if (scanState != 1) { scanState = 1; btVer = btVer + 1; scanReq = true; btOff = 0; btDevRow(); }
  } else if (hit(x, y, 278, 122, 38, 40)) {                                     // list up
    if (btOff > 0) { btOff = max(0, btOff - 4); btListDraw(); }
  } else if (hit(x, y, 278, 165, 38, 40)) {                                     // list down
    if (btOff + 4 < nBtView) { btOff += 4; btListDraw(); }
  } else {
    for (int r = 0; r < 4; r++) if (hit(x, y, 4, 122 + r * 21, 270, 19) && btOff + r < nBtView) { btPick(btOff + r); break; }
  }
}
static void setupTouch(int x, int y) {
  if (hit(x, y, 4, 28, 100, 18)) { setupSwitch(0); return; }
  if (hit(x, y, 108, 28, 100, 18)) { setupSwitch(1); return; }
  if (setupTab == 0) genTouch(x, y); else btTouch(x, y);
}

// ---------- warnings ----------
static int warnKind = -1; static bool warnDismissed[3];
static const StrId WLAB[3] = {S_COOLANT, S_OIL, S_BATT}, WMSG[3] = {S_HOT, S_OILHOT, S_LOWV};
static const char* const WUNIT[3] = {"C", "C", "V"};
static char lastWv[12];
static float warnValue() { bool ok; return getVal(warnKind == 0 ? 0x05 : warnKind == 1 ? 0x5C : 0x42, ok); }
static void checkWarn() {
  if (!warnOn) { if (warnKind >= 0) { warnKind = -1; redraw = true; } return; }
  bool ok0, ok1, ok2, okr;
  float v0 = getVal(0x05, ok0), v1 = getVal(0x5C, ok1), v2 = getVal(0x42, ok2), rpm = getVal(0x0C, okr);
  bool cond[3] = {ok0 && v0 >= 105, ok1 && v1 >= 130, ok2 && okr && rpm > 500 && v2 < 11.8f};
  for (int k = 0; k < 3; k++) if (!cond[k]) warnDismissed[k] = false;
  if (warnKind >= 0 && !cond[warnKind]) { warnKind = -1; redraw = true; return; }
  if (warnKind < 0) for (int k = 0; k < 3; k++) if (cond[k] && !warnDismissed[k]) { warnKind = k; redraw = true; break; }
}
static void warnStatic() {
  tft.fillScreen(C(20, 5, 12));
  tft.fillRoundRect(16, 20, 288, 200, 12, cWBG); tft.drawRoundRect(16, 20, 288, 200, 12, cBAD); tft.drawRoundRect(17, 21, 286, 198, 11, cBAD);
  labelC(S_WARNT, 160, 30, cBAD, cWBG); labelC(WLAB[warnKind], 160, 52, cTEXT, cWBG);
  txt(WUNIT[warnKind], 160, 140, 2, cDIM, cWBG, TC_DATUM); labelC(WMSG[warnKind], 160, 156, C(255, 179, 187), cWBG);
  card(100, 180, 120, 32, cBAD, cBAD); labelC(S_OK, 160, 188, cTEXT, cBAD); lastWv[0] = 0;
}
static void warnDyn() {
  char b[12]; fmtv(b, sizeof b, warnValue(), warnKind == 2 ? 1 : 0);
  if (strcmp(b, lastWv)) { txt(b, 160, 98, 6, cTEXT, cWBG, MC_DATUM, 140); strcpy(lastWv, b); }
}

// ---------- screen management ----------
static void fullDraw() {
  if (warnKind >= 0) { warnStatic(); return; }
  tft.fillScreen(cBG); drawTop(); drawNav();
  switch (scr) {
    case SC_DASH: dashStatic(); break; case SC_DATA: dataStatic(); break; case SC_GRAPH: graphStatic(); break;
    case SC_TRIP: tripStatic(); break; case SC_CODES: codesStatic(); break; case SC_SETUP: setupStatic(); break;
  }
}
static void updateScreen() {
  if (warnKind >= 0) { warnDyn(); return; }
  static int lastLink = -1; static char lastCar[24];
  if (lastLink != linkState || strcmp(lastCar, carBuf)) { lastLink = linkState; strcpy(lastCar, carBuf); drawTop(); }
  switch (scr) {
    case SC_DASH: dashDyn(); break; case SC_DATA: dataDyn(); break; case SC_GRAPH: graphDyn(); break;
    case SC_TRIP: tripDyn(); break; case SC_CODES: codesDyn(); break; case SC_SETUP: if (setupTab == 1) btDyn(); break; default: break;
  }
}

// ---------- touch ----------
static void demoCodes() {
  strcpy(codeList[0], "P0171"); strcpy(codeList[1], "P0420"); strcpy(codeList[2], "P0300"); codeCount = 3; codeState = 2; codeVer++;
}
static void handleTouch(int x, int y) {
  if (warnKind >= 0) {
    if (hit(x, y, 100, 180, 120, 32)) { warnDismissed[warnKind] = true; warnKind = -1; testUntil = 0; redraw = true; }
    return;
  }
  if (y >= 210) {
    int i = (x - 4) / 52;
    if (i >= 0 && i < 6 && i != (int)scr) { scr = (Screen)i; redraw = true; updateWants(); }
    return;
  }
  switch (scr) {
    case SC_DASH:
      for (int i = 0; i < 6; i++) if (hit(x, y, TILE_X[i], TILE_Y[i], 88, 50)) {
        slots[i] = (slots[i] + 1) % NMET; savePrefs(); tileStatic(i); updateWants();
      }
      if (hit(x, y, 96, 44, 128, 128) || hit(x, y, 4, 190, 312, 18)) { crpMain = !crpMain; savePrefs(); dashSwap(); }
      break;
    case SC_DATA:
      if (hit(x, y, 4, 28, 70, 18)) { allMode = false; redraw = true; updateWants(); }
      else if (hit(x, y, 78, 28, 90, 18)) { allMode = true; listOff = 0; redraw = true; updateWants(); }
      else if (allMode && hit(x, y, 292, 50, 24, 74)) { listOff = max(0, listOff - 4); memset(rcache, 0, sizeof rcache); }
      else if (allMode && hit(x, y, 292, 128, 24, 74)) { listOff = max(0, min(listOff + 4, nList - 8)); memset(rcache, 0, sizeof rcache); }
      break;
    case SC_GRAPH:
      for (int i = 0; i < 4; i++) if (hit(x, y, 4 + i * 78, 28, 76, 18)) { gsel = i; graphStatic(); }
      break;
    case SC_TRIP:
      if (hit(x, y, 4, 166, 150, 26)) { tDist = tSec = tTop = tFuel = tNow = 0; }
      break;
    case SC_CODES:
      if (hit(x, y, 4, 166, 150, 26)) {
        if (demoMode) demoCodes();
        else if (linkState == 2) { codeState = 1; codeVer++; req = 1; } else { codeCount = 0; codeState = 4; codeVer++; }
      } else if (hit(x, y, 160, 166, 150, 26)) {
        if (demoMode) { codeCount = 0; codeState = 3; mil = false; codeVer++; }
        else if (linkState == 2) { req = 2; } else { codeCount = 0; codeState = 4; codeVer++; }
      }
      break;
    case SC_SETUP: setupTouch(x, y); break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(4, OUTPUT); pinMode(16, OUTPUT); pinMode(17, OUTPUT);   // CYD RGB LED off (active low)
  digitalWrite(4, HIGH); digitalWrite(16, HIGH); digitalWrite(17, HIGH);
  loadPrefs();
  tft.init(); tft.setRotation(1);
#ifdef CYD_INVERT
  tft.invertDisplay(true);
#endif
  tft.fillScreen(cBG);
  ledcSetup(0, 5000, 8); ledcAttachPin(21, 0); applyBright();
  tsSPI.begin(25, 39, 32, 33); ts.begin(tsSPI); ts.setRotation(1);
  analogSetPinAttenuation(CRP_PIN, ADC_11db);
  linkState = demoMode ? 0 : (selAddr[0] ? 1 : 3);
  txt("MAZDA 2", 160, 100, 4, cTEXT, cBG, MC_DATUM);
  txt("SMART GAUGE", 160, 132, 2, cBLUE, cBG, MC_DATUM);
  updateWants(); delay(900);
  xTaskCreatePinnedToCore(obdTask, "obd", 16384, nullptr, 1, nullptr, 0);   // BLE service discovery runs on this stack too
}

void loop() {
  uint32_t now = millis();
  static bool wasTouched = false; static uint32_t lastAct = 0;
  bool t = ts.tirqTouched() && ts.touched();
  if (t) {
    TS_Point p = ts.getPoint();
    int x = constrain(map(p.x, 200, 3700, 1, 320), 0, 319), y = constrain(map(p.y, 240, 3800, 1, 240), 0, 239);
    if (!wasTouched || (scr == SC_DATA && now - lastAct > 400)) { handleTouch(x, y); lastAct = now; }
  }
  wasTouched = t;
  if (pinDirty) { pinDirty = false; savePrefs(); }       // the Classic PIN that worked, found on the OBD core, is stored from here

  static uint32_t tSim = 0, tUI = 0, tGraph = 0;
  if (now - tSim >= 100) {
    float dt = (now - tSim) / 1000.0f; tSim = now;
    if (demoMode) simStep(dt); else crpV = crpV * 0.7f + readCrp() * 0.3f;
    if (demoMode) { mil = codeCount > 0; }
    tripStep(dt); checkWarn();
  }
  if (now - tGraph >= 500) { tGraph = now; graphSample(); }
  if (now - tUI >= 80) {
    tUI = now;
    if (redraw) { redraw = false; fullDraw(); } else updateScreen();
  }
  delay(5);
}
