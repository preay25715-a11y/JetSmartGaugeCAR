// JetOBD2 Smart Gauge - ESP32 CYD (ESP32-2432S028R), 320x240 landscape
// OBD-II over ELM327 (Bluetooth Classic or BLE) for Toyota / Honda / Isuzu / Mitsubishi / Nissan / Mazda (and any OBD-II car),
// demo mode, Thai/English UI, black-blue / black-red themes, boot logo.   Needs ui_assets.h next to this file.
#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <BluetoothSerial.h>
#include <BLEDevice.h>
#include <Preferences.h>
#include <string>
#include "ui_assets.h"

#ifndef CRP_PIN
#define CRP_PIN 35          // input-only ADC1 pin on the CYD P3 connector
#endif
#ifndef CRP_RATIO
#define CRP_RATIO 3.0f      // divider ratio: (Rtop+Rbottom)/Rbottom, keep the pin <= 3.3 V
#endif
#define FW_VERSION "V3.0"

TFT_eSPI tft;
SPIClass tsSPI(VSPI);
XPT2046_Touchscreen ts(33, 36);
BluetoothSerial BT;
Preferences prefs;

constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
static const uint16_t RED_AL = C(255, 48, 56);       // alert red (same in both themes)

// ---------- themes (the background is black in both) ----------
struct Theme { uint16_t bg, panel, panel2, line, dark, acc, accL, dim, text, hot, good, warn, accDim; };
static const int NTHEME = 2;
static const Theme THEMES[NTHEME] = {
  // 0 = black-blue
  {C(0, 0, 0), C(8, 12, 18), C(14, 22, 34), C(30, 40, 56), C(17, 25, 36), C(41, 142, 250), C(126, 200, 255), C(138, 152, 170), C(245, 247, 250), C(255, 64, 64), C(48, 224, 120), C(255, 176, 32), C(12, 40, 74)},
  // 1 = black-red
  {C(0, 0, 0), C(16, 9, 11), C(30, 14, 18), C(60, 32, 38), C(36, 20, 24), C(238, 44, 56), C(255, 150, 156), C(166, 142, 146), C(250, 246, 246), C(255, 176, 32), C(48, 224, 120), C(255, 228, 110), C(72, 16, 22)},
};
static Theme T = THEMES[0];
static int theme = 0;
static void applyTheme(int t) { if (t < 0 || t >= NTHEME) t = 0; theme = t; T = THEMES[t]; }

// ---------- settings ----------
static bool thai = false, demoMode = true, warnOn = true, parkOn = true;
static int shiftRpm = 6500, bright = 80, tankL = 44, fuelCal = 100;
static bool invScreen = false;                   // panel colour polarity (runtime INVON / INVOFF)
static bool crpMain = false;                     // dash centre shows CRP volts (true) or speed (false)
static uint8_t slots[6] = {0, 1, 2, 3, 4, 5};
static bool firstRun = true;                     // nothing saved yet (every re-flash starts here)
static bool engineRan = false, screenOff = false;   // park screen: the engine has run since connecting / the backlight is off
static uint32_t rpmZeroAt = 0;

// ---------- vehicles (Thai market) ----------
enum { VF_D = 1, VF_H = 2, VF_T = 4 };           // diesel, hybrid, turbo
struct Model { uint8_t brand; const char* name; uint8_t flags; uint8_t tank; uint16_t cc; uint8_t redline; };   // redline in 100 rpm
static const char* const BRANDS[6] = {"TOYOTA", "HONDA", "ISUZU", "MITSUBISHI", "NISSAN", "MAZDA"};
static const Model MODELS[] = {
  {0, "VIOS", 0, 42, 1496, 66}, {0, "YARIS", 0, 42, 1197, 66}, {0, "YARIS ATIV", 0, 42, 1197, 66}, {0, "ALTIS", 0, 55, 1798, 66},
  {0, "ALTIS HYBRID", VF_H, 43, 1798, 60}, {0, "CAMRY", 0, 60, 2494, 66}, {0, "CAMRY HYBRID", VF_H, 50, 2487, 60}, {0, "COROLLA CROSS", 0, 47, 1798, 66},
  {0, "FORTUNER", VF_D | VF_T, 80, 2393, 47}, {0, "HILUX REVO", VF_D | VF_T, 80, 2393, 47}, {0, "INNOVA", VF_D | VF_T, 55, 2393, 47}, {0, "COMMUTER", VF_D | VF_T, 70, 2755, 46},
  {1, "CITY", 0, 40, 1497, 68}, {1, "CITY TURBO", VF_T, 40, 998, 62}, {1, "CITY HEV", VF_H, 40, 1498, 62}, {1, "CIVIC", 0, 47, 1799, 68},
  {1, "CIVIC TURBO", VF_T, 47, 1498, 62}, {1, "JAZZ", 0, 40, 1497, 68}, {1, "BRIO", 0, 35, 1199, 66}, {1, "MOBILIO", 0, 42, 1497, 66},
  {1, "BR-V", 0, 42, 1497, 66}, {1, "HR-V", 0, 50, 1799, 66}, {1, "CR-V", 0, 58, 2356, 66}, {1, "ACCORD", 0, 56, 1998, 66},
  {2, "D-MAX 1.9", VF_D | VF_T, 76, 1898, 48}, {2, "D-MAX 3.0", VF_D | VF_T, 76, 2999, 46}, {2, "D-MAX 2.5", VF_D | VF_T, 76, 2499, 46},
  {2, "MU-X 1.9", VF_D | VF_T, 80, 1898, 48}, {2, "MU-X 3.0", VF_D | VF_T, 80, 2999, 46}, {2, "MU-7", VF_D | VF_T, 65, 2999, 46},
  {3, "TRITON 2.4", VF_D | VF_T, 75, 2442, 46}, {3, "TRITON 2.5", VF_D | VF_T, 75, 2477, 46}, {3, "PAJERO SPORT", VF_D | VF_T, 68, 2442, 46},
  {3, "PAJERO 3.2", VF_D | VF_T, 88, 3200, 45}, {3, "MIRAGE", 0, 35, 1193, 66}, {3, "ATTRAGE", 0, 42, 1193, 66},
  {3, "XPANDER", 0, 45, 1499, 66}, {3, "LANCER EX", 0, 59, 1998, 68}, {3, "OUTLANDER PHEV", VF_H, 45, 2360, 58},
  {4, "ALMERA 1.0T", VF_T, 41, 999, 62}, {4, "ALMERA 1.2", 0, 41, 1198, 66}, {4, "MARCH", 0, 41, 1198, 66}, {4, "NOTE", 0, 41, 1198, 66},
  {4, "NOTE E-POWER", VF_H, 41, 1198, 60}, {4, "KICKS E-POWER", VF_H, 41, 1198, 60}, {4, "SYLPHY", 0, 52, 1598, 66}, {4, "TEANA", 0, 68, 1997, 66},
  {4, "X-TRAIL", 0, 60, 1997, 66}, {4, "NAVARA", VF_D | VF_T, 80, 2488, 45}, {4, "TERRA", VF_D | VF_T, 80, 2298, 45},
  {5, "MAZDA 2", 0, 44, 1496, 66}, {5, "MAZDA 2 DIESEL", VF_D | VF_T, 44, 1498, 52}, {5, "MAZDA 3", 0, 51, 1998, 66}, {5, "CX-3", 0, 48, 1998, 66},
  {5, "CX-30", 0, 51, 1998, 66}, {5, "CX-5", 0, 56, 1998, 66}, {5, "CX-5 DIESEL", VF_D | VF_T, 58, 2191, 52}, {5, "CX-8", VF_D | VF_T, 74, 2191, 52},
  {5, "MAZDA 6", 0, 62, 1998, 66}, {5, "BT-50 1.9", VF_D | VF_T, 80, 1898, 48}, {5, "BT-50 3.2", VF_D | VF_T, 80, 3198, 46}, {5, "MX-5", 0, 45, 1998, 72},
};
static const int NMODEL = sizeof(MODELS) / sizeof(MODELS[0]);
static int vehModel = -1;                        // chosen model (index into MODELS) or -1 = automatic (VIN)

// VIN world-manufacturer codes -> brand (0..5 = a brand above, 100+ = another make that is only named)
struct Wmi { const char* w; uint8_t brand; };
static const char* const OTHER[] = {"FORD", "SUZUKI", "MG", "CHEVROLET", "SUBARU", "BMW", "HINO", "GWM", "BENZ", "VOLVO", "KIA", "HYUNDAI"};
static const Wmi WMIS[] = {
  {"MR0", 0}, {"MR1", 0}, {"MR2", 0}, {"JT", 0}, {"4T1", 0}, {"2T1", 0}, {"5TD", 0}, {"MHF", 0}, {"SB1", 0},
  {"MRH", 1}, {"JHM", 1}, {"JH1", 1}, {"1HG", 1}, {"2HG", 1}, {"19X", 1}, {"SHH", 1}, {"MHR", 1}, {"LHG", 1},
  {"MPA", 2}, {"MP1", 2}, {"JAA", 2}, {"JAB", 2}, {"JAL", 2}, {"JAC", 2},
  {"MMA", 3}, {"MMB", 3}, {"MMC", 3}, {"MMD", 3}, {"MMT", 3}, {"ML3", 3}, {"JA3", 3}, {"JA4", 3}, {"JA7", 3}, {"JMB", 3}, {"JMY", 3}, {"JMA", 3},
  {"MNT", 4}, {"MDH", 4}, {"JN1", 4}, {"JN6", 4}, {"JN8", 4}, {"SJN", 4}, {"3N1", 4}, {"1N4", 4},
  {"MM0", 5}, {"MM6", 5}, {"MM7", 5}, {"MM8", 5}, {"MP2", 5}, {"JM1", 5}, {"JM2", 5}, {"JM6", 5}, {"JM7", 5}, {"JMZ", 5}, {"JM0", 5}, {"3MZ", 5}, {"3MV", 5},
  {"MNA", 100}, {"MNB", 100}, {"MNC", 100}, {"MPB", 100}, {"MMS", 101}, {"MML", 102}, {"MMM", 103}, {"MMR", 104}, {"MMF", 105}, {"WBA", 105},
  {"MNK", 106}, {"MNU", 107}, {"WDB", 108}, {"WDD", 108}, {"YV1", 109}, {"KNA", 110}, {"KMH", 111},
};
static int brandFromVin(const char* vin) {
  if (strlen(vin) < 3) return -1;
  for (const Wmi& w : WMIS) { if (!strncmp(vin, w.w, strlen(w.w))) return w.brand; }
  return -1;
}
static const char* brandName(int b) { return b >= 0 && b < 6 ? BRANDS[b] : (b >= 100 && b - 100 < (int)(sizeof(OTHER) / sizeof(OTHER[0])) ? OTHER[b - 100] : ""); }
// model name from the VIN where the code is known (Mazda Thailand: characters 4-5)
static const char* modelFromVin(int brand, const char* vin) {
  if (brand != 5 || strlen(vin) < 11) return "";
  char a = vin[3], b = vin[4];
  if (a == 'D' && (b == 'J' || b == 'E' || b == 'W')) return "MAZDA 2";
  if (a == 'B' && strchr("MNPLK", b)) return "MAZDA 3";
  if (a == 'G' && (b == 'J' || b == 'H' || b == 'L')) return "MAZDA 6";
  if (a == 'K' && (b == 'E' || b == 'F')) return "CX-5";
  if (a == 'D' && b == 'K') return "CX-3";
  if (a == 'D' && b == 'M') return "CX-30";
  if (a == 'N' && b == 'D') return "MX-5";
  return "";
}

// ---------- PID table (Mode 01) ----------
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
  {0x24, "O2 B1S1 lambda", "", 2, 0.97f, 1.03f, 'o'},
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
  {0x51, "Fuel type", "", 0, 1, 1, 'c'}, {0x52, "Ethanol fuel", "%", 0, 0, 0, 'c'},
  {0x59, "Fuel rail pressure (abs)", "kPa", 0, 6000, 6500, 'c'}, {0x5A, "Relative accel pedal", "%", 0, 5, 55, 'l'},
  {0x5B, "Hybrid battery life", "%", 0, 60, 60, 'c'},
  {0x5C, "Engine oil temp", "C", 0, 35, 92, 't'}, {0x5D, "Fuel injection timing", "deg", 1, -2, 8, 'l'},
  {0x5E, "Engine fuel rate", "L/h", 1, 0.8f, 9, 'l'}, {0x61, "Driver demand torque", "%", 0, 5, 60, 'l'},
  {0x62, "Actual engine torque", "%", 0, 5, 60, 'l'}, {0x63, "Engine reference torque", "Nm", 0, 130, 130, 'c'},
  {0x78, "Exhaust gas temp B1S1", "C", 0, 120, 480, 't'},
};
static const int NPID = sizeof(PIDS) / sizeof(PIDS[0]);

static float pv[256];
static bool psup[256]; static uint8_t want[256];     // want: 0 no, 1 background, 2 on screen
static uint32_t page[256];
static volatile int linkState = 0;        // 0 demo, 1 connecting, 2 connected, 3 no adapter chosen
static volatile bool mil = false;
static volatile int dtcCount = 0;
static volatile int protoNum = 0;         // ELM327 protocol number in use (0 = unknown)
static volatile bool ecuDiesel = false, ecuDieselKnown = false, ecuHybrid = false;
static char vinBuf[20] = "";
static int vinBrand = -1;
static char carBuf[28] = "";              // short name for the header (auto: from VIN)
static uint32_t testUntil = 0;
static char ecuName[24] = "", calId[20] = "";
static volatile int ecuInfoVer = 0;

// fault codes shared between UI and OBD task
struct DtcRec { char code[6]; uint8_t pend; };
static DtcRec codeList[24];
static volatile int codeCount = 0, codeState = 0, codeVer = 0, req = 0;
// readiness monitors (PID 01): bit set = supported / bit set = not complete (see updateMonitors)
static volatile uint32_t monRaw = 0; static volatile bool monValid = false; static volatile int monVer = 0;
// freeze frame (Mode 02)
static char frzCode[6] = ""; static float frzVal[12]; static bool frzOk[12]; static volatile int frzState = 0, frzVer = 0;   // 0 none, 1 reading, 2 done, 3 no data
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

static void btUp() { if (!btReady) { BT.begin("JETOBD-GAUGE", true); BT.setPin(PINS[pinIdx]); btReady = true; } }

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
  BLEDevice::init("JETOBD-GAUGE");
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

static float decodePid(uint8_t id, const uint8_t* b, int n) {
  float A = b[0], B = n > 1 ? b[1] : 0, Cc = n > 2 ? b[2] : 0, D = n > 3 ? b[3] : 0, AB = A * 256 + B;
  switch (id) {
    case 0x04: case 0x11: case 0x2C: case 0x2E: case 0x2F: case 0x45: case 0x47: case 0x49: case 0x4A:
    case 0x4C: case 0x52: case 0x5A: case 0x5B: return A * 100 / 255;
    case 0x05: case 0x0F: case 0x46: case 0x5C: return A - 40;
    case 0x06: case 0x07: case 0x08: case 0x09: return (A - 128) * 100 / 128;
    case 0x0A: return A * 3;
    case 0x0C: return AB / 4;
    case 0x0E: return A / 2 - 64;
    case 0x10: return AB / 100;
    case 0x14: case 0x15: return A / 200;
    case 0x1F: case 0x21: case 0x31: case 0x4D: case 0x4E: case 0x63: return AB;
    case 0x22: return AB * 0.079f;
    case 0x23: case 0x59: return AB * 10;
    case 0x24: case 0x44: return AB * 2 / 65536;
    case 0x2D: return A * 100 / 128 - 100;
    case 0x32: return (float)(int16_t)(uint16_t)AB / 4;
    case 0x34: return (Cc * 256 + D) / 256 - 128;
    case 0x3C: case 0x3E: return AB / 10 - 40;
    case 0x42: return AB / 1000;
    case 0x43: return AB * 100 / 255;
    case 0x5D: return AB / 128 - 210;
    case 0x5E: return AB * 0.05f;
    case 0x61: case 0x62: return A - 125;
    case 0x78: return (n >= 3 ? (B * 256 + Cc) : 0) / 10 - 40;     // exhaust gas temperature, sensor 1 of bank 1
    default: return A;
  }
}

static float getVal(uint8_t pid, bool& ok) {
  if (pid == 0x05 && millis() < testUntil) { ok = true; return 108; }
  if (demoMode) { ok = true; return pv[pid]; }
  ok = (linkState == 2) && psup[pid] && (millis() - page[pid] < 8000);
  return pv[pid];
}

// ---------- ELM327 communication (runs in obdTask) ----------
static int linkAvail(); static int linkRead(); static void linkSend(const char* cmd); static bool linkIsUp();   // Classic or BLE, see the Bluetooth section
static bool abortWait() { return scanReq || reconnectReq || demoMode || !linkIsUp(); }   // the user asked for something else, or the link dropped
static String elmQuery(const char* cmd, uint32_t tmo = 1500) {
  while (linkAvail()) linkRead();
  linkSend(cmd);
  String r; uint32_t t0 = millis();
  while (millis() - t0 < tmo) {
    while (linkAvail()) { char c = (char)linkRead(); if (c == '>') return r; r += c; }
    if (abortWait()) break;
    vTaskDelay(1);
  }
  return r;
}

// One reply = a list of messages (one per ECU / frame group). Handles the CAN multi-frame layout
// ("014" then "0: ..." "1: ...") and the K-line layout (one short line per frame).
struct Msg { uint8_t b[48]; uint8_t n; };
static int hv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static bool isHexStr(const char* s, int n) { for (int i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return false; return n > 0; }
static int parseMsgs(const String& raw, Msg* out, int maxm) {
  int nm = 0, i = 0, len = (int)raw.length(), mf = -1, expect = 0;
  while (i < len) {
    int e = i;
    while (e < len && raw[e] != '\r' && raw[e] != '\n') e++;
    char ln[96]; int k = 0;
    for (int j = i; j < e && k < 94; j++) if (raw[j] != ' ') ln[k++] = raw[j];
    ln[k] = 0; i = e + 1;
    if (!k) continue;
    const char* p = ln; int pl = k; bool frame = false;
    if (pl >= 2 && ln[1] == ':') { frame = true; p = ln + 2; pl = k - 2; }       // "0:" ... "F:" continuation lines
    if (!isHexStr(p, pl)) continue;                                              // SEARCHING..., NO DATA, BUS INIT, echoes ...
    if (frame) {
      if (mf < 0) { if (nm >= maxm) continue; mf = nm++; out[mf].n = 0; expect = 255; }
      Msg& m = out[mf];
      for (int j = 0; j + 1 < pl && m.n < (int)sizeof m.b; j += 2) m.b[m.n++] = (uint8_t)((hv(p[j]) << 4) | hv(p[j + 1]));
      if (m.n >= expect) { m.n = (uint8_t)expect; mf = -1; }
      continue;
    }
    mf = -1;
    if (pl == 3) { if (nm < maxm) { mf = nm++; out[mf].n = 0; expect = (int)strtol(ln, nullptr, 16); if (expect > (int)sizeof out[mf].b) expect = (int)sizeof out[mf].b; } continue; }
    if ((pl & 1) || nm >= maxm) continue;
    Msg& m = out[nm++]; m.n = 0;
    for (int j = 0; j + 1 < pl && m.n < (int)sizeof m.b; j += 2) m.b[m.n++] = (uint8_t)((hv(p[j]) << 4) | hv(p[j + 1]));
  }
  return nm;
}
static const int MAXM = 8;
static Msg msgs[MAXM];                                  // used by the OBD task only

static bool queryPid(uint8_t pid, uint8_t* b, int& n, uint32_t tmo = 1200, int mode = 1, int frame = -1) {
  char cmd[12];
  if (mode == 2) snprintf(cmd, sizeof cmd, "02%02X%02X", pid, frame < 0 ? 0 : frame); else snprintf(cmd, sizeof cmd, "%02X%02X", mode, pid);
  int nm = parseMsgs(elmQuery(cmd, tmo), msgs, MAXM);
  for (int i = 0; i < nm; i++) {
    const Msg& m = msgs[i];
    int off = mode == 2 ? 3 : 2;
    if (m.n > off && m.b[0] == 0x40 + mode && m.b[1] == pid) { n = m.n - off; if (n > 9) n = 9; memcpy(b, m.b + off, n); return n > 0; }
  }
  return false;
}
// support bitmaps: every ECU that answers is OR-ed together (engine + gearbox ECU...)
static bool queryBitmap(uint8_t pid, uint8_t* b4, uint32_t tmo) {
  char cmd[8]; snprintf(cmd, sizeof cmd, "01%02X", pid);
  int nm = parseMsgs(elmQuery(cmd, tmo), msgs, MAXM); bool any = false;
  memset(b4, 0, 4);
  for (int i = 0; i < nm; i++) {
    const Msg& m = msgs[i];
    if (m.n >= 6 && m.b[0] == 0x41 && m.b[1] == pid) { for (int k = 0; k < 4; k++) b4[k] |= m.b[2 + k]; any = true; }
  }
  return any;
}

static void discoverPids() {
  memset(psup, 0, sizeof psup);
  for (int base = 0; base < 0xE0; base += 0x20) {
    uint8_t b[4];
    if (!queryBitmap((uint8_t)base, b, base == 0 ? 8000 : 2000)) { if (base == 0) break; else continue; }
    for (int i = 0; i < 32; i++) if (b[i / 8] & (0x80 >> (i % 8))) psup[base + i + 1] = true;
    if (!(b[3] & 1)) break;
  }
}

static bool elmAlive = false;                // set by elmInit: the adapter answered like an ELM327 (so the link itself is fine, even if the car is off)
static int lastProto = 0;                    // protocol that worked last time (saved), tried first
static int userProto = 0;                    // 0 = automatic, 1..9 = bus protocol forced from Setup 2 (saved)
static volatile bool protoDirty = false;
static bool replyIs0100(const String& r) {
  int nm = parseMsgs(r, msgs, MAXM);
  for (int i = 0; i < nm; i++) if (msgs[i].n >= 2 && msgs[i].b[0] == 0x41 && msgs[i].b[1] == 0x00) return true;
  return false;
}
static bool tryProto(int p, uint32_t tmo) {
  char c[10]; snprintf(c, sizeof c, "ATSP%d", p);
  elmQuery(c, 1500);
  return replyIs0100(elmQuery("0100", tmo));
}
static bool elmInit() {
  String z = elmQuery("ATZ", 3500), e = elmQuery("ATE0");
  elmAlive = z.indexOf("ELM") >= 0 || e.indexOf("OK") >= 0;
  if (!elmAlive) { e = elmQuery("ATE0"); elmAlive = e.indexOf("OK") >= 0; }
  elmQuery("ATL0"); elmQuery("ATS0"); elmQuery("ATH0");
  protoNum = 0;
  bool ok = false, forced = userProto >= 1 && userProto <= 9;
  if (forced) ok = tryProto(userProto, userProto >= 6 ? 9000 : 15000);                                    // protocol chosen by the user
  if (!ok && !forced && lastProto >= 1 && lastProto <= 9) ok = tryProto(lastProto, lastProto >= 6 ? 7000 : 12000);   // the protocol that worked before: quick start
  if (!ok && !forced) { elmQuery("ATSP0", 1500); ok = replyIs0100(elmQuery("0100", 22000)); }             // automatic search (K-line slow init takes a few seconds per protocol)
  static int initFails = 0;
  if (!ok && !forced && elmAlive && !abortWait() && (initFails++ % 4) == 0) {                                          // manual sweep (first try, then every 4th), most common protocols first
    static const uint8_t SW[] = {6, 7, 5, 4, 3, 8, 9, 2, 1};
    for (unsigned i = 0; i < sizeof SW && !ok; i++) {
      if (SW[i] == lastProto) continue;
      if (tryProto(SW[i], SW[i] >= 6 ? 4000 : 9000)) { ok = true; break; }
      if (abortWait()) break;
    }
  }
  if (!ok) return false;
  initFails = 0;
  String d = elmQuery("ATDPN", 1000); d.trim();
  int p = 0;
  for (unsigned i = 0; i < d.length(); i++) { char c = d[i]; if (c >= '1' && c <= '9') { p = c - '0'; break; } }
  protoNum = p;
  if (p >= 3 && p <= 5) elmQuery("ATST64", 1000);            // slow K-line ECUs: allow 400 ms for an answer
  if (p && p != lastProto) { lastProto = p; protoDirty = true; }
  return true;
}

// ---------- vehicle identification ----------
static void collectAscii(int nm, uint8_t pid, char* dst, size_t cap, bool vinOnly) {
  size_t k = 0; dst[0] = 0;
  for (int i = 0; i < nm; i++) {
    const Msg& m = msgs[i];
    if (m.n < 4 || m.b[0] != 0x49 || m.b[1] != pid) continue;
    for (int j = 3; j < m.n && k + 1 < cap; j++) {
      uint8_t c = m.b[j];
      bool ok = vinOnly ? ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) : (c >= 0x20 && c < 0x7F);
      if (ok) dst[k++] = (char)c;
    }
  }
  dst[k] = 0;
}
static void readVin() {
  vinBuf[0] = 0; vinBrand = -1; carBuf[0] = 0;
  int nm = parseMsgs(elmQuery("0902", 5000), msgs, MAXM);
  collectAscii(nm, 0x02, vinBuf, sizeof vinBuf, true);
  if (strlen(vinBuf) < 11) { vinBuf[0] = 0; return; }
  vinBrand = brandFromVin(vinBuf);
  const char* mdl = modelFromVin(vinBrand, vinBuf);
  const char* br = brandName(vinBrand);
  if (mdl[0]) snprintf(carBuf, sizeof carBuf, "%s", mdl);
  else if (br[0]) snprintf(carBuf, sizeof carBuf, "%s", br);
  else snprintf(carBuf, sizeof carBuf, "VIN %.3s", vinBuf);
}
static void doEcuInfo() {
  int nm = parseMsgs(elmQuery("090A", 4000), msgs, MAXM);
  collectAscii(nm, 0x0A, ecuName, sizeof ecuName, false);
  nm = parseMsgs(elmQuery("0904", 4000), msgs, MAXM);
  collectAscii(nm, 0x04, calId, sizeof calId, false);
  ecuInfoVer = ecuInfoVer + 1;
}

// ---------- trouble codes ----------
static void addCode(uint8_t hi, uint8_t lo, bool pend) {
  if (!hi && !lo) return;
  char c[6]; snprintf(c, sizeof c, "%c%u%X%02X", "PCBU"[hi >> 6], (hi >> 4) & 3, hi & 0xF, lo);
  for (int i = 0; i < codeCount; i++) if (!strcmp(codeList[i].code, c)) return;
  if (codeCount < 24) { strcpy(codeList[codeCount].code, c); codeList[codeCount].pend = pend; codeCount = codeCount + 1; }
}
static bool parseCodeMsgs(int nm, uint8_t rsp, bool pend) {     // true if at least one ECU answered with a code list
  bool got = false;
  for (int i = 0; i < nm; i++) {
    const Msg& m = msgs[i];
    if (m.n < 1 || m.b[0] != rsp) continue;
    got = true;
    int off = ((m.n - 1) & 1) ? 2 : 1;                           // CAN replies start with a count byte (odd length), K-line frames do not
    for (int k = off; k + 1 < m.n; k += 2) addCode(m.b[k], m.b[k + 1], pend);
  }
  return got;
}
static void doReadCodes() {
  codeState = 1; codeCount = 0; codeVer = codeVer + 1;
  String r = elmQuery("03", 4500);
  int nm = parseMsgs(r, msgs, MAXM);
  bool got = parseCodeMsgs(nm, 0x43, false);
  if (!got && r.indexOf("NO DATA") < 0) { codeState = 4; codeVer = codeVer + 1; return; }   // "NO DATA" = nothing stored (some ECUs); silence = no link
  r = elmQuery("07", 4500);
  nm = parseMsgs(r, msgs, MAXM);
  parseCodeMsgs(nm, 0x47, true);
  codeState = 2; codeVer = codeVer + 1;
}
static void doClearCodes() {
  int nm = parseMsgs(elmQuery("04", 4500), msgs, MAXM); bool ok = false;
  for (int i = 0; i < nm; i++) if (msgs[i].n >= 1 && msgs[i].b[0] == 0x44) ok = true;
  if (ok) { codeCount = 0; codeState = 3; mil = false; monValid = false; } else codeState = 4;
  codeVer = codeVer + 1;
}
static const uint8_t FRZ_PID[12] = {0x0C, 0x0D, 0x05, 0x04, 0x06, 0x07, 0x0B, 0x0F, 0x11, 0x10, 0x0E, 0x42};
static void doFreeze() {
  frzState = 1; frzCode[0] = 0; frzVer = frzVer + 1;
  for (int i = 0; i < 12; i++) frzOk[i] = false;
  uint8_t b[9]; int n = 0;
  if (!queryPid(0x02, b, n, 3000, 2, 0) || n < 2 || (b[0] == 0 && b[1] == 0)) { frzState = 3; frzVer = frzVer + 1; return; }
  snprintf(frzCode, sizeof frzCode, "%c%u%X%02X", "PCBU"[b[0] >> 6], (b[0] >> 4) & 3, b[0] & 0xF, b[1]);
  for (int i = 0; i < 12; i++) {
    if (abortWait()) return;
    if (queryPid(FRZ_PID[i], b, n, 2500, 2, 0)) { frzVal[i] = decodePid(FRZ_PID[i], b, n); frzOk[i] = true; }
  }
  frzState = 2; frzVer = frzVer + 1;
}

static void pollPid(uint8_t pid, int& fails) {
  uint8_t b[9]; int n = 0;
  if (queryPid(pid, b, n)) {
    pv[pid] = decodePid(pid, b, n); page[pid] = millis(); fails = 0;
    if (pid == 0x01 && n >= 4) {
      mil = (b[0] & 0x80) != 0; dtcCount = b[0] & 0x7F;
      monRaw = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; monValid = true; monVer = monVer + 1;
      ecuDiesel = (b[1] & 0x08) != 0; ecuDieselKnown = true;            // SAE J1979: PID 01 byte B bit 3 = compression ignition
    }
    if (pid == 0x51) { int f = b[0]; if (f >= 0x10 && f <= 0x15) ecuHybrid = true; if (f == 4 || f == 0x12) { ecuDiesel = true; ecuDieselKnown = true; } }
  } else fails++;
}

static void detectVehicle() {
  uint8_t b[9]; int n = 0;
  ecuHybrid = psup[0x5B]; ecuDieselKnown = false; ecuDiesel = false;
  int f = 0;
  pollPid(0x01, f);
  if (psup[0x51]) pollPid(0x51, f);
  readVin();
}

static int nextWanted(int& rr, int tier) {
  for (int i = 0; i < 255; i++) {
    rr = (rr % 255) + 1;
    if (psup[rr] && want[rr] == tier && rr != 0x0C && rr != 0x0D && rr != 0x01) return rr;
  }
  return -1;
}

void obdTask(void*) {
  int rrFg = 0, rrBg = 0, fails = 0, iter = 0; uint32_t lastMil = 0;
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
      if (!elmInit()) {
        if (sb && !elmAlive && gen == selGen) bleTry = bleTry + 1;      // silent: try the next GATT candidate (an ELM that talks but has no car yet is not a reason)
        if (!(scanReq || reconnectReq || demoMode)) { linkDown(); vTaskDelay(2000); }
        continue;
      }
      if (!sb && gen == selGen && (!pinOk || pinIdx != pinSaved)) { pinOk = true; pinDirty = true; }   // this PIN works: remember it
      discoverPids(); detectVehicle(); fails = 0; linkState = 2; lastMil = millis();
    }
    while (!demoMode && linkIsUp() && fails < 8 && !scanReq && !reconnectReq) {
      if (req == 1) { req = 0; doReadCodes(); }
      else if (req == 2) { req = 0; doClearCodes(); }
      else if (req == 3) { req = 0; doFreeze(); }
      else if (req == 4) { req = 0; doEcuInfo(); }
      pollPid(0x0C, fails); pollPid(0x0D, fails);
      int others = protoNum >= 6 || protoNum == 0 ? 3 : 2;               // K-line answers are slower: fewer other PIDs per round keeps rpm/speed lively
      for (int k = 0; k < others; k++) {
        bool bg = (iter % 4 == 3 && k == 0);                              // PIDs that are on screen are polled 3 rounds out of 4, background ones (alerts, trip, graphs) in between
        int p = nextWanted(bg ? rrBg : rrFg, bg ? 1 : 2);
        if (p < 0) p = nextWanted(bg ? rrFg : rrBg, bg ? 2 : 1);
        if (p >= 0) pollPid((uint8_t)p, fails);
      }
      iter++;
      if (millis() - lastMil > 5000) { pollPid(0x01, fails); lastMil = millis(); }
      vTaskDelay(1);
    }
    linkDown();
    linkState = 1; if (!scanReq && !reconnectReq) vTaskDelay(1000);
  }
}

// ---------- demo simulation (a petrol hatchback) ----------
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
  pv[0x0C] = rpm; pv[0x0D] = spd; pv[0x44] = 1.0f + 0.03f * sinf(simT * 3); pv[0x24] = pv[0x44]; pv[0x5C] = 35 + 57 * w;
  pv[0x51] = 1; psup[0x5B] = false; psup[0x78] = false; psup[0x59] = false;
  pv[0x42] = 13.9f + 0.4f * sinf(simT * 0.7f);
  crpV = fmaxf(0, .8f + fminf(rpm / 7000, 1) * 3.6f + sinf(simT * 40) * .15f);
}
static float readCrp() {
  uint32_t s = 0;
  for (int i = 0; i < 32; i++) s += analogReadMilliVolts(CRP_PIN);
  return (s / 32.0f) / 1000.0f * CRP_RATIO;
}

// =====================================================================================
//  Text engine: anti-aliased fonts (4-bit alpha) from ui_assets.h, drawn through one RGB565 buffer so nothing flickers
// =====================================================================================
enum FontId : uint8_t { F_NXL, F_NHL, F_NL, F_NM, F_NS, F_UL, F_UM, F_US, F_LS, F_LM, F_NFONT };
static const FontRec* const FNT[F_NFONT] = {&FONT_NXL, &FONT_NHL, &FONT_NL, &FONT_NM, &FONT_NS, &FONT_UL, &FONT_UM, &FONT_US, &FONT_LS, &FONT_LM};
enum { AL_L = 0, AL_C = 1, AL_R = 2 };
#define DEGC "\x7F" "C"
#define DEG "\x7F"

static const int TB_CAP = 7000;
static uint16_t tPix[TB_CAP];

static inline uint16_t mix565(uint16_t b, uint16_t f, int a) {          // a = 0..15 : coverage of f over b
  if (a >= 15) return f;
  if (a <= 0) return b;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31, fr = (f >> 11) & 31, fg = (f >> 5) & 63, fb = f & 31;
  return (uint16_t)(((br + (fr - br) * a / 15) << 11) | ((bg + (fg - bg) * a / 15) << 5) | (bb + (fb - bb) * a / 15));
}
static const GlyphRec* glyphOf(const FontRec* f, unsigned char ch) {
  if (f == &FONT_UL || f == &FONT_UM || f == &FONT_US) { if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 32); }   // these fonts are capitals only
  if (ch < 32 || ch > 127) return nullptr;
  uint8_t gi = pgm_read_byte(&f->map[ch - 32]);
  return gi == 255 ? nullptr : &f->g[gi];
}
static void blit4(const uint8_t* bm, int w, int h, int px, int py, int bw, int bh, uint16_t fg) {
  for (int r = 0; r < h; r++) {
    int yy = py + r; if (yy < 0 || yy >= bh) continue;
    for (int c = 0; c < w; c++) {
      int xx = px + c; if (xx < 0 || xx >= bw) continue;
      int idx = r * w + c; uint8_t v = pgm_read_byte(&bm[idx >> 1]); v = (idx & 1) ? (v & 15) : (v >> 4);
      if (v) { uint16_t& d = tPix[yy * bw + xx]; d = mix565(d, fg, v); }
    }
  }
}

struct Run { FontId f; const char* s; uint16_t fg; int gap; };           // gap = extra pixels before this run
static int runAdv(const Run& r) { const FontRec* f = FNT[r.f]; int w = 0; for (const char* p = r.s; *p; p++) { const GlyphRec* g = glyphOf(f, (unsigned char)*p); w += g ? g->adv : 4; } return w; }
static int textW(FontId f, const char* s) { Run r = {f, s, 0, 0}; return runAdv(r); }

// draws runs side by side on one baseline.  al: anchor x is the left / centre / right edge.  minW: area that is always repainted with bg
static void drawRuns(const Run* rs, int nr, int x, int by, uint16_t bg, int al, int minW) {
  int adv = 0, inkL = 0, inkR = 0, asc = 0, desc = 0;
  for (int i = 0; i < nr; i++) {
    const FontRec* f = FNT[rs[i].f]; adv += rs[i].gap;
    asc = max(asc, (int)f->asc); desc = max(desc, (int)f->line - (int)f->asc);
    for (const char* p = rs[i].s; *p; p++) {
      const GlyphRec* g = glyphOf(f, (unsigned char)*p);
      if (!g) { adv += 4; continue; }
      if (g->w) { inkL = min(inkL, adv + g->dx); inkR = max(inkR, adv + g->dx + g->w); }
      adv += g->adv; inkR = max(inkR, adv);
    }
  }
  int ox = al == AL_C ? x - adv / 2 : al == AL_R ? x - adv : x;
  int x0 = ox + inkL, x1 = ox + inkR;
  if (minW > 0) {
    int a0 = al == AL_C ? x - minW / 2 : al == AL_R ? x - minW : x;
    x0 = min(x0, a0); x1 = max(x1, a0 + minW);
  }
  int w = x1 - x0, h = asc + desc, top = by - asc;
  if (w <= 0 || h <= 0 || w * h > TB_CAP) return;
  for (int i = 0; i < w * h; i++) tPix[i] = bg;
  int pen = ox - x0;
  for (int i = 0; i < nr; i++) {
    const FontRec* f = FNT[rs[i].f]; pen += rs[i].gap;
    for (const char* p = rs[i].s; *p; p++) {
      const GlyphRec* g = glyphOf(f, (unsigned char)*p);
      if (!g) { pen += 4; continue; }
      if (g->w) blit4(&f->bm[g->off], g->w, g->h, pen + g->dx, (by + g->dy) - top, w, h, rs[i].fg);
      pen += g->adv;
    }
  }
  tft.pushImage(x0, top, w, h, tPix);
}
static void txt(FontId f, const char* s, int x, int by, uint16_t fg, uint16_t bg, int al = AL_L, int minW = 0) {
  Run r = {f, s, fg, 0}; drawRuns(&r, 1, x, by, bg, al, minW);
}
// value + small unit on one line (the unit sits on the same baseline)
static void txtVU(FontId f, const char* v, FontId uf, const char* u, int x, int by, uint16_t fg, uint16_t ufg, uint16_t bg, int al, int minW) {
  Run r[2] = {{f, v, fg, 0}, {uf, u, ufg, 2}};
  drawRuns(r, u && u[0] ? 2 : 1, x, by, bg, al, minW);
}

// Thai (or English) fixed labels by id
struct LabKind { FontId en; uint8_t th; };               // th: 0 = small Thai bitmaps, 1 = large
static const LabKind LB_TILE = {F_LS, 0}, LB_ROW = {F_LM, 0}, LB_BTN = {F_UL, 1}, LB_BTNM = {F_UM, 0}, LB_SM = {F_US, 0};
static inline int tby(int by) { return thai ? by + 3 : by; }       // small Thai labels are taller than the English ones: sit them 3 px lower
static const ThBmp* thOf(StrId id, int sz) { const ThBmp* t = (sz ? THL : THS) + id; return pgm_read_ptr(&t->d) ? t : nullptr; }
static int thWidth(StrId id, int sz) { const ThBmp* t = thOf(id, sz); return t ? pgm_read_byte(&t->w) : 0; }
static bool thaiOn(StrId id, int sz) { return thai && thOf(id, sz); }
static int labW(StrId id, LabKind k) { return thaiOn(id, k.th) ? thWidth(id, k.th) : textW(k.en, EN[id]); }
static void lab(StrId id, LabKind k, int x, int by, uint16_t fg, uint16_t bg, int al = AL_L, int minW = 0) {
  if (!thaiOn(id, k.th)) { txt(k.en, EN[id], x, by, fg, bg, al, minW); return; }
  const ThBmp* t = thOf(id, k.th); int w = pgm_read_byte(&t->w), H = k.th ? THL_H : THS_H, BASE = k.th ? THL_BASE : THS_BASE;
  const uint8_t* bm = (const uint8_t*)pgm_read_ptr(&t->d);
  int ox = al == AL_C ? x - w / 2 : al == AL_R ? x - w : x, x0 = ox, x1 = ox + w;
  if (minW > 0) { int a0 = al == AL_C ? x - minW / 2 : al == AL_R ? x - minW : x; x0 = min(x0, a0); x1 = max(x1, a0 + minW); }
  int bw = x1 - x0;
  if (bw <= 0 || bw * H > TB_CAP) return;
  for (int i = 0; i < bw * H; i++) tPix[i] = bg;
  blit4(bm, w, H, ox - x0, 0, bw, H, fg);
  tft.pushImage(x0, by - BASE, bw, H, tPix);
}

// =====================================================================================
//  Shapes: chamfered panels, brackets, hexagons, segmented bars
// =====================================================================================
enum { CUT_TL = 1, CUT_TR = 2, CUT_BR = 4, CUT_BL = 8 };
static void panel(int x, int y, int w, int h, int ch, int cuts, uint16_t fill, uint16_t border) {
  tft.fillRect(x, y + ch, w, h - 2 * ch, fill);
  tft.fillRect(x + ch, y, w - 2 * ch, ch, fill);
  tft.fillRect(x + ch, y + h - ch, w - 2 * ch, ch, fill);
  if (cuts & CUT_TL) tft.fillTriangle(x + ch, y, x + ch, y + ch, x, y + ch, fill); else tft.fillRect(x, y, ch, ch, fill);
  if (cuts & CUT_TR) tft.fillTriangle(x + w - 1 - ch, y, x + w - 1 - ch, y + ch, x + w - 1, y + ch, fill); else tft.fillRect(x + w - ch, y, ch, ch, fill);
  if (cuts & CUT_BR) tft.fillTriangle(x + w - 1 - ch, y + h - 1, x + w - 1 - ch, y + h - 1 - ch, x + w - 1, y + h - 1 - ch, fill); else tft.fillRect(x + w - ch, y + h - ch, ch, ch, fill);
  if (cuts & CUT_BL) tft.fillTriangle(x + ch, y + h - 1, x + ch, y + h - 1 - ch, x, y + h - 1 - ch, fill); else tft.fillRect(x, y + h - ch, ch, ch, fill);
  if (border == fill) return;
  int xl = x, xr = x + w - 1, yt = y, yb = y + h - 1;
  tft.drawFastHLine(x + ((cuts & CUT_TL) ? ch : 0), yt, w - ((cuts & CUT_TL) ? ch : 0) - ((cuts & CUT_TR) ? ch : 0), border);
  tft.drawFastHLine(x + ((cuts & CUT_BL) ? ch : 0), yb, w - ((cuts & CUT_BL) ? ch : 0) - ((cuts & CUT_BR) ? ch : 0), border);
  tft.drawFastVLine(xl, y + ((cuts & CUT_TL) ? ch : 0), h - ((cuts & CUT_TL) ? ch : 0) - ((cuts & CUT_BL) ? ch : 0), border);
  tft.drawFastVLine(xr, y + ((cuts & CUT_TR) ? ch : 0), h - ((cuts & CUT_TR) ? ch : 0) - ((cuts & CUT_BR) ? ch : 0), border);
  if (cuts & CUT_TL) tft.drawLine(xl, yt + ch, xl + ch, yt, border);
  if (cuts & CUT_TR) tft.drawLine(xr - ch, yt, xr, yt + ch, border);
  if (cuts & CUT_BR) tft.drawLine(xr, yb - ch, xr - ch, yb, border);
  if (cuts & CUT_BL) tft.drawLine(xl + ch, yb, xl, yb - ch, border);
}
static void bracket(int x, int y, int w, uint16_t c, bool left, bool right) {      // accent strokes on the top corners of a tile
  if (left) { tft.fillRect(x + 3, y, 12, 2, c); tft.fillRect(x, y + 3, 2, 7, c); tft.drawLine(x, y + 3, x + 3, y, c); tft.drawLine(x + 1, y + 3, x + 4, y, c); }
  if (right) { tft.fillRect(x + w - 15, y, 12, 2, c); tft.fillRect(x + w - 2, y + 3, 2, 7, c); tft.drawLine(x + w - 1, y + 3, x + w - 4, y, c); tft.drawLine(x + w - 2, y + 3, x + w - 5, y, c); }
}
static void hexagon(int cx, int cy, int r, uint16_t fill, uint16_t line, bool thick) {
  int dx = (r * 7) / 8, hy = r / 2;
  tft.fillTriangle(cx, cy - r, cx + dx, cy - hy, cx - dx, cy - hy, fill);
  tft.fillRect(cx - dx, cy - hy, 2 * dx + 1, 2 * hy + 1, fill);
  tft.fillTriangle(cx, cy + r, cx + dx, cy + hy, cx - dx, cy + hy, fill);
  int px[7] = {cx, cx + dx, cx + dx, cx, cx - dx, cx - dx, cx}, py[7] = {cy - r, cy - hy, cy + hy, cy + r, cy + hy, cy - hy, cy - r};
  for (int i = 0; i < 6; i++) {
    tft.drawLine(px[i], py[i], px[i + 1], py[i + 1], line);
    if (thick) { tft.drawLine(px[i] + (px[i] < cx ? 1 : px[i] > cx ? -1 : 0), py[i] + (py[i] < cy ? 1 : -1), px[i + 1] + (px[i + 1] < cx ? 1 : px[i + 1] > cx ? -1 : 0), py[i + 1] + (py[i + 1] < cy ? 1 : -1), line); }
  }
}
static void segBar(int x, int y, int n, int lit, uint16_t on, uint16_t off) {       // n small dashes, 5x2 px, 1 px gap
  for (int i = 0; i < n; i++) tft.fillRect(x + i * 6, y, 5, 2, i < lit ? on : off);
}
static bool hit(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
static void fmtv(char* b, size_t n, float v, int dec) { if (v > -0.5f * powf(10, -dec) && v < 0) v = 0; snprintf(b, n, "%.*f", dec, v); }
static void fmtThou(char* b, size_t n, int v) { if (v >= 1000) snprintf(b, n, "%d,%03d", v / 1000, v % 1000); else snprintf(b, n, "%d", v); }
static void fitText(char* d, size_t n, const char* s, FontId f, int maxw) {   // shortens with ".." to fit
  strncpy(d, s, n - 1); d[n - 1] = 0;
  if (textW(f, d) <= maxw) return;
  int L = (int)strlen(d);
  while (L > 1) { L--; char t[40]; snprintf(t, sizeof t, "%.*s..", L, d); if (textW(f, t) <= maxw) { strncpy(d, t, n - 1); d[n - 1] = 0; return; } }
}

// ---------- buttons ----------
static void btnBox(int x, int y, int w, int h, bool on) {
  panel(x, y, w, h, 4, CUT_TL | CUT_BR, on ? T.acc : T.panel2, on ? T.acc : T.line);
}
static void btnId(StrId id, int x, int y, int w, int h, bool on, LabKind k = LB_BTN) {
  btnBox(x, y, w, h, on); uint16_t bg = on ? T.acc : T.panel2;
  lab(id, k, x + w / 2, y + h / 2 + (k.en == F_UL ? 6 : (k.en == F_UM ? 5 : 4)), T.text, bg, AL_C);
}
static void btnS(const char* s, int x, int y, int w, int h, bool on, FontId f = F_UL) {
  btnBox(x, y, w, h, on); uint16_t bg = on ? T.acc : T.panel2;
  txt(f, s, x + w / 2, y + h / 2 + (f == F_UL ? 6 : (f == F_UM ? 5 : 4)), T.text, bg, AL_C);
}

// =====================================================================================
//  Vehicle-dependent values, tile metrics, trip / peaks / graph history
// =====================================================================================
static bool vehDiesel() { if (ecuDieselKnown && !demoMode) return ecuDiesel; return vehModel >= 0 && (MODELS[vehModel].flags & VF_D); }
static bool vehHybrid() { return (ecuHybrid && !demoMode) || (vehModel >= 0 && (MODELS[vehModel].flags & VF_H)); }
static int ringMaxK() { return vehDiesel() ? 6 : 8; }          // tacho scale in 1000 rpm
static int vehCc() { return vehModel >= 0 ? MODELS[vehModel].cc : 1500; }

enum { M_AFR = 256, M_BOOST, M_FUELRATE, M_LAMBDA, M_CRP };

static float fuelRate(bool& ok, bool& est) {                    // litres per hour (PID 5E, else estimated from air flow)
  bool o; est = false; ok = false;
  float v = getVal(0x5E, o); if (o) { ok = true; return v * fuelCal / 100.0f; }
  est = true;
  bool orpm; float rpm = getVal(0x0C, orpm); if (!orpm) return 0;
  if (vehDiesel()) {
    bool o1, o2; float tq = getVal(0x62, o1), ref = getVal(0x63, o2);
    if (o1 && o2 && ref > 0) { float kw = fmaxf(0, ref * tq / 100.0f * rpm * 0.10472f / 1000.0f); ok = true; return (0.7f + kw * 0.36f) * fuelCal / 100.0f; }
    return 0;
  }
  float air = 0; bool oa;
  float maf = getVal(0x10, oa);
  if (oa) air = maf;
  else {
    bool om, oi; float map = getVal(0x0B, om), iat = getVal(0x0F, oi);
    if (!om) return 0;
    air = (map * 1000.0f / (287.05f * ((oi ? iat : 30.0f) + 273.15f))) * (vehCc() / 1e6f) * (rpm / 120.0f) * 0.82f * 1000.0f;   // speed-density, VE about 0.82
  }
  bool ol; float lam = getVal(0x24, ol); if (!ol || lam < 0.5f) lam = getVal(0x44, ol);
  if (!ol || lam < 0.5f || lam > 1.6f) lam = 1.0f;
  ok = true; return air / (14.7f * lam) * 3600.0f / 745.0f * fuelCal / 100.0f;
}
static float metricVal(int src, bool& ok) {
  if (src < 256) return getVal((uint8_t)src, ok);
  bool o1, o2;
  switch (src) {
    case M_AFR: case M_LAMBDA: {
      float l = getVal(0x24, o1); if (!o1 || l < 0.4f) l = getVal(0x44, o1);
      ok = o1 && l > 0.4f; return src == M_AFR ? l * 14.7f : l;
    }
    case M_BOOST: { float m = getVal(0x0B, o1), b = getVal(0x33, o2); ok = o1; return m - (o2 ? b : 101.0f); }
    case M_FUELRATE: { bool est; float f = fuelRate(ok, est); return f; }
    case M_CRP: ok = true; return crpV;
  }
  ok = false; return 0;
}

struct Met { StrId lab; const char* unit; uint8_t dec; float w, b, lo; int16_t src; float mul; float bmin, bmax; };
#define NOW_ 1e9f
static const Met MET[] = {
  {S_COOLANT, DEGC, 0, 100, 108, -NOW_, 0x05, 1, 40, 120},   {S_OIL, DEGC, 0, 115, 130, -NOW_, 0x5C, 1, 40, 140},
  {S_INTAKE, DEGC, 0, 60, 75, -NOW_, 0x0F, 1, 0, 80},        {S_BATT, "V", 1, NOW_, NOW_, 12, 0x42, 1, 10, 16},
  {S_AFR, "", 1, NOW_, NOW_, -NOW_, M_AFR, 1, 10, 20},        {S_LOAD, "%", 0, NOW_, NOW_, -NOW_, 0x04, 1, 0, 100},
  {S_THR, "%", 0, NOW_, NOW_, -NOW_, 0x11, 1, 0, 100},       {S_MAP, "kPa", 0, NOW_, NOW_, -NOW_, 0x0B, 1, 0, 250},
  {S_BOOST, "kPa", 0, NOW_, NOW_, -NOW_, M_BOOST, 1, -50, 200}, {S_MAF, "g/s", 1, NOW_, NOW_, -NOW_, 0x10, 1, 0, 150},
  {S_TIMING, DEG, 0, NOW_, NOW_, -NOW_, 0x0E, 1, -10, 50},  {S_STFT, "%", 1, NOW_, NOW_, -NOW_, 0x06, 1, -25, 25},
  {S_LTFT, "%", 1, NOW_, NOW_, -NOW_, 0x07, 1, -25, 25},     {S_FUEL, "%", 0, NOW_, NOW_, -NOW_, 0x2F, 1, 0, 100},
  {S_RPM, "rpm", 0, NOW_, NOW_, -NOW_, 0x0C, 1, 0, 8000},    {S_SPEED, "km/h", 0, NOW_, NOW_, -NOW_, 0x0D, 1, 0, 200},
  {S_EGT, DEGC, 0, 600, 750, -NOW_, 0x78, 1, 0, 800},        {S_AMBIENT, DEGC, 0, NOW_, NOW_, -NOW_, 0x46, 1, 0, 50},
  {S_RAIL, "MPa", 1, NOW_, NOW_, -NOW_, 0x23, 0.001f, 0, 200}, {S_PEDAL, "%", 0, NOW_, NOW_, -NOW_, 0x49, 1, 0, 100},
  {S_TORQUE, "%", 0, NOW_, NOW_, -NOW_, 0x62, 1, 0, 100},    {S_FUELRATE, "L/h", 1, NOW_, NOW_, -NOW_, M_FUELRATE, 1, 0, 30},
  {S_HVBATT, "%", 0, NOW_, NOW_, 20, 0x5B, 1, 0, 100},       {S_CATT, DEGC, 0, 850, 950, -NOW_, 0x3C, 1, 0, 900},
  {S_BARO, "kPa", 0, NOW_, NOW_, -NOW_, 0x33, 1, 80, 110},   {S_LAMBDA, "", 2, NOW_, NOW_, -NOW_, M_LAMBDA, 1, 0.6f, 1.4f},
  {S_RUNTIME, "min", 0, NOW_, NOW_, -NOW_, 0x1F, 1.0f / 60, 0, 60}, {S_CRPV, "V", 2, NOW_, NOW_, -NOW_, M_CRP, 1, 0, 5},
};
static const int NMET = sizeof(MET) / sizeof(MET[0]);
enum { MI_COOL = 0, MI_OIL, MI_INTAKE, MI_BATT, MI_AFR, MI_LOAD, MI_THR, MI_MAP, MI_BOOST, MI_MAF, MI_TIMING, MI_STFT, MI_LTFT, MI_FUEL, MI_RPM, MI_SPEED,
       MI_EGT, MI_AMB, MI_RAIL, MI_PEDAL, MI_TORQ, MI_FRATE, MI_HV, MI_CAT, MI_BARO, MI_LAMBDA, MI_RUNT, MI_CRP };
static bool metSupported(int i) {
  int s = MET[i].src;
  if (s == M_CRP) return true;
  if (s == M_AFR || s == M_LAMBDA) return psup[0x24] || psup[0x44];
  if (s == M_BOOST) return psup[0x0B];
  if (s == M_FUELRATE) return psup[0x5E] || psup[0x10] || (psup[0x0B] && psup[0x0C]);
  return psup[s];
}
static bool slotsCustom = false;
static bool inSlots(int m, int except) { for (int i = 0; i < 6; i++) if (i != except && slots[i] == m) return true; return false; }
static int nextMetric(int cur, int slot) {
  bool filter = !demoMode && linkState == 2;
  for (int k = 1; k <= NMET; k++) {
    int i = (cur + k) % NMET;
    if (filter && !metSupported(i)) continue;
    if (inSlots(i, slot)) continue;
    return i;
  }
  return cur;
}
static int firstSupported(const int* cand, int n, int fallback) {
  for (int i = 0; i < n; i++) if ((demoMode || metSupported(cand[i])) && !inSlots(cand[i], -1)) return cand[i];
  return fallback;
}
static void autoSlots() {                                       // default tiles for this car (until the user picks tiles themselves)
  if (slotsCustom) return;
  for (int i = 0; i < 6; i++) slots[i] = 255;
  bool d = vehDiesel();
  static const int C0[] = {MI_COOL}, C1[] = {MI_OIL, MI_THR, MI_MAP}, C2[] = {MI_LOAD, MI_THR, MI_PEDAL}, C3[] = {MI_INTAKE, MI_AMB},
                   C4[] = {MI_BATT}, C5P[] = {MI_AFR, MI_LAMBDA, MI_MAP, MI_TIMING}, C5D[] = {MI_BOOST, MI_RAIL, MI_MAP, MI_EGT};
  slots[0] = (uint8_t)firstSupported(C0, 1, MI_COOL);
  slots[3] = (uint8_t)firstSupported(C3, 2, MI_INTAKE);
  slots[1] = (uint8_t)firstSupported(C1, 3, MI_OIL);
  slots[4] = (uint8_t)firstSupported(C4, 1, MI_BATT);
  slots[2] = (uint8_t)firstSupported(C2, 3, MI_LOAD);
  slots[5] = (uint8_t)firstSupported(d ? C5D : C5P, 4, MI_AFR);
  for (int i = 0; i < 6; i++) if (slots[i] >= NMET) slots[i] = (uint8_t)i;
}

// data page: 16 tiles (petrol / diesel sets), items are metric indexes
static const uint8_t DSET_P[16] = {MI_LAMBDA, MI_LAMBDA, MI_STFT, MI_LTFT, 255, MI_CAT, MI_LOAD, MI_THR, MI_TIMING, MI_MAP, MI_MAF, MI_PEDAL, MI_RAIL, MI_RUNT, MI_BARO, MI_FUEL};
static const uint8_t DSET_D[16] = {MI_LOAD, MI_THR, MI_PEDAL, MI_TORQ, MI_MAP, MI_BOOST, MI_MAF, MI_BARO, MI_RAIL, MI_EGT, MI_INTAKE, MI_COOL, MI_OIL, MI_FRATE, MI_FUEL, MI_RUNT};
struct DTile { StrId lab; const char* unit; uint8_t dec; int16_t src; float mul; float bmin, bmax; int8_t mi; };
static const DTile DT_O2B = {S_O2REAR, "V", 2, 0x15, 1, 0, 1, -1}, DT_CMD = {S_CMDLAM, "", 2, 0x44, 1, 0.6f, 1.4f, -1};
static void dtileOf(int i, DTile& t) {                           // tile i of the data page for the current vehicle
  const uint8_t* set = vehDiesel() ? DSET_D : DSET_P;
  int m = set[i];
  if (!vehDiesel() && i == 1) { t = DT_CMD; return; }
  if (!vehDiesel() && i == 4) { t = DT_O2B; return; }
  const Met& M = MET[m]; t.lab = M.lab; t.unit = M.unit; t.dec = M.dec; t.src = M.src; t.mul = M.mul; t.bmin = M.bmin; t.bmax = M.bmax; t.mi = (int8_t)m;
}


// ---------- trip ----------
static float tDist = 0, tSec = 0, tFuel = 0, tNow = 0, tNowKmL = 0; static bool tEst = false;
static uint32_t driveStart = 0;
static const int NMIN = 30;
static float minFuel[NMIN], minDist[NMIN]; static int nMin = 0; static float curMinF = 0, curMinD = 0, minAcc = 0;
static bool unitL100 = false;
static void tripReset() { tDist = tSec = tFuel = tNow = tNowKmL = 0; nMin = 0; curMinF = curMinD = minAcc = 0; driveStart = millis(); }
static void tripStep(float dt) {
  bool ok, ro; float spd = getVal(0x0D, ok); float rpm = getVal(0x0C, ro);
  if (!ok) return;
  if (spd > 0 || (ro && rpm > 300)) tSec += dt;
  tDist += spd / 3600 * dt;
  bool fo, est; float lph = fuelRate(fo, est);
  if (fo) {
    tFuel += lph / 3600 * dt; tEst = est;
    tNow = spd > 3 ? lph / spd * 100 : 0; tNowKmL = lph > 0.05f && spd > 3 ? spd / lph : 0;
    curMinF += lph / 3600 * dt;
  }
  curMinD += spd / 3600 * dt; minAcc += dt;
  if (minAcc >= 60) {
    minAcc -= 60;
    if (nMin == NMIN) { memmove(minFuel, minFuel + 1, (NMIN - 1) * sizeof(float)); memmove(minDist, minDist + 1, (NMIN - 1) * sizeof(float)); nMin--; }
    minFuel[nMin] = curMinF; minDist[nMin] = curMinD; nMin++; curMinF = curMinD = 0;
  }
}
static float tripAvgL100() { return tDist > 0.2f && tFuel > 0.001f ? tFuel / tDist * 100 : 0; }
static float rangeKm() {
  bool ok; float lvl = getVal(0x2F, ok); if (!ok) return -1;
  float l100 = tripAvgL100(); if (l100 < 2) l100 = vehDiesel() ? 8.0f : 7.5f;
  return lvl / 100.0f * tankL / l100 * 100.0f;
}

// ---------- peaks ----------
struct Peaks { float rpm, spd, cool, oil, load, intake, minB, maxB, best; uint32_t tr, ts, tc, to, tl, ti, tmin, tmax; };
static Peaks pkD, pkA; static bool pkAllMode = false; static uint32_t pkSaved = 0; static bool pkDirty = false;
static void peaksClear(Peaks& p) { memset(&p, 0, sizeof p); p.minB = 99; }
static bool upd(float& dst, uint32_t& tdst, float v, bool hi, uint32_t t) { if (hi ? v > dst : v < dst) { dst = v; tdst = t; return true; } return false; }
static void peaksStep() {
  bool ok; uint32_t t = (millis() - driveStart) / 1000; bool ch = false;
  float v = getVal(0x0C, ok); if (ok) { ch |= upd(pkD.rpm, pkD.tr, v, true, t); ch |= upd(pkA.rpm, pkA.tr, v, true, t); }
  v = getVal(0x0D, ok);
  static float t0 = 0; static bool armed = true; static float lastS = 0;
  if (ok) {
    ch |= upd(pkD.spd, pkD.ts, v, true, t); ch |= upd(pkA.spd, pkA.ts, v, true, t);
    float now = millis() / 1000.0f;
    if (v < 3) { armed = true; t0 = now; }
    else if (armed) {
      if (v < lastS - 6) armed = false;
      else if (v >= 100) { float dt = now - t0; if (dt > 1 && (pkD.best == 0 || dt < pkD.best)) pkD.best = dt; if (dt > 1 && (pkA.best == 0 || dt < pkA.best)) { pkA.best = dt; ch = true; } armed = false; }
    }
    lastS = v;
  }
  v = getVal(0x05, ok); if (ok) { ch |= upd(pkD.cool, pkD.tc, v, true, t); ch |= upd(pkA.cool, pkA.tc, v, true, t); }
  v = getVal(0x5C, ok); if (ok) { ch |= upd(pkD.oil, pkD.to, v, true, t); ch |= upd(pkA.oil, pkA.to, v, true, t); }
  v = getVal(0x04, ok); if (ok) { ch |= upd(pkD.load, pkD.tl, v, true, t); ch |= upd(pkA.load, pkA.tl, v, true, t); }
  v = getVal(0x0F, ok); if (ok) { ch |= upd(pkD.intake, pkD.ti, v, true, t); ch |= upd(pkA.intake, pkA.ti, v, true, t); }
  bool orp; float rp = getVal(0x0C, orp);
  v = getVal(0x42, ok);
  if (ok && v > 5) {
    if (orp && rp > 500) ch |= upd(pkD.minB, pkD.tmin, v, false, t), ch |= upd(pkA.minB, pkA.tmin, v, false, t);
    ch |= upd(pkD.maxB, pkD.tmax, v, true, t); ch |= upd(pkA.maxB, pkA.tmax, v, true, t);
  }
  if (ch) pkDirty = true;
}

// ---------- graph history: one sample per second (60 s view) and one per 15 s (15 min view) ----------
enum { GC_RPM, GC_SPD, GC_COOL, GC_OIL, GC_INT, GC_AFR, GC_STFT, GC_BATT, GC_N };
static const int GN = 60;
static int16_t gH[GC_N][2][GN]; static int gCnt[2] = {0, 0}; static uint32_t gAcc15 = 0;
static float gLast[GC_N]; static bool gOk[GC_N]; static uint32_t gPush[2] = {0, 0};
static float gScale(int c) { return c == GC_RPM ? 1.0f : (c == GC_AFR || c == GC_BATT || c == GC_STFT) ? 100.0f : 10.0f; }
static void graphSample() {
  static const int SRC[GC_N] = {0x0C, 0x0D, 0x05, 0x5C, 0x0F, M_AFR, 0x06, 0x42};
  int16_t v[GC_N];
  for (int c = 0; c < GC_N; c++) {
    bool ok; float f = metricVal(SRC[c], ok); gOk[c] = ok; gLast[c] = f;
    v[c] = ok ? (int16_t)constrain((int)lroundf(f * gScale(c)), -30000, 30000) : (int16_t)-32768;
  }
  for (int m = 0; m < 2; m++) {
    if (m == 1) { gAcc15++; if (gAcc15 % 15) continue; }
    if (gCnt[m] == GN) { for (int c = 0; c < GC_N; c++) memmove(gH[c][m], gH[c][m] + 1, (GN - 1) * sizeof(int16_t)); gCnt[m]--; }
    for (int c = 0; c < GC_N; c++) gH[c][m][gCnt[m]] = v[c];
    gCnt[m]++; gPush[m]++;
  }
}

// ---------- persistence ----------
static void loadPrefs() {
  prefs.begin("gauge", true);
  firstRun = !prefs.isKey("inv");
  thai = prefs.getBool("thai", false); demoMode = prefs.getBool("demo", true); warnOn = prefs.getBool("warn", true); parkOn = prefs.getBool("park", true);
  shiftRpm = prefs.getInt("shift", 6500); bright = prefs.getInt("bright", 80); int th = prefs.getInt("theme", 0);
  crpMain = prefs.getBool("crpm", false); tankL = prefs.getInt("tank", 44); fuelCal = prefs.getInt("fcal", 100);
#ifdef TFT_INVERSION_ON
  invScreen = prefs.getBool("inv", true);                    // ST7789 boards normally need the inversion on
#else
  invScreen = prefs.getBool("inv", false);
#endif
  vehModel = prefs.getInt("veh", -1); lastProto = prefs.getInt("proto", 0); userProto = prefs.getInt("uproto", 0); unitL100 = prefs.getBool("unitl", false); slotsCustom = prefs.getBool("slc", false);
  String ba = prefs.getString("btaddr", ""), bn = prefs.getString("btname", "");
  int bk = prefs.getInt("btkind", 0), bp = prefs.getInt("btpin", 0); bool bo = prefs.getBool("btpinok", false);
  uint8_t s[6]; if (prefs.getBytes("slots", s, 6) == 6) { bool good = true; for (int i = 0; i < 6; i++) if (s[i] >= NMET) good = false; if (good) memcpy(slots, s, 6); }
  peaksClear(pkA); { Peaks tmp; if (prefs.getBytes("pkall", &tmp, sizeof tmp) == sizeof tmp) pkA = tmp; }
  prefs.end();
  applyTheme(th);
  if (shiftRpm < 3000 || shiftRpm > 9000) shiftRpm = 6500;
  if (tankL < 10 || tankL > 200) tankL = 44;
  if (fuelCal < 70 || fuelCal > 130) fuelCal = 100;
  if (vehModel < -1 || vehModel >= NMODEL) vehModel = -1;
  if (lastProto < 0 || lastProto > 9) lastProto = 0;
  if (userProto < 0 || userProto > 9) userProto = 0;
  uint8_t tmp6[6];
  pinIdx = pinSaved = (bp >= 0 && bp < NPINS) ? bp : 0; pinOk = bo;
  if (ba.length() == 17 && parseAddr(ba.c_str(), tmp6)) {
    strncpy(selAddr, ba.c_str(), sizeof selAddr - 1); cleanName(selName, sizeof selName, bn.c_str());
    selBle = bk >= 1 && bk <= 5; selAType = selBle ? (uint8_t)(bk - 1) : 0;
  }
}
static void savePrefs() {
  prefs.begin("gauge", false);
  prefs.putBool("thai", thai); prefs.putBool("demo", demoMode); prefs.putBool("warn", warnOn); prefs.putBool("park", parkOn); prefs.putBool("crpm", crpMain);
  prefs.putInt("shift", shiftRpm); prefs.putInt("theme", theme); prefs.putInt("bright", bright); prefs.putBytes("slots", slots, 6); prefs.putBool("slc", slotsCustom);
  prefs.putInt("tank", tankL); prefs.putInt("fcal", fuelCal); prefs.putBool("inv", invScreen); prefs.putInt("veh", vehModel); prefs.putInt("proto", lastProto); prefs.putInt("uproto", userProto);
  prefs.putBool("unitl", unitL100);
  prefs.putString("btaddr", selAddr); prefs.putString("btname", selName);
  prefs.putInt("btkind", selBle ? 1 + selAType : 0); prefs.putInt("btpin", pinIdx); prefs.putBool("btpinok", pinOk);
  prefs.putBytes("pkall", &pkA, sizeof pkA);
  prefs.end();
  pinSaved = pinIdx; firstRun = false;
}
static void applyBright() { ledcWrite(0, map(bright, 0, 100, 5, 255)); }
static void applyInvert() { tft.invertDisplay(invScreen); }

// =====================================================================================
//  Screens
// =====================================================================================
enum Page { PG_SPLASH, PG_DASH, PG_DATA, PG_ALL, PG_GRAPH, PG_TRIP, PG_PEAKS, PG_CODES, PG_STATUS, PG_FREEZE, PG_ECU, PG_SETUP, PG_SETUP2, PG_BT, PG_VEH, PG_PARK };
static Page scr = PG_SPLASH;
static bool redraw = true;
static int gsel = 0, gmode = 0;                           // graph set (0 rpm, 1 temps, 2 afr, 3 batt) and time base (0 = 60 s, 1 = 15 min)
static bool allMode = false;

static void markWant(int pid, int tier) { if (pid > 0 && pid < 256 && want[pid] < tier) want[pid] = (uint8_t)tier; }
static void markSrc(int s, int tier) {
  if (s < 256) { markWant(s, tier); return; }
  switch (s) {
    case M_AFR: case M_LAMBDA: markWant(0x24, tier); markWant(0x44, tier); break;
    case M_BOOST: markWant(0x0B, tier); markWant(0x33, tier); break;
    case M_FUELRATE: markWant(0x5E, tier); markWant(0x10, tier); markWant(0x0B, tier); markWant(0x0F, tier); markWant(0x62, tier); markWant(0x63, tier); break;
  }
}
static void updateWants() {
  memset(want, 0, sizeof want);
  static const uint8_t BG[] = {0x05, 0x42, 0x5C, 0x5E, 0x10, 0x0B, 0x0F, 0x2F, 0x44, 0x24, 0x33, 0x04, 0x06, 0x62, 0x63};
  for (unsigned i = 0; i < sizeof BG; i++) markWant(BG[i], 1);
  markWant(0x0C, 2); markWant(0x0D, 2);
  if (scr == PG_DASH) for (int i = 0; i < 6; i++) markSrc(MET[slots[i]].src, 2);
  else if (scr == PG_DATA) { for (int i = 0; i < 16; i++) { DTile t; dtileOf(i, t); markSrc(t.src, 2); } }
  else if (scr == PG_ALL) { for (int i = 0; i < NPID; i++) markWant(PIDS[i].id, 2); }
  else if (scr == PG_GRAPH) {
    static const int GS[4][3] = {{0x0C, 0x0D, 0}, {0x05, 0x5C, 0x0F}, {M_AFR, 0x06, 0x07}, {0x42, 0, 0}};
    for (int k = 0; k < 3; k++) if (GS[gsel][k]) markSrc(GS[gsel][k], 2);
  }
  else if (scr == PG_TRIP || scr == PG_PARK) { markSrc(M_FUELRATE, 2); markWant(0x2F, 2); }
  else if (scr == PG_STATUS) { markWant(0x21, 2); markWant(0x30, 2); markWant(0x31, 2); }
}

static const char* protoName(int p) {
  static const char* const N[] = {"", "J1850 PWM", "J1850 VPW", "ISO 9141", "KWP SLOW", "KWP FAST", "CAN 11/500", "CAN 29/500", "CAN 11/250", "CAN 29/250"};
  return p >= 0 && p <= 9 ? N[p] : "";
}
static void carName(char* d, size_t n) {
  d[0] = 0;
  if (vehModel >= 0) {
    const Model& m = MODELS[vehModel]; const char* br = BRANDS[m.brand];
    if (!strncmp(m.name, br, strlen(br))) snprintf(d, n, "%s", m.name); else snprintf(d, n, "%s %s", br, m.name);
  } else if (demoMode) snprintf(d, n, "MAZDA 2");
  else snprintf(d, n, "%s", carBuf);
}
static void goPage(Page p) { scr = p; redraw = true; updateWants(); }

// ---------- common header / footer ----------
static void titleBar(StrId title, int right /* 0 none, 1 DONE, 2 BACK */) {
  lab(title, LB_BTN, 160, 21, T.text, T.bg, AL_C, 120);
  tft.fillRect(124, 25, 72, 2, T.acc);
  if (right) btnId(right == 1 ? S_DONE : S_BACK, 246, 4, 68, 22, true);
  tft.drawFastHLine(0, 30, 320, T.line);
}
static void leftBtn(StrId id) { btnId(id, 6, 4, 76, 22, false, LB_BTNM); }
static void leftTxt(const char* s) { txt(F_LS, s, 10, 20, T.dim, T.bg, AL_L, 90); }
static void statusDot(int x, int y) { uint16_t c = linkState == 2 || demoMode ? T.good : T.warn; tft.fillCircle(x, y, 3, c); }

// ---------- dashboard ----------
static const int DT_X[6] = {6, 6, 6, 250, 250, 250}, DT_Y[6] = {36, 96, 156, 36, 96, 156};
static const int RV[6][2] = {{124, 192}, {84, 121}, {120, 52}, {200, 52}, {236, 121}, {196, 192}};   // centre line of the tacho, bottom-left -> bottom-right
static int16_t RQ[40][8];
static bool ringBuilt = false;
static void ringBuild() {
  ringBuilt = true;
  for (int e = 0; e < 5; e++) {
    float x0 = RV[e][0], y0 = RV[e][1], x1 = RV[e + 1][0], y1 = RV[e + 1][1], dx = x1 - x0, dy = y1 - y0, L = sqrtf(dx * dx + dy * dy), ux = dx / L, uy = dy / L;
    float nx = uy, ny = -ux, mx = (x0 + x1) / 2 - 160, my = (y0 + y1) / 2 - 122;
    if (nx * mx + ny * my < 0) { nx = -nx; ny = -ny; }
    for (int j = 0; j < 8; j++) {
      float t0 = (j + 0.10f) / 8, t1 = (j + 0.90f) / 8, ax = x0 + dx * t0, ay = y0 + dy * t0, bx = x0 + dx * t1, by = y0 + dy * t1;
      const float h = 5.0f, sk = 2.5f; int16_t* q = RQ[e * 8 + j];
      q[0] = (int16_t)lroundf(ax + nx * h); q[1] = (int16_t)lroundf(ay + ny * h); q[2] = (int16_t)lroundf(bx + nx * h); q[3] = (int16_t)lroundf(by + ny * h);
      q[4] = (int16_t)lroundf(bx - nx * h + ux * sk); q[5] = (int16_t)lroundf(by - ny * h + uy * sk); q[6] = (int16_t)lroundf(ax - nx * h + ux * sk); q[7] = (int16_t)lroundf(ay - ny * h + uy * sk);
    }
  }
}
static int ringRed() { int k = ringMaxK() * 1000; return constrain((int)lroundf(shiftRpm * 40.0f / k), 1, 40); }
static uint16_t hotDim() { return mix565(T.bg, T.hot, 5); }
static uint16_t segCol(int i, bool lit, bool lead) {
  if (i >= ringRed()) return lit ? T.hot : hotDim();
  return lit ? (lead ? T.accL : T.acc) : T.dark;
}
static void ringSeg(int i, uint16_t c) { const int16_t* q = RQ[i]; tft.fillTriangle(q[0], q[1], q[2], q[3], q[4], q[5], c); tft.fillTriangle(q[0], q[1], q[4], q[5], q[6], q[7], c); }
static int ringLit = -1;
static void ringNumerals() {
  float total = 0, el[5];
  for (int e = 0; e < 5; e++) { float dx = RV[e + 1][0] - RV[e][0], dy = RV[e + 1][1] - RV[e][1]; el[e] = sqrtf(dx * dx + dy * dy); total += el[e]; }
  int K = ringMaxK();
  for (int k = 0; k <= K; k++) {
    float s = total * k / K; int e = 0;
    while (e < 4 && s > el[e]) { s -= el[e]; e++; }
    float t = s / el[e], x = RV[e][0] + (RV[e + 1][0] - RV[e][0]) * t, y = RV[e][1] + (RV[e + 1][1] - RV[e][1]) * t;
    float dx = RV[e + 1][0] - RV[e][0], dy = RV[e + 1][1] - RV[e][1], L = el[e], nx = dy / L, ny = -dx / L;
    float mx = x - 160, my = y - 122; if (nx * mx + ny * my > 0) { nx = -nx; ny = -ny; }       // inward
    char b[4]; snprintf(b, sizeof b, "%d", k);
    bool red = k * 1000 >= shiftRpm;
    txt(F_LS, b, (int)lroundf(x + nx * 15), (int)lroundf(y + ny * 15) + 4, red ? T.hot : T.dim, T.bg, AL_C);
  }
}
static void ringPaint(int lit) {                                   // repaint segments between old and new lit count
  if (!ringBuilt) ringBuild();
  int a = ringLit < 0 ? 0 : min(ringLit, lit), b = ringLit < 0 ? 40 : max(ringLit, lit);
  for (int i = max(0, a - 1); i < min(40, b + 1); i++) ringSeg(i, segCol(i, i < lit, i == lit - 1));
  ringLit = lit;
}

static char dtCache[6][24];
static bool tileGood(const Met& m, float v) {
  switch (m.src) {
    case 0x42: return v >= 13.0f && v <= 15.0f;
    case M_AFR: return v >= 14.2f && v <= 15.2f;
    case M_LAMBDA: return v >= 0.97f && v <= 1.03f;
    case 0x06: case 0x07: return fabsf(v) <= 5.0f;
  }
  return false;
}
static uint16_t valCol(const Met& m, float v) { return v >= m.b ? T.hot : v >= m.w ? T.warn : v < m.lo ? T.warn : tileGood(m, v) ? T.good : T.text; }
static void dtileStatic(int i) {
  const Met& m = MET[slots[i]]; bool right = i >= 3; int x = DT_X[i], y = DT_Y[i];
  panel(x, y, 64, 40, 4, right ? (CUT_TR | CUT_BL) : (CUT_TL | CUT_BR), T.panel, T.line);
  bracket(x, y, 64, T.acc, !right, right);
  lab(m.lab, LB_TILE, right ? x + 58 : x + 6, thai ? y + 14 : y + 11, T.dim, T.panel, right ? AL_R : AL_L);
  dtCache[i][0] = 0;
}
static void valueFit(const char* b, const char* unit, int x, int by, uint16_t col, uint16_t bg, int al, int maxw, int clearW, bool big = false) {
  FontId f = F_NS; bool u = false;
  const int uw = unit[0] ? textW(F_LS, unit) + 3 : 0;
  if (big && textW(F_NL, b) + uw <= maxw) { f = F_NL; u = uw > 0; }
  else if (textW(F_NM, b) + uw <= maxw) { f = F_NM; u = uw > 0; }
  else if (textW(F_NM, b) <= maxw) f = F_NM;
  else u = uw > 0 && textW(F_NS, b) + uw <= maxw;
  txtVU(f, b, F_LS, u ? unit : "", x, by, col, T.dim, bg, al, clearW);
}
static void dtileDyn(int i) {
  const Met& m = MET[slots[i]]; bool right = i >= 3, ok; int x = DT_X[i], y = DT_Y[i];
  float v = metricVal(m.src, ok) * m.mul; char b[14], key[24]; uint16_t col = T.dim; int lit = 0;
  if (ok) { fmtv(b, sizeof b, v, m.dec); col = valCol(m, v); lit = constrain((int)lroundf((v - m.bmin) / (m.bmax - m.bmin) * 8), 0, 8); } else strcpy(b, "--");
  snprintf(key, sizeof key, "%s|%04X|%d", b, col, lit);
  if (!strcmp(key, dtCache[i])) return;
  bool first = dtCache[i][0] == 0; strcpy(dtCache[i], key);
  valueFit(b, m.unit, right ? x + 57 : x + 7, y + (thai ? 34 : 31), col, T.panel, right ? AL_R : AL_L, 52, 52, !thai);
  segBar(right ? x + 64 - 6 - 47 : x + 6, y + (thai ? 37 : 35), 8, lit, col == T.dim ? T.dark : (col == T.text ? T.text : col), T.dark);
  (void)first;
}

static int sbLast = -1; static bool sbBlink = false;
static const int SBX[10] = {6, 21, 36, 51, 66, 81, 96, 111, 126, 141};
static void sbSeg(int i, uint16_t c) {                             // i = 0..19, 0 = far left, 19 = far right (mirror image)
  int p = i < 10 ? i : 19 - i;
  int x = i < 10 ? SBX[p] : 320 - SBX[p] - 16, ax, bx, cx, dx;
  if (i < 10) { ax = x + 3; bx = x + 16; cx = x + 13; dx = x; } else { ax = x; bx = x + 13; cx = x + 16; dx = x + 3; }
  tft.fillTriangle(ax, 3, bx, 3, cx, 8, c); tft.fillTriangle(ax, 3, cx, 8, dx, 8, c);
}
static uint16_t sbCol(int pos, bool lit) {          // pos 0 = outermost segment
  if (pos < 4) return lit ? T.acc : T.accDim;
  if (pos < 7) return lit ? T.warn : mix565(T.bg, T.warn, 4);
  return lit ? T.hot : hotDim();
}
static void shiftBar(float rpm) {
  int r0 = max(1800, shiftRpm - 3000);
  int lvl = rpm <= r0 ? 0 : min(10, 1 + (int)((rpm - r0) * 10.0f / (shiftRpm - r0)));
  bool over = rpm >= shiftRpm; bool blink = over && ((millis() / 140) & 1);
  int state = lvl * 2 + (over ? 100 : 0) + (blink ? 1 : 0);
  if (state == sbLast) return;
  sbLast = state;
  for (int p = 0; p < 10; p++) {
    bool lit = !blink && p < lvl;
    uint16_t c = over ? (blink ? hotDim() : T.hot) : sbCol(p, lit);
    sbSeg(p, c); sbSeg(19 - p, c);
  }
}

static char lastRpm[14], lastSpd[10], lastHex[10], lastHdr[40];
static int lastLink = -1;
static void dashHeader() {
  char nm[40]; carName(nm, sizeof nm);
  char key[40]; snprintf(key, sizeof key, "%d|%d|%s|%d|%d|%d", linkState, (int)demoMode, nm, protoNum, (int)thai, (int)selBle);
  if (!strcmp(key, lastHdr)) return;
  strcpy(lastHdr, key);
  tft.fillRect(0, 11, 320, 19, T.bg);
  statusDot(11, 20);
  StrId sid = demoMode ? S_DEMO : linkState == 2 ? S_CONNECTED : linkState == 1 ? S_CONNECTING : S_SELDEV;
  if (!demoMode && linkState == 2) txt(F_LS, selBle ? "OBD BLE" : "OBD BT", 19, 23, T.dim, T.bg);
  else lab(sid, LB_TILE, 19, thai ? 24 : 23, T.dim, T.bg);
  const char* pn = demoMode ? "" : protoName(protoNum);
  if (pn[0]) txt(F_LS, pn, 313, 23, T.dim, T.bg, AL_R);
  if (nm[0]) {
    FontId f = F_UL; char t[40]; snprintf(t, sizeof t, "%s", nm);
    if (textW(f, t) > 168) f = F_UM;
    if (textW(f, t) > 168) fitText(t, sizeof t, nm, f, 168);
    txt(f, t, 160, f == F_UL ? 25 : 24, T.text, T.bg, AL_C);
  } else if (!demoMode && linkState == 2) lab(S_UNKNOWN, LB_BTNM, 160, 25, T.dim, T.bg, AL_C);
  else if (!demoMode && linkState == 1 && selName[0]) { char t[32]; fitText(t, sizeof t, selName, F_UM, 160); txt(F_UM, t, 160, 24, T.dim, T.bg, AL_C); }
}

static const int NAVX[5] = {6, 68, 131, 193, 256};
static const StrId NAVS[5] = {S_DATA, S_GRAPH, S_TRIP, S_CODES, S_SETUP};
static void navBar() { for (int i = 0; i < 5; i++) btnId(NAVS[i], NAVX[i], 214, 58, 24, false); }

static void centreCaption() {
  txt(F_LS, crpMain ? "CRP  VOLT" : "KM/H", 160, 159, T.dim, T.bg, AL_C, 60);
  hexagon(160, 190, 17, T.panel, T.acc, true); lastHex[0] = 0;
}
static void dashStatic() {
  if (!ringBuilt) ringBuild();
  dashHeader(); lastHdr[0] = 0; dashHeader();
  tft.drawFastHLine(0, 30, 320, T.line);
  for (int i = 0; i < 6; i++) dtileStatic(i);
  ringLit = -1; ringPaint(0); ringNumerals();
  centreCaption(); navBar();
  lastRpm[0] = lastSpd[0] = 0; sbLast = -1;
  for (int p = 0; p < 10; p++) { sbSeg(p, sbCol(p, false)); sbSeg(19 - p, sbCol(p, false)); }
}
static void dashSwap() {
  tft.fillRect(96, 66, 128, 104, T.bg);
  ringNumerals();
  lastSpd[0] = lastRpm[0] = 0; centreCaption();
}
static void dashDyn() {
  bool ok; float rpm = getVal(0x0C, ok); if (!ok) rpm = 0;
  float spd = getVal(0x0D, ok); if (!ok) spd = 0;
  dashHeader();
  int lit = constrain((int)lroundf(rpm / (ringMaxK() * 1000.0f) * 40), 0, 40);
  if (lit != ringLit) ringPaint(lit);
  shiftBar(rpm);
  char b[14], r[14]; fmtThou(r, sizeof r, (int)rpm);
  if (strcmp(r, lastRpm)) { txtVU(F_NM, r, F_LS, "RPM", 160, 92, T.text, T.dim, T.bg, AL_C, 100); strcpy(lastRpm, r); }
  int cv = (int)lroundf(constrain(crpV, 0.0f, 9.99f) * 100.0f);
  if (crpMain) snprintf(b, sizeof b, "%d.%02d", cv / 100, cv % 100); else snprintf(b, sizeof b, "%d", (int)spd);
  if (strcmp(b, lastSpd)) { txt(F_NXL, b, 160, 138, crpMain ? T.accL : T.text, T.bg, AL_C, 84); strcpy(lastSpd, b); }
  char h[10]; if (crpMain) snprintf(h, sizeof h, "%d", (int)spd); else snprintf(h, sizeof h, "%d.%d", cv / 100, (cv / 10) % 10);
  if (strcmp(h, lastHex)) { txt(F_NS, h, 160, 195, crpMain ? T.text : T.accL, T.panel, AL_C, 24); strcpy(lastHex, h); }
  for (int i = 0; i < 6; i++) dtileDyn(i);
}

// =====================================================================================
//  DATA grid, ALL PIDS list, GRAPH, TRIP, PEAKS
// =====================================================================================
static const int GX[4] = {6, 84, 162, 240}, GY[4] = {34, 85, 136, 187};         // data grid: tiles are 73 x 48
static char gCache[16][24];

static void dataTileStatic(int i) {
  DTile t; dtileOf(i, t); int x = GX[i & 3], y = GY[i >> 2];
  panel(x, y, 73, 48, 4, CUT_BL | CUT_BR, T.panel, T.line);
  bracket(x, y, 73, T.acc, true, true);
  lab(t.lab, LB_TILE, x + 36, thai ? y + 15 : y + 13, T.dim, T.panel, AL_C);
  gCache[i][0] = 0;
}
static void dataTileDyn(int i) {
  DTile t; dtileOf(i, t); int x = GX[i & 3], y = GY[i >> 2]; bool ok;
  float v = metricVal(t.src, ok) * t.mul; char b[14], key[24]; uint16_t col = T.dim; int lit = 0;
  if (ok) { fmtv(b, sizeof b, v, t.dec); col = t.mi >= 0 ? valCol(MET[t.mi], v) : T.text; lit = constrain((int)lroundf((v - t.bmin) / (t.bmax - t.bmin) * 8), 0, 8); } else strcpy(b, "--");
  snprintf(key, sizeof key, "%s|%04X|%d", b, col, lit);
  if (!strcmp(key, gCache[i])) return;
  strcpy(gCache[i], key);
  valueFit(b, t.unit, x + 36, y + 36, col, T.panel, AL_C, 64, 64, true);
  segBar(x + 13, y + 41, 8, lit, col == T.dim ? T.dark : col, T.dark);
}
static void dataStatic() {
  titleBar(S_ENGINE, 1); leftBtn(S_ALLPID);
  for (int i = 0; i < 16; i++) dataTileStatic(i);
}
static void dataDyn() { for (int i = 0; i < 16; i++) dataTileDyn(i); }

// ---------- all PIDs ----------
static const int ALL_ROWS = 7;
static uint8_t allIdx[NPID]; static int allN = 0, allPg = 0;
static char aCache[ALL_ROWS][16];
static void allBuild() {
  allN = 0;
  for (int i = 0; i < NPID; i++) if (allMode || demoMode || psup[PIDS[i].id]) allIdx[allN++] = (uint8_t)i;
  int pages = max(1, (allN + ALL_ROWS - 1) / ALL_ROWS);
  allPg = constrain(allPg, 0, pages - 1);
}
static int allPages() { return max(1, (allN + ALL_ROWS - 1) / ALL_ROWS); }
static void allStatic() {
  allBuild();
  titleBar(S_ALLPID, 2);
  for (int r = 0; r < ALL_ROWS; r++) {
    int k = allPg * ALL_ROWS + r, y = 34 + r * 25; aCache[r][0] = 0;
    if (k >= allN) continue;
    const Pid& p = PIDS[allIdx[k]];
    char id[6]; snprintf(id, sizeof id, "%02X", p.id);
    txt(F_LS, id, 8, y + 17, T.acc, T.bg);
    char nm[40]; fitText(nm, sizeof nm, p.name, F_LM, 190); txt(F_LM, nm, 30, y + 17, T.text, T.bg);
    txt(F_LS, p.unit, 276, y + 17, T.dim, T.bg);
    tft.drawFastHLine(6, y + 24, 308, T.dark);
  }
  btnS("<", 6, 214, 50, 24, false); btnS(">", 62, 214, 50, 24, false);
  char b[16]; snprintf(b, sizeof b, "%d/%d", allPg + 1, allPages()); txt(F_LM, b, 160, 231, T.dim, T.bg, AL_C, 70);
  btnId(allMode ? S_ALLPID : S_LIVE, 232, 214, 82, 24, allMode, LB_BTNM);
}
static void allDyn() {
  int n0 = allN; allBuild(); if (allN != n0) { redraw = true; return; }
  for (int r = 0; r < ALL_ROWS; r++) {
    int k = allPg * ALL_ROWS + r; if (k >= allN) break;
    const Pid& p = PIDS[allIdx[k]]; bool ok; float v = getVal(p.id, ok); char b[16];
    if (ok) fmtv(b, sizeof b, v, p.dec); else strcpy(b, "--");
    if (!strcmp(b, aCache[r])) continue;
    strcpy(aCache[r], b);
    txt(F_NM, b, 270, 34 + r * 25 + 19, ok ? T.text : T.dim, T.bg, AL_R, 76);
  }
}

// ---------- graph ----------
struct GSer { int8_t ch; uint16_t col; float lo, hi; StrId lab; const char* unit; uint8_t dec; };
static const int CP_X = 30, CP_Y = 68, CP_W = 252, CP_H = 120;                       // plot area
static uint32_t gDrawn = 0xFFFFFFFFu; static char grCache[3][28]; static uint32_t grAt = 0;
static void gDefs(GSer* s, int& n) {
  uint16_t cy = T.accL, vio = C(168, 130, 255);
  switch (gsel) {
    case 0: n = 2; s[0] = GSer{GC_RPM, T.text, 0, (float)(ringMaxK() * 1000), S_GRPM, "", 0}; s[1] = GSer{GC_SPD, cy, 0, 200, S_SPEED, "km/h", 0}; break;
    case 1: n = 3; s[0] = GSer{GC_COOL, T.text, 20, 140, S_COOLANT, DEGC, 0}; s[1] = GSer{GC_OIL, T.warn, 20, 140, S_OIL, DEGC, 0}; s[2] = GSer{GC_INT, vio, 20, 140, S_INTAKE, DEGC, 0}; break;
    case 2: n = 2; s[0] = GSer{GC_AFR, T.text, 10, 20, S_AFR, "", 1}; s[1] = GSer{GC_STFT, cy, -25, 25, S_STFT, "%", 1}; break;
    default: n = 1; s[0] = GSer{GC_BATT, T.text, 10, 16, S_BATT, "V", 1}; break;
  }
}
static void bufTxt(FontId f, const char* s, int x, int by, uint16_t col, int bx0, int bw, int bh) {     // text drawn into the stripe buffer (no flicker)
  const FontRec* F = FNT[f];
  for (const char* p = s; *p; p++) {
    const GlyphRec* g = glyphOf(F, (unsigned char)*p);
    if (!g) { x += 4; continue; }
    if (g->w) blit4(&F->bm[g->off], g->w, g->h, x + g->dx - bx0, by + g->dy, bw, bh, col);
    x += g->adv;
  }
}
static int16_t gY[3][GN];
static bool gYAt(int k, int X, float x0f, float dxp, int cnt, int& y) {
  if (cnt < 1) return false;
  float t = (X - x0f) / dxp;
  if (t < -0.01f || t > cnt - 1 + 0.01f) return false;
  if (cnt == 1) { if (gY[k][0] < 0) return false; y = gY[k][0]; return true; }
  int i = (int)t; if (i > cnt - 2) i = cnt - 2; if (i < 0) i = 0;
  float f = t - i; int a = gY[k][i], b = gY[k][i + 1];
  if (a < 0 && b < 0) return false;
  if (a < 0) { y = b; return true; }
  if (b < 0) { y = a; return true; }
  y = (int)lroundf(a + (b - a) * f); return true;
}
static int gTh(const GSer* s, int n, int idx, float v) {      // y of a threshold on the scale of series idx
  (void)n; float f = (v - s[idx].lo) / (s[idx].hi - s[idx].lo);
  return (CP_H - 1) - (int)lroundf(constrain(f, 0.0f, 1.0f) * (CP_H - 1));
}
static void chartRender(const GSer* s, int n) {
  int cnt = gCnt[gmode]; const float dxp = (CP_W - 1) / (float)(GN - 1), x0f = (CP_W - 1) - (cnt - 1) * dxp;
  for (int k = 0; k < n; k++) for (int j = 0; j < GN; j++) {
    int16_t raw = j < cnt ? gH[s[k].ch][gmode][j] : (int16_t)-32768;
    if (raw == -32768) { gY[k][j] = -1; continue; }
    float v = raw / gScale(s[k].ch), f = (v - s[k].lo) / (s[k].hi - s[k].lo);
    gY[k][j] = (int16_t)((CP_H - 1) - (int)lroundf(constrain(f, 0.0f, 1.0f) * (CP_H - 1)));
  }
  int thN = 0, thY[2]; uint16_t thC[2]; char thT[2][20]; thT[0][0] = thT[1][0] = 0;     // dashed limit lines
  if (gsel == 0) { thY[thN] = gTh(s, n, 0, (float)shiftRpm); thC[thN++] = T.hot; }
  else if (gsel == 1) {
    thY[thN] = gTh(s, n, 0, 100); thC[thN] = T.warn; snprintf(thT[thN], 20, "%s 100", EN[S_COOLANT]); thN++;
    thY[thN] = gTh(s, n, 0, 120); thC[thN] = T.hot; snprintf(thT[thN], 20, "%s 120", EN[S_OIL]); thN++;
  }
  else if (gsel == 2) { thY[thN] = gTh(s, n, 0, 14.7f); thC[thN++] = T.good; }
  else { thY[thN] = gTh(s, n, 0, 12.0f); thC[thN++] = T.warn; }
  const int SW = TB_CAP / CP_H;
  for (int x0 = 0; x0 < CP_W; x0 += SW) {
    int w = min(SW, CP_W - x0);
    for (int i = 0; i < w * CP_H; i++) tPix[i] = T.panel;
    for (int r = 0; r <= 4; r++) { int y = r * (CP_H - 1) / 4; for (int c = 0; c < w; c++) tPix[y * w + c] = T.dark; }
    for (int q = 0; q <= 4; q++) { int X = q * (CP_W - 1) / 4 - x0; if (X >= 0 && X < w) for (int y = 0; y < CP_H; y++) tPix[y * w + X] = T.dark; }
    for (int t = 0; t < thN; t++) for (int c = 0; c < w; c++) if (((x0 + c) & 7) < 5) tPix[thY[t] * w + c] = thC[t];
    if (CP_W - 1 >= x0 && CP_W - 1 < x0 + w) for (int y = 0; y < CP_H; y++) tPix[y * w + (CP_W - 1 - x0)] = T.accDim;     // "now" edge
    for (int k = 0; k < n; k++) {
      for (int c = 0; c < w; c++) {
        int X = x0 + c, ya, yb;
        if (!gYAt(k, X, x0f, dxp, cnt, ya)) continue;
        if (!gYAt(k, X + 1, x0f, dxp, cnt, yb)) yb = ya;
        int lo = min(ya, yb), hi = max(ya, yb) + 1;
        for (int y = max(0, lo); y <= min(CP_H - 1, hi); y++) tPix[y * w + c] = s[k].col;
      }
    }
    for (int t = 0; t < thN; t++) if (thT[t][0]) bufTxt(F_LS, thT[t], CP_W - 6 - textW(F_LS, thT[t]), thY[t] - 3, thC[t], x0, w, CP_H);
    tft.pushImage(CP_X + x0, CP_Y, w, CP_H, tPix);
  }
}
static void graphAxes() {
  GSer s[3]; int n; gDefs(s, n);
  panel(6, 64, 308, 148, 4, CUT_BL | CUT_BR, T.panel, T.line);
  for (int r = 0; r <= 4; r++) {
    int y = CP_Y + r * (CP_H - 1) / 4; char b[10];
    float v = s[0].lo + (s[0].hi - s[0].lo) * (4 - r) / 4;
    if (gsel == 0) { if (v >= 1000 && ((int)v % 1000) == 0) snprintf(b, sizeof b, "%dK", (int)v / 1000); else if (v >= 1000) snprintf(b, sizeof b, "%.1fK", v / 1000); else snprintf(b, sizeof b, "%d", (int)v); }
    else snprintf(b, sizeof b, "%.*f", s[0].dec, v);
    txt(F_LS, b, CP_X - 4, y + 3, T.dim, T.panel, AL_R, 18);
    if (n >= 2 && gsel != 1) {
      float v2 = s[1].lo + (s[1].hi - s[1].lo) * (4 - r) / 4; snprintf(b, sizeof b, "%.*f", s[1].dec, v2);
      txt(F_LS, b, CP_X + CP_W + 4, y + 3, s[1].col, T.panel, AL_L, 22);
    }
  }
  static const char* const T60[5] = {"-60S", "-45", "-30", "-15", "NOW"}, *const T15[5] = {"-15M", "-11", "-8", "-4", "NOW"};
  for (int q = 0; q < 5; q++) txt(F_LS, gmode ? T15[q] : T60[q], CP_X + q * (CP_W - 1) / 4, 204, T.dim, T.panel, q == 4 ? AL_R : (q == 0 ? AL_L : AL_C), 0);
}
static void graphReadouts(bool statics) {
  GSer s[3]; int n; gDefs(s, n);
  int pw = (308 - (n - 1) * 4) / n;
  for (int k = 0; k < n; k++) {
    int x = 6 + k * (pw + 4), y = 33, lb = thai ? y + 12 : y + 11;
    if (statics) {
      panel(x, y, pw, 29, 4, CUT_BL | CUT_BR, T.panel, T.line); bracket(x, y, pw, T.acc, true, true);
      tft.fillCircle(x + 9, y + 8, 2, s[k].col);
      lab(s[k].lab, LB_TILE, x + 15, lb, T.dim, T.panel);
      if (pw >= 140) lab(S_GMAX, LB_TILE, x + pw - 48, tby(y + 26), T.dim, T.panel, AL_R);
      tft.fillTriangle(x + pw - 43, y + 25, x + pw - 39, y + 25, x + pw - 41, y + 19, T.hot);
      grCache[k][0] = 0;
    }
    int ch = s[k].ch; float mx = -1e9f; bool any = false;
    for (int j = 0; j < gCnt[gmode]; j++) { int16_t r = gH[ch][gmode][j]; if (r != -32768) { mx = fmaxf(mx, r / gScale(ch)); any = true; } }
    char v[14], m[14], key[28];
    if (gOk[ch]) { if (ch == GC_RPM) fmtThou(v, sizeof v, (int)gLast[ch]); else fmtv(v, sizeof v, gLast[ch], s[k].dec); } else strcpy(v, "--");
    if (any) { if (ch == GC_RPM) fmtThou(m, sizeof m, (int)mx); else fmtv(m, sizeof m, mx, s[k].dec); } else strcpy(m, "--");
    snprintf(key, sizeof key, "%s|%s", v, m);
    if (!statics && !strcmp(key, grCache[k])) continue;
    strcpy(grCache[k], key);
    int vw = pw >= 140 ? pw - 100 : pw - 52; bool u = textW(F_NM, v) + textW(F_LS, s[k].unit) + 3 <= vw;
    txtVU(F_NM, v, F_LS, u ? s[k].unit : "", x + 8, y + 26, gOk[ch] ? T.text : T.dim, T.dim, T.panel, AL_L, vw);
    txt(F_LS, m, x + pw - 6, y + 25, T.hot, T.panel, AL_R, 34);
  }
}
static void graphStatic() {
  titleBar(S_GRAPH, 1);
  lab(gmode ? S_G15M : S_G60S, LB_TILE, 8, thai ? 20 : 19, T.dim, T.bg);
  graphReadouts(true); graphAxes();
  static const StrId GN_[4] = {S_GRPM, S_GTEMPS, S_GAFR, S_GBATT};
  for (int i = 0; i < 4; i++) btnId(GN_[i], NAVX[i], 214, 58, 24, gsel == i);
  btnId(gmode ? S_G15M : S_G60S, NAVX[4], 214, 58, 24, false);
  gDrawn = 0xFFFFFFFFu;
}
static void graphDyn() {
  GSer s[3]; int n; gDefs(s, n);
  uint32_t sig = gPush[gmode] * 8 + gsel + (uint32_t)gmode * 3;
  if (sig != gDrawn) { gDrawn = sig; chartRender(s, n); graphReadouts(false); }
}

// ---------- trip ----------
static char tcache[8][26];
static void tripTile(int i, int x, int y, int w, int h, StrId lb, const char* v, const char* unit, uint16_t col, bool statics) {
  if (statics) { panel(x, y, w, h, 4, CUT_BL | CUT_BR, T.panel, T.line); bracket(x, y, w, T.acc, true, true); lab(lb, LB_TILE, x + w / 2, thai ? y + 15 : y + 13, T.dim, T.panel, AL_C); tcache[i][0] = 0; }
  char key[26]; snprintf(key, sizeof key, "%s|%04X", v, col);
  if (!statics && !strcmp(key, tcache[i])) return;
  strcpy(tcache[i], key);
  valueFit(v, unit, x + w / 2, y + h - 7, col, T.panel, AL_C, w - 8, w - 8, true);
}
static void tripBars();
static void tripDraw(bool st) {
  char b[24], u[12]; bool have = tFuel > 0.002f || tNow > 0 || tNowKmL > 0;
  // left header text: range
  float rg = rangeKm();
  if (st) { lab(S_RANGE, LB_TILE, 8, thai ? 20 : 19, T.dim, T.bg); }
  { char r[12]; if (rg >= 0) snprintf(r, sizeof r, "%d km", (int)rg); else strcpy(r, "-- km"); char key[14]; snprintf(key, sizeof key, "%s", r);
    if (st || strcmp(key, tcache[7])) { strcpy(tcache[7], key); txt(F_LS, r, 8 + labW(S_RANGE, LB_TILE) + 5, 19, T.text, T.bg, AL_L, 44); } }
  // NOW + AVERAGE
  float nowv = unitL100 ? tNow : tNowKmL; if (!have || nowv <= 0) strcpy(b, "--"); else fmtv(b, sizeof b, nowv, 1);
  if (st) { panel(6, 34, 150, 58, 4, CUT_BL | CUT_BR, T.panel, T.line); bracket(6, 34, 150, T.acc, true, false); lab(S_NOW, LB_TILE, 14, thai ? 49 : 47, T.dim, T.panel);
    txt(F_LS, unitL100 ? "L/100km" : "km/L", 148, 47, T.dim, T.panel, AL_R); tcache[0][0] = 0; }
  if (st || strcmp(b, tcache[0])) { strcpy(tcache[0], b); txt(F_NHL, b, 81, 78, T.text, T.panel, AL_C, 120); }
  float al = tripAvgL100(), avgv = al > 0 ? (unitL100 ? al : 100.0f / al) : 0; if (avgv > 0) fmtv(b, sizeof b, avgv, 1); else strcpy(b, "--");
  if (st) { panel(160, 34, 154, 58, 4, CUT_TL | CUT_BR, T.panel, T.line); bracket(160, 34, 154, T.good, false, true); lab(S_TAVG, LB_TILE, 168, thai ? 49 : 47, T.dim, T.panel);
    txt(F_LS, unitL100 ? "L/100km" : "km/L", 306, 47, T.dim, T.panel, AL_R); tcache[1][0] = 0; }
  snprintf(u, sizeof u, "%s", al > 0 ? (unitL100 ? "km/L" : "L/100km") : ""); char b2[24]; snprintf(b2, sizeof b2, "%s|%s", b, u);
  if (st || strcmp(b2, tcache[1])) {
    strcpy(tcache[1], b2); txt(F_NHL, b, 237, 76, avgv > 0 ? T.good : T.dim, T.panel, AL_C, 130);
    char s2[24]; if (al > 0) snprintf(s2, sizeof s2, "%.1f %s", unitL100 ? 100.0f / al : al, u); else s2[0] = 0;
    txt(F_LS, s2, 237, 88, T.dim, T.panel, AL_C, 120);
  }
  // small tiles
  char v[16];
  fmtv(v, sizeof v, tDist, 1); tripTile(2, GX[0], 96, 73, 44, S_DIST, v, "km", T.text, st);
  int mins = (int)(tSec / 60); snprintf(v, sizeof v, "%d:%02d", mins / 60, mins % 60); tripTile(3, GX[1], 96, 73, 44, S_DTIME, v, "h", T.text, st);
  float avs = tSec > 5 ? tDist / (tSec / 3600.0f) : 0; fmtv(v, sizeof v, avs, 0); tripTile(4, GX[2], 96, 73, 44, S_ASPD, v, "km/h", T.text, st);
  if (tFuel > 0.002f) fmtv(v, sizeof v, tFuel, 2); else strcpy(v, "--"); tripTile(5, GX[3], 96, 73, 44, S_FUELUSED, v, "L", tFuel > 0.002f ? T.text : T.dim, st);
  if (st) { panel(6, 144, 308, 68, 4, CUT_BL | CUT_BR, T.panel, T.line); lab(S_EACHMIN, LB_TILE, 14, thai ? 160 : 157, T.dim, T.panel); }
  tripBars();
}
static int tbN = -1; static uint32_t tbAt = 0;
static void tripBars() {
  static bool first = true;
  if (!redraw && !first && tbN == nMin && millis() - tbAt < 4000) return;
  first = false; tbN = nMin; tbAt = millis();
  float eff[31]; int ne = 0;
  for (int i = max(0, nMin - 29); i < nMin; i++) eff[ne++] = minFuel[i] > 0.0004f ? minDist[i] / minFuel[i] : 0;
  if (curMinF > 0.0004f && ne < 30) eff[ne++] = curMinD / curMinF;
  float al = tripAvgL100(), avg = al > 0 ? 100.0f / al : 0, mx = 20;
  for (int i = 0; i < ne; i++) mx = fmaxf(mx, eff[i] * 1.15f);
  if (avg > 0) mx = fmaxf(mx, avg * 1.3f);
  tft.fillRect(8, 160, 304, 50, T.panel);
  const int BASE = 206, BH = 38;
  for (int i = 0; i < ne; i++) {
    int slot = 30 - ne + i, x = 12 + slot * 10, h = constrain((int)lroundf(eff[i] / mx * BH), eff[i] > 0 ? 2 : 0, BH);
    tft.fillRect(x, BASE - h, 7, h, (avg > 0 && eff[i] >= avg) ? T.good : T.dim);
  }
  if (avg > 0) { int y = BASE - constrain((int)lroundf(avg / mx * BH), 0, BH); for (int x = 10; x < 312; x += 6) tft.drawFastHLine(x, y, 3, T.good); }
  char t[24]; if (avg > 0) snprintf(t, sizeof t, "%s %.1f", EN[S_AVG], avg); else snprintf(t, sizeof t, "%s --", EN[S_AVG]);
  txt(F_LS, t, 200, 157, avg > 0 ? T.good : T.dim, T.panel, AL_R, 0);
  lab(S_LAST30, LB_TILE, 306, thai ? 160 : 157, T.dim, T.panel, AL_R);
}
static void tripStatic() {
  titleBar(S_TRIP, 1);
  tripDraw(true);
  btnId(S_RESET, 6, 214, 100, 24, false, LB_BTNM); btnId(S_UNITS, 110, 214, 100, 24, false, LB_BTNM); btnId(S_PEAKS, 214, 214, 100, 24, false, LB_BTNM);
}
static void tripDyn() { tripDraw(false); }

// ---------- peaks ----------
static const int PKX[3] = {6, 110, 214}, PKY[3] = {34, 93, 152};
static char pcache[9][26];
static void clockStr(char* d, size_t n, uint32_t s) { if (s >= 3600) snprintf(d, n, "%u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60)); else snprintf(d, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }
static void peaksDraw(bool st) {
  const Peaks& p = pkAllMode ? pkA : pkD;
  struct E { StrId lb; float v; int dec; const char* unit; uint32_t t; bool good, bad; };
  const E e[9] = {
    {S_MAXRPM, p.rpm, 0, "", p.tr, false, false}, {S_TOPSPEED, p.spd, 0, "km/h", p.ts, false, false}, {S_BEST0100, p.best, 2, "s", 0, true, false},
    {S_MAXCOOL, p.cool, 0, DEGC, p.tc, false, p.cool >= 108}, {S_MAXOIL, p.oil, 0, DEGC, p.to, false, p.oil >= 130}, {S_MAXLOAD, p.load, 0, "%", p.tl, false, false},
    {S_MAXINT, p.intake, 0, DEGC, p.ti, false, false}, {S_MINBATT, p.minB, 1, "V", p.tmin, false, p.minB < 11.8f && p.minB > 1}, {S_MAXBATT, p.maxB, 1, "V", p.tmax, false, false}};
  for (int i = 0; i < 9; i++) {
    int x = PKX[i % 3], y = PKY[i / 3]; char v[16], key[26], tm[16] = "";
    bool has = i == 7 ? (p.minB < 90) : (e[i].v > 0.0001f);
    if (!has) strcpy(v, "--"); else if (i == 0) fmtThou(v, sizeof v, (int)e[i].v); else fmtv(v, sizeof v, e[i].v, e[i].dec);
    if (has && !pkAllMode && i != 2) clockStr(tm, sizeof tm, e[i].t);
    uint16_t col = !has ? T.dim : e[i].bad ? T.hot : e[i].good ? T.good : T.text;
    snprintf(key, sizeof key, "%s|%s|%04X", v, tm, col);
    if (st) { panel(x, y, 100, 56, 4, CUT_BL | CUT_BR, T.panel, T.line); bracket(x, y, 100, e[i].good ? T.good : T.acc, true, true); lab(e[i].lb, LB_TILE, x + 50, thai ? y + 15 : y + 13, T.dim, T.panel, AL_C); pcache[i][0] = 0; }
    if (!st && !strcmp(key, pcache[i])) continue;
    strcpy(pcache[i], key);
    valueFit(v, e[i].unit, x + 50, y + 38, col, T.panel, AL_C, 90, 90, true);
    if (tm[0]) { int aw = labW(S_ATTIME, LB_TILE), tw = textW(F_LS, tm), x0 = x + 50 - (aw + 4 + tw) / 2; tft.fillRect(x + 6, y + 44, 88, 11, T.panel); lab(S_ATTIME, LB_TILE, x0, thai ? y + 54 : y + 52, T.dim, T.panel); txt(F_LS, tm, x0 + aw + 4, y + 52, T.dim, T.panel); }
    else tft.fillRect(x + 6, y + 44, 88, 11, T.panel);
  }
}
static void peaksStatic() {
  titleBar(S_PEAKS, 2);
  lab(pkAllMode ? S_ALLTIME : S_THISDRIVE, LB_TILE, 8, thai ? 20 : 19, T.dim, T.bg);
  peaksDraw(true);
  btnId(S_RESET, 6, 214, 150, 24, false, LB_BTNM); btnId(pkAllMode ? S_THISDRIVE : S_ALLTIME, 164, 214, 150, 24, pkAllMode, LB_BTNM);
}
static void peaksDyn() { peaksDraw(false); }

// =====================================================================================
//  CODES, STATUS (monitors / freeze frame / ECU info), SETUP, SETUP 2, BLUETOOTH, VEHICLE
// =====================================================================================

struct DtcD { const char* c; const char* d; };
static const DtcD DTCD[] = {
  {"P0016", "CRANK / CAM CORRELATION B1 A"}, {"P0100", "MAF SENSOR CIRCUIT"}, {"P0101", "MAF RANGE / PERFORMANCE"}, {"P0102", "MAF CIRCUIT LOW"}, {"P0103", "MAF CIRCUIT HIGH"},
  {"P0105", "MAP SENSOR CIRCUIT"}, {"P0106", "MAP RANGE / PERFORMANCE"}, {"P0107", "MAP CIRCUIT LOW"}, {"P0108", "MAP CIRCUIT HIGH"},
  {"P0110", "INTAKE AIR TEMP CIRCUIT"}, {"P0112", "INTAKE AIR TEMP LOW"}, {"P0113", "INTAKE AIR TEMP HIGH"},
  {"P0115", "COOLANT TEMP CIRCUIT"}, {"P0116", "COOLANT TEMP RANGE"}, {"P0117", "COOLANT TEMP LOW"}, {"P0118", "COOLANT TEMP HIGH"},
  {"P0120", "THROTTLE SENSOR A CIRCUIT"}, {"P0121", "THROTTLE SENSOR A RANGE"}, {"P0122", "THROTTLE SENSOR A LOW"}, {"P0123", "THROTTLE SENSOR A HIGH"},
  {"P0125", "COOLANT TOO COLD FOR CLOSED LOOP"}, {"P0128", "COOLANT BELOW THERMOSTAT TEMP"},
  {"P0130", "O2 SENSOR CIRCUIT B1 S1"}, {"P0131", "O2 SENSOR LOW VOLTAGE B1 S1"}, {"P0132", "O2 SENSOR HIGH VOLTAGE B1 S1"}, {"P0133", "O2 SENSOR SLOW RESPONSE B1 S1"},
  {"P0134", "O2 SENSOR NO ACTIVITY B1 S1"}, {"P0135", "O2 HEATER CIRCUIT B1 S1"}, {"P0136", "O2 SENSOR CIRCUIT B1 S2"}, {"P0137", "O2 SENSOR LOW VOLTAGE B1 S2"},
  {"P0138", "O2 SENSOR HIGH VOLTAGE B1 S2"}, {"P0139", "O2 SENSOR SLOW RESPONSE B1 S2"}, {"P0141", "O2 HEATER CIRCUIT B1 S2"},
  {"P0171", "SYSTEM TOO LEAN BANK 1"}, {"P0172", "SYSTEM TOO RICH BANK 1"}, {"P0174", "SYSTEM TOO LEAN BANK 2"}, {"P0175", "SYSTEM TOO RICH BANK 2"},
  {"P0190", "FUEL RAIL PRESSURE SENSOR"}, {"P0191", "FUEL RAIL PRESSURE RANGE"}, {"P0192", "FUEL RAIL PRESSURE LOW"}, {"P0193", "FUEL RAIL PRESSURE HIGH"},
  {"P0201", "INJECTOR CIRCUIT CYL 1"}, {"P0202", "INJECTOR CIRCUIT CYL 2"}, {"P0203", "INJECTOR CIRCUIT CYL 3"}, {"P0204", "INJECTOR CIRCUIT CYL 4"},
  {"P0217", "ENGINE OVERTEMPERATURE"}, {"P0219", "ENGINE OVERSPEED"}, {"P0230", "FUEL PUMP PRIMARY CIRCUIT"},
  {"P0234", "TURBO OVERBOOST"}, {"P0235", "TURBO BOOST SENSOR A"}, {"P0237", "TURBO BOOST SENSOR LOW"}, {"P0238", "TURBO BOOST SENSOR HIGH"}, {"P0299", "TURBO UNDERBOOST"},
  {"P0300", "RANDOM / MULTIPLE CYLINDER MISFIRE"}, {"P0301", "CYLINDER 1 MISFIRE"}, {"P0302", "CYLINDER 2 MISFIRE"}, {"P0303", "CYLINDER 3 MISFIRE"}, {"P0304", "CYLINDER 4 MISFIRE"},
  {"P0325", "KNOCK SENSOR 1 CIRCUIT"}, {"P0335", "CRANKSHAFT SENSOR A CIRCUIT"}, {"P0336", "CRANKSHAFT SENSOR A RANGE"}, {"P0340", "CAMSHAFT SENSOR A CIRCUIT"}, {"P0341", "CAMSHAFT SENSOR A RANGE"},
  {"P0351", "IGNITION COIL A CIRCUIT"}, {"P0352", "IGNITION COIL B CIRCUIT"}, {"P0353", "IGNITION COIL C CIRCUIT"}, {"P0354", "IGNITION COIL D CIRCUIT"},
  {"P0400", "EGR FLOW"}, {"P0401", "EGR FLOW INSUFFICIENT"}, {"P0402", "EGR FLOW EXCESSIVE"}, {"P0403", "EGR CIRCUIT"},
  {"P0420", "CATALYST EFFICIENCY BELOW LIMIT B1"}, {"P0430", "CATALYST EFFICIENCY BELOW LIMIT B2"},
  {"P0440", "EVAP SYSTEM"}, {"P0441", "EVAP PURGE FLOW"}, {"P0442", "EVAP SMALL LEAK"}, {"P0443", "EVAP PURGE VALVE CIRCUIT"}, {"P0446", "EVAP VENT CONTROL"}, {"P0455", "EVAP LARGE LEAK"}, {"P0456", "EVAP VERY SMALL LEAK"},
  {"P0480", "COOLING FAN 1 CONTROL"}, {"P0500", "VEHICLE SPEED SENSOR"}, {"P0505", "IDLE CONTROL SYSTEM"}, {"P0506", "IDLE RPM LOWER THAN EXPECTED"}, {"P0507", "IDLE RPM HIGHER THAN EXPECTED"},
  {"P0562", "SYSTEM VOLTAGE LOW"}, {"P0563", "SYSTEM VOLTAGE HIGH"}, {"P0571", "BRAKE SWITCH CIRCUIT"},
  {"P0600", "SERIAL COMMUNICATION LINK"}, {"P0601", "ECU MEMORY CHECKSUM"}, {"P0606", "ECU PROCESSOR FAULT"}, {"P0627", "FUEL PUMP CONTROL CIRCUIT"},
  {"P0638", "THROTTLE ACTUATOR RANGE B1"}, {"P0650", "MIL CONTROL CIRCUIT"}, {"P0700", "TRANSMISSION CONTROL SYSTEM"},
  {"P2002", "DPF EFFICIENCY BELOW LIMIT B1"}, {"P2101", "THROTTLE ACTUATOR MOTOR CIRCUIT"}, {"P2135", "THROTTLE SENSOR A / B CORRELATION"},
  {"P2452", "DPF PRESSURE SENSOR A"}, {"P2459", "DPF REGENERATION FREQUENCY"}, {"P20EE", "SCR NOX CATALYST EFFICIENCY"},
  {"U0100", "LOST COMMUNICATION WITH ECM / PCM"}, {"U0101", "LOST COMMUNICATION WITH TCM"}, {"U0121", "LOST COMMUNICATION WITH ABS"},
  {"U0140", "LOST COMMUNICATION WITH BODY CONTROL"}, {"U0155", "LOST COMMUNICATION WITH CLUSTER"},
};
static const char* dtcDesc(const char* c) {
  for (const DtcD& e : DTCD) if (!strcmp(e.c, c)) return e.d;
  switch (c[0]) {
    case 'P': return c[1] == '0' || c[1] == '2' ? "GENERIC POWERTRAIN CODE" : "MAKER SPECIFIC POWERTRAIN CODE";
    case 'B': return "BODY CODE";
    case 'C': return "CHASSIS CODE";
    case 'U': return "NETWORK COMMUNICATION CODE";
  }
  return "";
}

// ---------- fault codes ----------
static const int CODE_ROWS = 5;
static int codePg = 0; static bool demoCleared = false, clearArmed = false; static uint32_t clearAt = 0, codesSig = 0xFFFFFFFFu;
static void demoReadCodes() {
  memset(codeList, 0, sizeof codeList);
  if (demoCleared) { codeCount = 0; codeState = 2; mil = false; }
  else {
    strcpy(codeList[0].code, "P0171"); strcpy(codeList[1].code, "P0420"); strcpy(codeList[2].code, "P0300"); codeList[2].pend = 1;
    codeCount = 3; codeState = 2; mil = true;
  }
  codeVer = codeVer + 1;
}
static void codesRead() {
  codePg = 0; clearArmed = false;
  if (demoMode) { demoReadCodes(); return; }
  if (linkState != 2) { codeState = 4; codeVer = codeVer + 1; return; }
  codeState = 1; codeVer = codeVer + 1; req = 1;
}
static void codesClear() {
  clearArmed = false;
  if (demoMode) { demoCleared = true; codeCount = 0; codeState = 3; mil = false; codeVer = codeVer + 1; return; }
  if (linkState != 2) { codeState = 4; codeVer = codeVer + 1; return; }
  codeState = 1; codeVer = codeVer + 1; req = 2;
}
static void codesBanner() {
  panel(6, 34, 308, 21, 4, CUT_TL | CUT_BR, T.panel, T.line);
  int n = codeCount, st = codeState; bool linkOk = demoMode || linkState == 2;
  uint16_t col; StrId single = S_COUNT, lamp = S_COUNT;
  if (clearArmed) { col = T.warn; single = S_SURE; }
  else if (!linkOk) { col = T.warn; single = S_NOLINK; }
  else if (st == 1 || st == 0) { col = T.warn; single = S_READING; }
  else if (st == 4) { col = T.warn; single = S_NOLINK; }
  else if (st == 3) { col = T.good; single = S_CLEARED; }
  else if (n == 0) { col = T.good; single = S_NOCODE; }
  else { col = mil ? RED_AL : T.warn; lamp = mil ? S_LAMPON : S_LAMPOFF; }
  if (single != S_COUNT) {
    int w = labW(single, LB_TILE), x0 = 160 - (w + 10) / 2;
    tft.fillCircle(x0 + 3, 44, 3, col); lab(single, LB_TILE, x0 + 10, tby(48), col, T.panel);
  } else {
    char nb[8]; snprintf(nb, sizeof nb, "%d", n);
    int wn = textW(F_LS, nb), wc = labW(S_CODESW, LB_TILE), wl = labW(lamp, LB_TILE), tot = 10 + wn + 4 + wc + 14 + wl, x0 = 160 - tot / 2;
    tft.fillCircle(x0 + 3, 44, 3, col);
    txt(F_LS, nb, x0 + 10, 48, col, T.panel); lab(S_CODESW, LB_TILE, x0 + 14 + wn, tby(48), col, T.panel);
    txt(F_LS, "-", x0 + 10 + wn + 4 + wc + 4, 48, T.dim, T.panel); lab(lamp, LB_TILE, x0 + tot - wl, tby(48), col, T.panel);
  }
}
static void codeRow(int r) {
  int y = 58 + r * 22, k = codePg * CODE_ROWS + r;
  tft.fillRect(6, y, 308, 22, T.bg);
  if (k >= codeCount || k >= 24) return;
  const DtcRec& d = codeList[k]; uint16_t c = d.pend ? T.warn : RED_AL;
  panel(6, y, 308, 21, 3, 0, T.panel, T.line);
  tft.fillRect(6, y, 3, 21, c);
  txt(F_UL, d.code, 16, y + 17, T.text, T.panel);
  char ds[44]; fitText(ds, sizeof ds, dtcDesc(d.code), F_LS, 172); txt(F_LS, ds, 76, y + 14, T.text, T.panel);
  lab(d.pend ? S_PENDING : S_STORED, LB_TILE, 308, tby(y + 14), c, T.panel, AL_R);
}
static void codesBtns() {
  int pages = max(1, (min((int)codeCount, 24) + CODE_ROWS - 1) / CODE_ROWS);
  btnId(S_READ, 6, 214, 96, 24, false, LB_BTNM);
  btnId(S_CLEAR, 106, 214, 124, 24, codeCount > 0 || mil || clearArmed, LB_BTNM);
  btnS("<", 236, 214, 36, 24, false); btnS(">", 278, 214, 36, 24, false);
  (void)pages;
}
static void codesStatic() {
  titleBar(S_FAULT, 2); leftBtn(S_STATUS);
  codesBanner();
  for (int r = 0; r < CODE_ROWS; r++) codeRow(r);
  lab(S_CLRNOTE1, LB_TILE, 160, tby(183), T.dim, T.bg, AL_C); lab(S_CLRNOTE2, LB_TILE, 160, tby(198), T.dim, T.bg, AL_C);
  codesBtns(); codesSig = 0xFFFFFFFFu;
}
static void codesDyn() {
  if (clearArmed && millis() - clearAt > 4000) clearArmed = false;
  int pages = max(1, (min((int)codeCount, 24) + CODE_ROWS - 1) / CODE_ROWS); codePg = constrain(codePg, 0, pages - 1);
  uint32_t sig = (uint32_t)codeVer * 256u + (uint32_t)codePg * 16u + (clearArmed ? 1 : 0) + ((demoMode || linkState == 2) ? 2 : 0) + (mil ? 4 : 0) + ((uint32_t)codeState << 20);
  if (sig == codesSig) return;
  codesSig = sig; codesBanner();
  for (int r = 0; r < CODE_ROWS; r++) codeRow(r);
  codesBtns();
}

// ---------- readiness monitors ----------
struct MonDef { StrId lb; uint8_t kind, bit; };            // kind 0 = continuous monitor (byte B), 1 = test (bytes C = supported, D = not complete)
static const MonDef MON_P[8] = {{S_MISFIRE, 0, 0}, {S_FUELSYS, 0, 1}, {S_COMPON, 0, 2}, {S_CATALYST, 1, 0}, {S_EVAP, 1, 2}, {S_O2SENS, 1, 5}, {S_O2HEAT, 1, 6}, {S_EGRVVT, 1, 7}};
static const MonDef MON_D[9] = {{S_MISFIRE, 0, 0}, {S_FUELSYS, 0, 1}, {S_COMPON, 0, 2}, {S_NMHC, 1, 0}, {S_NOXSCR, 1, 1}, {S_BOOSTM, 1, 3}, {S_EXHSENS, 1, 5}, {S_PMFILT, 1, 6}, {S_EGRVVT, 1, 7}};
static int monState(const MonDef& m, uint32_t raw) {        // 0 = not supported, 1 = ready, 2 = not ready
  uint32_t B = (raw >> 16) & 255, C_ = (raw >> 8) & 255, D = raw & 255; bool sup, inc;
  if (m.kind == 0) { sup = B & (1u << m.bit); inc = B & (0x10u << m.bit); } else { sup = C_ & (1u << m.bit); inc = D & (1u << m.bit); }
  return !sup ? 0 : inc ? 2 : 1;
}
static uint32_t monSig = 0xFFFFFFFFu;
static void monitorsDraw() {
  uint32_t raw = monRaw; bool valid = monValid || demoMode, dsl = valid && ((raw >> 16) & 8);
  int nm = dsl ? 9 : 8, rows = (nm + 1) / 2, sup = 0, rdy = 0, pitch = rows > 4 ? 22 : 24, rh = pitch - 2;
  for (int i = 0; i < nm; i++) {
    const MonDef& m = dsl ? MON_D[i] : MON_P[i]; int s = valid ? monState(m, raw) : 0;
    int x = 6 + (i & 1) * 158, y = 36 + (i >> 1) * pitch;
    if (s) { sup++; if (s == 1) rdy++; }
    uint16_t c = s == 1 ? T.good : s == 2 ? T.warn : T.dim;
    panel(x, y, 150, rh, 3, 0, T.panel, T.line);
    tft.fillCircle(x + 9, y + rh / 2, 3, c);
    lab(m.lb, LB_ROW, x + 18, y + rh / 2 + (thai ? 5 : 4), s ? T.text : T.dim, T.panel);
    lab(s == 0 ? S_NA : s == 1 ? S_READY : S_NOTREADY, LB_TILE, x + 144, y + rh / 2 + (thai ? 6 : 4), c, T.panel, AL_R);
  }
  int sy = 36 + rows * pitch + 4, sh = 212 - sy;
  bool all = valid && sup > 0 && rdy == sup; uint16_t c = !valid ? T.dim : all ? T.good : T.warn;
  panel(6, sy, 308, sh, 5, CUT_TL | CUT_BR, T.panel, c);
  if (!valid) lab(S_READING, LB_TILE, 160, tby(sy + 15), T.dim, T.panel, AL_C);
  else if (all) lab(S_ALLREADY, LB_TILE, 160, tby(sy + 15), c, T.panel, AL_C);
  else { char nb[16]; snprintf(nb, sizeof nb, "%d OF %d", rdy, sup); int wn = textW(F_LS, nb), wl = labW(S_READYOF, LB_TILE), x0 = 160 - (wn + 5 + wl) / 2; txt(F_LS, nb, x0, sy + 15, c, T.panel); lab(S_READYOF, LB_TILE, x0 + wn + 5, tby(sy + 15), c, T.panel); }
  static const struct { StrId lb; uint8_t pid; const char* unit; } ST3[3] = {{S_SINCECLR, 0x31, "km"}, {S_WARMUPS, 0x30, ""}, {S_LAMPFOR, 0x21, "km"}};
  for (int i = 0; i < 3; i++) {
    int cx = 6 + 51 + i * 103; bool ok; float v = getVal(ST3[i].pid, ok); char b[14]; if (ok) fmtv(b, sizeof b, v, 0); else strcpy(b, "--");
    lab(ST3[i].lb, LB_TILE, cx, sy + sh - 31, T.dim, T.panel, AL_C);
    txtVU(F_NL, b, F_LS, ST3[i].unit, cx, sy + sh - 9, ok ? T.text : T.dim, T.dim, T.panel, AL_C, 96);
  }
}
static void statusTabs(int active) {
  static const StrId TS[3] = {S_MONITORS, S_FREEZE, S_ECUINFO};
  for (int i = 0; i < 3; i++) btnId(TS[i], 6 + i * 104, 214, 100, 24, i == active, LB_BTNM);
}
static void statusEnter() { if (demoMode) { monRaw = 0x0007E504u; monValid = true; monVer = monVer + 1; } }
static void statusStatic() { titleBar(S_STATUS, 2); leftBtn(S_READ); monitorsDraw(); statusTabs(0); monSig = 0; }
static void statusDyn() {
  uint32_t sig = (uint32_t)monVer + ((uint32_t)(millis() / 2000) << 8);        // the three counters refresh every 2 s
  if (sig == monSig) return;
  monSig = sig; monitorsDraw();
}

// ---------- freeze frame ----------
static const uint8_t FZM[12] = {MI_RPM, MI_SPEED, MI_COOL, MI_LOAD, MI_STFT, MI_LTFT, MI_MAP, MI_INTAKE, MI_THR, MI_MAF, MI_TIMING, MI_BATT};
static void freezeRead() {
  if (demoMode) {
    static const float dv[12] = {2150, 64, 91, 38, 18.8f, 9.4f, 41, 37, 16, 7.3f, 12, 13.9f};
    strcpy(frzCode, demoCleared ? "" : "P0171"); for (int i = 0; i < 12; i++) { frzVal[i] = dv[i]; frzOk[i] = true; }
    frzState = demoCleared ? 3 : 2; frzVer = frzVer + 1; return;
  }
  if (linkState != 2) { frzState = 3; frzVer = frzVer + 1; return; }
  frzState = 1; frzVer = frzVer + 1; req = 3;
}
static void freezeStatic() {
  titleBar(S_STATUS, 2); leftBtn(S_READ);
  panel(6, 34, 308, 32, 4, CUT_TL | CUT_BR, T.panel, T.line);
  if (frzState == 2 && frzCode[0]) {
    txt(F_UL, frzCode, 16, 56, T.text, T.panel);
    char ds[44]; fitText(ds, sizeof ds, dtcDesc(frzCode), F_LS, 200); txt(F_LS, ds, 90, 46, T.text, T.panel);
    lab(S_COND, LB_TILE, 90, tby(59), T.dim, T.panel);
  } else if (frzState == 1) lab(S_READING, LB_TILE, 160, tby(54), T.warn, T.panel, AL_C);
  else lab(S_NOFREEZE, LB_TILE, 160, tby(54), T.dim, T.panel, AL_C);
  for (int i = 0; i < 12; i++) {
    int x = GX[i & 3], y = 70 + (i >> 2) * 48; const Met& m = MET[FZM[i]];
    panel(x, y, 73, 44, 4, CUT_BL | CUT_BR, T.panel, T.line);
    lab(m.lab, LB_TILE, x + 36, thai ? y + 15 : y + 13, T.dim, T.panel, AL_C);
    char b[14]; bool has = frzState == 2 && frzOk[i];
    if (has) fmtv(b, sizeof b, frzVal[i] * m.mul, m.dec); else strcpy(b, "--");
    valueFit(b, m.unit, x + 36, y + 37, has ? T.text : T.dim, T.panel, AL_C, 64, 64, true);
  }
  statusTabs(1);
}

// ---------- ECU info ----------
static void ecuRead() {
  if (demoMode) { strcpy(vinBuf, "JETOBD2DEMO000000"); strcpy(ecuName, "DEMO-ECM"); strcpy(calId, "DEMOCAL01"); ecuInfoVer = ecuInfoVer + 1; return; }
  if (linkState == 2) req = 4;
}
static void ecuBox(int y, int h, StrId lb, const char* v) {
  panel(6, y, 308, h, 5, CUT_TL | CUT_BR, T.panel, T.line); bracket(6, y, 308, T.acc, true, true);
  lab(lb, LB_TILE, 160, tby(y + 14), T.dim, T.panel, AL_C);
  char t[32]; FontId f = F_UL; strncpy(t, v[0] ? v : "--", sizeof t - 1); t[sizeof t - 1] = 0;
  if (textW(f, t) > 292) f = F_UM;
  if (textW(f, t) > 292) fitText(t, sizeof t, t, f, 292);
  txt(f, t, 160, y + h - 11, v[0] ? T.text : T.dim, T.panel, AL_C);
}
static void ecuStatic() {
  titleBar(S_STATUS, 2); leftBtn(S_READ);
  ecuBox(34, 46, S_VIN, vinBuf); ecuBox(84, 46, S_ECUNAME, ecuName); ecuBox(134, 46, S_CALID, calId);
  panel(6, 184, 308, 26, 5, CUT_TL | CUT_BR, T.panel, T.line);
  lab(S_PROTO, LB_TILE, 16, tby(201), T.dim, T.panel);
  const char* pn = demoMode ? "--" : (protoNum ? protoName(protoNum) : "--"); txt(F_UM, pn, 306, 202, T.text, T.panel, AL_R);
  statusTabs(2);
}

// ---------- setup ----------
static const int SU_Y0 = 74, SU_H = 35;
static void ctlBtn(int x, int y, int w, int h, StrId id, bool on) { btnId(id, x, y, w, h, on, LB_BTNM); }
static void ctlTxt(int x, int y, int w, int h, const char* s, bool on) { btnS(s, x, y, w, h, on, F_UM); }
static void rowLabel(int y, int h, StrId mainL, StrId sub) {
  tft.fillRect(6, y, 308, h, T.bg); tft.drawFastHLine(6, y + h - 1, 308, T.dark);
  tft.fillCircle(11, y + h / 2, 2, T.acc);
  if (sub != S_COUNT) {
    lab(mainL, LB_ROW, 20, y + (thai ? h / 2 - 1 : h / 2 - 3), T.text, T.bg);
    lab(sub, LB_TILE, 20, y + h - (thai ? 4 : 6), T.dim, T.bg);
  } else lab(mainL, LB_ROW, 20, y + h / 2 + (thai ? 5 : 4), T.text, T.bg);
}
static void stepBtns(int y, int h, const char* val, const char* unit) {
  btnS("-", 154, y + 4, 38, h - 8, false); btnS("+", 276, y + 4, 38, h - 8, false);
  txtVU(F_NL, val, F_UM, unit, 234, y + h / 2 + 9, T.text, T.dim, T.bg, AL_C, 78);
}
static void setupRow(int i) {
  int y = SU_Y0 + i * SU_H; char b[16];
  switch (i) {
    case 0: rowLabel(y, SU_H, S_SRC, S_COUNT); ctlBtn(154, y + 4, 76, SU_H - 8, S_DEMO, demoMode); ctlTxt(234, y + 4, 80, SU_H - 8, "OBD BT", !demoMode); break;
    case 1: rowLabel(y, SU_H, S_LANG, S_COUNT); ctlTxt(154, y + 4, 76, SU_H - 8, "EN", !thai); ctlBtn(234, y + 4, 80, SU_H - 8, S_THAI, thai); break;
    case 2: rowLabel(y, SU_H, S_SHIFT, S_SHIFTD); fmtThou(b, sizeof b, shiftRpm); stepBtns(y, SU_H, b, ""); break;
    case 3: rowLabel(y, SU_H, S_COLORS, S_COLTIP); ctlBtn(154, y + 4, 76, SU_H - 8, S_STD, !invScreen); ctlBtn(234, y + 4, 80, SU_H - 8, S_INV, invScreen); break;
  }
}
static void themeCard(int i) {
  int x = 6 + i * 156, y = 34; bool sel = theme == i; uint16_t bg = sel ? T.panel2 : T.panel;
  panel(x, y, 152, 36, 5, CUT_TL | CUT_BR, bg, sel ? T.acc : T.line);
  hexagon(x + 22, y + 18, 10, THEMES[i].bg, THEMES[i].acc, true); tft.fillCircle(x + 22, y + 18, 3, THEMES[i].acc);
  lab(i ? S_THRED : S_THBLUE, LB_BTNM, x + 42, y + (thai ? 16 : 15), sel ? T.text : T.dim, bg);
  lab(sel ? S_SELECTED : S_TAPUSE, LB_TILE, x + 42, y + (thai ? 32 : 29), sel ? T.accL : T.dim, bg);
}
static void setupStatic() {
  titleBar(S_SETUP, 1);
  txt(F_LS, FW_VERSION, 10, 20, T.dim, T.bg);
  themeCard(0); themeCard(1);
  for (int i = 0; i < 4; i++) setupRow(i);
  btnId(S_BTNAME, 6, 214, 100, 24, false, LB_BTNM); btnId(S_VEHICLE, 110, 214, 100, 24, false, LB_BTNM); btnId(S_MORE, 214, 214, 100, 24, false, LB_BTNM);
}

// ---------- setup 2 ----------
static const int S2_Y0 = 34, S2_H = 30;
static uint32_t uprotoAt = 0; static int userProtoShown = -1;
static void setup2Row(int i) {
  int y = S2_Y0 + i * S2_H, h = S2_H; char b[16];
  switch (i) {
    case 0: rowLabel(y, h, S_ALERTS, S_ALERTSD); ctlBtn(154, y + 3, 76, h - 6, S_ON, warnOn); ctlBtn(234, y + 3, 80, h - 6, S_OFF, !warnOn); break;
    case 1: rowLabel(y, h, S_PARKSCR, S_PARKSCRD); ctlBtn(154, y + 3, 76, h - 6, S_ON, parkOn); ctlBtn(234, y + 3, 80, h - 6, S_OFF, !parkOn); break;
    case 2: rowLabel(y, h, S_BRIGHT, S_COUNT); snprintf(b, sizeof b, "%d", bright); stepBtns(y, h, b, "%"); break;
    case 3: rowLabel(y, h, S_TANK, S_TANKD); snprintf(b, sizeof b, "%d", tankL); stepBtns(y, h, b, "L"); break;
    case 4: rowLabel(y, h, S_FUELCAL, S_FUELCALD); snprintf(b, sizeof b, "%d", fuelCal); stepBtns(y, h, b, "%"); break;
    case 5:
      rowLabel(y, h, S_PROTO, S_PROTOD);
      btnS("-", 154, y + 4, 38, h - 8, false); btnS("+", 276, y + 4, 38, h - 8, false);
      tft.fillRect(196, y + 2, 78, h - 4, T.bg);
      if (userProto == 0) lab(S_AUTO, LB_BTNM, 235, y + h / 2 + 5, T.text, T.bg, AL_C); else txt(F_UM, protoName(userProto), 235, y + h / 2 + 5, T.text, T.bg, AL_C);
      break;
  }
}
static void setup2Static() {
  titleBar(S_SETUP2, 1); btnS("<", 6, 4, 44, 22, false);
  for (int i = 0; i < 6; i++) setup2Row(i);
  btnId(S_TESTALERT, 6, 214, 150, 24, false, LB_BTNM); btnId(S_RESETPK, 164, 214, 150, 24, false, LB_BTNM);
}

// ---------- bluetooth ----------
static BtDev btView[MAXBT]; static int nBtView = 0, btOff = 0, btViewVer = -1, btViewScan = -1; static uint32_t lastBtDraw = 0;
static void btBuildView() {
  portENTER_CRITICAL(&btMux); nBtView = btCount; memcpy(btView, btList, sizeof(BtDev) * nBtView); portEXIT_CRITICAL(&btMux);
  bool byRssi = scanState != 1;                 // keep the rows still while the scan is running
  for (int i = 1; i < nBtView; i++) {
    BtDev t = btView[i]; int j = i - 1;
    while (j >= 0 && btBetter(t, btView[j], byRssi)) { btView[j + 1] = btView[j]; j--; }
    btView[j + 1] = t;
  }
  if (btOff > max(0, nBtView - 4)) btOff = max(0, nBtView - 4);
}
static void btStartScan() { if (scanState != 1) { scanState = 1; btVer = btVer + 1; scanReq = true; btOff = 0; } }
static void btSelRow() {
  panel(6, 34, 308, 28, 4, CUT_TL | CUT_BR, T.panel2, T.acc);
  if (selAddr[0]) {
    char t[32]; fitText(t, sizeof t, selName[0] ? selName : selAddr, F_UM, 190); txt(F_UM, t, 14, 53, T.text, T.panel2);
    StrId s = demoMode ? S_DEMO : linkState == 2 ? S_CONNECTED : S_CONNECTING; uint16_t c = demoMode ? T.dim : linkState == 2 ? T.good : T.warn;
    lab(s, LB_TILE, 306, tby(52), c, T.panel2, AL_R);
  } else lab(S_NOSEL, LB_BTNM, 160, 53, T.dim, T.panel2, AL_C);
}
static void btRow(int r) {
  int y = 76 + r * 28, i = btOff + r;
  tft.fillRect(6, y, 308, 28, T.bg);
  if (i >= nBtView) return;
  const BtDev& d = btView[i]; bool sel = d.ble == selBle && !strcmp(d.addr, selAddr); uint16_t bg = sel ? T.panel2 : T.panel;
  panel(6, y, 308, 26, 3, 0, bg, sel ? T.acc : T.line);
  int x = 14;
  if (d.obd) { panel(12, y + 6, 30, 14, 3, 0, T.acc, T.acc); txt(F_LS, "OBD", 27, y + 16, T.text, T.acc, AL_C); x = 50; }
  char t[32]; fitText(t, sizeof t, d.name[0] ? d.name : d.addr, F_UM, (d.ble ? 176 : 196) - x);
  txt(F_UM, t, x, y + 18, d.name[0] ? (sel ? T.text : T.accL) : T.dim, bg);
  if (d.ble) { tft.drawRect(194, y + 6, 28, 14, T.accL); txt(F_LS, "BLE", 208, y + 16, T.accL, bg, AL_C); }
  if (sel) { tft.drawLine(228, y + 13, 232, y + 17, T.good); tft.drawLine(229, y + 13, 233, y + 17, T.good); tft.drawLine(233, y + 17, 241, y + 7, T.good); tft.drawLine(234, y + 17, 242, y + 7, T.good); }
  int lv = !d.rssi ? 0 : d.rssi >= -55 ? 4 : d.rssi >= -67 ? 3 : d.rssi >= -78 ? 2 : 1;
  for (int k = 0; k < 4; k++) { int h = 4 + k * 3; tft.fillRect(268 + k * 8, y + 22 - h, 5, h, k < lv ? T.accL : T.dark); }
}
static void btListDraw() {
  btBuildView(); btViewVer = btVer; btViewScan = scanState; lastBtDraw = millis();
  int st = scanState, n = nBtView;
  tft.fillRect(6, 64, 308, 12, T.bg);
  lab(S_NEARBY, LB_TILE, 8, tby(73), T.dim, T.bg);
  if (st == 1) lab(S_SCANNING, LB_TILE, 312, tby(73), T.warn, T.bg, AL_R);
  else if (n > 0) { char c[12]; snprintf(c, sizeof c, "%d", n); int w = labW(S_FOUND, LB_TILE), cw = textW(F_LS, c); txt(F_LS, c, 312 - w - 4 - cw, 73, T.accL, T.bg); lab(S_FOUND, LB_TILE, 312, tby(73), T.dim, T.bg, AL_R); }
  for (int r = 0; r < 4; r++) btRow(r);
  if (n == 0) { StrId m = st == 1 ? S_SCANNING : st == 2 ? S_NOTFOUND : st == 3 ? S_SCANFAIL : S_PRESSSCAN; lab(m, LB_BTNM, 160, 134, st == 3 ? T.hot : T.dim, T.bg, AL_C); }
  bool up = btOff > 0, dn = btOff + 4 < nBtView;
  btnS("<", 6, 214, 50, 24, up); btnS(">", 62, 214, 50, 24, dn);
  lab(S_TAPPICK, LB_TILE, 220, tby(230), T.dim, T.bg, AL_C, 190);
}
static void btStatic() {
  titleBar(S_BTDEV, 2); leftBtn(S_SCAN);
  btSelRow(); btListDraw();
  if (!selAddr[0] && scanState != 1 && !demoMode) btStartScan();
}
static void btDyn() {
  bool stateChg = btViewScan != scanState;
  if (btViewVer == btVer && !stateChg) return;
  if (!stateChg && millis() - lastBtDraw < 250) return;
  btListDraw(); if (stateChg) btSelRow();
}
static void btPick(int i) {                  // the user tapped a device: remember it, switch to OBD mode and connect
  BtDev d = btView[i];
  portENTER_CRITICAL(&btMux);
  bool same = !strcmp(d.addr, selAddr);                         // tapping the adapter that is already chosen keeps what we learned about its PIN
  memcpy(selAddr, d.addr, sizeof selAddr); strcpy(selName, d.name[0] ? d.name : d.addr);
  selBle = d.ble; selAType = d.atype; bleTry = 0; selGen = selGen + 1;
  if (!same) { pinIdx = 0; pinOk = false; }
  portEXIT_CRITICAL(&btMux);
  demoMode = false; carBuf[0] = 0; vinBuf[0] = 0; linkState = 1; memset(psup, 0, sizeof psup); engineRan = false;
  scanStop = true; reconnectReq = true;
  savePrefs(); updateWants();
  btSelRow(); btListDraw();
}

// ---------- vehicle ----------
static int vehStep = 0, vehBrandSel = 0;
static const int VBX[2] = {6, 164};
static void vehStatic() {
  titleBar(S_VEHICLE, 2);
  if (vehStep == 0) {
    for (int i = 0; i < 6; i++) {
      int x = VBX[i & 1], y = 36 + (i >> 1) * 42; bool cur = vehModel >= 0 && MODELS[vehModel].brand == i;
      panel(x, y, 150, 38, 5, CUT_TL | CUT_BR, cur ? T.panel2 : T.panel, cur ? T.acc : T.line);
      txt(F_UL, BRANDS[i], x + 75, y + 25, cur ? T.text : T.accL, cur ? T.panel2 : T.panel, AL_C);
    }
    btnId(S_AUTOVIN, 6, 166, 308, 30, vehModel < 0, LB_BTNM);
    lab(S_PICKBRAND, LB_TILE, 160, tby(210), T.dim, T.bg, AL_C);
  } else {
    int r = 0;
    for (int i = 0; i < NMODEL; i++) {
      if (MODELS[i].brand != vehBrandSel) continue;
      int x = VBX[r & 1], y = 36 + (r >> 1) * 29; bool cur = i == vehModel;
      char t[24]; fitText(t, sizeof t, MODELS[i].name, F_UM, 136);
      btnBox(x, y, 150, 26, cur); txt(F_UM, t, x + 75, y + 18, T.text, cur ? T.acc : T.panel2, AL_C);
      r++;
    }
    btnS("<", 6, 214, 50, 24, false); btnId(S_AUTOVIN, 62, 214, 252, 24, vehModel < 0, LB_BTNM);
  }
}
static int vehModelAt(int slot) {            // slot-th model of the chosen brand
  int r = 0;
  for (int i = 0; i < NMODEL; i++) { if (MODELS[i].brand != vehBrandSel) continue; if (r == slot) return i; r++; }
  return -1;
}

// =====================================================================================
//  Splash (logo), warning / check-engine overlays, park screen
// =====================================================================================
static uint32_t splashAt = 0;
static int splashPx = -1;
static uint32_t splashMs() { return firstRun ? 7000u : 2300u; }
static void splashDraw() {
  tft.fillScreen(T.bg);
  tft.pushImage(60, 2, LOGO_W, LOGO_H, LOGO_PX);
  if (firstRun) {                                  // the language is not chosen yet: show the tip in Thai and in English
    bool t0 = thai;
    thai = true;  lab(S_FIRSTTIP, LB_BTNM, 160, 222, T.warn, T.bg, AL_C);
    thai = false; lab(S_FIRSTTIP, LB_TILE, 160, 236, T.dim, T.bg, AL_C);
    thai = t0;
  } else {
    char t[28]; snprintf(t, sizeof t, "%s  %s", EN[S_SPLASHHINT], FW_VERSION);
    txt(F_UM, t, 160, 226, T.dim, T.bg, AL_C);
  }
  splashPx = -1;
}
static void splashDyn() {
  uint32_t el = millis() - splashAt; int px = (int)min<uint32_t>(100, el * 100 / splashMs());
  if (px != splashPx) { splashPx = px; tft.fillRect(110, 207, 100, 3, T.dark); tft.fillRect(110, 207, px, 3, T.acc); }
}

// ---------- alerts ----------
enum { AK_NONE = 0, AK_COOL, AK_OIL, AK_BATT, AK_MIL, AK_N };
static int alertOn = AK_NONE; static bool alertArmed[AK_N] = {true, true, true, true, true}; static uint32_t alertSnooze[AK_N]; static bool alertTest = false; static uint32_t alertTestAt = 0;
static int testNext = 0;
static const uint16_t ALBG = C(20, 5, 8);
static float alertVal(int k, bool& ok) {
  switch (k) {
    case AK_COOL: return getVal(0x05, ok);
    case AK_OIL: return getVal(0x5C, ok);
    case AK_BATT: return getVal(0x42, ok);
  }
  ok = false; return 0;
}
static bool alertCond(int k, bool release) {                 // true while the condition holds (with hysteresis for clearing)
  bool ok; float v;
  if (!(demoMode || linkState == 2)) return false;
  switch (k) {
    case AK_COOL: v = getVal(0x05, ok); return ok && v >= (release ? MET[MI_COOL].b - 3 : MET[MI_COOL].b);
    case AK_OIL: v = getVal(0x5C, ok); return ok && v >= (release ? MET[MI_OIL].b - 5 : MET[MI_OIL].b);
    case AK_BATT: { bool orp; float rp = getVal(0x0C, orp); v = getVal(0x42, ok); return ok && orp && rp > 700 && v < (release ? 11.8f : 11.0f); }
    case AK_MIL: return mil && !demoMode && linkState == 2;
  }
  return false;
}
static void alertValueDraw(bool force) {
  static char last[16];
  char b[16]; bool ok = false; float v;
  if (alertOn == AK_MIL) { if (!force) return; strcpy(b, ""); }
  else { v = alertTest ? (alertOn == AK_BATT ? 10.6f : alertOn == AK_OIL ? 131.0f : 107.0f) : alertVal(alertOn, ok); if (alertTest) ok = true;
         if (ok) fmtv(b, sizeof b, v, alertOn == AK_BATT ? 1 : 0); else strcpy(b, "--"); }
  if (!force && !strcmp(b, last)) return;
  strcpy(last, b);
  if (alertOn == AK_MIL) lab(S_LAMPON2, LB_BTN, 190, 140, RED_AL, ALBG, AL_C, 130);
  else txtVU(F_NXL, b, F_UM, alertOn == AK_BATT ? "V" : DEGC, 190, 148, RED_AL, T.dim, ALBG, AL_C, 130);
}
static void alertDraw() {
  tft.fillScreen(T.bg);
  panel(24, 36, 272, 170, 8, CUT_TL | CUT_BR, ALBG, RED_AL);
  panel(26, 38, 268, 26, 6, CUT_TL, RED_AL, RED_AL);
  lab(alertOn == AK_MIL ? S_CHECKENG : S_WARNT, LB_BTN, 160, 58, C(255, 255, 255), RED_AL, AL_C);
  if (alertOn != AK_MIL) lab(alertOn == AK_COOL ? S_COOLANT : alertOn == AK_OIL ? S_OIL : S_BATT, LB_TILE, 160, tby(82), T.dim, ALBG, AL_C);
  hexagon(78, 126, 24, C(46, 8, 12), RED_AL, true); txt(F_UL, "!", 78, 134, RED_AL, C(46, 8, 12), AL_C);
  alertValueDraw(true);
  StrId m = alertOn == AK_COOL ? S_HOT : alertOn == AK_OIL ? S_OILHOT : alertOn == AK_BATT ? S_LOWV : S_NEWFAULT;
  lab(m, LB_TILE, 160, tby(170), T.text, ALBG, AL_C);
  panel(40, 180, 118, 22, 4, CUT_TL | CUT_BR, RED_AL, RED_AL); lab(S_OK, LB_BTNM, 99, 196, C(255, 255, 255), RED_AL, AL_C);
  panel(162, 180, 118, 22, 4, CUT_TL | CUT_BR, T.panel2, T.line); lab(S_SNOOZE, LB_BTNM, 221, 196, T.text, T.panel2, AL_C);
}
static void alertClose(bool snooze) {
  if (snooze) alertSnooze[alertOn] = millis() + 300000u;
  alertTest = false; alertOn = AK_NONE; redraw = true;
}
static void alertOpen(int k) { alertOn = k; alertArmed[k] = false; redraw = true; }
static void startTestAlert() { alertTest = true; alertTestAt = millis(); alertOpen(testNext == 0 ? AK_COOL : AK_MIL); testNext ^= 1; }
static void alertsStep() {
  uint32_t now = millis();
  if (alertOn != AK_NONE) {
    if (alertTest) { if (now - alertTestAt > 9000) alertClose(false); return; }
    if (!alertCond(alertOn, true)) { alertArmed[alertOn] = true; alertClose(false); }
    return;
  }
  for (int k = 1; k < AK_N; k++) {
    if (!alertCond(k, true)) alertArmed[k] = true;
    else if (warnOn && alertArmed[k] && alertCond(k, false) && (int32_t)(now - alertSnooze[k]) > 0 && scr != PG_SPLASH) { alertOpen(k); return; }
  }
}

// ---------- park screen ----------
static uint32_t parkAt = 0; static float parkBatt = 0;
static char pkCache[8][24];
static void parkStatic() {
  lab(S_PARKED, LB_BTNM, 160, 26, T.dim, T.bg, AL_C);
  tft.fillRect(110, 92, 100, 2, T.acc);
  panel(6, 100, 152, 52, 5, CUT_TL | CUT_BR, T.panel, T.line); bracket(6, 100, 152, T.good, true, false);
  lab(S_LASTBATT, LB_TILE, 82, tby(116), T.dim, T.panel, AL_C);
  panel(162, 100, 152, 52, 5, CUT_TR | CUT_BL, T.panel, T.line); bracket(162, 100, 152, T.acc, false, true);
  lab(S_RANGE, LB_TILE, 238, tby(116), T.dim, T.panel, AL_C);
  panel(6, 158, 308, 50, 5, CUT_TL | CUT_BR, T.panel, T.line);
  static const StrId LB4[4] = {S_THISDRIVE, S_DTIME, S_AVG, S_TOPSPEED};
  for (int i = 0; i < 4; i++) lab(LB4[i], LB_TILE, 6 + 38 + i * 77, tby(173), T.dim, T.panel, AL_C);
  for (int i = 0; i < 8; i++) pkCache[i][0] = 0;
}
static void parkSet(int i, const char* s, uint16_t col, int cx, int by, FontId f, const char* unit, int clearW, uint16_t bg) {
  char key[24]; snprintf(key, sizeof key, "%s|%04X", s, col);
  if (!strcmp(key, pkCache[i])) return;
  strcpy(pkCache[i], key);
  txtVU(f, s, F_LS, unit, cx, by, col, T.dim, bg, AL_C, clearW);
}
static void parkDyn() {
  uint32_t secs = (millis() - parkAt) / 1000; char b[24];
  clockStr(b, sizeof b, secs); parkSet(0, b, T.text, 160, 78, F_NXL, "", 150, T.bg);
  if (parkBatt > 1) { fmtv(b, sizeof b, parkBatt, 1); parkSet(1, b, parkBatt >= 12.3f ? T.good : T.warn, 82, 143, F_NHL, "V", 140, T.panel); } else parkSet(1, "--", T.dim, 82, 143, F_NHL, "", 140, T.panel);
  float rg = rangeKm(); if (rg >= 0) { snprintf(b, sizeof b, "%d", (int)rg); parkSet(2, b, T.text, 238, 143, F_NHL, "km", 140, T.panel); } else parkSet(2, "--", T.dim, 238, 143, F_NHL, "", 140, T.panel);
  fmtv(b, sizeof b, tDist, 1); parkSet(3, b, T.text, 44, 198, F_NL, "km", 72, T.panel);
  int mins = (int)(tSec / 60); snprintf(b, sizeof b, "%d:%02d", mins / 60, mins % 60); parkSet(4, b, T.text, 121, 198, F_NL, "h", 72, T.panel);
  float al = tripAvgL100(); if (al > 0) { fmtv(b, sizeof b, 100.0f / al, 1); parkSet(5, b, T.text, 198, 198, F_NL, "km/L", 72, T.panel); } else parkSet(5, "--", T.dim, 198, 198, F_NL, "", 72, T.panel);
  fmtv(b, sizeof b, pkD.spd, 0); parkSet(6, b, T.text, 275, 198, F_NL, "km/h", 72, T.panel);
  uint32_t left = secs >= 180 ? 0 : 180 - secs; char c[12]; snprintf(c, sizeof c, "%u:%02u", (unsigned)(left / 60), (unsigned)(left % 60));
  char key[24]; snprintf(key, sizeof key, "%s", c);
  if (strcmp(key, pkCache[7])) {
    strcpy(pkCache[7], key);
    int w1 = labW(S_SCROFF, LB_TILE), w2 = textW(F_LS, c), w3 = labW(S_TAPGAUGE, LB_TILE), tot = w1 + 4 + w2 + 12 + w3, x0 = 160 - tot / 2;
    tft.fillRect(0, 216, 320, 24, T.bg);
    lab(S_SCROFF, LB_TILE, x0, tby(231), T.dim, T.bg); txt(F_LS, c, x0 + w1 + 4, 231, T.text, T.bg); lab(S_TAPGAUGE, LB_TILE, x0 + w1 + 4 + w2 + 12, tby(231), T.dim, T.bg);
  }
}

// =====================================================================================
//  Navigation, drawing dispatch, touch handling, main loop
// =====================================================================================
static bool prefsDirty = false; static uint32_t prefsAt = 0;
static void saveSoon() { prefsDirty = true; prefsAt = millis(); }

static void newDrive() { peaksClear(pkD); driveStart = millis(); }
static void nav(Page p) {
  goPage(p);
  switch (p) {
    case PG_ALL: allPg = 0; break;
    case PG_CODES: codesRead(); break;
    case PG_STATUS: statusEnter(); break;
    case PG_FREEZE: freezeRead(); break;
    case PG_ECU: ecuRead(); break;
    case PG_VEH: vehStep = 0; break;
    default: break;
  }
}
static void fullDraw() {
  if (alertOn != AK_NONE) { alertDraw(); return; }
  if (scr == PG_SPLASH) { splashDraw(); return; }
  tft.fillScreen(T.bg);
  switch (scr) {
    case PG_DASH: dashStatic(); break;
    case PG_DATA: dataStatic(); break;
    case PG_ALL: allStatic(); break;
    case PG_GRAPH: graphStatic(); break;
    case PG_TRIP: tripStatic(); break;
    case PG_PEAKS: peaksStatic(); break;
    case PG_CODES: codesStatic(); break;
    case PG_STATUS: statusStatic(); break;
    case PG_FREEZE: freezeStatic(); break;
    case PG_ECU: ecuStatic(); break;
    case PG_SETUP: setupStatic(); break;
    case PG_SETUP2: setup2Static(); break;
    case PG_BT: btStatic(); break;
    case PG_VEH: vehStatic(); break;
    case PG_PARK: parkStatic(); break;
    default: break;
  }
}
static void updateScreen() {
  if (alertOn != AK_NONE) { alertValueDraw(false); return; }
  static int fv = -1, ev = -1, ls = -1;
  switch (scr) {
    case PG_SPLASH: splashDyn(); break;
    case PG_DASH: dashDyn(); break;
    case PG_DATA: dataDyn(); break;
    case PG_ALL: allDyn(); break;
    case PG_GRAPH: graphDyn(); break;
    case PG_TRIP: tripDyn(); break;
    case PG_PEAKS: peaksDyn(); break;
    case PG_CODES: codesDyn(); break;
    case PG_STATUS: statusDyn(); break;
    case PG_FREEZE: if (fv != frzVer) { fv = frzVer; redraw = true; } break;
    case PG_ECU: if (ev != ecuInfoVer) { ev = ecuInfoVer; redraw = true; } break;
    case PG_BT: btDyn(); if (ls != linkState) { ls = linkState; btSelRow(); } break;
    case PG_PARK: parkDyn(); break;
    default: break;
  }
}

// ---------- touch ----------
static bool doneHit(int x, int y) { return hit(x, y, 246, 4, 68, 22); }
static void applyVehicle(int idx) {                 // idx = -1: automatic (from the VIN)
  vehModel = idx;
  if (idx >= 0) { tankL = MODELS[idx].tank; shiftRpm = max(3000, MODELS[idx].redline * 100 - 300); }
  carBuf[0] = 0; slotsCustom = false; autoSlots();
  savePrefs(); nav(PG_SETUP);
}
static void setSource(bool demo) {
  if (demo == demoMode) return;
  demoMode = demo; carBuf[0] = 0; vinBuf[0] = 0; ecuName[0] = 0; calId[0] = 0; vehModel = vehModel;     // (the chosen model stays)
  ecuDieselKnown = false; ecuDiesel = false; ecuHybrid = false; mil = false; codeCount = 0; codeState = 0; monValid = false;
  memset(psup, 0, sizeof psup); memset(page, 0, sizeof page); engineRan = false; rpmZeroAt = 0;
  if (demo) { linkState = 0; simT = 0; if (!slotsCustom) autoSlots(); }
  else { linkState = selAddr[0] ? 1 : 3; reconnectReq = true; if (!slotsCustom) autoSlots(); }
  savePrefs(); updateWants();
}
static void stepVal(int& v, int lo, int hi, int dir, int st) { v = constrain(v + dir * st, lo, hi); }

static void handleTouch(int x, int y, bool first) {
  if (alertOn != AK_NONE) {
    if (!first) return;
    if (hit(x, y, 40, 180, 118, 22)) alertClose(false); else if (hit(x, y, 162, 180, 118, 22)) alertClose(true);
    return;
  }
  switch (scr) {
    case PG_SPLASH:
      if (!first) return;
      if (firstRun) { invScreen = !invScreen; applyInvert(); splashAt = millis(); splashPx = -1; }
      else nav(PG_DASH);
      return;
    case PG_DASH: {
      if (!first) return;
      if (y >= 214) {
        static const Page P5[5] = {PG_DATA, PG_GRAPH, PG_TRIP, PG_CODES, PG_SETUP};
        for (int i = 0; i < 5; i++) if (hit(x, y, NAVX[i], 214, 58, 24)) { nav(P5[i]); return; }
        return;
      }
      for (int i = 0; i < 6; i++) if (hit(x, y, DT_X[i], DT_Y[i], 64, 40)) {
        slots[i] = (uint8_t)nextMetric(slots[i], i); slotsCustom = true; dtileStatic(i); updateWants(); saveSoon(); return;
      }
      if (hit(x, y, 76, 36, 168, 176)) { crpMain = !crpMain; dashSwap(); saveSoon(); }
      return;
    }
    case PG_DATA:
      if (!first) return;
      if (doneHit(x, y)) nav(PG_DASH); else if (hit(x, y, 6, 4, 76, 22)) nav(PG_ALL);
      return;
    case PG_ALL:
      if (!first) return;
      if (doneHit(x, y)) nav(PG_DATA);
      else if (hit(x, y, 6, 214, 50, 24)) { if (allPg > 0) { allPg--; redraw = true; } }
      else if (hit(x, y, 62, 214, 50, 24)) { if (allPg + 1 < allPages()) { allPg++; redraw = true; } }
      else if (hit(x, y, 232, 214, 82, 24)) { allMode = !allMode; allPg = 0; redraw = true; }
      return;
    case PG_GRAPH:
      if (!first) return;
      if (doneHit(x, y)) { nav(PG_DASH); return; }
      for (int i = 0; i < 4; i++) if (hit(x, y, NAVX[i], 214, 58, 24)) { gsel = i; updateWants(); redraw = true; return; }
      if (hit(x, y, NAVX[4], 214, 58, 24)) { gmode ^= 1; redraw = true; }
      return;
    case PG_TRIP:
      if (!first) return;
      if (doneHit(x, y)) nav(PG_DASH);
      else if (hit(x, y, 6, 214, 100, 24)) { tripReset(); redraw = true; }
      else if (hit(x, y, 110, 214, 100, 24)) { unitL100 = !unitL100; saveSoon(); redraw = true; }
      else if (hit(x, y, 214, 214, 100, 24)) nav(PG_PEAKS);
      return;
    case PG_PEAKS:
      if (!first) return;
      if (doneHit(x, y)) nav(PG_TRIP);
      else if (hit(x, y, 6, 214, 150, 24)) { peaksClear(pkAllMode ? pkA : pkD); if (pkAllMode) pkDirty = true; redraw = true; }
      else if (hit(x, y, 164, 214, 150, 24)) { pkAllMode = !pkAllMode; redraw = true; }
      return;
    case PG_CODES:
      if (!first) return;
      if (doneHit(x, y)) nav(PG_DASH);
      else if (hit(x, y, 6, 4, 76, 22)) nav(PG_STATUS);
      else if (hit(x, y, 6, 214, 96, 24)) codesRead();
      else if (hit(x, y, 106, 214, 124, 24)) { if (!clearArmed) { clearArmed = true; clearAt = millis(); } else codesClear(); codesSig = 0xFFFFFFFFu; }
      else if (hit(x, y, 236, 214, 36, 24)) { if (codePg > 0) codePg--; codesSig = 0xFFFFFFFFu; }
      else if (hit(x, y, 278, 214, 36, 24)) { codePg++; codesSig = 0xFFFFFFFFu; }
      return;
    case PG_STATUS: case PG_FREEZE: case PG_ECU:
      if (!first) return;
      if (doneHit(x, y)) nav(PG_DASH);
      else if (hit(x, y, 6, 4, 76, 22)) nav(PG_CODES);
      else if (hit(x, y, 6, 214, 100, 24)) { if (scr != PG_STATUS) nav(PG_STATUS); }
      else if (hit(x, y, 110, 214, 100, 24)) { if (scr != PG_FREEZE) nav(PG_FREEZE); }
      else if (hit(x, y, 214, 214, 100, 24)) { if (scr != PG_ECU) nav(PG_ECU); }
      return;
    case PG_SETUP: {
      if (first) {
        if (doneHit(x, y)) { savePrefs(); nav(PG_DASH); return; }
        for (int i = 0; i < NTHEME; i++) if (hit(x, y, 6 + i * 156, 34, 152, 36)) { if (theme != i) { applyTheme(i); savePrefs(); redraw = true; } return; }
        if (hit(x, y, 6, 214, 100, 24)) { nav(PG_BT); return; }
        if (hit(x, y, 110, 214, 100, 24)) { nav(PG_VEH); return; }
        if (hit(x, y, 214, 214, 100, 24)) { nav(PG_SETUP2); return; }
      }
      for (int i = 0; i < 4; i++) {
        int ry = SU_Y0 + i * SU_H + 4, rh = SU_H - 8;
        if (y < ry || y >= ry + rh) continue;
        if (i == 0 && first) {
          if (hit(x, y, 154, ry, 76, rh)) { setSource(true); setupRow(0); }
          else if (hit(x, y, 234, ry, 80, rh)) { setSource(false); if (!selAddr[0]) nav(PG_BT); else setupRow(0); }
        } else if (i == 1 && first) {
          if (hit(x, y, 154, ry, 76, rh) && thai) { thai = false; savePrefs(); redraw = true; }
          else if (hit(x, y, 234, ry, 80, rh) && !thai) { thai = true; savePrefs(); redraw = true; }
        } else if (i == 2) {
          if (hit(x, y, 154, ry, 38, rh)) { stepVal(shiftRpm, 3000, 9000, -1, 100); setupRow(2); saveSoon(); }
          else if (hit(x, y, 276, ry, 38, rh)) { stepVal(shiftRpm, 3000, 9000, 1, 100); setupRow(2); saveSoon(); }
        } else if (i == 3 && first) {
          if (hit(x, y, 154, ry, 76, rh) && invScreen) { invScreen = false; applyInvert(); savePrefs(); setupRow(3); }
          else if (hit(x, y, 234, ry, 80, rh) && !invScreen) { invScreen = true; applyInvert(); savePrefs(); setupRow(3); }
        }
      }
      return;
    }
    case PG_SETUP2: {
      if (first) {
        if (doneHit(x, y)) { savePrefs(); nav(PG_DASH); return; }
        if (hit(x, y, 6, 4, 44, 22)) { savePrefs(); nav(PG_SETUP); return; }
        if (hit(x, y, 6, 214, 150, 24)) { startTestAlert(); return; }
        if (hit(x, y, 164, 214, 150, 24)) { peaksClear(pkA); peaksClear(pkD); pkDirty = true; savePrefs(); return; }
      }
      for (int i = 0; i < 6; i++) {
        int ry = S2_Y0 + i * S2_H + 3, rh = S2_H - 6;
        if (y < ry || y >= ry + rh) continue;
        bool minus = hit(x, y, 154, ry + 1, 38, rh - 2), plus = hit(x, y, 276, ry + 1, 38, rh - 2), on = hit(x, y, 154, ry, 76, rh), off = hit(x, y, 234, ry, 80, rh);
        if (i == 0 && first && (on || off)) { warnOn = on; saveSoon(); setup2Row(0); }
        else if (i == 1 && first && (on || off)) { parkOn = on; saveSoon(); setup2Row(1); }
        else if (i == 2 && (minus || plus)) { stepVal(bright, 10, 100, plus ? 1 : -1, 10); applyBright(); saveSoon(); setup2Row(2); }
        else if (i == 3 && (minus || plus)) { stepVal(tankL, 10, 200, plus ? 1 : -1, 1); saveSoon(); setup2Row(3); }
        else if (i == 4 && (minus || plus)) { stepVal(fuelCal, 70, 130, plus ? 1 : -1, 1); saveSoon(); setup2Row(4); }
        else if (i == 5 && first && (minus || plus)) { userProto = (userProto + (plus ? 1 : 9)) % 10; uprotoAt = millis(); saveSoon(); setup2Row(5); }
      }
      return;
    }
    case PG_BT:
      if (!first) return;
      if (doneHit(x, y)) { nav(PG_SETUP); return; }
      if (hit(x, y, 6, 4, 76, 22)) { btStartScan(); btListDraw(); return; }
      if (hit(x, y, 6, 214, 50, 24)) { if (btOff > 0) { btOff = max(0, btOff - 4); btListDraw(); } return; }
      if (hit(x, y, 62, 214, 50, 24)) { if (btOff + 4 < nBtView) { btOff += 4; btListDraw(); } return; }
      for (int r = 0; r < 4; r++) if (hit(x, y, 6, 76 + r * 28, 308, 26) && btOff + r < nBtView) { btPick(btOff + r); return; }
      return;
    case PG_VEH:
      if (!first) return;
      if (doneHit(x, y)) { nav(PG_SETUP); return; }
      if (vehStep == 0) {
        for (int i = 0; i < 6; i++) if (hit(x, y, VBX[i & 1], 36 + (i >> 1) * 42, 150, 38)) { vehBrandSel = i; vehStep = 1; redraw = true; return; }
        if (hit(x, y, 6, 166, 308, 30)) applyVehicle(-1);
      } else {
        if (hit(x, y, 6, 214, 50, 24)) { vehStep = 0; redraw = true; return; }
        if (hit(x, y, 62, 214, 252, 24)) { applyVehicle(-1); return; }
        for (int r = 0; r < 14; r++) if (hit(x, y, VBX[r & 1], 36 + (r >> 1) * 29, 150, 26)) { int m = vehModelAt(r); if (m >= 0) applyVehicle(m); return; }
      }
      return;
    case PG_PARK:
      if (first) nav(PG_DASH);
      return;
    default: return;
  }
}

// ---------- engine stop -> park screen ----------
static void parkStep(uint32_t now) {
  bool ok; float rpm = getVal(0x0C, ok); if (!ok) rpm = 0;
  if (rpm > 500) { if (!demoMode) engineRan = true; rpmZeroAt = 0; bool bo; float bv = getVal(0x42, bo); if (bo && bv > 5) parkBatt = bv; }
  if (scr == PG_PARK) {
    if (rpm > 500 && !demoMode) { if (screenOff) { screenOff = false; applyBright(); } newDrive(); nav(PG_DASH); return; }
    if (!screenOff && now - parkAt > 180000u) { screenOff = true; ledcWrite(0, 0); }
    return;
  }
  if (!parkOn || demoMode || scr == PG_SPLASH || alertOn != AK_NONE || !engineRan) return;
  if (rpm < 300) {
    if (!rpmZeroAt) rpmZeroAt = now;
    else if (now - rpmZeroAt > 6000) { parkAt = now; engineRan = false; screenOff = false; rpmZeroAt = 0; nav(PG_PARK); }
  } else rpmZeroAt = 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(4, OUTPUT); pinMode(16, OUTPUT); pinMode(17, OUTPUT);              // CYD RGB LED off (active low)
  digitalWrite(4, HIGH); digitalWrite(16, HIGH); digitalWrite(17, HIGH);
  loadPrefs();
  tft.init(); tft.setRotation(1); tft.setSwapBytes(true); applyInvert(); tft.fillScreen(T.bg);
  ledcSetup(0, 5000, 8); ledcAttachPin(21, 0); applyBright();
  tsSPI.begin(25, 39, 32, 33); ts.begin(tsSPI); ts.setRotation(1);
  analogSetPinAttenuation(CRP_PIN, ADC_11db);
  linkState = demoMode ? 0 : (selAddr[0] ? 1 : 3);
  if (demoMode && !slotsCustom) autoSlots();
  tripReset(); peaksClear(pkD);
  scr = PG_SPLASH; splashAt = millis(); redraw = true; updateWants();
  xTaskCreatePinnedToCore(obdTask, "obd", 16384, nullptr, 1, nullptr, 0);   // BLE service discovery runs on this stack too
}

void loop() {
  uint32_t now = millis();
  static bool wasTouched = false; static uint32_t repAt = 0;
  bool t = ts.tirqTouched() && ts.touched();
  if (t) {
    TS_Point p = ts.getPoint();
    int x = constrain(map(p.x, 200, 3700, 1, 320), 0, 319), y = constrain(map(p.y, 240, 3800, 1, 240), 0, 239);
    if (!wasTouched) {
      repAt = now + 450;
      if (screenOff) { screenOff = false; applyBright(); } else handleTouch(x, y, true);
    } else if ((int32_t)(now - repAt) >= 0) { repAt = now + 110; if (!screenOff) handleTouch(x, y, false); }
  }
  wasTouched = t;

  if (pinDirty || protoDirty) { pinDirty = false; protoDirty = false; savePrefs(); }
  if (prefsDirty && now - prefsAt > 1500) { prefsDirty = false; savePrefs(); }
  if (pkDirty && now - pkSaved > 60000u) { pkDirty = false; pkSaved = now; savePrefs(); }
  if (uprotoAt && now - uprotoAt > 1500) { uprotoAt = 0; if (!demoMode) reconnectReq = true; }

  static uint32_t tSim = 0, tUI = 0, tGraph = 0; static int lastLs = -1; static bool lastDsl = false;
  if (now - tSim >= 100) {
    float dt = (now - tSim) / 1000.0f; tSim = now;
    if (demoMode) simStep(dt); else crpV = crpV * 0.7f + readCrp() * 0.3f;
    tripStep(dt); peaksStep(); alertsStep(); parkStep(now);
    if (linkState != lastLs) { lastLs = linkState; if (linkState == 2 && !slotsCustom) { autoSlots(); if (scr == PG_DASH || scr == PG_DATA) redraw = true; } }
    bool dsl = vehDiesel(); if (dsl != lastDsl) { lastDsl = dsl; if (scr == PG_DASH || scr == PG_DATA || scr == PG_GRAPH) redraw = true; }
    if (scr == PG_SPLASH && now - splashAt > splashMs()) { if (firstRun) savePrefs(); nav(PG_DASH); }
  }
  if (now - tGraph >= 1000) { tGraph = now; graphSample(); }
  if (now - tUI >= 80) {
    tUI = now;
    if (redraw) { redraw = false; fullDraw(); } else updateScreen();
  }
  delay(5);
}
