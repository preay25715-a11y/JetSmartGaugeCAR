// JetOBD2 Smart Gauge - ESP32 CYD (ESP32-2432S028R), 320x240 landscape
// OBD-II over ELM327 (Bluetooth Classic or BLE) for Toyota / Honda / Isuzu / Mitsubishi / Nissan / Mazda (and any OBD-II car),
// demo mode, Thai/English UI, black-blue / black-red themes, boot logo.   Needs ui_assets.h next to this file.
#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#ifdef UPD_BUILD
// Updater image: Wi-Fi + TLS instead of Bluetooth (the flash is too small for both). Tiny stand-ins keep the shared UI code compiling;
// nothing here is ever called, the linker drops it.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/version.h"
#include "mbedtls/sha256.h"
#include <map>
typedef int esp_ble_addr_type_t;
struct BTAddr { String toString() { return String(""); } };
struct BTAdvertisedDevice { BTAddr getAddress() { return {}; } String getName() { return String(""); } int getRSSI() { return 0; } };
struct BTScanResults { int getCount() { return 0; } BTAdvertisedDevice* getDevice(int) { return nullptr; } };
struct BluetoothSerial { bool begin(const char*, bool) { return true; } void setPin(const char*) {} bool discoverAsync(void (*)(BTAdvertisedDevice*)) { return true; } void discoverAsyncStop() {}
  BTScanResults* getScanResults() { return nullptr; } void end() {} int available() { return 0; } int read() { return -1; } void print(const char*) {} void print(char) {} bool connected() { return false; } void disconnect() {} bool connect(uint8_t*) { return false; } };
struct BLEUUID { BLEUUID(uint16_t) {} String toString() { return String(""); } };
struct BLEAddress { BLEAddress(uint8_t*) {} String toString() { return String(""); } };
struct BLERemoteCharacteristic { bool canWrite() { return 0; } bool canWriteNoResponse() { return 0; } bool canNotify() { return 0; } bool canIndicate() { return 0; }
  void registerForNotify(void (*)(BLERemoteCharacteristic*, uint8_t*, size_t, bool), bool) {} void writeValue(uint8_t*, size_t, bool) {} };
struct BLERemoteService { BLEUUID getUUID() { return BLEUUID((uint16_t)0); } std::map<std::string, BLERemoteCharacteristic*>* getCharacteristics() { return nullptr; } };
struct BLEClient { bool connect(BLEAddress, esp_ble_addr_type_t) { return false; } bool isConnected() { return false; } void disconnect() {} std::map<std::string, BLERemoteService*>* getServices() { return nullptr; } };
struct BLEAdvertisedDevice { BLEAddress getAddress() { return BLEAddress(nullptr); } String getName() { return String(""); } int getRSSI() { return 0; } bool isAdvertisingService(BLEUUID) { return 0; } int getAddressType() { return 0; } };
struct BLEScanResults { int getCount() { return 0; } BLEAdvertisedDevice getDevice(int) { return {}; } };
struct BLEScan { void setActiveScan(bool) {} void setInterval(int) {} void setWindow(int) {} BLEScanResults start(int, bool) { return {}; } void clearResults() {} };
struct BLEDevice { static void init(const char*) {} static BLEClient* createClient() { static BLEClient c; return &c; } static BLEScan* getScan() { static BLEScan s; return &s; } static void deinit(bool) {} };
#else
#include <BluetoothSerial.h>
#include <BLEDevice.h>
#endif
#include <string>
#include "ui_assets.h"

#define FW_VERSION "v1.0.13"

TFT_eSPI tft;
SPIClass tsSPI(VSPI);
XPT2046_Touchscreen ts(33, 36);
BluetoothSerial BT;
Preferences prefs;


// ---------- settings ----------
static const bool demoMode = false;               // the built-in demo was removed in v1.0.2; the constant keeps the old branches compiled out
static bool warnOn = true, parkOn = true;
static int shiftRpm = 6500, bright = 80, tankL = 44, fuelCal = 100;
static bool invScreen = false;                   // panel colour polarity (runtime INVON / INVOFF)
static uint8_t dashMain = 0;                      // dash centre shows 0 speed, 1 CRP volts, 2 AFR
static uint8_t slots[6] = {0, 1, 2, 3, 4, 5};
static bool firstRun = true;                     // nothing saved yet (every re-flash starts here)
static bool engineRan = false, screenOff = false;   // park screen: the engine has run since connecting / the backlight is off
static uint32_t rpmZeroAt = 0;

// ---------- vehicles (Thai market) ----------
enum { VF_D = 1, VF_H = 2, VF_T = 4, VF_M = 8 };    // diesel, hybrid, turbo, motorcycle
struct Model { uint8_t brand; const char* name; uint8_t flags; uint8_t tank; uint16_t cc; uint8_t redline; };   // redline in 100 rpm
static const char* const BRANDS[7] = {"TOYOTA", "HONDA", "ISUZU", "MITSUBISHI", "NISSAN", "MAZDA", "HONDA BIKE"};
static const int NBRAND = 7;
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
  {6, "WAVE 110", VF_M, 4, 109, 90},
  {6, "WAVE 110i", VF_M, 4, 109, 90},
  {6, "WAVE 125i", VF_M, 4, 125, 90},
  {6, "SUPER CUB", VF_M, 4, 109, 90},
  {6, "C125", VF_M, 4, 125, 90},
  {6, "SCOOPY i", VF_M, 4, 110, 90},
  {6, "LEAD 125", VF_M, 6, 125, 90},
  {6, "CLICK 125", VF_M, 5, 125, 90},
  {6, "CLICK 160", VF_M, 6, 157, 95},
  {6, "GIORNO+", VF_M, 5, 125, 90},
  {6, "PCX 160", VF_M, 8, 157, 95},
  {6, "ADV 160", VF_M, 8, 157, 95},
  {6, "ADV 350", VF_M, 12, 330, 85},
  {6, "FORZA 350", VF_M, 12, 330, 85},
  {6, "GROM", VF_M, 6, 125, 95},
  {6, "MONKEY", VF_M, 6, 125, 95},
  {6, "CB150R", VF_M, 12, 149, 110},
  {6, "CBR150R", VF_M, 12, 149, 115},
  {6, "CBR250RR", VF_M, 15, 249, 140},
  {6, "CB300R", VF_M, 10, 286, 105},
  {6, "CBR300R", VF_M, 13, 286, 105},
  {6, "CB500X", VF_M, 18, 471, 95},
  {6, "CBR500R", VF_M, 17, 471, 95},
  {6, "NX500", VF_M, 18, 471, 95},
  {6, "CB650R", VF_M, 15, 649, 120},
  {6, "CBR650R", VF_M, 15, 649, 120},
  {6, "CBR600RR", VF_M, 18, 599, 150},
  {6, "CBR1000RR-R", VF_M, 16, 999, 150},
  {6, "CB1000R", VF_M, 16, 998, 115},
  {6, "CB1100RS", VF_M, 17, 1140, 85},
  {6, "CB1300 SUPER 4", VF_M, 21, 1284, 90},
  {6, "CB1300 BOL D'OR", VF_M, 21, 1284, 90},
  {6, "CT125", VF_M, 5, 125, 90},
  {6, "CRF300L", VF_M, 8, 286, 95},
  {6, "CRF300 RALLY", VF_M, 13, 286, 95},
  {6, "CRF450RL", VF_M, 8, 449, 95},
  {6, "NC750X", VF_M, 14, 745, 70},
  {6, "X-ADV 750", VF_M, 13, 745, 70},
  {6, "AFRICA TWIN", VF_M, 19, 1084, 75},
  {6, "AFRICA TWIN AS", VF_M, 25, 1084, 75},
  {6, "REBEL 300", VF_M, 11, 286, 105},
  {6, "REBEL 500", VF_M, 11, 471, 85},
  {6, "REBEL 1100", VF_M, 12, 1084, 75},
  {6, "GB350C", VF_M, 15, 348, 65},
  {6, "NT1100", VF_M, 20, 1084, 75},
  {6, "GOLD WING", VF_M, 21, 1833, 65},
};
static const int NMODEL = sizeof(MODELS) / sizeof(MODELS[0]);
static int vehModel = -1;                        // chosen model (index into MODELS) or -1 = automatic (VIN)

// ---- v1.0.4: screen flip, motorcycle auto detection ----
static int autoModel = -1;                       // bike model taught by an earlier pick for this VIN code (-1 = none)
static bool autoBike = false;                    // the VIN says this is a motorcycle (saved, so the scale is right before the car answers)
static bool flipScr = false;                     // screen turned 180 degrees (gauge fitted upside down)
struct VMap { char c[6]; uint8_t m; };            // VIN characters 4-8 (model / type code) -> model the owner picked for them
static VMap vmap[8]; static int nvmap = 0, vmapNext = 0;
static volatile bool vinNew = false;             // a VIN was read: loop() applies it
static volatile bool btBrowsing = false;         // the Bluetooth page is open: no automatic adapter search meanwhile
static int effModel() { return vehModel >= 0 ? vehModel : autoModel; }

// VIN world-manufacturer codes -> brand (0..5 = a brand above, 100+ = another make that is only named)
struct Wmi { const char* w; uint8_t brand; };
static const char* const OTHER[] = {"FORD", "SUZUKI", "MG", "CHEVROLET", "SUBARU", "BMW", "HINO", "GWM", "BENZ", "VOLVO", "KIA", "HYUNDAI"};
static const Wmi WMIS[] = {
  {"MR0", 0}, {"MR1", 0}, {"MR2", 0}, {"JT", 0}, {"4T1", 0}, {"2T1", 0}, {"5TD", 0}, {"MHF", 0}, {"SB1", 0},
  {"MLH", 6}, {"JH2", 6}, {"ME4", 6}, {"MRH", 1}, {"JHM", 1}, {"JH1", 1}, {"1HG", 1}, {"2HG", 1}, {"19X", 1}, {"SHH", 1}, {"MHR", 1}, {"LHG", 1},
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
static const char* brandName(int b) { return b >= 0 && b < 7 ? BRANDS[b] : (b >= 100 && b - 100 < (int)(sizeof(OTHER) / sizeof(OTHER[0])) ? OTHER[b - 100] : ""); }
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
static volatile uint8_t linkMode = 0;     // 0 = ELM327 over Bluetooth, 1 = Honda K-line board on the UART pins (v1.0.13)
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

static bool snapOn = false; static uint32_t snapNow = 0; static float pvS[256]; static bool psupS[256]; static uint32_t pageS[256];   // data snapshot taken once per frame
static float getVal(uint8_t pid, bool& ok) {
  uint32_t now = snapOn ? snapNow : millis();
  if (pid == 0x05 && now < testUntil) { ok = true; return 108; }
  if (demoMode) { ok = true; return snapOn ? pvS[pid] : pv[pid]; }
  ok = (linkState == 2) && (snapOn ? psupS[pid] : psup[pid]) && (now - (snapOn ? pageS[pid] : page[pid]) < ((pid == 0x0C || pid == 0x0D) ? 3000u : 8000u));
  return snapOn ? pvS[pid] : pv[pid];
}

// ---------- ELM327 communication (runs in obdTask) ----------
static int linkAvail(); static int linkRead(); static void linkSend(const char* cmd); static bool linkIsUp();   // Classic or BLE, see the Bluetooth section
static bool abortWait() { return scanReq || reconnectReq || demoMode || !linkIsUp(); }   // the user asked for something else, or the link dropped
static String elmQuery(const char* cmd, uint32_t tmo = 1500) {
  while (linkAvail()) linkRead();
  linkSend(cmd);
  String r; r.reserve(96); uint32_t t0 = millis();
  while (millis() - t0 < tmo) {
    bool got = false;
    while (linkAvail()) { char c = (char)linkRead(); got = true; if (c == '>') return r; r += c; }
    if (abortWait()) break;
    if (!got) vTaskDelay(1);
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

// ---- poll speed (v1.0.3) ----
static bool cntOk = true;                                   // the adapter takes the "number of replies" digit (010C1): it stops waiting as soon as the first answer is in
static bool multiOk = false;                                // several PIDs in one request (CAN only); switched off by itself when the car does not answer them
static uint8_t multiBad = 0, multiShort = 0;
static bool kMulti = false;                                 // K-line: RPM + speed in ONE request (J1979 only promises this on CAN, so it is tried and dropped by itself when the car does not answer both)
static uint8_t pfail[256]; static uint32_t pskip[256];      // a PID that keeps answering nothing is skipped for a while, so it cannot slow the others down
static uint32_t iterNow = 0;                                // poll round counter
static bool vinPending = false;                             // the VIN is read a few rounds after the link came up, so the first values show at once
static volatile uint16_t obdRpmMs = 0;                      // measured time between two RPM updates (shown on the ECU page)
static void resetPollState() { memset(pfail, 0, sizeof pfail); memset(pskip, 0, sizeof pskip); multiBad = 0; multiShort = 0; }
static void multiOff() { multiOk = false; kMulti = false; resetPollState(); }       // the car does not take combined requests: one PID per request from now on
static uint32_t pollTmo() { return protoNum >= 6 ? 800 : 1500; }   // CAN answers in milliseconds: a silent adapter is noticed quickly
static bool queryPid(uint8_t pid, uint8_t* b, int& n, uint32_t tmo = 1200, int mode = 1, int frame = -1) {
  char cmd[12];
  bool dig = mode == 1 && cntOk;
  if (mode == 2) snprintf(cmd, sizeof cmd, "02%02X%02X", pid, frame < 0 ? 0 : frame); else snprintf(cmd, sizeof cmd, "%02X%02X%s", mode, pid, dig ? "1" : "");
  String raw = elmQuery(cmd, tmo);
  if (dig && raw.indexOf('?') >= 0) { cntOk = false; snprintf(cmd, sizeof cmd, "%02X%02X", mode, pid); raw = elmQuery(cmd, tmo); }   // this adapter does not know the digit
  int nm = parseMsgs(raw, msgs, MAXM);
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
  cntOk = true; multiOk = p >= 6; resetPollState(); kMulti = p >= 3 && p <= 5; obdRpmMs = 0;       // CAN: combined requests are tried first and dropped by themselves if the car does not take them
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
  vinNew = true;
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

static bool pollPid(uint8_t pid, int& fails);
static uint16_t ftAsk = 0, ftSilent = 0;                             // requests for RPM / speed, and how many of them were not answered (the fast-timing watch counts them)
static uint8_t pidLen(uint8_t id) {                                  // data bytes (SAE J1979) of the PIDs that may be asked together; 0 = ask alone
  switch (id) {
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0D: case 0x0E: case 0x0F: case 0x11:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x33: case 0x45: case 0x46: case 0x47: case 0x49: case 0x4A: case 0x4C:
    case 0x51: case 0x52: case 0x5A: case 0x5B: case 0x5C: case 0x61: case 0x62: return 1;
    case 0x0C: case 0x10: case 0x14: case 0x15: case 0x1F: case 0x21: case 0x22: case 0x23: case 0x31: case 0x32: case 0x3C: case 0x3E:
    case 0x42: case 0x43: case 0x44: case 0x4D: case 0x4E: case 0x59: case 0x5D: case 0x5E: case 0x63: return 2;
    case 0x24: case 0x34: return 4;
  }
  return 0;
}
static void notePidFail(uint8_t pid) { if (++pfail[pid] >= 3) { pfail[pid] = 3; pskip[pid] = iterNow + 30; } }   // 3 misses in a row: leave it alone for 30 rounds
static void noteRpm() {
  static uint32_t last = 0; uint32_t t = millis();
  if (last && t - last < 3000) obdRpmMs = obdRpmMs ? (uint16_t)((obdRpmMs * 3 + (t - last)) / 4) : (uint16_t)(t - last);
  last = t;
}
static bool plausible(uint8_t pid, float v) {                        // "not available" answers (0xFF..) must never reach the screen as numbers
  switch (pid) {
    case 0x0C: return v < 16000; case 0x10: return v < 650; case 0x42: return v < 40; case 0x5E: return v < 3000;
    case 0x24: case 0x44: return v < 1.9999f;
  }
  return true;
}
static void storePid(uint8_t pid, const uint8_t* b, int n, uint32_t t) { float v = decodePid(pid, b, n); if (plausible(pid, v)) { pv[pid] = v; page[pid] = t; } }
static int queryMulti(const uint8_t* ids, int cnt, uint8_t* got) {  // one request for several PIDs; got[i] = 1 when ids[i] was decoded; returns how many
  char cmd[24]; int k = snprintf(cmd, sizeof cmd, "01");
  for (int i = 0; i < cnt; i++) k += snprintf(cmd + k, sizeof cmd - k, "%02X", ids[i]);
  String r;
  for (int attempt = 0; attempt < 2; attempt++) {
    char c2[24]; snprintf(c2, sizeof c2, "%s%s", cmd, cntOk ? "1" : "");
    r = elmQuery(c2, pollTmo());
    if (!cntOk || r.indexOf('?') < 0) break;
    cntOk = false;                                                   // the adapter does not know the digit: ask again without it
  }
  int nm = parseMsgs(r, msgs, MAXM), ok = 0; uint32_t t = millis();
  for (int i = 0; i < nm; i++) {
    const Msg& m = msgs[i];
    if (m.n < 3 || m.b[0] != 0x41) continue;
    int j = 1;
    while (j < m.n) {
      uint8_t pid = m.b[j]; int len = pidLen(pid);
      if (!len || j + 1 + len > m.n) break;
      for (int q = 0; q < cnt; q++) if (ids[q] == pid && !got[q]) { storePid(pid, m.b + j + 1, len, t); got[q] = 1; ok++; }
      j += 1 + len;
    }
  }
  return ok;
}
static int pollGroup(const uint8_t* ids, int cnt, int& fails) {      // returns how many PIDs the car answered
  uint8_t got[6] = {0, 0, 0, 0, 0, 0};
  ftAsk++;
  int ok = queryMulti(ids, cnt, got);
  if (ok == 0) {                                                     // nothing decoded: the car is silent, or it does not take combined requests - one plain RPM query tells which
    int f2 = 0;
    if (pollPid(0x0C, f2)) { fails = 0; if (++multiBad >= 2) multiOff(); }
    else fails++;
    ftSilent++;
    return 0;
  }
  fails = 0; multiBad = 0;
  if (got[0]) noteRpm();
  bool all = true;
  for (int q = 0; q < cnt; q++) { if (got[q]) { pfail[ids[q]] = 0; pskip[ids[q]] = 0; } else { all = false; notePidFail(ids[q]); } }
  if (all) multiShort = 0; else if (++multiShort >= 6) multiOff();   // the car only answers part of a combined request: back to single requests
  return ok;
}
static bool pollPid(uint8_t pid, int& fails) {
  uint8_t b[9]; int n = 0;
  if (pid == 0x0C || pid == 0x0D) ftAsk++;
  if (queryPid(pid, b, n, pollTmo())) {
    storePid(pid, b, n, millis()); fails = 0; pfail[pid] = 0; pskip[pid] = 0;
    if (pid == 0x0C) noteRpm();
    if (pid == 0x01 && n >= 4) {
      mil = (b[0] & 0x80) != 0; dtcCount = b[0] & 0x7F;
      monRaw = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; monValid = true; monVer = monVer + 1;
      ecuDiesel = (b[1] & 0x08) != 0; ecuDieselKnown = true;            // SAE J1979: PID 01 byte B bit 3 = compression ignition
    }
    if (pid == 0x51) { int f = b[0]; if (f >= 0x10 && f <= 0x15) ecuHybrid = true; if (f == 4 || f == 0x12) { ecuDiesel = true; ecuDieselKnown = true; } }
  } else { fails++; notePidFail(pid); if (pid == 0x0C || pid == 0x0D) ftSilent++; return false; }
  return true;
}

// ---------- faster ELM timing on CAN (v1.0.12) ----------
static uint8_t ftState = 0;                                          // 0 not used (K-line, adapter refused), 1 on trial, 2 kept, 3 given up (the adapter did worse with it)
static char ftBadAddr[18] = "";                                      // the adapter that did worse: not tried again
static uint16_t ftN = 0, ftMiss = 0;
static void ftBegin() {
  ftState = 0; ftN = ftMiss = 0;
  if (protoNum < 6 || !selAddr[0] || !strcmp(ftBadAddr, selAddr)) return;
  if (elmQuery("ATAT2", 800).indexOf("OK") < 0) return;              // an adapter without adaptive timing: leave it as it is
  elmQuery("ATST19", 800);                                           // 25 x 4 ms = 100 ms to wait for an answer (the ELM default is 200 ms)
  ftState = 1;
}
static void ftRevert() {
  elmQuery("ATAT1", 800); elmQuery("ATST32", 800);                   // the ELM defaults
  ftState = 3;
  int f = 0;
  if (pollPid(0x0C, f)) { strncpy(ftBadAddr, selAddr, sizeof ftBadAddr - 1); ftBadAddr[sizeof ftBadAddr - 1] = 0; protoDirty = true; }   // the car answers now: it was the timing. (Silent still = ignition off, not the adapter's fault: not remembered)
}
static void ftWatch(uint16_t asks, uint16_t misses) {                // after each round while the timing is on trial
  if (ftState != 1) return;
  ftN += asks; ftMiss += misses;
  if (ftMiss >= 6 && ftMiss * 3 > ftN) ftRevert();                   // more than 1 request in 3 came back empty: worse than before
  else if (ftN >= 100) ftState = 2;                                  // 100 requests without trouble: keep it
}

// ---------- CAN listen (v1.0.12) ----------
struct LsRow { uint16_t id; uint16_t n; uint8_t d[8]; uint8_t dn; };
static const int LS_MAX = 40;                                        // distinct IDs followed in "all IDs" mode
static volatile bool listenWant = false;                             // the CAN listen page is open (set by the UI)
static volatile uint8_t listenMode = 0, listenGen = 0, listenSt = 0; // mode 0 = ID 202 only, 1 = all IDs; state 0 off, 1 starting, 2 listening, 4 cannot (no CAN 11-bit)
static volatile uint32_t lsTotal = 0, lsStart = 0; static volatile uint16_t lsRate = 0; static volatile uint8_t lsBuf = 0, lsDistinct = 0;
static LsRow lsLast, lsTop[5]; static volatile uint8_t lsTopN = 0;  // the last frame of the filtered ID; the five busiest IDs of the last second
static LsRow lsTab[LS_MAX]; static uint8_t lsTabN = 0; static uint16_t lsWin = 0;
static void lsLine(const char* s, int k) {                           // "2020B5C0000FF0000": 3 hex digits of ID, then the data bytes (ATH1, spaces off)
  if (k >= 10 && !strncmp(s, "BUFFERFULL", 10)) { lsBuf = 1; return; }
  if (k < 3 || k > 19 || ((k - 3) & 1) || !isHexStr(s, k)) return;
  uint16_t id = (uint16_t)((hv(s[0]) << 8) | (hv(s[1]) << 4) | hv(s[2]));
  int dn = (k - 3) / 2; uint8_t d[8];
  for (int i = 0; i < dn; i++) d[i] = (uint8_t)((hv(s[3 + 2 * i]) << 4) | hv(s[4 + 2 * i]));
  if (listenMode == 0 && id != 0x202) return;
  lsTotal = lsTotal + 1; lsWin++;
  if (listenMode == 0) { lsLast.id = id; lsLast.dn = (uint8_t)dn; memcpy(lsLast.d, d, dn); lsLast.n = 1; return; }
  int i = 0; while (i < lsTabN && lsTab[i].id != id) i++;
  if (i == lsTabN) { if (lsTabN >= LS_MAX) return; lsTab[i].id = id; lsTab[i].n = 0; lsTabN++; }
  lsTab[i].n++; lsTab[i].dn = (uint8_t)dn; memcpy(lsTab[i].d, d, dn);
}
static void lsSecond() {                                             // once per second: the rate, and the five busiest IDs
  lsRate = lsWin; lsWin = 0;
  LsRow top[5]; int nt = 0;
  for (int i = 0; i < lsTabN; i++) {
    if (!lsTab[i].n) continue;
    int pos;
    if (nt < 5) pos = nt++; else if (lsTab[i].n > top[4].n) pos = 4; else continue;
    while (pos > 0 && top[pos - 1].n < lsTab[i].n) { top[pos] = top[pos - 1]; pos--; }
    top[pos] = lsTab[i];
  }
  for (int i = 0; i < nt; i++) lsTop[i] = top[i];
  lsTopN = (uint8_t)nt; lsDistinct = (uint8_t)lsTabN;
  for (int i = 0; i < lsTabN; i++) lsTab[i].n = 0;
}
static void doListen() {
  uint8_t g = listenGen, mode = listenMode;
  listenSt = 1; lsTotal = 0; lsRate = 0; lsWin = 0; lsTabN = 0; lsTopN = 0; lsDistinct = 0; lsBuf = 0; lsLast.n = 0; lsLast.dn = 0; lsStart = millis();
  elmQuery("ATH1", 800);                                             // frames with their ID
  if (mode == 0) elmQuery("ATCRA202", 800);                          // only the engine data frame (Mazda 3 2017 layout: RPM, speed, pedal)
  while (linkAvail()) linkRead();
  linkSend("ATMA");
  listenSt = 2;
  char ln[40]; int k = 0; uint32_t tWin = millis();
  while (listenWant && g == listenGen && !abortWait()) {
    bool got = false;
    while (linkAvail()) {
      char c = (char)linkRead(); got = true;
      if (c == '\r' || c == '\n') { if (k) lsLine(ln, k); k = 0; }
      else if (c != ' ' && c != '>') { if (k < 38) ln[k++] = c; else k = 39; }
    }
    uint32_t t = millis();
    if (t - tWin >= 1000) { tWin = t; lsSecond(); }
    if (!got) vTaskDelay(1);
  }
  if (!linkIsUp()) return;
  linkSend("");                                                      // any character stops the monitor
  uint32_t t0 = millis(); bool prompt = false;
  while (millis() - t0 < 1500 && linkIsUp()) { if (linkAvail()) { if (linkRead() == '>') { prompt = true; break; } } else vTaskDelay(1); }
  if (!prompt) { linkDown(); return; }                               // the adapter does not come back: reconnect (the normal start-up runs again)
  elmQuery("ATCRA", 800); elmQuery("ATAR", 800); elmQuery("ATH0", 800);   // back to what the polling expects
  listenSt = 0;
}

// ---------- Honda K-line, UART straight to a K-line board (v1.0.13) ----------
#ifndef KL_RX
#define KL_RX 22                                                     // K-line board TX -> here (the board's 5 V output through a divider: this pin is 3.3 V)
#endif
#ifndef KL_TX
#define KL_TX 27                                                     // here -> K-line board RX
#endif
static uint8_t klT11[24]; static volatile uint8_t klT11n = 0;        // the last live table (bytes after "71 11"), as the ECU sent it
static uint8_t klId[10]; static volatile uint8_t klIdN = 0;          // ECU id (table 0x00)
static volatile uint32_t klOk = 0, klBad = 0;                        // tables answered / requests without a usable answer
static volatile uint16_t klRate = 0;                                 // tables answered in the last second
static volatile uint8_t klEchoSt = 0;                                // 0 not known yet, 1 the board repeats what we send, 2 it does not
static volatile uint8_t klSt = 0;                                    // 0 off, 1 waking the ECU, 2 linked, 3 no answer
static bool klUp = false;
struct KlLive { int rpm, spd, ect, iat, map; float tps, batt, inj, ign; bool injOk, ignOk; };
static bool klAbort() { return linkMode != 1 || scanReq || reconnectReq; }
static void klOpen() { if (!klUp) { Serial2.begin(10400, SERIAL_8N1, KL_RX, KL_TX); klUp = true; } while (Serial2.available()) Serial2.read(); }
static void klClose() { if (klUp) { Serial2.end(); klUp = false; } pinMode(KL_TX, OUTPUT); digitalWrite(KL_TX, HIGH); }     // the TX pin rests high: a floating pin must not pull the K line down
static void klWake() {                                               // K line low for 70 ms, then high for 130 ms: the ECU starts to listen
  klClose();
  pinMode(KL_TX, OUTPUT); digitalWrite(KL_TX, HIGH); vTaskDelay(5);
  digitalWrite(KL_TX, LOW); vTaskDelay(70);
  digitalWrite(KL_TX, HIGH); vTaskDelay(130);
  klOpen();
}
// one request: frame = type, length (whole frame), data, checksum (the frame adds up to 0). Returns the reply length (0 = no usable answer), the reply in r.
// A single-wire board repeats every byte we send: that echo is recognised by itself (the reply type never equals the request type) and skipped.
static int klSend(uint8_t mt, const uint8_t* d, int dl, uint8_t* r, uint32_t tmo = 350) {
  uint8_t f[12]; int n = 0; f[n++] = mt; f[n++] = (uint8_t)(dl + 3);
  for (int i = 0; i < dl && n < 10; i++) f[n++] = d[i];
  uint8_t s = 0; for (int i = 0; i < n; i++) s = (uint8_t)(s + f[i]);
  f[n++] = (uint8_t)(0x100 - s);
  while (Serial2.available()) Serial2.read();
  Serial2.write(f, n);
  uint8_t b[64]; int k = 0, off = -1; uint32_t t0 = millis(), lim = tmo + (uint32_t)n * 2;
  while ((uint32_t)(millis() - t0) < lim) {
    bool got = false;
    while (Serial2.available() && k < (int)sizeof b) { b[k++] = (uint8_t)Serial2.read(); got = true; }
    if (off < 0 && k >= 1) { if (b[0] != f[0]) off = 0; else if (k >= n) off = memcmp(b, f, n) ? 0 : n; }
    if (off >= 0 && k - off >= 2) {
      int len = b[off + 1];
      if (len < 4 || len > 40) break;                                // not a frame
      if (k - off >= len) {
        uint8_t sum = 0; for (int i = 0; i < len; i++) sum = (uint8_t)(sum + b[off + i]);
        if (sum || b[off] != (mt & 0x0F)) break;                     // checksum or reply type wrong
        memcpy(r, b + off, len); klEchoSt = off ? 1 : 2; return len;
      }
    }
    if (klAbort()) break;
    if (!got) vTaskDelay(1);
  }
  return 0;
}
static int klTable(uint8_t t, uint8_t* out, int cap) {               // the data bytes of one table (after "71 tt"); -1 = no answer
  uint8_t d[2] = {0x71, t}, r[44];
  int n = klSend(0x72, d, 2, r);
  if (n < 5 || r[2] != 0x71 || r[3] != t) return -1;
  int m = n - 5; if (m > cap) m = cap;
  memcpy(out, r + 4, m); return m;
}
static bool klDecode(const uint8_t* v, int n, KlLive& o) {           // table 0x11 (community layout, as in the Jet_HONDA_APP); unconfirmed on most models
  if (n < 14) return false;
  o.rpm = (v[0] << 8) | v[1]; o.tps = v[3] / 1.6f; o.ect = (int)v[5] - 40; o.iat = (int)v[7] - 40; o.map = v[9]; o.batt = v[12] / 10.0f; o.spd = v[13];
  o.injOk = n >= 16; o.inj = o.injOk ? (float)((v[14] << 8) | v[15]) / 250.0f : 0;
  o.ignOk = n >= 17; o.ign = o.ignOk ? v[16] / 2.0f - 64.0f : 0;
  return true;
}
static void klPut(uint8_t pid, float v, uint32_t t) { if (plausible(pid, v)) { pv[pid] = v; page[pid] = t; } }
static void klStore(const KlLive& o) {                               // the gauge's own PIDs: the same screens as with an ELM327
  uint32_t t = millis();
  klPut(0x0C, (float)o.rpm, t); klPut(0x0D, (float)o.spd, t); klPut(0x05, (float)o.ect, t); klPut(0x0F, (float)o.iat, t);
  klPut(0x0B, (float)o.map, t); klPut(0x11, fminf(o.tps, 100.0f), t); klPut(0x42, o.batt, t);
  if (o.ignOk) { klPut(0x0E, o.ign, t); psup[0x0E] = true; }
  noteRpm();
}
static bool klConnect() {                                            // wake, ping, ECU id (the live table is accepted too): all within a few seconds, or the ECU goes back to sleep
  uint8_t r[44], p = 0x72, o[24];
  for (int a = 0; a < 2 && !klAbort(); a++) {
    klSt = 1; klWake();
    if (!klSend(0xFE, &p, 1, r)) continue;
    int n = klTable(0x00, o, sizeof o);
    if (n < 0 && !klAbort()) n = klTable(0x00, o, sizeof o);
    if (n >= 0) { klIdN = (uint8_t)(n > 10 ? 10 : n); memcpy(klId, o, klIdN); } else klIdN = 0;   // an ECU without table 0 may still give the live table
    if (klAbort()) break;
    KlLive lv; n = klTable(0x11, o, sizeof o);
    if (n >= 14 && klDecode(o, n, lv)) { memcpy(klT11, o, n); klT11n = (uint8_t)n; klStore(lv); klOk = klOk + 1; return true; }   // linked only when the live table really comes
  }
  return false;
}
static void klHexStr(char* d, size_t cap, const uint8_t* b, int n) { size_t k = 0; d[0] = 0; for (int i = 0; i < n && k + 3 < cap; i++) k += snprintf(d + k, cap - k, i ? " %02X" : "%02X", b[i]); }
static void klRun() {                                                // obdTask: this is the whole link while the K-line mode is on
  linkDown(); reconnectReq = false; klSt = 0; linkState = 1;
  while (!klAbort()) {
    if (!klConnect()) {
      linkState = 1; klSt = 3;
      for (int i = 0; i < 12 && !klAbort(); i++) vTaskDelay(100);    // the next wake in about a second
      continue;
    }
    memset(psup, 0, sizeof psup);
    { static const uint8_t KP[] = {0x0C, 0x0D, 0x05, 0x0F, 0x0B, 0x11, 0x42}; for (unsigned i = 0; i < sizeof KP; i++) psup[KP[i]] = true; }
    protoNum = 0; cntOk = true; multiOk = false; kMulti = false; resetPollState(); obdRpmMs = 0;
    ecuHybrid = false; ecuDieselKnown = true; ecuDiesel = false; mil = false; dtcCount = 0; monValid = false; vinPending = false; ftState = 0; klRate = 0;
    klSt = 2; linkState = 2;
    int misses = 0; uint16_t rn = 0; uint32_t tRate = millis();
    while (!klAbort()) {
      if (req) {                                                     // the code / freeze / ECU info pages: only the ECU info is available on this link
        int q = req; req = 0;
        if (q == 1 || q == 2) { codeState = 4; codeVer = codeVer + 1; }
        else if (q == 3) { frzState = 3; frzVer = frzVer + 1; }
        else if (q == 4) { strcpy(ecuName, "HONDA K-LINE"); klHexStr(calId, sizeof calId, klId, klIdN > 6 ? 6 : klIdN); ecuInfoVer = ecuInfoVer + 1; }
      }
      if (listenWant) listenSt = 4;                                  // the CAN listen page has nothing to listen to here
      uint8_t t[24]; int n = klTable(0x11, t, sizeof t); KlLive lv;
      if (n >= 14 && klDecode(t, n, lv)) {
        memcpy(klT11, t, n); klT11n = (uint8_t)n; klStore(lv); klOk = klOk + 1; rn++; misses = 0;
      } else {
        klBad = klBad + 1;
        if (++misses >= 4) break;                                    // 4 requests in a row without a usable answer: wake the ECU again
        vTaskDelay(20);
      }
      uint32_t now = millis(); if (now - tRate >= 1000) { klRate = rn; rn = 0; tRate = now; }
      vTaskDelay(1);
    }
    linkState = 1; klSt = klAbort() ? 0 : 3; klRate = 0;
  }
  klClose(); klSt = 0;
}

static void detectVehicle() {
  uint8_t b[9]; int n = 0;
  ecuHybrid = psup[0x5B]; ecuDieselKnown = false; ecuDiesel = false;
  int f = 0;
  pollPid(0x01, f);
  if (psup[0x51]) pollPid(0x51, f);
  vinPending = true;                                   // the VIN follows a few rounds later (obdTask), after the first values are on the screen
}

// CRP: rail pressure from the ECU -> sensor volts through a line  V = V0 + (4.50 - V0) x MPa / full scale  (set on the CRP calibration page)
static int crpFs = 180;                               // rail pressure (MPa) at 4.50 V (a 1800 bar sensor)
static int crpV0 = 50;                                // sensor volts x100 at 0 MPa
static const int CRP_VTOP = 450;                      // sensor volts x100 at full scale (5 V sensor: 0.5 - 4.5 V)
static int crpMeter = 125; static uint8_t crpMsg = 0; static uint32_t crpMsgAt = 0;   // calibration page: the volts the owner measured with a meter, and the message after Match
static int crpTrim = 100;                             // % factor on the ECU pressure, to match another tool
static uint8_t crpSrc = 0;                            // 0 = auto (PID 23, else 59), 1 = PID 23 only, 2 = PID 59 only
static uint8_t crpFastPid = 0;                        // the rail PID while it is on screen: asked in every request group instead of once per rotation
static bool crpHave() { return crpSrc == 1 ? psup[0x23] : crpSrc == 2 ? psup[0x59] : (psup[0x23] || psup[0x59]); }
static uint8_t crpPid() { return crpSrc == 1 ? 0x23 : crpSrc == 2 ? 0x59 : psup[0x23] ? 0x23 : psup[0x59] ? 0x59 : 0; }
static uint8_t thrFast = 0;                           // the pedal / throttle PID on screen (CAN groups): asked in every request instead of once per rotation
static uint16_t pidEvery(uint8_t pid, int tier) {          // shortest time between two requests of one PID (ms): fast movers every round, slow ones on a timer
  uint16_t e;
  switch (pid) {
    case 0x5E: e = 300; break;                                                       // fuel rate
    case 0x78: case 0x79: case 0x7A: case 0x7B: e = 250; break;                      // exhaust gas temperatures
    case 0x06: case 0x07: case 0x08: case 0x09: e = 400; break;                      // fuel trims
    case 0x0F: case 0x42: e = 600; break;                                            // intake air temperature, battery volts
    case 0x05: case 0x5C: case 0x1F: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x5B: e = 1000; break;   // coolant, oil, run time, catalyst, hybrid battery
    case 0x2F: case 0x33: case 0x46: e = 2500; break;                                // fuel level, barometer, ambient (the screen calls a value old after 8 s)
    default: e = 0;                                                                  // load, MAP, MAF, throttle, pedal, lambda, rail pressure, torque, timing: as often as the car answers
  }
  if (tier == 1 && e < 400) e = 400;                                                 // background PIDs (alerts, trip, graphs) never need more
  return e;
}
static bool pidDue(uint8_t pid, int tier) { uint32_t p = page[pid]; if (!p) return true; uint16_t e = pidEvery(pid, tier); return !e || (uint32_t)(millis() - p) >= e; }
static int nextWanted(int& rr, int tier) {
  for (int i = 0; i < 255; i++) {
    rr = (rr % 255) + 1;
    if (psup[rr] && want[rr] == tier && rr != 0x0C && rr != 0x0D && rr != 0x01 && rr != crpFastPid && rr != thrFast && (int32_t)(iterNow - pskip[rr]) >= 0 && pidDue((uint8_t)rr, tier)) return rr;
  }
  return -1;
}

static uint32_t autoAt = 0;
static void autoPick() {
  doScan();
  if (btBrowsing || selAddr[0]) return;                  // the owner is on the Bluetooth page / has picked one meanwhile
  int nObd = 0, top = -999, second = -999, bi = -1; BtDev best; memset(&best, 0, sizeof best);
  portENTER_CRITICAL(&btMux);
  for (int i = 0; i < btCount; i++) if (btList[i].obd) {
    nObd++; int r = btList[i].rssi;
    if (r > top) { second = top; top = r; bi = i; } else if (r > second) second = r;
  }
  if (bi >= 0) best = btList[bi];
  portEXIT_CRITICAL(&btMux);
  if (bi < 0 || (nObd > 1 && top - second < 8)) return;  // none, or several that are about equally close: the owner picks
  portENTER_CRITICAL(&btMux);
  if (!selAddr[0]) {
    memcpy(selAddr, best.addr, sizeof selAddr); strcpy(selName, best.name[0] ? best.name : best.addr);
    selBle = best.ble; selAType = best.atype; bleTry = 0; selGen = selGen + 1; pinIdx = 0; pinOk = false;
  }
  portEXIT_CRITICAL(&btMux);
  carBuf[0] = 0; vinBuf[0] = 0; pinDirty = true;         // loop() saves the choice
}

static void pollOther(int& rrFg, int& rrBg, bool bg, int& fails) {   // one PID that is not RPM/speed: the ones on screen first, background ones (alerts, trip, graphs) in between
  int p = nextWanted(bg ? rrBg : rrFg, bg ? 1 : 2);
  if (p < 0) p = nextWanted(bg ? rrFg : rrBg, bg ? 2 : 1);
  if (p >= 0) pollPid((uint8_t)p, fails);
}
void obdTask(void*) {
  int rrFg = 0, rrBg = 0, fails = 0, iter = 0; uint32_t lastMil = 0;
  for (;;) {
    if (scanReq) { scanReq = false; doScan(); continue; }
    if (linkMode == 1) { klRun(); continue; }                       // Honda K-line board: no Bluetooth, no ELM327
    if (demoMode || !selAddr[0]) {            // demo, or OBD mode with no adapter chosen yet
      linkDown();
      linkState = demoMode ? 0 : 3;
      if (!demoMode && !btBrowsing && millis() > 2500 && (autoAt == 0 || millis() - autoAt > 25000)) {   // nothing chosen yet: search by itself (first time, then every 25 s)
        autoAt = millis(); linkState = 1; autoPick(); if (!selAddr[0]) linkState = 3;
        continue;
      }
      vTaskDelay(200); continue;
    }
    if (reconnectReq) { reconnectReq = false; linkDown(); }
    if (!linkIsUp()) {
      linkState = 1;
      char sa[18]; bool sb; uint8_t st; uint32_t gen;
      portENTER_CRITICAL(&btMux); memcpy(sa, selAddr, sizeof sa); sb = selBle; st = selAType; gen = selGen; portEXIT_CRITICAL(&btMux);
      bool ok = linkConnect(sa, sb, st);
      if (!ok) {
        if (!sb && !pinOk && gen == selGen) pinIdx = (pinIdx + 1) % NPINS;   // wrong PIN? the next common one is tried on the next attempt
        if (!scanReq && !reconnectReq) vTaskDelay(1500);
        continue;
      }
      if (demoMode || scanReq || reconnectReq) continue;
      if (!elmInit()) {
        if (sb && !elmAlive && gen == selGen) bleTry = bleTry + 1;      // silent: try the next GATT candidate (an ELM that talks but has no car yet is not a reason)
        if (!(scanReq || reconnectReq || demoMode)) { linkDown(); vTaskDelay(2000); }
        continue;
      }
      if (!sb && gen == selGen && (!pinOk || pinIdx != pinSaved)) { pinOk = true; pinDirty = true; }   // this PIN works: remember it
      discoverPids(); detectVehicle(); ftBegin(); fails = 0; iter = 0; linkState = 2; lastMil = millis();
    }
    while (!demoMode && linkIsUp() && fails < 8 && !scanReq && !reconnectReq) {
      if (req == 1) { req = 0; doReadCodes(); }
      else if (req == 2) { req = 0; doClearCodes(); }
      else if (req == 3) { req = 0; doFreeze(); }
      else if (req == 4) { req = 0; doEcuInfo(); }
      if (listenWant && !demoMode) {
        if (protoNum == 6 || protoNum == 8) { doListen(); fails = 0; lastMil = millis(); continue; }      // 11-bit CAN: the polling waits while the page listens
        listenSt = 4;                                                                                    // anything else: nothing to listen to, the polling goes on
      }
      iter++; iterNow = (uint32_t)iter;
      uint16_t ftA0 = ftAsk, ftS0 = ftSilent;
      { uint8_t cp = crpPid(); crpFastPid = (cp && psup[cp] && want[cp] == 2 && (int32_t)(iterNow - pskip[cp]) >= 0) ? cp : 0; }
      thrFast = 0;
      if (multiOk && protoNum >= 6) {                                                       // CAN: the pedal (else the throttle) on screen travels with RPM in every request; K-line has no room for it
        static const uint8_t TP[2] = {0x49, 0x11};                                          // accelerator pedal D first, then throttle position (they follow each other)
        for (int i = 0; i < 2 && !thrFast; i++) if (psup[TP[i]] && want[TP[i]] == 2 && (int32_t)(iterNow - pskip[TP[i]]) >= 0) thrFast = TP[i];
      }
      if (vinPending && iter >= 6) { vinPending = false; readVin(); }
      if (multiOk && protoNum >= 6) {
        // CAN: RPM, speed and up to 4 more PIDs travel in ONE request (the last place rotates the background PIDs: alerts, trip, graphs)
        uint8_t ids[6]; int cnt = 0, single = -1;
        ids[cnt++] = 0x0C; ids[cnt++] = 0x0D;
        if (crpFastPid && pidLen(crpFastPid)) ids[cnt++] = crpFastPid;                      // the rail pressure on screen: in every request
        if (thrFast) ids[cnt++] = thrFast;                                                  // so is the pedal / throttle (at most 2 pinned in all: 2 places stay for the rotation)
        int nOther = 6 - cnt;
        for (int k = 0; k < nOther; k++) {
          bool bg = (k == nOther - 1);
          int p = nextWanted(bg ? rrBg : rrFg, bg ? 1 : 2);
          if (p < 0) p = nextWanted(bg ? rrFg : rrBg, bg ? 2 : 1);
          if (p < 0) continue;
          bool dup = false; for (int q = 0; q < cnt; q++) if (ids[q] == p) dup = true;
          if (dup) continue;
          if (pidLen((uint8_t)p)) ids[cnt++] = (uint8_t)p; else if (single < 0) single = p;
        }
        pollGroup(ids, cnt, fails);
        if (single >= 0 && multiOk) pollPid((uint8_t)single, fails);
      } else if (kMulti && protoNum >= 3 && protoNum <= 5) {
        // K-line, pair mode: RPM + speed in ONE request, then one ordinary PID, twice per round -> RPM every 2nd request
        static const uint8_t RS[2] = {0x0C, 0x0D};
        for (int h = 0; h < 2 && kMulti && fails < 8 && !abortWait(); h++) {
          if (pollGroup(RS, 2, fails) < 2 && !abortWait()) pollPid(0x0D, fails);        // the car gave only one of them: speed is asked alone
          if (crpFastPid && h == 0) pollPid(crpFastPid, fails);                         // rail pressure on screen: once per round
          else pollOther(rrFg, rrBg, iter % 4 == 3 && h == (crpFastPid ? 1 : 0), fails);
        }
      } else {
        // one PID per request: RPM twice per round (the gauge people watch), speed once, then the others (K-line: 1, CAN fallback: 2)
        pollPid(0x0C, fails); pollPid(0x0D, fails); pollPid(0x0C, fails);
        int others = protoNum >= 6 || protoNum == 0 ? 2 : 1;
        int bgk = (crpFastPid && others > 1) ? 1 : 0;
        for (int k = 0; k < others; k++) {
          if (crpFastPid && ((others > 1 && k == 0) || (others == 1 && (iter & 1) == 0))) pollPid(crpFastPid, fails);   // rail pressure on screen: every round (CAN fallback) / every 2nd round (K-line)
          else pollOther(rrFg, rrBg, iter % 4 == 3 && k == bgk, fails);                // on-screen PIDs 3 rounds out of 4, background ones in between
        }
      }
      ftWatch((uint16_t)(ftAsk - ftA0), (uint16_t)(ftSilent - ftS0));
      if (millis() - lastMil > (protoNum >= 6 ? 2000u : 4000u)) { pollPid(0x01, fails); lastMil = millis(); }   // warning lamp / readiness bits (K-line: every request counts, so less often)
      vTaskDelay(1);
    }
    linkDown();
    linkState = 1; if (!scanReq && !reconnectReq) vTaskDelay(300);
  }
}

static float crpMn = 1e9f, crpMx = -1e9f;             // lowest / highest rail pressure (MPa) seen since the CRP page was opened or reset

// =====================================================================================
//  Graphics core (V4).  The screen is composed in horizontal bands of BAND_H rows inside tPix and pushed band by band, so every
//  pixel is written exactly once per frame (no flicker, no stale pixels) and shapes can be anti-aliased and blended freely.
//  Every drawing call below clips itself to the band that is being composed.
// =====================================================================================
enum FontId : uint8_t { F_NXL, F_NHL, F_NL, F_NM, F_NS, F_UL, F_UM, F_US, F_LS, F_LM, F_NXM, F_NFONT };
static const FontRec* const FNT[F_NFONT] = {&FONT_NXL, &FONT_NHL, &FONT_NL, &FONT_NM, &FONT_NS, &FONT_UL, &FONT_UM, &FONT_US, &FONT_LS, &FONT_LM, &FONT_NXM};
enum { AL_L = 0, AL_C = 1, AL_R = 2 };
#define DEGC "\x7F" "C"
#define DEG "\x7F"

static const int SCR_W = 320, SCR_H = 240, BAND_H = 20, NBAND = SCR_H / BAND_H;
alignas(4) static uint16_t tPix[SCR_W * BAND_H];            // the band being composed (12.8 KB)
static int bandY0 = 0, bandY1 = BAND_H;
static uint32_t uiNow = 0;                                    // time of the frame being drawn
static float rpmUi = 0;                                       // the RPM the dash shows this frame (rpmFollow)

static inline uint16_t* rowPtr(int y) { return tPix + (y - bandY0) * SCR_W; }
static inline int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// ---------- colours ----------
struct Rgb { uint8_t r, g, b; };
static inline uint16_t c565(Rgb c) { return (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3)); }
constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
struct Theme { Rgb bg, panel, panel2, line, dark, acc, accL, acc2, accD, dim, text, hot, good, warn, track, disc, discLine, tabbg, tabline, sep, chartbg, grid, onacc; };
struct Pal { uint16_t bg, panel, panel2, line, dark, acc, accL, acc2, accD, dim, text, hot, good, warn, track, disc, discLine, tabbg, tabline, sep, chartbg, grid, onacc; };
static const int NTHEME = 2;
static const Theme THEMES[NTHEME] = {
  // 0 = black-blue
  {{6, 13, 27}, {10, 20, 38}, {14, 28, 53}, {23, 49, 92}, {18, 36, 74}, {47, 140, 255}, {155, 216, 255}, {74, 163, 255}, {27, 95, 216}, {111, 136, 179}, {232, 241, 255},
   {255, 77, 94}, {53, 224, 176}, {255, 176, 32}, {16, 33, 63}, {6, 15, 32}, {20, 41, 79}, {5, 13, 28}, {15, 33, 64}, {17, 36, 68}, {6, 16, 31}, {18, 38, 74}, {2, 16, 36}},
  // 1 = black-red
  {{13, 6, 9}, {24, 10, 14}, {38, 15, 21}, {88, 34, 46}, {60, 22, 30}, {255, 70, 85}, {255, 170, 178}, {255, 110, 120}, {170, 30, 50}, {178, 138, 146}, {255, 240, 241},
   {255, 176, 32}, {53, 224, 176}, {255, 222, 110}, {46, 18, 24}, {16, 6, 9}, {70, 26, 35}, {11, 5, 8}, {50, 20, 27}, {52, 22, 30}, {15, 7, 10}, {56, 24, 32}, {30, 2, 6}},
};
static Theme TA = THEMES[0];                  // current theme as RGB888 ...
static Pal T;                                 // ... and as RGB565
static int theme = 0;
static void applyTheme(int t) {
  static_assert(sizeof(Theme) == 23 * 3 && sizeof(Pal) == 23 * 2, "theme layout");
  if (t < 0 || t >= NTHEME) t = 0;
  theme = t; TA = THEMES[t];
  const Rgb* s = &TA.bg; uint16_t* d = &T.bg;
  for (int i = 0; i < 23; i++) d[i] = c565(s[i]);
}
static const Rgb RGB_ALRED = {255, 77, 94}, RGB_ALBG = {16, 5, 10}, RGB_ALCARD = {26, 8, 16}, RGB_VIOLET = {168, 130, 255};
static const uint16_t RED_AL = C(255, 77, 94), ALBG = C(16, 5, 10), ALTXT = C(255, 179, 187), WHITE = C(255, 255, 255), VIOLET = C(168, 130, 255), ALCARD = C(26, 8, 16);

// blend src over dst with alpha a (0..255)
static inline uint16_t blendA(uint16_t d, uint16_t s, uint32_t a) {
  if (a == 0) return d;
  if (a >= 255) return s;
  a += a >> 7; uint32_t ia = 256 - a;
  uint32_t r = (((d >> 11) & 31) * ia + ((s >> 11) & 31) * a + 128) >> 8;
  uint32_t g = (((d >> 5) & 63) * ia + ((s >> 5) & 63) * a + 128) >> 8;
  uint32_t b = ((d & 31) * ia + (s & 31) * a + 128) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
static uint16_t mixRgb(Rgb a, Rgb b, int t256) {          // t = 0..256 : a -> b, as RGB565
  return C((uint8_t)(a.r + ((b.r - a.r) * t256 + 128) / 256), (uint8_t)(a.g + ((b.g - a.g) * t256 + 128) / 256), (uint8_t)(a.b + ((b.b - a.b) * t256 + 128) / 256));
}

// ---------- plain fills ----------
static void fillBand(uint16_t c) {
  uint32_t cc = (uint32_t)c | ((uint32_t)c << 16); uint32_t* p = (uint32_t*)tPix;
  for (int i = 0; i < SCR_W * BAND_H / 2; i++) p[i] = cc;
}
static void fr(int x, int y, int w, int h, uint16_t c) {
  int x0 = max(x, 0), x1 = min(x + w, SCR_W), y0 = max(y, bandY0), y1 = min(y + h, bandY1);
  if (x1 <= x0 || y1 <= y0) return;
  for (int yy = y0; yy < y1; yy++) { uint16_t* p = rowPtr(yy); for (int i = x0; i < x1; i++) p[i] = c; }
}
static void frA(int x, int y, int w, int h, uint16_t c, uint32_t a) {
  int x0 = max(x, 0), x1 = min(x + w, SCR_W), y0 = max(y, bandY0), y1 = min(y + h, bandY1);
  if (x1 <= x0 || y1 <= y0) return;
  for (int yy = y0; yy < y1; yy++) { uint16_t* p = rowPtr(yy); for (int i = x0; i < x1; i++) p[i] = blendA(p[i], c, a); }
}
static inline void hl(int x, int y, int w, uint16_t c) { fr(x, y, w, 1, c); }
static inline void plotA(int x, int y, uint16_t c, uint32_t a) {
  if (x < 0 || x >= SCR_W || y < bandY0 || y >= bandY1 || a == 0) return;
  uint16_t* p = rowPtr(y) + x; *p = blendA(*p, c, a);
}

// ---------- rounded rectangles ----------
static uint8_t cvTab[15][14][14]; static bool cvInit[15];          // coverage of the corner pixels of a circle of radius r (index = pixel counted from the outermost one)
static void cvBuild(int r) {
  for (int k = 0; k < r; k++) for (int j = 0; j < r; j++) {
    float dx = j + 0.5f - r, dy = k + 0.5f - r, d = sqrtf(dx * dx + dy * dy);
    cvTab[r][k][j] = (uint8_t)(clampf(r - d + 0.5f, 0, 1) * 255 + 0.5f);
  }
  cvInit[r] = true;
}
static inline uint32_t cvOut(int r, int k, int j) { if (r <= 0 || k >= r || j >= r) return 255; return cvTab[r][k][j]; }
static inline uint32_t cvIn(int r, int k, int j) {            // the shape inset by 1 px (radius r-1)
  if (k < 1 || j < 1) return 0;
  return cvOut(r - 1, k - 1, j - 1);
}
// Rounded rectangle.  Fill: solid (top == nullptr) or a vertical gradient top -> bot.  bc: 1 px border colour inside the box, or -1.
static void rrImpl(int x, int y, int w, int h, int r, const Rgb* top, const Rgb* bot, uint16_t solid, int bc) {
  if (w <= 0 || h <= 0) return;
  int y0 = max(y, bandY0), y1 = min(y + h, bandY1);
  if (y1 <= y0 || x >= SCR_W || x + w <= 0) return;
  r = min(r, min(w / 2, h / 2)); if (r > 14) r = 14; if (r < 0) r = 0;
  if (r > 0 && !cvInit[r]) cvBuild(r);
  if (r > 1 && bc >= 0 && !cvInit[r - 1]) cvBuild(r - 1);
  for (int yy = y0; yy < y1; yy++) {
    int kt = yy - y, kb = y + h - 1 - yy, k = min(kt, kb);
    uint16_t fc = top ? mixRgb(*top, *bot, ((2 * kt + 1) * 256) / (2 * h)) : solid;
    uint16_t* p = rowPtr(yy);
    if (k >= r && k >= 1) {                                   // straight rows
      int xa = max(x, 0), xb = min(x + w, SCR_W);
      if (bc < 0) { for (int i = xa; i < xb; i++) p[i] = fc; }
      else {
        for (int i = max(x + 1, 0); i < min(x + w - 1, SCR_W); i++) p[i] = fc;
        if (x >= 0 && x < SCR_W) p[x] = (uint16_t)bc;
        if (x + w - 1 >= 0 && x + w - 1 < SCR_W) p[x + w - 1] = (uint16_t)bc;
      }
      continue;
    }
    // rows in the rounded zone, or the first / last row (border line)
    for (int zone = 0; zone < 2; zone++) {                     // 0 = left corner, 1 = right corner
      for (int j = 0; j < r; j++) {
        int px = zone == 0 ? x + j : x + w - 1 - j;
        if (px < 0 || px >= SCR_W) continue;
        uint32_t o = cvOut(r, k, j), in = cvIn(r, k, j);
        if (o == 0) continue;
        uint16_t d = p[px];
        d = blendA(d, fc, o);
        if (bc >= 0 && o > in) d = blendA(d, (uint16_t)bc, o - in);
        p[px] = d;
      }
    }
    int sa = max(x + r, 0), sb = min(x + w - r, SCR_W);        // straight part of this row
    uint16_t sc = fc;
    if (bc >= 0 && k < 1) sc = (uint16_t)bc;
    for (int i = sa; i < sb; i++) p[i] = sc;
  }
}
static void rr(int x, int y, int w, int h, int r, uint16_t fill, int bc = -1) { rrImpl(x, y, w, h, r, nullptr, nullptr, fill, bc); }
static void rrG(int x, int y, int w, int h, int r, const Rgb& top, const Rgb& bot, int bc = -1) { rrImpl(x, y, w, h, r, &top, &bot, 0, bc); }
static void tile(int x, int y, int w, int h, int r = 9) { rrG(x, y, w, h, r, TA.panel2, TA.panel, T.line); }

// signed distance (px) of a pixel centre to a rounded rectangle; negative inside
static inline float sdfRR(float px, float py, float x, float y, float w, float h, float r) {
  float qx = fabsf(px - (x + w * 0.5f)) - (w * 0.5f - r), qy = fabsf(py - (y + h * 0.5f)) - (h * 0.5f - r);
  float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
  return sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.0f) - r;
}
static inline float covD(float d) { float c = 0.5f - d; return c < 0 ? 0 : c > 1 ? 1 : c; }
static const uint8_t HALO_A[4] = {87, 51, 26, 12};          // alpha of the 1 px rings around a shape: .34 .20 .10 .045
// soft glow: n one-pixel rings around a rounded rectangle
static void halo(int x, int y, int w, int h, int r, uint16_t col, int n) {
  r = min(r, min(w / 2, h / 2));
  int ya = max(y - n, bandY0), yb = min(y + h + n, bandY1);
  for (int yy = ya; yy < yb; yy++) {
    uint16_t* p = rowPtr(yy);
    bool mid = yy >= y + r && yy < y + h - r;
    for (int i = 1; i <= n && mid; i++) {                      // straight sides: ring i is exactly the i-th pixel
      int xl = x - i, xr = x + w - 1 + i;
      if (xl >= 0 && xl < SCR_W) p[xl] = blendA(p[xl], col, HALO_A[i - 1]);
      if (xr >= 0 && xr < SCR_W) p[xr] = blendA(p[xr], col, HALO_A[i - 1]);
    }
    if (mid) continue;
    int irow = yy < y ? y - yy : yy >= y + h ? yy - (y + h) + 1 : 0;
    for (int px = x - n; px < x + w + n; px++) {
      if (px < 0 || px >= SCR_W) continue;
      bool straight = px >= x + r && px < x + w - r;
      if (straight) { if (irow > 0) p[px] = blendA(p[px], col, HALO_A[irow - 1]); continue; }
      float d0 = sdfRR(px + 0.5f, yy + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
      if (d0 < -0.5f || d0 > n + 0.5f) continue;
      for (int i = 1; i <= n; i++) {
        float cv = covD(d0 - i) - covD(d0 - i + 1);
        if (cv > 0.004f) p[px] = blendA(p[px], col, (uint32_t)(HALO_A[i - 1] * cv + 0.5f));
      }
    }
  }
}
// border of a rounded rectangle with a stroke thickness t (px) drawn inside the box
static void rrBorder(int x, int y, int w, int h, int r, float t, uint16_t col) {
  int ti = (int)ceilf(t), ya = max(y, bandY0), yb = min(y + h, bandY1);
  for (int yy = ya; yy < yb; yy++) {
    uint16_t* p = rowPtr(yy);
    int kd = min(yy - y, y + h - 1 - yy);                       // rows from the nearest horizontal edge
    if (kd >= r + ti) {                                         // straight sides
      for (int i = 0; i < ti; i++) {
        uint32_t a = (uint32_t)(clampf(t - i, 0, 1) * 255 + 0.5f);
        int xl = x + i, xr = x + w - 1 - i;
        if (xl >= 0 && xl < SCR_W) p[xl] = blendA(p[xl], col, a);
        if (xr >= 0 && xr < SCR_W) p[xr] = blendA(p[xr], col, a);
      }
      continue;
    }
    float din = kd + 0.5f; uint32_t as = kd < ti ? (uint32_t)((covD(-din) - covD(-din + t)) * 255 + 0.5f) : 0;
    for (int px = max(x, 0); px < min(x + w, SCR_W); px++) {
      bool corner = px < x + r + ti || px >= x + w - r - ti;
      if (!corner) { if (as) p[px] = blendA(p[px], col, as); continue; }
      float d0 = sdfRR(px + 0.5f, yy + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
      float cv = covD(d0) - covD(d0 + t);
      if (cv > 0.004f) p[px] = blendA(p[px], col, (uint32_t)(cv * 255 + 0.5f));
    }
  }
}

// ---------- discs and strokes ----------
static void discA(float cx, float cy, float r, uint16_t col, uint32_t a) {
  int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = max((int)floorf(cy - r - 1), bandY0), y1 = min((int)ceilf(cy + r + 1), bandY1 - 1);
  for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
    if (px < 0 || px >= SCR_W) continue;
    float dx = px - cx, dy = py - cy, cv = clampf(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0, 1);
    if (cv > 0.004f) { uint16_t* p = rowPtr(py) + px; *p = blendA(*p, col, (uint32_t)(cv * a + 0.5f)); }
  }
}
static inline void disc(float cx, float cy, float r, uint16_t col) { discA(cx, cy, r, col, 255); }
static inline float segDist(float px, float py, float x0, float y0, float x1, float y1, float* tt = nullptr) {
  float vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0, L = vx * vx + vy * vy, t = L > 0 ? (wx * vx + wy * vy) / L : 0;
  if (tt) *tt = t;
  t = clampf(t, 0, 1);
  float dx = wx - vx * t, dy = wy - vy * t;
  return sqrtf(dx * dx + dy * dy);
}
// straight line of width wd between pixel centres (x0,y0) and (x1,y1); butt ends unless round
static void strokeSeg(float x0, float y0, float x1, float y1, float wd, uint16_t col, bool round) {
  float hw = wd * 0.5f;
  int ax = (int)floorf(fminf(x0, x1) - hw - 1), bx = (int)ceilf(fmaxf(x0, x1) + hw + 1), ay = max((int)floorf(fminf(y0, y1) - hw - 1), bandY0), by = min((int)ceilf(fmaxf(y0, y1) + hw + 1), bandY1 - 1);
  for (int py = ay; py <= by; py++) for (int px = ax; px <= bx; px++) {
    if (px < 0 || px >= SCR_W) continue;
    float t, d = segDist((float)px, (float)py, x0, y0, x1, y1, &t);
    if (!round && (t < 0 || t > 1)) { float e = t < 0 ? -t : t - 1; float L = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)); if (e * L > 0.5f) continue; }
    float cv = clampf(hw - d + 0.5f, 0, 1);
    if (cv > 0.004f) { uint16_t* p = rowPtr(py) + px; *p = blendA(*p, col, (uint32_t)(cv * 255 + 0.5f)); }
  }
}
static inline void ln(int x0, int y0, int x1, int y1, uint16_t col) { strokeSeg((float)x0, (float)y0, (float)x1, (float)y1, 1.0f, col, false); }
// "<" or ">" arrow head used on the pager chips (two round-capped strokes, width 1.6)
static void chevron(int cx, int cy, int dir, uint16_t col) {
  float x0 = (float)(cx - dir * 2), y0 = (float)(cy - 4), x1 = (float)(cx + dir * 2), y1 = (float)cy, x2 = x0, y2 = (float)(cy + 4);
  int ay = max(cy - 6, bandY0), by = min(cy + 6, bandY1 - 1);
  for (int py = ay; py <= by; py++) for (int px = cx - 6; px <= cx + 6; px++) {
    if (px < 0 || px >= SCR_W) continue;
    float d = fminf(segDist((float)px, (float)py, x0, y0, x1, y1), segDist((float)px, (float)py, x1, y1, x2, y2));
    float cv = clampf(0.8f - d + 0.5f, 0, 1);
    if (cv > 0.004f) { uint16_t* p = rowPtr(py) + px; *p = blendA(*p, col, (uint32_t)(cv * 255 + 0.5f)); }
  }
}
static void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) {      // small filled triangle, 4x4 supersampled
  int ax = min(x0, min(x1, x2)) - 1, bx = max(x0, max(x1, x2)) + 1, ay = max(min(y0, min(y1, y2)) - 1, bandY0), by = min(max(y0, max(y1, y2)) + 1, bandY1 - 1);
  float den = (float)((y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2));
  if (den == 0) return;
  for (int py = ay; py <= by; py++) for (int px = ax; px <= bx; px++) {
    if (px < 0 || px >= SCR_W) continue;
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
      float fx = px + (sx + 0.5f) / 4 - 0.5f, fy = py + (sy + 0.5f) / 4 - 0.5f;
      float a = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / den, b = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / den, c = 1 - a - b;
      if (a >= 0 && b >= 0 && c >= 0) hits++;
    }
    if (hits) plotA(px, py, col, (uint32_t)(hits * 255 / 16));
  }
}

// ---------- text: Poppins bitmaps with 1/8 px advances; the small text fonts have four sub-pixel phases ----------
static inline const GlyphRec* glyphAt(const FontRec* f, unsigned char ch, int phase) {
  if (ch < 32 || ch > 127) return nullptr;
  uint8_t gi = pgm_read_byte(&f->map[ch - 32]);
  if (gi == 255) return nullptr;
  return &f->g[((int)gi << f->ph) + phase];
}
static int textW(FontId fid, const char* s) {
  const FontRec* f = FNT[fid]; int w8 = 0;
  for (const char* p = s; *p; p++) { const GlyphRec* g = glyphAt(f, (unsigned char)*p, 0); w8 += g ? g->adv : 32; }
  return (w8 + 4) >> 3;
}
static void blit4(const uint8_t* bm, int w, int h, int px, int py, uint16_t fg) {
  int r0 = max(0, bandY0 - py), r1 = min(h, bandY1 - py), c0 = max(0, -px), c1 = min(w, SCR_W - px);
  for (int r = r0; r < r1; r++) {
    uint16_t* d = rowPtr(py + r) + px;
    for (int c = c0; c < c1; c++) {
      int idx = r * w + c; uint8_t v = pgm_read_byte(&bm[idx >> 1]); v = (idx & 1) ? (v & 15) : (v >> 4);
      if (v) d[c] = v == 15 ? fg : blendA(d[c], fg, (uint32_t)v * 17);
    }
  }
}
static void drawStr(FontId fid, const char* s, int x, int by, uint16_t fg) {
  const FontRec* f = FNT[fid];
  if (by - (int)f->asc >= bandY1 || by + ((int)f->line - (int)f->asc) <= bandY0) return;
  int pen = 0;
  for (const char* p = s; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    const GlyphRec* g0 = glyphAt(f, ch, 0);
    if (!g0) { pen += 32; continue; }
    if (ch != ' ') {
      int q = pen & 7, gx, ph;
      if (f->ph) { ph = q >> 1; gx = x + (pen >> 3); } else { ph = 0; gx = x + ((pen + 4) >> 3); }
      const GlyphRec* g = glyphAt(f, ch, ph);
      if (g->w) blit4(&f->bm[g->off], g->w, g->h, gx + g->dx, by + g->dy, fg);
    }
    pen += g0->adv;
  }
}
static inline int anchorX(int x, int w, int al) { return al == AL_C ? x - w / 2 : al == AL_R ? x - w : x; }
static void txt(FontId f, const char* s, int x, int by, uint16_t fg, int al = AL_L) { drawStr(f, s, anchorX(x, textW(f, s), al), by, fg); }
static void txtVU(FontId f, const char* v, FontId uf, const char* u, int x, int by, uint16_t fg, uint16_t ufg, int al) {      // value and a small unit on one baseline
  int w1 = textW(f, v); bool has = u && u[0]; int tot = w1 + (has ? 2 + textW(uf, u) : 0), ox = anchorX(x, tot, al);
  drawStr(f, v, ox, by, fg); if (has) drawStr(uf, u, ox + w1 + 2, by, ufg);
}

// ---------- fixed labels by string id: English in the Poppins fonts, Thai from the Loma bitmaps ----------
struct LabKind { FontId en; uint8_t th, off; };            // th: 0 small / 1 large Thai bitmaps; off: how far callers move a baseline down for Thai (taken off again for English-only strings)
static const LabKind LB_TILE = {F_LS, 0, 3}, LB_ROW = {F_US, 0, 1}, LB_BTN = {F_UM, 0, 1}, LB_BTNM = {F_UM, 0, 1}, LB_SM = {F_LM, 0, 1}, LB_TAB = {F_LM, 0, 2}, LB_TTL = {F_UL, 1, 2};
static bool thai = false;
static inline int tby(int by) { return thai ? by + 3 : by; }
static const ThBmp* thOf(StrId id, int sz) { const ThBmp* t = (sz ? THL : THS) + id; return pgm_read_ptr(&t->d) ? t : nullptr; }
static bool thaiOn(StrId id, int sz) { return thai && thOf(id, sz); }
static int labW(StrId id, LabKind k, int maxW = 0) {
  if (!thaiOn(id, k.th)) return textW(k.en, EN[id]);
  int w = pgm_read_byte(&thOf(id, k.th)->w); return maxW && w > maxW ? maxW : w;
}
static void blit4Scaled(const uint8_t* bm, int w, int h, int sw, int px, int py, uint16_t fg) {     // Thai bitmap squeezed horizontally to sw px (box filter)
  int r0 = max(0, bandY0 - py), r1 = min(h, bandY1 - py);
  for (int r = r0; r < r1; r++) {
    uint16_t* d = rowPtr(py + r);
    for (int dc = 0; dc < sw; dc++) {
      int xx = px + dc; if (xx < 0 || xx >= SCR_W) continue;
      int s0 = dc * w / sw, s1 = ((dc + 1) * w + sw - 1) / sw; if (s1 <= s0) s1 = s0 + 1; if (s1 > w) s1 = w;
      int acc = 0;
      for (int sc = s0; sc < s1; sc++) { int idx = r * w + sc; uint8_t v = pgm_read_byte(&bm[idx >> 1]); acc += (idx & 1) ? (v & 15) : (v >> 4); }
      int a = acc * 17 / (s1 - s0);
      if (a > 0) d[xx] = blendA(d[xx], fg, (uint32_t)(a > 255 ? 255 : a));
    }
  }
}
static void lab(StrId id, LabKind k, int x, int by, uint16_t fg, int al = AL_L, int maxW = 0) {
  if (!thaiOn(id, k.th)) { txt(k.en, EN[id], x, thai ? by - k.off : by, fg, al); return; }
  const ThBmp* t = thOf(id, k.th); int w = pgm_read_byte(&t->w), H = k.th ? THL_H : THS_H, BASE = k.th ? THL_BASE : THS_BASE;
  const uint8_t* bm = (const uint8_t*)pgm_read_ptr(&t->d);
  int sw = (maxW && w > maxW) ? maxW : w, ox = anchorX(x, sw, al);
  if (by - BASE >= bandY1 || by - BASE + H <= bandY0) return;
  if (sw == w) blit4(bm, w, H, ox, by - BASE, fg); else blit4Scaled(bm, w, H, sw, ox, by - BASE, fg);
}
static void fmtv(char* b, size_t n, float v, int dec) { if (v > -0.5f * powf(10, -dec) && v < 0) v = 0; snprintf(b, n, "%.*f", dec, v); }
static void fmtThou(char* b, size_t n, int v) { if (v >= 1000) snprintf(b, n, "%d,%03d", v / 1000, v % 1000); else snprintf(b, n, "%d", v); }
static void fitText(char* d, size_t n, const char* s, FontId f, int maxw) {   // shortens with ".." to fit
  strncpy(d, s, n - 1); d[n - 1] = 0;
  if (textW(f, d) <= maxw) return;
  int L = (int)strlen(d);
  while (L > 1) { L--; char t[48]; snprintf(t, sizeof t, "%.*s..", L, d); if (textW(f, t) <= maxw) { strncpy(d, t, n - 1); d[n - 1] = 0; return; } }
}
static bool hit(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

// ---------- chips, tab bar, headers ----------
static void chipBox(int x, int y, int w, int h, bool on) { rr(x, y, w, h, min(8, h >> 1), on ? T.acc : T.panel, on ? T.acc : T.line); }
static inline int chipBy(int y, int h) { return y + (h >> 1) + (thai ? 5 : 4); }
static void chipId(StrId id, int x, int y, int w, int h, bool on, LabKind k = LB_BTNM) { chipBox(x, y, w, h, on); lab(id, k, x + (w >> 1), chipBy(y, h), on ? T.onacc : T.text, AL_C, w - 6); }
static void chipS(const char* s, int x, int y, int w, int h, bool on, FontId f = F_UM) { chipBox(x, y, w, h, on); txt(f, s, x + (w >> 1), y + (h >> 1) + 4, on ? T.onacc : T.text, AL_C); }
static void chipArrow(int x, int y, int w, int h, int dir, bool on) { chipBox(x, y, w, h, on); chevron(x + (w >> 1), y + (h >> 1), dir, on ? T.onacc : T.text); }
static const int TABX[6] = {6, 58, 109, 161, 213, 264};
static const StrId TABS[6] = {S_DASH, S_DATA, S_GRAPH, S_TRIP, S_CODES, S_SETUP};
static void tabBar(int active) {
  fr(0, 200, 320, 40, T.tabbg); hl(0, 200, 320, T.tabline);
  for (int i = 0; i < 6; i++) {
    int x = TABX[i]; bool on = i == active;
    if (on) { halo(x, 206, 50, 26, 8, T.acc, 3); rr(x, 206, 50, 26, 8, T.panel2, T.acc); }
    lab(TABS[i], LB_TAB, x + 25, thai ? 224 : 222, on ? T.text : T.dim, AL_C, 46);
  }
}
static int tabOf(int x) { for (int i = 0; i < 6; i++) if (x >= TABX[i] - 3 && x < TABX[i] + 53) return i; return -1; }
static void subHeader(StrId title, StrId rightId, bool hasRight) {
  chipBox(9, 4, 64, 20, false); chevron(22, 14, -1, T.text); lab(S_BACK, LB_BTNM, 46, chipBy(4, 20), T.text, AL_C, 44);
  lab(title, LB_TTL, 160, thai ? 22 : 20, T.text, AL_C, 150);
  if (hasRight) chipId(rightId, 247, 4, 64, 20, false);
}
static bool hitBack(int x, int y) { return hit(x, y, 9, 4, 64, 20); }

// ---------- band loop ----------
static uint32_t bandHash[NBAND]; static bool bandHashOk[NBAND];
static void presentBand(int b) {                              // push the band unless it is identical to what is already on the panel
  uint32_t h = 2166136261u; const uint32_t* p = (const uint32_t*)tPix;
  for (int i = 0; i < SCR_W * BAND_H / 2; i++) h = (h ^ p[i]) * 16777619u;
  if (bandHashOk[b] && bandHash[b] == h) return;
  bandHash[b] = h; bandHashOk[b] = true;
  tft.pushImage(0, b * BAND_H, SCR_W, BAND_H, tPix);
}
static void invalidatePanel() { for (int i = 0; i < NBAND; i++) bandHashOk[i] = false; }

static bool vehDiesel() { if (ecuDieselKnown) return ecuDiesel; int m = effModel(); return m >= 0 && (MODELS[m].flags & VF_D); }
static bool vehFromEcu() { return ecuDieselKnown; }           // true when petrol / diesel was read from the car itself
static bool vehHybrid() { int m = effModel(); return (ecuHybrid && !demoMode) || (m >= 0 && (MODELS[m].flags & VF_H)); }
static bool vehBike() { int m = effModel(); return m >= 0 ? (MODELS[m].flags & VF_M) != 0 : (vehModel < 0 && autoBike); }   // a car picked by the owner always wins
static int ringMaxK() { if (vehBike()) { int m = effModel(); return m >= 0 ? constrain((MODELS[m].redline + 9) / 10, 6, 16) : 12; } return vehDiesel() ? 6 : 8; }          // tacho scale in 1000 rpm
static int shiftMax() { return vehBike() ? 16000 : 9000; }
static int vehCc() { int m = effModel(); return m >= 0 ? MODELS[m].cc : (vehBike() ? 150 : 1500); }

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
static float crpMPa(bool& ok) {                       // common-rail pressure from the ECU: PID 23 (diesel / direct injection) or 59; kPa -> MPa, times the trim
  bool o; float k;
  if (crpSrc == 2) k = getVal(0x59, o);
  else { k = getVal(0x23, o); if (!o && crpSrc == 0) k = getVal(0x59, o); }
  ok = o; return k / 1000.0f * crpTrim / 100.0f;
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
    case M_CRP: { float p = crpMPa(ok); return (crpV0 + (CRP_VTOP - crpV0) * clampf(p / crpFs, 0, 1.2f)) / 100.0f; }
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
  if (s == M_CRP) return crpHave();
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
  static const int C0[] = {MI_COOL}, C1[] = {MI_OIL, MI_THR, MI_MAP}, C2[] = {MI_LOAD, MI_THR, MI_PEDAL, MI_TIMING}, C3[] = {MI_INTAKE, MI_AMB},
                   C4[] = {MI_BATT}, C5P[] = {MI_AFR, MI_LAMBDA, MI_MAP, MI_TIMING}, C5D[] = {MI_BOOST, MI_RAIL, MI_MAP, MI_EGT};
  slots[0] = (uint8_t)firstSupported(C0, 1, MI_COOL);
  slots[3] = (uint8_t)firstSupported(C3, 2, MI_INTAKE);
  slots[1] = (uint8_t)firstSupported(C1, 3, MI_OIL);
  slots[4] = (uint8_t)firstSupported(C4, 1, MI_BATT);
  slots[2] = (uint8_t)firstSupported(C2, 4, MI_LOAD);
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
  thai = prefs.getBool("thai", false); warnOn = prefs.getBool("warn", true); parkOn = prefs.getBool("park", true);
  shiftRpm = prefs.getInt("shift", 6500); bright = prefs.getInt("bright", 80); int th = prefs.getInt("theme", 0);
  dashMain = prefs.getUChar("dmain", prefs.getBool("crpm", false) ? 1 : 0); if (dashMain > 2) dashMain = 0; tankL = prefs.getInt("tank", 44); fuelCal = prefs.getInt("fcal", 100); crpFs = prefs.getInt("crpfs2", 180); crpV0 = prefs.getInt("crpv0", 50); crpTrim = prefs.getInt("crptrim", 100); crpSrc = (uint8_t)prefs.getInt("crpsrc", 0);
#ifdef TFT_INVERSION_ON
  invScreen = prefs.getBool("inv", true);                    // ST7789 boards normally need the inversion on
#else
  invScreen = prefs.getBool("inv", false);
#endif
  linkMode = prefs.getUChar("lmode", 0) == 1 ? 1 : 0;
  { String fb = prefs.getString("ftbad", ""); strncpy(ftBadAddr, fb.c_str(), sizeof ftBadAddr - 1); ftBadAddr[sizeof ftBadAddr - 1] = 0; }
  vehModel = prefs.getInt("veh", -1); lastProto = prefs.getInt("proto", 0); userProto = prefs.getInt("uproto", 0); unitL100 = prefs.getBool("unitl", false); slotsCustom = prefs.getBool("slc", false);
  flipScr = prefs.getBool("flip", false); autoBike = prefs.getBool("abike", false); autoModel = prefs.getInt("amodel", -1);
  nvmap = prefs.getInt("nvmap", 0); if (nvmap < 0 || nvmap > 8 || prefs.getBytes("vmap", vmap, sizeof vmap) != sizeof vmap) nvmap = 0;
  String ba = prefs.getString("btaddr", ""), bn = prefs.getString("btname", "");
  int bk = prefs.getInt("btkind", 0), bp = prefs.getInt("btpin", 0); bool bo = prefs.getBool("btpinok", false);
  uint8_t s[6]; if (prefs.getBytes("slots", s, 6) == 6) { bool good = true; for (int i = 0; i < 6; i++) if (s[i] >= NMET) good = false; if (good) memcpy(slots, s, 6); }
  peaksClear(pkA); { Peaks tmp; if (prefs.getBytes("pkall", &tmp, sizeof tmp) == sizeof tmp) pkA = tmp; }
  prefs.end();
  applyTheme(th);
  if (shiftRpm < 3000 || shiftRpm > 9000) shiftRpm = 6500;
  if (tankL < 3 || tankL > 200) tankL = 44;                 // motorcycles carry 4-25 L
  if (fuelCal < 70 || fuelCal > 130) fuelCal = 100;
  if (crpFs < 50 || crpFs > 400) crpFs = 180;
  if (crpV0 < 0 || crpV0 > 200) crpV0 = 50;
  if (crpTrim < 50 || crpTrim > 150) crpTrim = 100;
  if (crpSrc > 2) crpSrc = 0;
  if (vehModel < -1 || vehModel >= NMODEL) vehModel = -1;
  if (autoModel < -1 || autoModel >= NMODEL) autoModel = -1;
  for (int i = 0; i < nvmap; i++) { vmap[i].c[5] = 0; if (vmap[i].m >= NMODEL) vmap[i].m = 0; }
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
  prefs.putBool("thai", thai); prefs.putBool("warn", warnOn); prefs.putBool("park", parkOn); prefs.putUChar("dmain", dashMain);
  prefs.putInt("shift", shiftRpm); prefs.putInt("theme", theme); prefs.putInt("bright", bright); prefs.putBytes("slots", slots, 6); prefs.putBool("slc", slotsCustom);
  prefs.putInt("tank", tankL); prefs.putInt("fcal", fuelCal); prefs.putInt("crpfs2", crpFs); prefs.putInt("crpv0", crpV0); prefs.putInt("crptrim", crpTrim); prefs.putInt("crpsrc", crpSrc); prefs.putBool("inv", invScreen); prefs.putInt("veh", vehModel); prefs.putInt("proto", lastProto); prefs.putInt("uproto", userProto);
  prefs.putBool("unitl", unitL100);
  prefs.putBool("flip", flipScr); prefs.putBool("abike", autoBike); prefs.putInt("amodel", autoModel);
  prefs.putInt("nvmap", nvmap); prefs.putBytes("vmap", vmap, sizeof vmap);
  prefs.putString("btaddr", selAddr); prefs.putString("btname", selName); prefs.putString("ftbad", ftBadAddr); prefs.putUChar("lmode", linkMode);
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
enum Page { PG_SPLASH, PG_DASH, PG_DATA, PG_ALL, PG_GRAPH, PG_TRIP, PG_PEAKS, PG_CODES, PG_STATUS, PG_FREEZE, PG_ECU, PG_SETUP, PG_SETUP2, PG_BT, PG_VEH, PG_PARK, PG_LOG, PG_UPD, PG_CRP, PG_CRPCAL, PG_LISTEN };
static Page scr = PG_SPLASH;
static bool redraw = true;
static void applyFlip() { tft.setRotation(flipScr ? 3 : 1); invalidatePanel(); redraw = true; }      // the picture is turned 180 degrees; every band is sent again
static int gsel = 0, gmode = 0;                           // graph set (0 rpm, 1 temps, 2 afr, 3 batt) and time base (0 = 60 s, 1 = 15 min)
static bool allMode = false;

static void markWant(int pid, int tier) { if (pid > 0 && pid < 256 && want[pid] < tier) want[pid] = (uint8_t)tier; }
static void markSrc(int s, int tier) {
  if (s < 256) { markWant(s, tier); return; }
  switch (s) {
    case M_AFR: case M_LAMBDA: markWant(0x24, tier); markWant(0x44, tier); break;
    case M_BOOST: markWant(0x0B, tier); markWant(0x33, tier); break;
    case M_CRP: if (crpSrc != 2) markWant(0x23, tier); if (crpSrc != 1) markWant(0x59, tier); break;
    case M_FUELRATE: markWant(0x5E, tier); markWant(0x10, tier); markWant(0x0B, tier); markWant(0x0F, tier); markWant(0x62, tier); markWant(0x63, tier); break;
  }
}
static const int ALL_ROWS = 5;
static uint8_t allIdx[NPID]; static int allN = 0, allPg = 0;
static void updateWants() {
  memset(want, 0, sizeof want);
  static const uint8_t BG[] = {0x05, 0x42, 0x5C, 0x5E, 0x10, 0x0B, 0x0F, 0x2F, 0x44, 0x24, 0x33, 0x04, 0x06, 0x62, 0x63};
  for (unsigned i = 0; i < sizeof BG; i++) markWant(BG[i], 1);
  markSrc(M_CRP, 1);                                   // rail pressure: asked slowly in the background so the CRP log keeps filling
  markWant(0x0C, 2); markWant(0x0D, 2);
  markWant(0x5E, 2); markWant(0x10, 2); markWant(0x24, 2); markWant(0x44, 2);      // fuel rate / air flow / lambda feed the trip and consumption figures all the time
  if (scr == PG_DASH) { for (int i = 0; i < 6; i++) markSrc(MET[slots[i]].src, 2); if (dashMain != 2) markSrc(M_CRP, 2); }
  else if (scr == PG_DATA) { for (int i = 0; i < 16; i++) { DTile t; dtileOf(i, t); markSrc(t.src, 2); } }
  else if (scr == PG_ALL) {
    for (int i = 0; i < NPID; i++) markWant(PIDS[i].id, 1);
    for (int r = 0; r < ALL_ROWS; r++) { int k = allPg * ALL_ROWS + r; if (k < allN) markWant(PIDS[allIdx[k]].id, 2); }      // the rows that are on screen
  }
  else if (scr == PG_GRAPH) {
    static const int GS[4][3] = {{0x0C, 0x0D, 0}, {0x05, 0x5C, 0x0F}, {M_AFR, 0x06, 0x07}, {0x42, 0, 0}};
    for (int k = 0; k < 3; k++) if (GS[gsel][k]) markSrc(GS[gsel][k], 2);
  }
  else if (scr == PG_TRIP || scr == PG_PARK) { markSrc(M_FUELRATE, 2); markWant(0x2F, 2); }
  else if (scr == PG_STATUS) { markWant(0x21, 2); markWant(0x30, 2); markWant(0x31, 2); }
  else if (scr == PG_LOG || scr == PG_CRP || scr == PG_CRPCAL) markSrc(M_CRP, 2);
  listenWant = (scr == PG_LISTEN);
}

static const char* protoName(int p) {
  static const char* const N[] = {"", "J1850 PWM", "J1850 VPW", "ISO 9141", "KWP SLOW", "KWP FAST", "CAN 11/500", "CAN 29/500", "CAN 11/250", "CAN 29/250"};
  return p >= 0 && p <= 9 ? N[p] : "";
}
static void carName(char* d, size_t n) {
  d[0] = 0;
  int em = effModel();
  if (em >= 0) {
    const Model& m = MODELS[em]; const char* br = m.brand == 6 ? "HONDA" : BRANDS[m.brand];     // the bike tile is named HONDA BIKE, the car name just HONDA
    if (!strncmp(m.name, br, strlen(br))) snprintf(d, n, "%s", m.name); else snprintf(d, n, "%s %s", br, m.name);
  } else if (demoMode) snprintf(d, n, "MAZDA 2");
  else if (linkMode == 1) snprintf(d, n, "HONDA BIKE");                 // K-line link, no model picked
  else snprintf(d, n, "%s", carBuf);
}
static void goPage(Page p) { scr = p; redraw = true; updateWants(); }
// =====================================================================================
//  Pages, part 1: top bar, dashboard, data, all PIDs, graph, trip, peaks   (layouts, fonts and touch areas are those of the web preview)
// =====================================================================================
static void nav(Page p);
static void saveSoon();
static float gv0(int pid) { bool ok; float v = getVal((uint8_t)pid, ok); return ok ? v : 0; }
static const int DT_X[6] = {9, 9, 9, 228, 228, 228}, DT_Y[6] = {36, 80, 124, 36, 80, 124};      // dashboard tiles 83 x 38
static const int GX[4] = {9, 86, 163, 240}, GY[4] = {56, 92, 128, 164};                         // 4-column tile grid, tiles 71 wide

// ---------- colours of values ----------
static bool tileGood(const Met& m, float v) { (void)m; (void)v; return false; }
static uint16_t valCol(const Met& m, float v) { return v >= m.b ? T.hot : v >= m.w ? T.warn : v < m.lo ? T.warn : T.text; }     // white unless it is high or low
static void clockStr(char* d, size_t n, uint32_t s) { if (s >= 3600) snprintf(d, n, "%u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60)); else snprintf(d, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }

static void valueFit(const char* b, const char* unit, int x, int by, uint16_t col, int al, int maxw) {
  bool has = unit && unit[0]; FontId f = F_NS; bool u = false;
  static const FontId order[3] = {F_NL, F_NM, F_NS};
  for (int i = 0; i < 3; i++) {
    int w = textW(order[i], b);
    if (has && w + 2 + textW(F_LS, unit) <= maxw) { f = order[i]; u = true; break; }
    if (w <= maxw) { f = order[i]; u = false; break; }
  }
  txtVU(f, b, F_LS, u ? unit : "", x, by, col, T.dim, al);
}

// ---------- header row + 14-pill shift strip, shown on every main page ----------
static const int SPX[14] = {11, 32, 54, 75, 97, 118, 140, 161, 183, 204, 225, 247, 268, 290};
static void shiftStrip(float rpm) {
  float r0 = (float)max(1800, shiftRpm - 3000); bool over = rpm >= shiftRpm, blink = over && ((uiNow / 140) & 1);
  for (int i = 0; i < 14; i++) {
    float th = r0 + i * (shiftRpm - r0) / 14; bool lit = rpm >= th && !blink, hot = over || i >= 11;
    uint16_t col = hot ? T.hot : T.acc;
    if (over && blink) { rr(SPX[i], 24, 19, 4, 2, mixRgb(TA.bg, TA.hot, 85)); continue; }
    if (lit) halo(SPX[i], 24, 19, 4, 2, col, 2);
    rr(SPX[i], 24, 19, 4, 2, lit ? col : T.dark);
  }
}
static void topBar(float rpm) {
  bool linkOk = linkState == 2;
  uint16_t dc = linkOk ? T.good : T.warn;
  discA(14, 13, 4.6f, dc, 41); discA(14, 13, 3.6f, dc, 71); disc(14, 13, 2.5f, dc);
  StrId sid = linkState == 2 ? S_CONNECTED : linkState == 1 ? S_CONNECTING : S_SELDEV;
  if (linkState == 2) txt(F_LM, linkMode == 1 ? "K-LINE" : selBle ? "OBD BLE" : "OBD BT", 22, 16, T.dim);
  else lab(sid, LB_SM, 22, thai ? 17 : 16, T.dim);
  const char* pn = linkState == 2 ? (linkMode == 1 ? "HONDA" : protoName(protoNum)) : "";
  if (pn[0]) txt(F_LM, pn, 311, 16, T.dim, AL_R);
  else if (linkState != 2) txt(F_LM, FW_VERSION, 311, 16, T.dim, AL_R);         // the screen's firmware version while there is no link
  char nm[32]; carName(nm, sizeof nm);
  if (nm[0]) { char t[32]; if (textW(F_UM, nm) > 150) fitText(t, sizeof t, nm, F_UM, 150); else strcpy(t, nm); txt(F_UM, t, 160, 16, T.accL, AL_C); }
  else if (!demoMode && linkState == 2) lab(S_UNKNOWN, LB_BTNM, 160, thai ? 17 : 16, T.dim, AL_C);
  else if (!demoMode && linkMode == 0 && linkState == 1 && selName[0]) { char t[32]; fitText(t, sizeof t, selName, F_UM, 150); txt(F_UM, t, 160, 16, T.dim, AL_C); }
  shiftStrip(rpm);
}
// mode chips on the first row of the Data / Trip / Codes pages
struct ModeChip { StrId id; int x, w; };
static void modeChips(const ModeChip* l, int n, int active) { for (int i = 0; i < n; i++) chipId(l[i].id, l[i].x, 32, l[i].w, 20, i == active); }
static int modeTap(const ModeChip* l, int n, int x, int y) { if (y < 32 || y >= 52) return -1; for (int i = 0; i < n; i++) if (x >= l[i].x && x < l[i].x + l[i].w) return i; return -1; }
static const Page TABPG[6] = {PG_DASH, PG_DATA, PG_GRAPH, PG_TRIP, PG_CODES, PG_SETUP};
static bool tabTap(int x, int y, Page cur) {                    // true when the touch was on the tab bar
  if (y < 200) return false;
  int i = tabOf(x); if (i >= 0 && TABPG[i] != cur) nav(TABPG[i]);
  return true;
}

// ---------- dashboard ----------
static void dtile(int i) {
  const Met& m = MET[slots[i]]; int x = DT_X[i], y = DT_Y[i];
  tile(x, y, 83, 38);
  lab(m.lab, LB_TILE, x + 8, thai ? y + 15 : y + 12, T.dim, AL_L, 68);
  bool ok; float f = metricVal(m.src, ok); char b[16] = "--"; uint16_t col = T.dim;
  if (ok) { float v = f * m.mul; fmtv(b, sizeof b, v, m.dec); col = valCol(m, v); }
  valueFit(b, m.unit, x + 8, y + 30, col, AL_L, 67);
}
static const int RCX = 160, RCY = 97, RR = 53;
static void ringDraw(float rpm) {                                 // tacho: track, glow, gradient arc with round caps, inner disc (per pixel, no angle maths)
  float frac = clampf(rpm / (ringMaxK() * 1000.0f), 0, 1); bool over = rpm >= shiftRpm;
  uint16_t glow = over ? T.hot : T.acc;
  const float PI_ = 3.14159265f, s2 = 0.70710678f;
  float S = 1.5f * PI_ * frac + 0.0001f, ae = 0.75f * PI_ + S;
  float u0x = -s2, u0y = s2, uex = cosf(ae), uey = sinf(ae);
  float e0x = RR * u0x, e0y = RR * u0y, e1x = RR * uex, e1y = RR * uey;           // ends of the lit arc (relative to the centre)
  float t0x = -RR * s2, t0y = RR * s2, t1x = RR * s2, t1y = RR * s2;               // ends of the track
  static const float GW[4] = {7.5f, 6.0f, 4.5f, 3.5f};                               // half widths of the glow strokes
  static const uint8_t GA[4] = {13, 18, 26, 36};                                     // alphas .05 .07 .10 .14
  int ya = max(RCY - 62, bandY0), yb = min(RCY + 62, bandY1 - 1);
  for (int py = ya; py <= yb; py++) {
    uint16_t* row = rowPtr(py); int dy = py - RCY;
    for (int px = RCX - 62; px <= RCX + 62; px++) {
      int dx = px - RCX, d2 = dx * dx + dy * dy;
      if (d2 > 62 * 62 || px < 0 || px >= SCR_W) continue;
      uint16_t* p = row + px;
      if (d2 <= 41 * 41) { *p = T.disc; continue; }
      float d = sqrtf((float)d2);
      if (d2 < 45 * 45) {
        float cv = clampf(43.0f - d + 0.5f, 0, 1); *p = blendA(*p, T.disc, (uint32_t)(cv * 255 + 0.5f));
        float c2 = clampf(1.0f - fabsf(d - 42.5f), 0, 1); if (c2 > 0.004f) *p = blendA(*p, T.discLine, (uint32_t)(c2 * 255 + 0.5f));
        continue;
      }
      float ro = fabsf(d - RR), fx = (float)dx, fy = (float)dy;
      // track (full sweep, round caps)
      bool inT = !(dy > abs(dx)); float dT = ro;
      if (!inT) { float a = (fx - t0x) * (fx - t0x) + (fy - t0y) * (fy - t0y), b = (fx - t1x) * (fx - t1x) + (fy - t1y) * (fy - t1y); dT = sqrtf(a < b ? a : b); }
      float cT = clampf(2.5f - dT + 0.5f, 0, 1);
      if (cT > 0.004f) *p = blendA(*p, T.track, (uint32_t)(cT * 255 + 0.5f));
      // lit arc
      float c0 = u0x * fy - u0y * fx, ce = fx * uey - fy * uex;
      bool inL = S <= PI_ ? (c0 >= 0 && ce >= 0) : (c0 >= 0 || ce >= 0); float dL = ro;
      if (!inL) { float a = (fx - e0x) * (fx - e0x) + (fy - e0y) * (fy - e0y), b = (fx - e1x) * (fx - e1x) + (fy - e1y) * (fy - e1y); float m = a < b ? a : b; dL = m > 72.25f ? 99.0f : sqrtf(m); }
      if (dL > 8.0f) continue;
      for (int k = 0; k < 4; k++) { float cv = clampf(GW[k] - dL + 0.5f, 0, 1); if (cv > 0.004f) *p = blendA(*p, glow, (uint32_t)(GA[k] * cv + 0.5f)); }
      float c5 = clampf(2.5f - dL + 0.5f, 0, 1);
      if (c5 > 0.004f) {
        uint16_t lc = over ? T.hot : mixRgb(TA.accD, TA.accL, clampi((px - py + 43) * 256 / 212, 0, 256));
        *p = blendA(*p, lc, (uint32_t)(c5 * 255 + 0.5f));
      }
    }
  }
}
// dash centre + bottom bar show one of three values: 0 speed, 1 CRP volts, 2 AFR (a tap on the ring or the bar steps to the next)
static void dashOf(int m, char* t, size_t n, const char*& unit, float& frac, StrId& lid) {
  if (m == 1) { bool o; float v = metricVal(M_CRP, o); if (o) { int cv = (int)lroundf(clampf(v, 0, 9.99f) * 100); snprintf(t, n, "%d.%02d", cv / 100, cv % 100); } else strcpy(t, "--"); unit = "V"; frac = o ? v / 5 : 0; lid = S_CRPV; }
  else if (m == 2) { bool o; float a = metricVal(M_AFR, o); if (o) fmtv(t, n, clampf(a, 0, 99.9f), 1); else strcpy(t, "--"); unit = ""; frac = o ? (a - 10) / 10 : 0; lid = S_AFR; }
  else { float sp = gv0(0x0D); snprintf(t, n, "%d", (int)sp); unit = "km/h"; frac = sp / 200; lid = S_SPEED; }
}
static void drawDash() {
  float rpm = rpmUi;
  topBar(rpm);
  for (int i = 0; i < 6; i++) dtile(i);
  ringDraw(rpm);
  char b[16]; fmtThou(b, sizeof b, (int)rpm);
  txtVU(F_NS, b, F_LS, "rpm", RCX, 76, T.accL, T.accL, AL_C);
  char ct[12], bt[12]; const char *cu, *bu; float cf, bf; StrId cl, lid;
  dashOf(dashMain, ct, sizeof ct, cu, cf, cl); dashOf((dashMain + 1) % 3, bt, sizeof bt, bu, bf, lid);   // centre value, and the one a tap brings next
  if (dashMain == 0) { txt(F_NXL, ct, RCX, 115, T.text, AL_C); txt(F_LS, "km/h", RCX, 131, T.dim, AL_C); }
  else { txt(F_NXM, ct, RCX, 111, T.accL, AL_C); txt(F_LS, dashMain == 1 ? "CRP volts" : "AFR", RCX, 131, T.dim, AL_C); }
  tile(9, 168, 302, 29);
  int lw = labW(lid, LB_TILE), vx = max(67, 18 + lw + 8);
  lab(lid, LB_TILE, 18, thai ? 188 : 185, T.dim);
  txtVU(F_NL, bt, F_LS, bu, vx, 188, T.text, T.dim, AL_L);
  int bx = max(122, vx + textW(F_NL, bt) + 2 + textW(F_LS, bu) + 10), bl = 302 - bx;
  rr(bx, 181, bl, 3, 1, T.dark);
  int bw = (int)lroundf(bl * clampf(bf, 0, 1)); if (bw >= 2) rr(bx, 181, bw, 3, 1, T.acc);
  tabBar(0);
}
static void tapDash(int x, int y, bool first) {
  if (!first) return;
  if (tabTap(x, y, PG_DASH)) return;
  for (int i = 0; i < 6; i++) if (hit(x, y, DT_X[i], DT_Y[i], 83, 38)) { slots[i] = (uint8_t)nextMetric(slots[i], i); slotsCustom = true; updateWants(); saveSoon(); return; }
  if (hit(x, y, 94, 34, 132, 132) || hit(x, y, 9, 168, 302, 29)) { dashMain = (uint8_t)((dashMain + 1) % 3); updateWants(); saveSoon(); }
}

// ---------- data grid ----------
static const ModeChip DATA_MODES[4] = {{S_MAIN, 9, 44}, {S_ALLPID, 57, 58}, {S_CRPLOG, 119, 64}, {S_AFRLOG, 187, 64}};
static void fuelTag() {                                         // Petrol / Diesel / Hybrid: green = read from the car (ECU), grey = from the chosen model until the car has answered
  StrId id = vehDiesel() ? S_FUELD : vehHybrid() ? S_FUELH : S_FUELP;
  lab(id, LB_TILE, 311, thai ? 48 : 46, vehFromEcu() ? T.good : T.dim, AL_R);
}
static uint8_t logKind = 0;                                     // log page: 0 = CRP volts, 1 = AFR
static void drawData() {
  topBar(rpmUi); modeChips(DATA_MODES, 4, 0); fuelTag();
  for (int i = 0; i < 16; i++) {
    DTile t; dtileOf(i, t); int x = GX[i & 3], y = GY[i >> 2];
    tile(x, y, 71, 33);
    lab(t.lab, LB_TILE, x + 6, thai ? y + 14 : y + 11, T.dim, AL_L, 62);
    bool ok; float f = metricVal(t.src, ok); char b[16] = "--"; uint16_t col = T.dim;
    if (ok) { float v = f * t.mul; fmtv(b, sizeof b, v, t.dec); col = t.mi >= 0 ? valCol(MET[t.mi], v) : T.text; }
    valueFit(b, t.unit, x + 7, y + 28, col, AL_L, 58);
  }
  tabBar(1);
}
static void tapData(int x, int y, bool first) {
  if (!first) return;
  if (tabTap(x, y, PG_DATA)) return;
  int m = modeTap(DATA_MODES, 4, x, y);
  if (m == 1) nav(PG_ALL); else if (m == 2 || m == 3) { logKind = (uint8_t)(m - 2); nav(PG_LOG); }
}

// ---------- data log: CRP volts (diesel) or AFR (petrol) for each 1000 rpm band ----------
static const int LOGN = 8;
struct LogBin { uint32_t n; float sum, mn, mx; };
static LogBin logB[2][LOGN];                                    // [0] CRP volts, [1] AFR
static int logW() { return vehBike() ? 2000 : 1000; }           // rpm per row (bikes rev higher)
static int logBin(float rpm) { return min(LOGN - 1, (int)(rpm / logW())); }
static const int TBN = 7, TBV[TBN] = {0, 5, 10, 20, 35, 50, 75};            // AFR map: throttle rows x 1000 rpm columns
static uint32_t amN[TBN][LOGN]; static float amSum[TBN][LOGN];
static int tpsRow(float t) { int r = 0; for (int i = 0; i < TBN; i++) if (t >= TBV[i]) r = i; return r; }
static void logClear() { memset(amN, 0, sizeof amN); memset(amSum, 0, sizeof amSum); for (auto& s : logB) for (auto& b : s) { b.n = 0; b.sum = 0; b.mn = 1e9f; b.mx = -1e9f; } }
static void logStep() {
  bool ok; float rpm = getVal(0x0C, ok); if (!ok || rpm < 400) return;
  int b = logBin(rpm);
  { bool o1, o2; float t = getVal(0x11, o1), a = metricVal(M_AFR, o2); if (o1 && o2) { int r = tpsRow(t); amN[r][b]++; amSum[r][b] += a; } }
  for (int k = 0; k < 2; k++) { bool o; float v = metricVal(k ? M_AFR : M_CRP, o); if (!o) continue; LogBin& L = logB[k][b]; L.n++; L.sum += v; L.mn = fminf(L.mn, v); L.mx = fmaxf(L.mx, v); }
}
static void afrMap(bool rok, float rpm) {                         // petrol: average AFR for each throttle row and 1000 rpm column
  tile(9, 56, 302, 141);
  bool ot, oa; float tps = getVal(0x11, ot), afr = metricVal(M_AFR, oa);
  int cc = rok && rpm >= 400 ? logBin(rpm) : -1, cr = ot ? tpsRow(tps) : -1; char b[40];
  if (rok) { char t[12]; fmtThou(t, sizeof t, (int)rpm); snprintf(b, sizeof b, "%s rpm   TPS ", t); } else strcpy(b, "-- rpm   TPS ");
  { char t[8]; if (ot) snprintf(t, sizeof t, "%d%%", (int)lroundf(tps)); else strcpy(t, "--"); strcat(b, t); }
  txt(F_US, b, 18, 70, T.text);
  { char t[16]; strcpy(t, "AFR "); if (oa) { char u[8]; fmtv(u, sizeof u, afr, 1); strcat(t, u); } else strcat(t, "--"); txt(F_US, t, 303, 70, oa && afr >= 14.2f && afr <= 15.2f ? T.good : T.text, AL_R); }
  for (int r = 0; r < TBN; r++) {
    int y = 78 + (TBN - 1 - r) * 13; char l[6]; snprintf(l, sizeof l, "%d", TBV[r]);
    txt(F_LS, l, 34, y + 10, T.dim, AL_R);
    for (int c = 0; c < LOGN; c++) {
      int x = 38 + c * 33; uint32_t n = amN[r][c]; float v = n ? amSum[r][c] / n : 0; bool cur = r == cr && c == cc;
      uint16_t fill = !n ? mixRgb(TA.panel, TA.line, 115) : (v >= 13.8f && v <= 15.2f ? mixRgb(TA.panel, TA.good, 128) : mixRgb(TA.line, TA.dim, 77));
      rr(x, y, 32, 12, 3, fill, cur ? T.good : -1);
      if (n) { fmtv(b, sizeof b, v, 1); txt(F_LS, b, x + 16, y + 10, T.text, AL_C); }
      if (cur) disc(x + 3, y + 3, 1.6f, T.good);
    }
  }
  for (int c = 0; c < LOGN; c++) { snprintf(b, sizeof b, "%dK", c * logW() / 1000); txt(F_LS, b, 38 + c * 33 + 16, 183, T.dim, AL_C); }
}
static void drawLog() {
  bool dsl = vehDiesel(); int k = logKind; const LogBin* L = logB[k]; int rows = dsl && !vehBike() ? 6 : 8, dec = k == 0 ? 2 : 1; bool ok; float rpm = getVal(0x0C, ok);
  int cur = ok && rpm >= 400 ? logBin(rpm) : -1;
  topBar(ok ? rpm : 0); modeChips(DATA_MODES, 4, 2 + k); chipId(S_RESET, 255, 32, 56, 20, false);
  if (k == 1 && !dsl) { afrMap(ok, rpm); tabBar(1); return; }        // petrol AFR: map of throttle rows x rpm columns; diesel AFR and every CRP log: table per rpm row
  tile(9, 56, 302, 141);
  lab(S_RPM, LB_TILE, 18, tby(68), T.dim); lab(S_AVG, LB_TILE, 138, tby(68), T.dim, AL_R); lab(S_LOGMIN, LB_TILE, 200, tby(68), T.dim, AL_R); lab(S_GMAX, LB_TILE, 262, tby(68), T.dim, AL_R);
  hl(15, 73, 290, T.sep);
  uint32_t nmax = 1; for (int i = 0; i < rows; i++) nmax = max(nmax, L[i].n);
  for (int i = 0; i < rows; i++) {
    int y = 75 + i * 15; uint32_t n = L[i].n; char b[16], lb[8];
    if (i == cur) rr(12, y, 296, 14, 5, T.panel2, T.acc);
    if (i == LOGN - 1) snprintf(lb, sizeof lb, "%dK+", i * logW() / 1000); else snprintf(lb, sizeof lb, "%d-%dK", i * logW() / 1000, (i + 1) * logW() / 1000);
    txt(F_US, lb, 18, y + 11, i == cur ? T.accL : T.text);
    uint16_t col = n ? T.text : T.dim;
    if (n) fmtv(b, sizeof b, L[i].sum / n, dec); else strcpy(b, "--"); txt(F_US, b, 138, y + 11, col, AL_R);
    if (n) fmtv(b, sizeof b, L[i].mn, dec); else strcpy(b, "--"); txt(F_US, b, 200, y + 11, col, AL_R);
    if (n) fmtv(b, sizeof b, L[i].mx, dec); else strcpy(b, "--"); txt(F_US, b, 262, y + 11, col, AL_R);
    rr(272, y + 5, 32, 4, 1, T.dark); int w = (int)lroundf(32.0f * n / nmax); if (w >= 2) rr(272, y + 5, w, 4, 1, T.acc);
  }
  tabBar(1);
}
static void tapLog(int x, int y, bool first) {
  if (!first) return;
  if (tabTap(x, y, PG_LOG)) return;
  int m = modeTap(DATA_MODES, 4, x, y);
  if (m == 0) nav(PG_DATA); else if (m == 1) nav(PG_ALL);
  else if (m == 2 || m == 3) logKind = (uint8_t)(m - 2);
  else if (hit(x, y, 255, 32, 56, 20)) logClear();
}

// ---------- all PIDs ----------
static void allBuild() { allN = 0; for (int i = 0; i < NPID; i++) if (allMode || demoMode || psup[PIDS[i].id]) allIdx[allN++] = (uint8_t)i; }
static int allPages() { return max(1, (allN + ALL_ROWS - 1) / ALL_ROWS); }
static void drawAll() {
  int pages = allPages(); allPg = clampi(allPg, 0, pages - 1);
  topBar(rpmUi); modeChips(DATA_MODES, 2, 1);
  chipArrow(121, 32, 24, 20, -1, false);
  char pg[12]; snprintf(pg, sizeof pg, "%d/%d", allPg + 1, pages); txt(F_LM, pg, 170, 45, T.dim, AL_C);
  chipArrow(195, 32, 24, 20, 1, false);
  chipId(allMode ? S_ALLPID : S_LIVE, 227, 32, 84, 20, allMode);
  tile(9, 56, 302, 141);
  for (int r = 0; r < ALL_ROWS; r++) {
    int k = allPg * ALL_ROWS + r, y = 57 + r * 28; if (k >= allN) continue;
    const Pid& p = PIDS[allIdx[k]];
    char id[6]; snprintf(id, sizeof id, "%02X", p.id); txt(F_UM, id, 17, y + 17, T.accL);
    char nm[40]; fitText(nm, sizeof nm, p.name, F_US, 168); txt(F_US, nm, 42, y + 17, T.text);
    bool ok; float v = getVal(p.id, ok); char b[16];
    if (ok) fmtv(b, sizeof b, v, p.dec); else strcpy(b, "--");
    const char* un = !ok ? "" : (!strcmp(p.unit, "C") ? DEGC : p.unit);
    txtVU(F_NM, b, F_LS, un, 304, y + 17, ok ? T.text : T.dim, T.dim, AL_R);
    if (r < ALL_ROWS - 1) hl(15, y + 27, 290, T.sep);
  }
  tabBar(1);
}
static void tapAll(int x, int y, bool first) {
  if (!first) return;
  if (tabTap(x, y, PG_DATA)) return;
  int m = modeTap(DATA_MODES, 2, x, y);
  if (m == 0) { nav(PG_DATA); return; }
  if (hit(x, y, 121, 32, 24, 20)) { if (allPg > 0) allPg--; }
  else if (hit(x, y, 195, 32, 24, 20)) { if (allPg + 1 < allPages()) allPg++; }
  else if (hit(x, y, 227, 32, 84, 20)) { allMode = !allMode; allPg = 0; allBuild(); }
}

// ---------- graph ----------
struct GSer { int8_t ch; uint16_t col; float lo, hi; StrId lab; const char* unit; uint8_t dec; };
static const int CP_X = 36, CP_Y = 70, CP_W = 244, CP_H = 76;                         // plot area
static void gDefs(GSer* s, int& n) {
  uint16_t c1 = c565(TA.acc2), cy = T.accL;
  switch (gsel) {
    case 0: n = 2; s[0] = GSer{GC_RPM, c1, 0, (float)(ringMaxK() * 1000), S_GRPM, "", 0}; s[1] = GSer{GC_SPD, cy, 0, 200, S_SPEED, "km/h", 0}; break;
    case 1: n = 3; s[0] = GSer{GC_COOL, c1, 20, 140, S_COOLANT, DEGC, 0}; s[1] = GSer{GC_OIL, T.warn, 20, 140, S_OIL, DEGC, 0}; s[2] = GSer{GC_INT, VIOLET, 20, 140, S_INTAKE, DEGC, 0}; break;
    case 2: n = 2; s[0] = GSer{GC_AFR, c1, 10, 20, S_AFR, "", 1}; s[1] = GSer{GC_STFT, cy, -25, 25, S_STFT, "%", 1}; break;
    default: n = 1; s[0] = GSer{GC_BATT, c1, 10, 16, S_BATT, "V", 1}; break;
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
static int gTh(const GSer* s, float v) { float f = (v - s[0].lo) / (s[0].hi - s[0].lo); return (CP_H - 1) - (int)lroundf(clampf(f, 0, 1) * (CP_H - 1)); }
static void chartRender(const GSer* s, int n) {
  int cnt = gCnt[gmode]; const float dxp = (CP_W - 1) / (float)(GN - 1), x0f = (CP_W - 1) - (cnt - 1) * dxp;
  for (int k = 0; k < n; k++) for (int j = 0; j < GN; j++) {
    int16_t raw = j < cnt ? gH[s[k].ch][gmode][j] : (int16_t)-32768;
    if (raw == -32768) { gY[k][j] = -1; continue; }
    float v = raw / gScale(s[k].ch), f = (v - s[k].lo) / (s[k].hi - s[k].lo);
    gY[k][j] = (int16_t)((CP_H - 1) - (int)lroundf(clampf(f, 0, 1) * (CP_H - 1)));
  }
  int thN = 0, thY[2]; uint16_t thC[2]; char thT[2][24]; thT[0][0] = thT[1][0] = 0;     // dashed limit lines
  if (gsel == 0) { thY[thN] = gTh(s, (float)shiftRpm); thC[thN++] = T.hot; }
  else if (gsel == 1) {
    thY[thN] = gTh(s, 100); thC[thN] = T.warn; snprintf(thT[thN], 24, "%s 100", EN[S_COOLANT]); thN++;
    thY[thN] = gTh(s, 120); thC[thN] = T.hot; snprintf(thT[thN], 24, "%s 120", EN[S_OIL]); thN++;
  }
  else if (gsel == 2) { thY[thN] = gTh(s, 14.7f); thC[thN++] = T.good; }
  else { thY[thN] = gTh(s, 12.0f); thC[thN++] = T.warn; }
  for (int r = 0; r <= 4; r++) hl(CP_X, CP_Y + r * (CP_H - 1) / 4, CP_W, T.grid);
  for (int t = 0; t < thN; t++) for (int x = 0; x < CP_W; x += 8) fr(CP_X + x, CP_Y + thY[t], min(5, CP_W - x), 1, thC[t]);
  if (CP_Y + CP_H <= bandY0 || CP_Y >= bandY1) return;
  for (int k = 0; k < n; k++) {
    for (int X = 0; X < CP_W; X++) {
      int ya, yb;
      if (!gYAt(k, X, x0f, dxp, cnt, ya)) continue;
      if (!gYAt(k, X + 1, x0f, dxp, cnt, yb)) yb = ya;
      int lo = max(0, min(ya, yb)), hi = min(CP_H - 2, max(ya, yb));
      if (k == 0 && hi + 2 < CP_H) frA(CP_X + X, CP_Y + hi + 2, 1, CP_H - hi - 2, T.acc, 34);
      fr(CP_X + X, CP_Y + lo, 1, hi - lo + 2, s[k].col);
    }
  }
  for (int t = 0; t < thN; t++) if (thT[t][0]) txt(F_LS, thT[t], CP_X + CP_W - 6 - textW(F_LS, thT[t]), CP_Y + thY[t] - 3, thC[t]);
}
static void graphAxes(const GSer* s, int n) {
  rr(9, 64, 302, 99, 9, T.chartbg, T.line);
  chartRender(s, n);
  for (int r = 0; r <= 4; r++) {
    int y = CP_Y + r * (CP_H - 1) / 4; char b[12];
    float v = s[0].lo + (s[0].hi - s[0].lo) * (4 - r) / 4;
    if (gsel == 0) { if (v >= 1000 && ((int)v % 1000) == 0) snprintf(b, sizeof b, "%dK", (int)v / 1000); else if (v >= 1000) snprintf(b, sizeof b, "%.1fK", v / 1000); else snprintf(b, sizeof b, "%d", (int)v); }
    else snprintf(b, sizeof b, "%.*f", s[0].dec, v);
    txt(F_LS, b, CP_X - 5, y + 3, T.dim, AL_R);
    if (n >= 2 && gsel != 1) { float v2 = s[1].lo + (s[1].hi - s[1].lo) * (4 - r) / 4; snprintf(b, sizeof b, "%.*f", s[1].dec, v2); txt(F_LS, b, CP_X + CP_W + 5, y + 3, s[1].col, AL_L); }
  }
  static const char* const T60[5] = {"-60s", "-45", "-30", "-15", "now"}, *const T15[5] = {"-15m", "-11", "-8", "-4", "now"};
  for (int q = 0; q < 5; q++) txt(F_LS, gmode ? T15[q] : T60[q], CP_X + q * (CP_W - 1) / 4, 158, T.dim, q == 4 ? AL_R : (q == 0 ? AL_L : AL_C));
}
static void graphReadouts(const GSer* s, int n) {
  int pw = (302 - (n - 1) * 5) / n;
  for (int k = 0; k < n; k++) {
    int x = 9 + k * (pw + 5), y = 32, ch = s[k].ch;
    tile(x, y, pw, 30);
    disc(x + 9, y + 8, 2.4f, s[k].col);
    lab(s[k].lab, LB_TILE, x + 16, thai ? y + 13 : y + 11, T.dim);
    float mx = -1e9f; bool any = false;
    for (int j = 0; j < gCnt[gmode]; j++) { int16_t r = gH[ch][gmode][j]; if (r != -32768) { mx = fmaxf(mx, r / gScale(ch)); any = true; } }
    char v[16] = "--", m[16] = "--";
    if (gOk[ch]) { if (ch == GC_RPM) fmtThou(v, sizeof v, (int)gLast[ch]); else fmtv(v, sizeof v, gLast[ch], s[k].dec); }
    if (any) { if (ch == GC_RPM) fmtThou(m, sizeof m, (int)mx); else fmtv(m, sizeof m, mx, s[k].dec); }
    int mby = thai ? y + 13 : y + 11;
    if (pw >= 140) {
      int mw = textW(F_LS, m), xr = x + pw - 7;
      txt(F_LS, m, xr, mby, T.hot, AL_R);
      tri(xr - mw - 9, y + 10, xr - mw - 5, y + 10, xr - mw - 7, y + 5, T.hot);
      lab(S_GMAX, LB_TILE, xr - mw - 12, mby, T.dim, AL_R);
    } else txt(F_LS, m, x + pw - 7, mby, T.hot, AL_R);
    int vw = pw - 16; bool u = textW(F_NL, v) + textW(F_LS, s[k].unit) + 3 <= vw;
    txtVU(F_NL, v, F_LS, u ? s[k].unit : "", x + 8, y + 27, gOk[ch] ? T.text : T.dim, T.dim, AL_L);
  }
}
static const StrId GBTN[4] = {S_GRPM, S_GTEMPS, S_GAFR, S_GBATT};
static const int GBX[5] = {9, 70, 131, 192, 253};
static void drawGraph() {
  GSer s[3]; int n; gDefs(s, n);
  topBar(rpmUi);
  graphReadouts(s, n); graphAxes(s, n);
  for (int i = 0; i < 4; i++) chipId(GBTN[i], GBX[i], 168, 56, 22, gsel == i);
  chipId(gmode ? S_G15M : S_G60S, GBX[4], 168, 58, 22, false);
  tabBar(2);
}
static void tapGraph(int x, int y, bool first) {
  if (!first) return;
  if (tabTap(x, y, PG_GRAPH)) return;
  if (y < 168 || y >= 190) return;
  for (int i = 0; i < 4; i++) if (hit(x, y, GBX[i], 168, 56, 22)) { gsel = i; updateWants(); return; }
  if (hit(x, y, GBX[4], 168, 58, 22)) gmode ^= 1;
}

// ---------- trip ----------
static const ModeChip TRIP_MODES[2] = {{S_TRIP, 9, 44}, {S_PEAKS, 57, 52}};
static void smallTile(int x, int y, int w, int h, StrId lb, const char* v, const char* unit, uint16_t col) {
  tile(x, y, w, h);
  lab(lb, LB_TILE, x + 6, thai ? y + 13 : y + 10, T.dim, AL_L, w - 10);
  valueFit(v, unit, x + 7, y + h - 5, col, AL_L, w - 14);
}
static void tripBars() {
  float eff[32]; int ne = 0;
  for (int i = max(0, nMin - 29); i < nMin; i++) eff[ne++] = minFuel[i] > 0.0004f ? minDist[i] / minFuel[i] : 0;
  if (curMinF > 0.0004f && ne < 30) eff[ne++] = curMinD / curMinF;
  float al = tripAvgL100(), avg = al > 0 ? 100 / al : 0, mx = 20;
  for (int i = 0; i < ne; i++) mx = fmaxf(mx, eff[i] * 1.15f);
  if (avg > 0) mx = fmaxf(mx, avg * 1.3f);
  const int BASE = 166, BH = 24;
  for (int i = 0; i < ne; i++) {
    int slot = 30 - ne + i, x = (int)(15 + slot * 9.6f), h = clampi((int)lroundf(eff[i] / mx * BH), eff[i] > 0 ? 2 : 0, BH);
    rr(x, BASE - h, 7, h, 2, (avg > 0 && eff[i] >= avg) ? T.good : T.dim);
  }
  if (avg > 0) { int y = BASE - clampi((int)lroundf(avg / mx * BH), 0, BH); for (int x = 14; x < 304; x += 6) hl(x, y, 3, T.good); }
  { char at[12]; if (avg > 0) snprintf(at, sizeof at, "%.1f", avg); else strcpy(at, "--"); uint16_t ac = avg > 0 ? T.good : T.dim;
    txt(F_LS, at, 210, tby(142), ac, AL_R); lab(S_AVG, LB_TILE, 210 - textW(F_LS, at) - 4, tby(142), ac, AL_R); }
  lab(S_LAST30, LB_TILE, 304, tby(142), T.dim, AL_R);
}
static void drawTrip() {
  topBar(rpmUi); modeChips(TRIP_MODES, 2, 0);
  bool have = tFuel > 0.002f || tNow > 0 || tNowKmL > 0; float rg = rangeKm();
  char rt[16]; if (rg >= 0) snprintf(rt, sizeof rt, "%d km", (int)rg); else strcpy(rt, "-- km");
  int rw = textW(F_US, rt);
  txt(F_US, rt, 311, tby(45), T.text, AL_R); lab(S_RANGE, LB_TILE, 311 - rw - 5, tby(45), T.dim, AL_R);
  float nowv = unitL100 ? tNow : tNowKmL; const char* uL = unitL100 ? "L/100km" : "km/L";
  char b[16];
  tile(9, 55, 149, 38); lab(S_NOW, LB_TILE, 17, tby(66), T.dim);
  if (!have || nowv <= 0) strcpy(b, "--"); else fmtv(b, sizeof b, nowv, 1);
  txtVU(F_NHL, b, F_LS, uL, 17, 88, T.text, T.dim, AL_L);
  float al = tripAvgL100(), avgv = al > 0 ? (unitL100 ? al : 100 / al) : 0;
  tile(162, 55, 149, 38); lab(S_TAVG, LB_TILE, 170, tby(66), T.dim);
  if (al > 0) { char t[24]; snprintf(t, sizeof t, "%.1f %s", unitL100 ? 100 / al : al, unitL100 ? "km/L" : "L/100km"); txt(F_LS, t, 303, tby(66), T.dim, AL_R); }
  if (avgv > 0) fmtv(b, sizeof b, avgv, 1); else strcpy(b, "--");
  txtVU(F_NHL, b, F_LS, uL, 170, 88, avgv > 0 ? T.good : T.dim, T.dim, AL_L);
  int mins = (int)(tSec / 60); float avs = tSec > 5 ? tDist / (tSec / 3600) : 0; char c1[16], c2[16], c3[16], c4[16];
  fmtv(c1, sizeof c1, tDist, 1); smallTile(GX[0], 96, 71, 30, S_DIST, c1, "km", T.text);
  snprintf(c2, sizeof c2, "%d:%02d", mins / 60, mins % 60); smallTile(GX[1], 96, 71, 30, S_DTIME, c2, "h", T.text);
  fmtv(c3, sizeof c3, avs, 0); smallTile(GX[2], 96, 71, 30, S_ASPD, c3, "km/h", T.text);
  if (tFuel > 0.002f) fmtv(c4, sizeof c4, tFuel, 2); else strcpy(c4, "--"); smallTile(GX[3], 96, 71, 30, S_FUELUSED, c4, "L", tFuel > 0.002f ? T.text : T.dim);
  tile(9, 130, 302, 42); lab(S_EACHMIN, LB_TILE, 17, tby(142), T.dim);
  tripBars();
  chipId(S_RESET, 9, 176, 146, 20, false); chipId(S_UNITS, 165, 176, 146, 20, false);
  tabBar(3);
}
static void tapTrip(int x, int y, bool first) {
  if (!first) return;
  if (tabTap(x, y, PG_TRIP)) return;
  if (modeTap(TRIP_MODES, 2, x, y) == 1) { nav(PG_PEAKS); return; }
  if (hit(x, y, 9, 176, 146, 20)) tripReset();
  else if (hit(x, y, 165, 176, 146, 20)) { unitL100 = !unitL100; saveSoon(); }
}

// ---------- peaks ----------
static const int PKX[3] = {9, 112, 215}, PKY[3] = {56, 103, 150};
static void drawPeaks() {
  const Peaks& p = pkAllMode ? pkA : pkD;
  topBar(rpmUi); modeChips(TRIP_MODES, 2, 1);
  chipId(pkAllMode ? S_ALLTIME : S_THISDRIVE, 181, 32, 72, 20, pkAllMode); chipId(S_RESET, 257, 32, 54, 20, false);
  struct E { StrId lb; float v; int dec; const char* unit; uint32_t t; bool good, bad; };
  E e[9] = {
    {S_MAXRPM, p.rpm, 0, "", p.tr, false, false}, {S_TOPSPEED, p.spd, 0, "km/h", p.ts, false, false}, {S_BEST0100, p.best, 2, "s", 0, true, false},
    {S_MAXCOOL, p.cool, 0, DEGC, p.tc, false, p.cool >= 108}, {S_MAXOIL, p.oil, 0, DEGC, p.to, false, p.oil >= 130}, {S_MAXLOAD, p.load, 0, "%", p.tl, false, false},
    {S_MAXINT, p.intake, 0, DEGC, p.ti, false, false}, {S_MINBATT, p.minB, 1, "V", p.tmin, false, p.minB < 11.8f && p.minB > 1}, {S_MAXBATT, p.maxB, 1, "V", p.tmax, false, false}};
  for (int i = 0; i < 9; i++) {
    int x = PKX[i % 3], y = PKY[i / 3]; const E& E_ = e[i]; bool has = i == 7 ? p.minB < 90 : E_.v > 0.0001f;
    char v[16] = "--", tm[16] = "";
    if (has) { if (i == 0) fmtThou(v, sizeof v, (int)E_.v); else fmtv(v, sizeof v, E_.v, E_.dec); }
    if (has && !pkAllMode && i != 2) clockStr(tm, sizeof tm, E_.t);
    uint16_t col = !has ? T.dim : E_.bad ? T.hot : E_.good ? T.good : T.text;
    tile(x, y, 96, 44);
    lab(E_.lb, LB_TILE, x + 48, thai ? y + 14 : y + 11, T.dim, AL_C, 88);
    valueFit(v, E_.unit, x + 48, y + 29, col, AL_C, 86);
    if (tm[0]) { int aw = labW(S_ATTIME, LB_TILE), w2 = textW(F_LS, tm), x0 = x + 48 - (aw + 4 + w2) / 2, aby = thai ? y + 41 : y + 39; lab(S_ATTIME, LB_TILE, x0, aby, T.dim); txt(F_LS, tm, x0 + aw + 4, aby, T.dim); }
  }
  tabBar(3);
}
static void tapPeaks(int x, int y, bool first) {
  if (!first) return;
  if (tabTap(x, y, PG_TRIP)) return;
  if (modeTap(TRIP_MODES, 2, x, y) == 0) { nav(PG_TRIP); return; }
  if (hit(x, y, 181, 32, 72, 20)) pkAllMode = !pkAllMode;
  else if (hit(x, y, 257, 32, 54, 20)) { peaksClear(pkAllMode ? pkA : pkD); if (pkAllMode) pkDirty = true; }
}

// =====================================================================================
//  Pages, part 2: codes, monitors, freeze frame, ECU info, setup, more settings, Bluetooth, vehicle, splash, alerts, park
// =====================================================================================
struct DtcD { const char* c; const char* d; };
static const DtcD DTCD[] = {
  {"P0016", "Crank / cam correlation B1 A"},
  {"P0100", "MAF sensor circuit"},
  {"P0101", "MAF range / performance"},
  {"P0102", "MAF circuit low"},
  {"P0103", "MAF circuit high"},
  {"P0105", "MAP sensor circuit"},
  {"P0106", "MAP range / performance"},
  {"P0107", "MAP circuit low"},
  {"P0108", "MAP circuit high"},
  {"P0110", "Intake air temp circuit"},
  {"P0112", "Intake air temp low"},
  {"P0113", "Intake air temp high"},
  {"P0115", "Coolant temp circuit"},
  {"P0116", "Coolant temp range"},
  {"P0117", "Coolant temp low"},
  {"P0118", "Coolant temp high"},
  {"P0120", "Throttle sensor A circuit"},
  {"P0121", "Throttle sensor A range"},
  {"P0122", "Throttle sensor A low"},
  {"P0123", "Throttle sensor A high"},
  {"P0125", "Coolant too cold for closed loop"},
  {"P0128", "Coolant below thermostat temp"},
  {"P0130", "O2 sensor circuit B1 S1"},
  {"P0131", "O2 sensor low voltage B1 S1"},
  {"P0132", "O2 sensor high voltage B1 S1"},
  {"P0133", "O2 sensor slow response B1 S1"},
  {"P0134", "O2 sensor no activity B1 S1"},
  {"P0135", "O2 heater circuit B1 S1"},
  {"P0136", "O2 sensor circuit B1 S2"},
  {"P0137", "O2 sensor low voltage B1 S2"},
  {"P0138", "O2 sensor high voltage B1 S2"},
  {"P0139", "O2 sensor slow response B1 S2"},
  {"P0141", "O2 heater circuit B1 S2"},
  {"P0171", "System too lean bank 1"},
  {"P0172", "System too rich bank 1"},
  {"P0174", "System too lean bank 2"},
  {"P0175", "System too rich bank 2"},
  {"P0190", "Fuel rail pressure sensor"},
  {"P0191", "Fuel rail pressure range"},
  {"P0192", "Fuel rail pressure low"},
  {"P0193", "Fuel rail pressure high"},
  {"P0201", "Injector circuit cyl 1"},
  {"P0202", "Injector circuit cyl 2"},
  {"P0203", "Injector circuit cyl 3"},
  {"P0204", "Injector circuit cyl 4"},
  {"P0217", "Engine overtemperature"},
  {"P0219", "Engine overspeed"},
  {"P0230", "Fuel pump primary circuit"},
  {"P0234", "Turbo overboost"},
  {"P0235", "Turbo boost sensor A"},
  {"P0237", "Turbo boost sensor low"},
  {"P0238", "Turbo boost sensor high"},
  {"P0299", "Turbo underboost"},
  {"P0300", "Random / multiple cylinder misfire"},
  {"P0301", "Cylinder 1 misfire"},
  {"P0302", "Cylinder 2 misfire"},
  {"P0303", "Cylinder 3 misfire"},
  {"P0304", "Cylinder 4 misfire"},
  {"P0325", "Knock sensor 1 circuit"},
  {"P0335", "Crankshaft sensor A circuit"},
  {"P0336", "Crankshaft sensor A range"},
  {"P0340", "Camshaft sensor A circuit"},
  {"P0341", "Camshaft sensor A range"},
  {"P0351", "Ignition coil A circuit"},
  {"P0352", "Ignition coil B circuit"},
  {"P0353", "Ignition coil C circuit"},
  {"P0354", "Ignition coil D circuit"},
  {"P0400", "EGR flow"},
  {"P0401", "EGR flow insufficient"},
  {"P0402", "EGR flow excessive"},
  {"P0403", "EGR circuit"},
  {"P0420", "Catalyst efficiency below limit B1"},
  {"P0430", "Catalyst efficiency below limit B2"},
  {"P0440", "EVAP system"},
  {"P0441", "EVAP purge flow"},
  {"P0442", "EVAP small leak"},
  {"P0443", "EVAP purge valve circuit"},
  {"P0446", "EVAP vent control"},
  {"P0455", "EVAP large leak"},
  {"P0456", "EVAP very small leak"},
  {"P0480", "Cooling fan 1 control"},
  {"P0500", "Vehicle speed sensor"},
  {"P0505", "Idle control system"},
  {"P0506", "Idle RPM lower than expected"},
  {"P0507", "Idle RPM higher than expected"},
  {"P0562", "System voltage low"},
  {"P0563", "System voltage high"},
  {"P0571", "Brake switch circuit"},
  {"P0600", "Serial communication link"},
  {"P0601", "ECU memory checksum"},
  {"P0606", "ECU processor fault"},
  {"P0627", "Fuel pump control circuit"},
  {"P0638", "Throttle actuator range B1"},
  {"P0650", "MIL control circuit"},
  {"P0700", "Transmission control system"},
  {"P2002", "DPF efficiency below limit B1"},
  {"P2101", "Throttle actuator motor circuit"},
  {"P2135", "Throttle sensor A / B correlation"},
  {"P2452", "DPF pressure sensor A"},
  {"P2459", "DPF regeneration frequency"},
  {"P20EE", "SCR NOx catalyst efficiency"},
  {"U0100", "Lost communication with ECM / PCM"},
  {"U0101", "Lost communication with TCM"},
  {"U0121", "Lost communication with ABS"},
  {"U0140", "Lost communication with body control"},
  {"U0155", "Lost communication with cluster"},
};
static const char* dtcDesc(const char* c) {
  for (const DtcD& e : DTCD) if (!strcmp(e.c, c)) return e.d;
  switch (c[0]) {
    case 'P': return c[1] == '0' || c[1] == '2' ? "Generic powertrain code" : "Maker specific powertrain code";
    case 'B': return "Body code";
    case 'C': return "Chassis code";
    case 'U': return "Network communication code";
  }
  return "";
}


// ---------- fault codes ----------
static const int CODE_ROWS = 4;
static int codePg = 0; static bool demoCleared = false, clearArmed = false; static uint32_t clearAt = 0;
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
static const ModeChip CODE_MODES[4] = {{S_CODES, 9, 48}, {S_MONITORS, 61, 63}, {S_FREEZE, 128, 65}, {S_ECUINFO, 197, 68}};
static const Page CODE_PAGE[4] = {PG_CODES, PG_STATUS, PG_FREEZE, PG_ECU};
static const int READ_X = 269;
static void codeChips(int active) { modeChips(CODE_MODES, 4, active); chipId(S_READ, READ_X, 32, 42, 20, false); }
static void codesBanner() {
  tile(9, 55, 302, 19, 8);
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
    disc(x0 + 3, 64, 3, col); lab(single, LB_TILE, x0 + 10, tby(68), col);
  } else {
    char nb[8]; snprintf(nb, sizeof nb, "%d", n);
    int wn = textW(F_LS, nb), wc = labW(S_CODESW, LB_TILE), wl = labW(lamp, LB_TILE), tot = 10 + wn + 4 + wc + 14 + wl, x0 = 160 - tot / 2;
    disc(x0 + 3, 64, 3, col);
    txt(F_LS, nb, x0 + 10, tby(68), col); lab(S_CODESW, LB_TILE, x0 + 14 + wn, tby(68), col);
    txt(F_LS, "-", x0 + 10 + wn + 4 + wc + 4, tby(68), T.dim); lab(lamp, LB_TILE, x0 + tot - wl, tby(68), col);
  }
}
static void codeRow(int r) {
  int y = 77 + r * 23, k = codePg * CODE_ROWS + r;
  if (k >= codeCount || k >= 24) return;
  const DtcRec& d = codeList[k]; uint16_t c = d.pend ? T.warn : RED_AL;
  tile(9, y, 302, 21, 7); rr(15, y + 5, 3, 11, 1, c);
  txt(F_UL, d.code, 24, y + 15, T.text);
  int x = 24 + textW(F_UL, d.code) + 9;
  char ds[56]; fitText(ds, sizeof ds, dtcDesc(d.code), F_LS, 250 - x); txt(F_LS, ds, x, y + 13, T.text);
  lab(d.pend ? S_PENDING : S_STORED, LB_TILE, 303, tby(y + 13), c, AL_R);
}
static void drawCodes() {
  if (clearArmed && uiNow - clearAt > 4000) clearArmed = false;
  int pages = max(1, (min((int)codeCount, 24) + CODE_ROWS - 1) / CODE_ROWS); codePg = clampi(codePg, 0, pages - 1);
  topBar(rpmUi); codeChips(0);
  codesBanner();
  if (clearArmed) {
    rrG(9, 77, 302, 89, 9, TA.panel2, TA.panel, T.warn);
    lab(S_CLRNOTE1, LB_TILE, 160, tby(114), T.text, AL_C); lab(S_CLRNOTE2, LB_TILE, 160, tby(134), T.warn, AL_C);
  } else for (int r = 0; r < CODE_ROWS; r++) codeRow(r);
  chipId(S_CLEAR, 9, 172, 166, 22, codeCount > 0 || mil || clearArmed);
  chipArrow(181, 172, 62, 22, -1, false); chipArrow(249, 172, 62, 22, 1, false);
  tabBar(4);
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
static void statusEnter() { if (demoMode) { monRaw = 0x0007E504u; monValid = true; monVer = monVer + 1; } }
static void monitorsDraw() {
  uint32_t raw = monRaw; bool valid = monValid || demoMode, dsl = valid && ((raw >> 16) & 8);
  int nm = dsl ? 9 : 8, rows = (nm + 1) / 2, pitch = rows > 4 ? 18 : 22, rh = pitch - 2, sup = 0, rdy = 0;
  for (int i = 0; i < nm; i++) {
    const MonDef& m = dsl ? MON_D[i] : MON_P[i]; int s = valid ? monState(m, raw) : 0;
    int x = 9 + (i & 1) * 154, y = 77 + (i >> 1) * pitch;
    if (s) { sup++; if (s == 1) rdy++; }
    uint16_t c = s == 1 ? T.good : s == 2 ? T.warn : T.dim; int mid = y + (rh >> 1);
    tile(x, y, 148, rh, 7);
    disc(x + 10, mid, 3, c);
    lab(m.lb, LB_ROW, x + 19, mid + (thai ? 5 : 4), s ? T.text : T.dim);
    lab(s == 0 ? S_NA : s == 1 ? S_READY : S_NOTREADY, LB_TILE, x + 142, mid + (thai ? 5 : 4), c, AL_R);
  }
  bool all = valid && sup > 0 && rdy == sup; uint16_t c = !valid ? T.dim : all ? T.good : T.warn;
  tile(9, 55, 302, 19, 8);
  if (!valid) lab(S_READING, LB_TILE, 160, tby(68), T.dim, AL_C);
  else if (all) lab(S_ALLREADY, LB_TILE, 160, tby(68), c, AL_C);
  else { char nb[16]; snprintf(nb, sizeof nb, "%d/%d", rdy, sup); int wn = textW(F_LS, nb), wl = labW(S_READYOF, LB_TILE), x0 = 160 - (wn + 5 + wl) / 2; txt(F_LS, nb, x0, tby(68), c); lab(S_READYOF, LB_TILE, x0 + wn + 5, tby(68), c); }
  int sy = 77 + rows * pitch + 2, sh = 197 - sy;
  tile(9, sy, 302, sh, 9);
  static const struct { StrId lb; uint8_t pid; const char* unit; } ST3[3] = {{S_SINCECLR, 0x31, "km"}, {S_WARMUPS, 0x30, ""}, {S_LAMPFOR, 0x21, "km"}};
  for (int i = 0; i < 3; i++) {
    int cx = 60 + i * 100; bool ok; float v = getVal(ST3[i].pid, ok); char b[14]; if (ok) fmtv(b, sizeof b, v, 0); else strcpy(b, "--");
    lab(ST3[i].lb, LB_TILE, cx, tby(sy + 11), T.dim, AL_C);
    txtVU(F_NM, b, F_LS, ST3[i].unit, cx, sy + sh - 5, ok ? T.text : T.dim, T.dim, AL_C);
  }
}
static void drawStatus() { topBar(rpmUi); codeChips(1); monitorsDraw(); tabBar(4); }

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
static void drawFreeze() {
  topBar(rpmUi); codeChips(2);
  tile(9, 56, 302, 28, 9);
  if (frzState == 2 && frzCode[0]) {
    txt(F_UL, frzCode, 19, 75, T.text);
    int x = 19 + textW(F_UL, frzCode) + 10;
    char ds[56]; fitText(ds, sizeof ds, dtcDesc(frzCode), F_LS, 304 - x); txt(F_LS, ds, x, 67, T.text);
    lab(S_COND, LB_TILE, x, tby(78), T.dim);
  } else if (frzState == 1) lab(S_READING, LB_TILE, 160, tby(73), T.warn, AL_C);
  else lab(S_NOFREEZE, LB_TILE, 160, tby(73), T.dim, AL_C);
  for (int i = 0; i < 12; i++) {
    int x = GX[i & 3], y = 88 + (i >> 2) * 35; const Met& m = MET[FZM[i]];
    tile(x, y, 71, 32);
    lab(m.lab, LB_TILE, x + 35, thai ? y + 14 : y + 11, T.dim, AL_C, 64);
    bool has = frzState == 2 && frzOk[i]; char b[16];
    if (has) fmtv(b, sizeof b, frzVal[i] * m.mul, m.dec); else strcpy(b, "--");
    valueFit(b, m.unit, x + 35, y + 27, has ? T.text : T.dim, AL_C, 62);
  }
  tabBar(4);
}

// ---------- ECU info ----------
static void ecuRead() {
  if (demoMode) { strcpy(vinBuf, "JETOBD2DEMO000000"); strcpy(ecuName, "DEMO-ECM"); strcpy(calId, "DEMOCAL01"); ecuInfoVer = ecuInfoVer + 1; return; }
  if (linkState == 2) req = 4;
}
static void ecuBox(int y, StrId lb, const char* v) {
  tile(9, y, 302, 36);
  lab(lb, LB_TILE, 18, tby(y + 13), T.dim);
  char t[32]; FontId f = F_UL; strncpy(t, v[0] ? v : "--", sizeof t - 1); t[sizeof t - 1] = 0;
  if (textW(f, t) > 284) f = F_UM;
  if (textW(f, t) > 284) { char t2[32]; fitText(t2, sizeof t2, t, f, 284); strcpy(t, t2); }
  txt(f, t, 18, y + 30, v[0] ? T.text : T.dim);
}
static void hexBytes(char* d, size_t cap, const uint8_t* b, int n) { size_t k = 0; d[0] = 0; for (int i = 0; i < n && k + 3 < cap; i++) k += snprintf(d + k, cap - k, i ? " %02X" : "%02X", b[i]); }
static void drawListen() {
  subHeader(S_LISTEN, S_COUNT, false);
  uint8_t mode = listenMode; char b[48];
  chipS("ID 202", 9, 30, 146, 22, mode == 0); chipId(S_LSALL, 165, 30, 146, 22, mode == 1);
  tile(9, 58, 302, 50);
  lab(S_LSRATE, LB_TILE, 18, thai ? 75 : 73, T.dim);
  snprintf(b, sizeof b, "%u", (unsigned)lsRate);
  txtVU(F_NL, b, F_LS, "/s", 18, 100, T.text, T.dim, AL_L);
  lab(S_LSTIM, LB_TILE, 303, thai ? 75 : 73, T.dim, AL_R);
  txt(F_UM, (ftState == 1 || ftState == 2) ? "ATAT2  ATST19" : "ATAT1  ATST32", 303, 98, T.text, AL_R);
  tile(9, 112, 302, 96);
  uint32_t el = uiNow - lsStart; uint32_t tot = lsTotal; bool have = tot > 0;
  if (mode == 0) {
    lab(S_LSLAST, LB_TILE, 18, thai ? 129 : 127, T.dim);
    lab(S_LSDEC, LB_TILE, 303, thai ? 129 : 127, T.dim, AL_R, 190);
    if (lsLast.n && lsLast.dn) { hexBytes(b, sizeof b, lsLast.d, lsLast.dn); txt(F_UL, b, 18, 156, T.text); } else txt(F_UL, "--", 18, 156, T.dim);
    if (lsLast.n && lsLast.dn >= 5) {
      const uint8_t* d = lsLast.d;
      int rpm = ((d[0] << 8) | d[1]) / 4, spd = ((d[2] << 8) | d[3]) / 100;
      snprintf(b, sizeof b, "RPM %d   km/h %d   B4 %d", rpm, spd, d[4]); txt(F_UM, b, 18, 190, T.accL);
    } else txt(F_UM, "RPM --   km/h --   B4 --", 18, 190, T.dim);
  } else {
    lab(S_LSIDS, LB_TILE, 18, thai ? 129 : 127, T.dim);
    snprintf(b, sizeof b, "%u", (unsigned)lsDistinct); txt(F_UM, b, 303, 130, T.text, AL_R);
    for (int i = 0; i < 5; i++) {
      int by = 145 + i * 14;
      if (i < lsTopN) {
        snprintf(b, sizeof b, "%03X", lsTop[i].id); txt(F_UM, b, 18, by, T.accL);
        snprintf(b, sizeof b, "%u/s", (unsigned)lsTop[i].n); txt(F_UM, b, 100, by, T.dim, AL_R);
        hexBytes(b, sizeof b, lsTop[i].d, lsTop[i].dn); txt(F_UM, b, 114, by, T.text);
      } else if (i == 0) txt(F_UM, "--", 18, by, T.dim);
    }
  }
  StrId st = S_LSWAIT; uint16_t col = T.dim;
  if (!demoMode && linkState != 2) { st = S_CRPNOLINK; col = T.warn; }
  else if (!demoMode && listenSt == 4) { st = S_LSCAN; col = T.warn; }
  else if (have && lsBuf) { st = S_LSFULL; col = T.warn; }
  else if (have) { st = S_LSOK; col = T.good; }
  else if (el > 2500) { st = S_LSNONE; col = T.warn; }
  tile(9, 212, 302, 24, 8);
  lab(st, LB_ROW, 160, thai ? 228 : 226, col, AL_C, 290);
}
static void tapListen(int x, int y, bool first) {
  if (!first) return;
  if (hitBack(x, y)) { nav(PG_ECU); return; }
  int m = hit(x, y, 9, 30, 146, 22) ? 0 : hit(x, y, 165, 30, 146, 22) ? 1 : -1;
  if (m >= 0 && m != listenMode) { listenMode = (uint8_t)m; listenGen = listenGen + 1; lsTotal = 0; lsRate = 0; lsTopN = 0; lsDistinct = 0; lsBuf = 0; lsLast.n = 0; lsStart = millis(); }
}
static void drawEcu() {
  topBar(rpmUi); codeChips(3);
  ecuBox(56, S_VIN, vinBuf); ecuBox(96, S_ECUNAME, ecuName); ecuBox(136, S_CALID, calId);
  tile(9, 175, 198, 22, 8);
  lab(S_PROTO, LB_TILE, 18, tby(189), T.dim);
  chipId(S_LISTEN, 215, 175, 96, 22, false);
  char pn[32]; if (linkMode == 1) { if (obdRpmMs && linkState == 2) snprintf(pn, sizeof pn, "HONDA  %u ms", (unsigned)obdRpmMs); else strcpy(pn, "HONDA K-LINE"); } else if (!protoNum) strcpy(pn, "--"); else if (obdRpmMs && linkState == 2) snprintf(pn, sizeof pn, "%s  %u ms", protoName(protoNum), (unsigned)obdRpmMs); else snprintf(pn, sizeof pn, "%s", protoName(protoNum));
  txt(F_UM, pn, 199, thai ? 192 : 189, T.text, AL_R);
  tabBar(4);
}
// the four code pages share one row of chips (and its Read chip); returns true when the touch was handled
static bool codeTouch(int x, int y, Page cur) {
  if (tabTap(x, y, PG_CODES)) return true;
  if (y < 32 || y >= 52) return false;
  int i = modeTap(CODE_MODES, 4, x, y);
  if (i >= 0) { if (CODE_PAGE[i] != cur) nav(CODE_PAGE[i]); return true; }
  if (hit(x, y, READ_X, 32, 42, 20)) {
    switch (cur) { case PG_CODES: codesRead(); break; case PG_STATUS: statusEnter(); break; case PG_FREEZE: freezeRead(); break; case PG_ECU: ecuRead(); break; default: break; }
    return true;
  }
  return false;
}
static void tapCodes(int x, int y, bool first) {
  if (!first) return;
  if (codeTouch(x, y, PG_CODES)) return;
  if (hit(x, y, 9, 172, 166, 22)) { if (!clearArmed) { clearArmed = true; clearAt = millis(); } else codesClear(); }
  else if (hit(x, y, 181, 172, 62, 22)) { if (codePg > 0) codePg--; }
  else if (hit(x, y, 249, 172, 62, 22)) codePg++;
}

// ---------- setup ----------
static const int SU_Y0 = 64, SU_H = 27, SU_N = 4;
static void rowLabel(int y, int h, StrId mainL, StrId sub, bool hasSub) {
  hl(9, y + h - 1, 302, T.sep);
  int o = (h - 28) / 2;                                              // rows taller than 28 px keep their text centred
  if (hasSub) { lab(mainL, LB_ROW, 14, y + 11 + o, T.text); lab(sub, LB_TILE, 14, y + h - (thai ? 3 : 5) - o, T.dim); }
  else lab(mainL, LB_ROW, 14, y + (h >> 1) + (thai ? 5 : 4), T.text);
}
static void stepBtns(int y, int h, const char* val, const char* unit) {
  chipS("-", 164, y + 4, 30, h - 9, false); chipS("+", 281, y + 4, 30, h - 9, false);
  txtVU(F_NL, val, F_UM, unit, 237, y + (h >> 1) + 6, T.text, T.dim, AL_C);
}
static void setupRow(int i) {
  int y = SU_Y0 + i * SU_H; char b[16];
  switch (i) {
    case 0: rowLabel(y, SU_H, S_LANG, S_COUNT, false); chipS("EN", 164, y + 4, 70, SU_H - 9, !thai); chipId(S_THAI, 238, y + 4, 73, SU_H - 9, thai); break;
    case 1: rowLabel(y, SU_H, S_SHIFT, S_SHIFTD, true); fmtThou(b, sizeof b, shiftRpm); stepBtns(y, SU_H, b, ""); break;
    case 2: rowLabel(y, SU_H, S_COLORS, S_COLTIP, true); chipId(S_STD, 164, y + 4, 70, SU_H - 9, !invScreen); chipId(S_INV, 238, y + 4, 73, SU_H - 9, invScreen); break;
    case 3: rowLabel(y, SU_H, S_FLIP, S_FLIPD, true); chipId(S_NORMAL, 164, y + 4, 70, SU_H - 9, !flipScr); chipId(S_FLIPPED, 238, y + 4, 73, SU_H - 9, flipScr); break;
  }
}
static void themeCard(int i) {
  int x = 9 + i * 154, y = 32; bool sel = theme == i; const Theme& th = THEMES[i];
  if (sel) halo(x, y, 148, 28, 9, T.acc, 3);
  if (sel) rr(x, y, 148, 28, 9, T.panel2, T.acc); else rrG(x, y, 148, 28, 9, TA.panel2, TA.panel, T.line);
  discA(x + 17, y + 14, 8, c565(th.acc), 71); disc(x + 17, y + 14, 6.4f, c565(th.bg)); disc(x + 17, y + 14, 3.4f, c565(th.acc));
  lab(i ? S_THRED : S_THBLUE, LB_BTNM, x + 32, y + (thai ? 19 : 18), sel ? T.text : T.dim);
  lab(sel ? S_SELECTED : S_TAPUSE, LB_TILE, x + 142, y + (thai ? 20 : 17), sel ? T.accL : T.dim, AL_R);
}
static void drawSetup() {
  topBar(rpmUi);
  themeCard(0); themeCard(1);
  for (int i = 0; i < SU_N; i++) setupRow(i);
  chipId(S_BTNAME, 9, 177, 71, 20, false); chipId(S_VEHICLE, 86, 177, 71, 20, false); chipId(S_MORE, 163, 177, 71, 20, false); chipId(S_UPDTITLE, 240, 177, 71, 20, false);
  tabBar(5);
}
static bool vmapPut(const char* key, int m) {          // true when something changed
  for (int i = 0; i < nvmap; i++) if (!strcmp(vmap[i].c, key)) { if (vmap[i].m == m) return false; vmap[i].m = (uint8_t)m; return true; }
  int i = nvmap < 8 ? nvmap++ : (vmapNext++ & 7);
  memcpy(vmap[i].c, key, 6); vmap[i].m = (uint8_t)m; return true;
}
static void vinApply() {                               // a VIN came in: motorcycle? which model did the owner pick for this code last time?
  if (strlen(vinBuf) < 11) return;
  bool bike = vinBrand == 6; int mdl = -1; bool learned = false;
  char key[6] = ""; if (bike) { memcpy(key, vinBuf + 3, 5); key[5] = 0; }
  if (bike && vehModel >= 0 && MODELS[vehModel].brand == 6) learned = vmapPut(key, vehModel);      // the owner picked this bike's model: remember it for this VIN code
  else if (bike && vehModel < 0) for (int i = 0; i < nvmap; i++) if (!strcmp(vmap[i].c, key)) mdl = vmap[i].m;
  if (mdl >= NMODEL) mdl = -1;
  if (bike != autoBike || mdl != autoModel) {
    bool was = autoBike; autoBike = bike; autoModel = mdl;
    if (vehModel < 0) {
      if (mdl >= 0) { tankL = MODELS[mdl].tank; shiftRpm = max(3000, MODELS[mdl].redline * 100 - 300); }
      else if (bike) { if (tankL == 44) tankL = 6; if (shiftRpm == 6500) shiftRpm = 8500; }         // first time on a bike: scooter-sized defaults (still adjustable in SETUP)
      else if (was) { if (tankL < 20) tankL = 44; if (shiftRpm > 8000) shiftRpm = 6500; }           // back on a car: a bike's tank / shift values must not stay
    }
    autoSlots(); logClear(); redraw = true; learned = true;
  }
  if (learned) savePrefs();
}
static void applyVehicle(int idx) {                 // idx = -1: automatic (from the VIN)
  vehModel = idx; autoModel = -1; vinNew = vinBuf[0] != 0;     // the VIN is looked at again (remembers a bike model / goes back to automatic)
  if (idx >= 0) { tankL = MODELS[idx].tank; shiftRpm = max(3000, MODELS[idx].redline * 100 - 300); }
  carBuf[0] = 0; slotsCustom = false; autoSlots(); logClear();
  savePrefs(); nav(PG_SETUP);
}
static void stepVal(int& v, int lo, int hi, int dir, int st) { v = constrain(v + dir * st, lo, hi); }
static void tapSetup(int x, int y, bool first) {
  if (first) {
    if (tabTap(x, y, PG_SETUP)) return;
    for (int i = 0; i < NTHEME; i++) if (hit(x, y, 9 + i * 154, 32, 148, 28)) { if (theme != i) { applyTheme(i); savePrefs(); } return; }
    if (hit(x, y, 9, 177, 71, 20)) { nav(PG_BT); return; }
    if (hit(x, y, 86, 177, 71, 20)) { nav(PG_VEH); return; }
    if (hit(x, y, 163, 177, 71, 20)) { nav(PG_SETUP2); return; }
    if (hit(x, y, 240, 177, 71, 20)) { nav(PG_UPD); return; }
  }
  for (int i = 0; i < SU_N; i++) {
    int ry = SU_Y0 + i * SU_H + 4, rh = SU_H - 9;
    if (y < ry || y >= ry + rh) continue;
    if (i == 0 && first) {
      if (hit(x, y, 164, ry, 70, rh) && thai) { thai = false; savePrefs(); }
      else if (hit(x, y, 238, ry, 73, rh) && !thai) { thai = true; savePrefs(); }
    } else if (i == 1) {
      if (hit(x, y, 164, ry, 30, rh)) { stepVal(shiftRpm, 3000, shiftMax(), -1, 100); saveSoon(); }
      else if (hit(x, y, 281, ry, 30, rh)) { stepVal(shiftRpm, 3000, shiftMax(), 1, 100); saveSoon(); }
    } else if (i == 2 && first) {
      if (hit(x, y, 164, ry, 70, rh) && invScreen) { invScreen = false; applyInvert(); savePrefs(); }
      else if (hit(x, y, 238, ry, 73, rh) && !invScreen) { invScreen = true; applyInvert(); savePrefs(); }
    } else if (i == 3 && first) {
      if (hit(x, y, 164, ry, 70, rh) && flipScr) { flipScr = false; applyFlip(); savePrefs(); }
      else if (hit(x, y, 238, ry, 73, rh) && !flipScr) { flipScr = true; applyFlip(); savePrefs(); }
    }
  }
}

// ---------- more settings (setup 2) ----------
static const int S2_Y0 = 32, S2_H = 29;
static uint32_t uprotoAt = 0;
static void setup2Row(int i) {
  int y = S2_Y0 + i * S2_H, h = S2_H; char b[16];
  switch (i) {
    case 0: rowLabel(y, h, S_ALERTS, S_ALERTSD, true); chipId(S_ON, 164, y + 4, 70, h - 9, warnOn); chipId(S_OFF, 238, y + 4, 73, h - 9, !warnOn); break;
    case 1: rowLabel(y, h, S_PARKSCR, S_PARKSCRD, true); chipId(S_ON, 164, y + 4, 70, h - 9, parkOn); chipId(S_OFF, 238, y + 4, 73, h - 9, !parkOn); break;
    case 2: rowLabel(y, h, S_BRIGHT, S_COUNT, false); snprintf(b, sizeof b, "%d", bright); stepBtns(y, h, b, "%"); break;
    case 3: rowLabel(y, h, S_TANK, S_TANKD, true); snprintf(b, sizeof b, "%d", tankL); stepBtns(y, h, b, "L"); break;
    case 4: rowLabel(y, h, S_FUELCAL, S_FUELCALD, true); snprintf(b, sizeof b, "%d", fuelCal); stepBtns(y, h, b, "%"); break;
    case 5:
      rowLabel(y, h, S_PROTO, S_PROTOD, true);
      chipS("-", 164, y + 4, 30, h - 9, false); chipS("+", 281, y + 4, 30, h - 9, false);
      if (userProto == 0) lab(S_AUTO, LB_BTNM, 237, y + (h >> 1) + (thai ? 6 : 5), T.text, AL_C); else txt(F_UM, protoName(userProto), 237, y + (h >> 1) + 5, T.text, AL_C);
      break;
  }
}
static void drawSetup2() {
  subHeader(S_SETUP2, S_COUNT, false);
  txt(F_LM, FW_VERSION, 311, 18, T.dim, AL_R);
  for (int i = 0; i < 6; i++) setup2Row(i);
  chipId(S_TESTALERT, 9, 210, 96, 22, false); chipId(S_RESETPK, 112, 210, 96, 22, false); chipId(S_CRPCAL, 215, 210, 96, 22, false);
}
static void startTestAlert();
static void tapSetup2(int x, int y, bool first) {
  if (first) {
    if (hitBack(x, y)) { savePrefs(); nav(PG_SETUP); return; }
    if (hit(x, y, 9, 210, 96, 22)) { startTestAlert(); return; }
    if (hit(x, y, 112, 210, 96, 22)) { peaksClear(pkA); peaksClear(pkD); pkDirty = true; savePrefs(); return; }
    if (hit(x, y, 215, 210, 96, 22)) { nav(PG_CRP); return; }
  }
  for (int i = 0; i < 6; i++) {
    int ry = S2_Y0 + i * S2_H + 4, rh = S2_H - 9;
    if (y < ry || y >= ry + rh) continue;
    bool minus = hit(x, y, 164, ry, 30, rh), plus = hit(x, y, 281, ry, 30, rh), on = hit(x, y, 164, ry, 70, rh), off = hit(x, y, 238, ry, 73, rh);
    if (i == 0 && first && (on || off)) { warnOn = on; saveSoon(); }
    else if (i == 1 && first && (on || off)) { parkOn = on; saveSoon(); }
    else if (i == 2 && (minus || plus)) { stepVal(bright, 10, 100, plus ? 1 : -1, 10); applyBright(); saveSoon(); }
    else if (i == 3 && (minus || plus)) { stepVal(tankL, 3, 200, plus ? 1 : -1, 1); saveSoon(); }
    else if (i == 4 && (minus || plus)) { stepVal(fuelCal, 70, 130, plus ? 1 : -1, 1); saveSoon(); }
    else if (i == 5 && first && (minus || plus)) { userProto = (userProto + (plus ? 1 : 9)) % 10; uprotoAt = millis(); saveSoon(); }
  }
}

// ---------- CRP sensor: the rail pressure the ECU reports, and the volts it is shown as ----------
static void drawCrpCal() {
  subHeader(S_CRPCAL, S_COUNT, false);
  txt(F_LM, FW_VERSION, 311, 18, T.dim, AL_R);
  bool ok; float p = clampf(crpMPa(ok), 0, 999); char b[40];
  if (ok) { crpMn = fminf(crpMn, p); crpMx = fmaxf(crpMx, p); }
  tile(9, 30, 302, 50);
  lab(S_CRPRAIL, LB_TILE, 18, thai ? 47 : 45, T.dim);
  if (ok) { int d = (int)lroundf(p * 10); snprintf(b, sizeof b, "%d.%d", d / 10, d % 10); } else strcpy(b, "--");
  txtVU(F_NL, b, F_LS, "MPa", 18, 72, T.text, T.dim, AL_L);
  lab(S_CRPRANGE, LB_TILE, 303, thai ? 47 : 45, T.dim, AL_R);
  if (crpMn <= crpMx) { int lo = (int)lroundf(crpMn * 10), hi = (int)lroundf(crpMx * 10); snprintf(b, sizeof b, "%d.%d - %d.%d", lo / 10, lo % 10, hi / 10, hi % 10); } else strcpy(b, "-- - --");
  txt(F_UM, b, 303, 70, T.text, AL_R);
  tile(9, 84, 302, 50);
  lab(S_CRPV, LB_TILE, 18, thai ? 101 : 99, T.dim);
  bool ov; float v = metricVal(M_CRP, ov);
  if (ov) { int cv = (int)lroundf(clampf(v, 0, 99.99f) * 100); snprintf(b, sizeof b, "%d.%02d", cv / 100, cv % 100); } else strcpy(b, "--");
  txtVU(F_NL, b, F_LS, "V", 18, 126, T.accL, T.dim, AL_L);
  uint8_t cp = crpPid(); txt(F_UM, cp == 0x23 ? "PID 23" : cp == 0x59 ? "PID 59" : "", 303, 124, T.dim, AL_R);
  snprintf(b, sizeof b, "%d.%02d - 4.50 V = 0 - %d MPa", crpV0 / 100, crpV0 % 100, crpFs);      // the line the volts follow
  txt(F_UM, b, 160, 156, T.dim, AL_C);
  StrId st = S_CRPMOVE; uint16_t col = T.good;
  if (!demoMode && linkState != 2) { st = S_CRPNOLINK; col = T.warn; }
  else if (!demoMode && !crpHave()) { st = S_CRPNOPID; col = T.warn; }
  else if (!ok) { st = S_CRPWAIT; col = T.dim; }
  tile(9, 171, 302, 32, 8);
  lab(st, LB_ROW, 160, thai ? 192 : 190, col, AL_C, 290);
  chipId(S_CRPRESET, 9, 210, 146, 22, false); chipId(S_CRPCALB, 165, 210, 146, 22, false);
}
static void tapCrpCal(int x, int y, bool first) {
  if (!first) return;
  if (hitBack(x, y)) { savePrefs(); nav(PG_SETUP2); return; }
  if (hit(x, y, 9, 210, 146, 22)) { crpMn = 1e9f; crpMx = -1e9f; return; }
  if (hit(x, y, 165, 210, 146, 22)) { nav(PG_CRPCAL); return; }
}

// ---------- CRP calibration: the two-point line from pressure to sensor volts, a trim, and the PID ----------
static const int CC_Y0 = 32, CC_H = 29;
static void crpCalRow(int i) {
  int y = CC_Y0 + i * CC_H, h = CC_H; char b[16];
  switch (i) {
    case 0: rowLabel(y, h, S_CRPFS, S_CRPFSD, true); snprintf(b, sizeof b, "%d", crpFs); stepBtns(y, h, b, "MPa"); break;
    case 1: rowLabel(y, h, S_CRPV0, S_CRPV0D, true); snprintf(b, sizeof b, "%d.%02d", crpV0 / 100, crpV0 % 100); stepBtns(y, h, b, "V"); break;
    case 2: rowLabel(y, h, S_CRPMETER, S_CRPMETERD, true); snprintf(b, sizeof b, "%d.%02d", crpMeter / 100, crpMeter % 100); stepBtns(y, h, b, "V"); break;
    case 3: rowLabel(y, h, S_CRPTRIM, S_CRPTRIMD, true); snprintf(b, sizeof b, "%d", crpTrim); stepBtns(y, h, b, "%"); break;
    case 4:
      rowLabel(y, h, S_CRPPID, S_CRPPIDD, true);
      chipS("-", 164, y + 4, 30, h - 9, false); chipS("+", 281, y + 4, 30, h - 9, false);
      if (crpSrc == 0) lab(S_AUTO, LB_BTNM, 237, y + (h >> 1) + (thai ? 6 : 5), T.text, AL_C); else txt(F_UM, crpSrc == 1 ? "0x23" : "0x59", 237, y + (h >> 1) + 5, T.text, AL_C);
      break;
  }
}
static void drawCrpCalib() {
  subHeader(S_CRPCALT, S_COUNT, false);
  txt(F_LM, FW_VERSION, 311, 18, T.dim, AL_R);
  for (int i = 0; i < 5; i++) crpCalRow(i);
  bool o1, o2; float p = clampf(crpMPa(o1), 0, 999), v = metricVal(M_CRP, o2); char b[40];
  tile(9, 181, 302, 25, 8);                                                 // what the settings give right now, or the answer to Match meter
  if (crpMsg && millis() - crpMsgAt < 2500) lab(crpMsg == 1 ? S_CRPAPPLIED : S_CRPNEEDP, LB_ROW, 160, thai ? 198 : 197, crpMsg == 1 ? T.good : T.warn, AL_C, 290);
  else {
    if (o1 && o2) { int d = (int)lroundf(p * 10), cv = (int)lroundf(clampf(v, 0, 99.99f) * 100); snprintf(b, sizeof b, "%d.%d MPa = %d.%02d V", d / 10, d % 10, cv / 100, cv % 100); }
    else strcpy(b, "-- MPa = -- V");
    txt(F_UM, b, 160, 198, T.accL, AL_C);
  }
  chipId(S_CRPDEF, 9, 210, 146, 22, false); chipId(S_CRPMATCH, 165, 210, 146, 22, false);
}
static void tapCrpCalib(int x, int y, bool first) {
  if (first) {
    if (hitBack(x, y)) { savePrefs(); nav(PG_CRP); return; }
    if (hit(x, y, 9, 210, 146, 22)) { crpFs = 180; crpV0 = 50; crpTrim = 100; crpSrc = 0; updateWants(); saveSoon(); return; }
    if (hit(x, y, 165, 210, 146, 22)) {                                       // the owner's meter says crpMeter at this very moment: pick the full scale that gives that volts at the pressure now
      bool o; float p = crpMPa(o);
      if (!o || p < 5 || crpMeter <= crpV0 + 5) crpMsg = 2;
      else { crpFs = clampi((int)lroundf((CRP_VTOP - crpV0) * p / (crpMeter - crpV0)), 50, 400); crpMsg = 1; saveSoon(); }
      crpMsgAt = millis(); return;
    }
  }
  for (int i = 0; i < 5; i++) {
    int ry = CC_Y0 + i * CC_H + 4, rh = CC_H - 9;
    if (y < ry || y >= ry + rh) continue;
    bool minus = hit(x, y, 164, ry, 30, rh), plus = hit(x, y, 281, ry, 30, rh); int d = plus ? 1 : -1;
    if (!minus && !plus) break;
    if (i == 0) stepVal(crpFs, 50, 400, d, 10);
    else if (i == 1) stepVal(crpV0, 0, 200, d, 5);
    else if (i == 2) stepVal(crpMeter, 50, 450, d, 1);
    else if (i == 3) stepVal(crpTrim, 50, 150, d, 1);
    else if (first) { crpSrc = (uint8_t)((crpSrc + (plus ? 1 : 2)) % 3); updateWants(); }
    saveSoon();
    break;
  }
}

// ---------- Bluetooth ----------
static BtDev btView[MAXBT]; static int nBtView = 0, btOff = 0;
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
  halo(9, 32, 196, 26, 9, T.acc, 2);
  rr(9, 32, 196, 26, 9, T.panel2, T.acc);
  if (selAddr[0]) {
    char t[32]; fitText(t, sizeof t, selName[0] ? selName : selAddr, F_UM, 108); txt(F_UM, t, 19, 49, T.text);
    StrId s = linkState == 2 ? S_CONNECTED : S_CONNECTING; uint16_t c = linkState == 2 ? T.good : T.warn;
    lab(s, LB_TILE, 198, tby(48), c, AL_R);
  } else lab(S_NOSEL, LB_BTNM, 107, 49, T.dim, AL_C);
  chipId(S_KLHONDA, 211, 32, 100, 26, false);                      // switch to the Honda K-line board
}
static void btRow(int r) {
  int y = 76 + r * 28, i = btOff + r;
  if (i >= nBtView) return;
  const BtDev& d = btView[i]; bool sel = d.ble == selBle && !strcmp(d.addr, selAddr);
  if (sel) rr(9, y, 302, 26, 8, T.panel2, T.acc); else rrG(9, y, 302, 26, 8, TA.panel2, TA.panel, T.line);
  int x = 19;
  if (d.obd) { rr(16, y + 6, 30, 14, 5, T.acc); txt(F_LS, "OBD", 31, y + 16, T.onacc, AL_C); x = 54; }
  const char* nm = d.name[0] ? d.name : d.addr; char t[32]; fitText(t, sizeof t, nm, F_UM, (d.ble ? 176 : 196) - x);
  txt(F_UM, t, x, y + 18, d.name[0] ? (sel ? T.text : T.accL) : T.dim);
  if (d.ble) { rr(194, y + 6, 28, 14, 5, T.panel, T.accL); txt(F_LS, "BLE", 208, y + 16, T.accL, AL_C); }
  if (sel) { ln(228, y + 13, 232, y + 17, T.good); ln(229, y + 13, 233, y + 17, T.good); ln(233, y + 17, 241, y + 7, T.good); ln(234, y + 17, 242, y + 7, T.good); }
  int lv = !d.rssi ? 0 : d.rssi >= -55 ? 4 : d.rssi >= -67 ? 3 : d.rssi >= -78 ? 2 : 1;
  for (int k = 0; k < 4; k++) { int h = 4 + k * 3; rr(268 + k * 8, y + 22 - h, 5, h, 1, k < lv ? T.accL : T.dark); }
}
static void linkModeSet(uint8_t m) {                                 // Bluetooth ELM327 <-> Honda K-line board
  if (m == linkMode) return;
  linkMode = m;
  carBuf[0] = 0; vinBuf[0] = 0; memset(psup, 0, sizeof psup); engineRan = false; klT11n = 0; klIdN = 0; klOk = 0; klBad = 0; klRate = 0;
  if (vehModel < 0) {                                              // no model picked: a K-line link is a Honda bike, Bluetooth is whatever the VIN says
    autoModel = -1;
    if (m == 1) { autoBike = true; if (tankL == 44) tankL = 6; if (shiftRpm == 6500) shiftRpm = 8500; }
    else { autoBike = false; if (tankL < 20) tankL = 44; if (shiftRpm > 8000) shiftRpm = 6500; }
  }
  autoSlots(); logClear();
  linkState = (m == 1 || selAddr[0]) ? 1 : 3;
  scanStop = true; reconnectReq = true;
  savePrefs(); updateWants(); redraw = true;
}
static void drawKline() {
  subHeader(S_KLINE, S_KLBT, true);
  char b[48]; bool up = klSt == 2;
  halo(9, 32, 302, 26, 9, T.acc, 2);
  rr(9, 32, 302, 26, 9, T.panel2, T.acc);
  txt(F_UM, "HONDA K-LINE  10400", 19, 49, T.text);
  lab(up ? S_CONNECTED : S_CONNECTING, LB_TILE, 303, tby(48), up ? T.good : T.warn, AL_R);
  tile(9, 64, 302, 54);
  lab(S_KLTABLE, LB_TILE, 18, tby(77), T.dim);
  snprintf(b, sizeof b, "%u/s", (unsigned)klRate); txt(F_UM, up ? b : "--", 303, 78, T.text, AL_R);
  uint8_t n = klT11n; uint8_t t[24]; memcpy(t, klT11, sizeof t);
  if (n >= 14 && up) {
    hexBytes(b, sizeof b, t, n > 9 ? 9 : n); txt(F_UM, b, 18, 96, T.text);
    if (n > 9) { hexBytes(b, sizeof b, t + 9, n > 18 ? 9 : n - 9); txt(F_UM, b, 18, 110, T.text); }
  } else txt(F_UM, "-- -- -- -- -- -- -- -- --", 18, 96, T.dim);
  tile(9, 122, 302, 56);
  KlLive v; bool have = up && n >= 14 && klDecode(t, n, v);
  if (have) {
    snprintf(b, sizeof b, "RPM %d   KMH %d   TPS %d%%   MAP %d", v.rpm, v.spd, (int)lroundf(fminf(v.tps, 100.0f)), v.map); txt(F_UM, b, 18, 141, T.accL);
    int bv = (int)lroundf(v.batt * 10); snprintf(b, sizeof b, "ECT %d   IAT %d   BATT %d.%dV", v.ect, v.iat, bv / 10, bv % 10); txt(F_UM, b, 18, 157, T.text);
    if (v.injOk || v.ignOk) {
      int ij = (int)lroundf(v.inj * 100), ig = (int)lroundf(v.ign * 10);
      snprintf(b, sizeof b, "INJ %d.%02d ms   IGN %s%d.%d", ij / 100, ij % 100, ig < 0 ? "-" : "", abs(ig) / 10, abs(ig) % 10); txt(F_UM, b, 18, 173, T.text);
    }
  } else { txt(F_UM, "RPM --   KMH --   TPS --   MAP --", 18, 141, T.dim); txt(F_UM, "ECT --   IAT --   BATT --", 18, 157, T.dim); }
  tile(9, 182, 302, 24, 8);
  lab(S_KLECU, LB_TILE, 18, tby(197), T.dim);
  if (klIdN) { uint8_t id[10]; memcpy(id, klId, sizeof id); hexBytes(b, sizeof b, id, klIdN > 8 ? 8 : klIdN); txt(F_UM, b, 303, 198, T.text, AL_R); } else txt(F_UM, "--", 303, 198, T.dim, AL_R);
  StrId st = S_KLWAIT; uint16_t col = T.dim;
  if (klSt == 2 && klRate > 0) { st = S_KLOKMSG; col = T.good; } else if (klSt == 3) { st = S_KLNONE; col = T.warn; }
  tile(9, 210, 302, 26, 8);
  lab(st, LB_ROW, 160, thai ? 227 : 226, col, AL_C, 290);
}
static void tapKline(int x, int y, bool first) {
  if (!first) return;
  if (hitBack(x, y)) { nav(PG_SETUP); return; }
  if (hit(x, y, 247, 4, 64, 20)) linkModeSet(0);
}
static void drawBt() {
  if (linkMode == 1) { drawKline(); return; }
  subHeader(S_BTDEV, S_SCAN, true);
  btSelRow();
  int n = nBtView, st = scanState;
  lab(S_NEARBY, LB_TILE, 11, tby(70), T.dim);
  if (st == 1) lab(S_SCANNING, LB_TILE, 309, tby(70), T.warn, AL_R);
  else if (n > 0) { char c[8]; snprintf(c, sizeof c, "%d", n); int w = labW(S_FOUND, LB_TILE), cw = textW(F_LS, c); txt(F_LS, c, 309 - w - 4 - cw, tby(70), T.accL); lab(S_FOUND, LB_TILE, 309, tby(70), T.dim, AL_R); }
  for (int r = 0; r < 4; r++) btRow(r);
  if (n == 0) { StrId m = st == 1 ? S_SCANNING : st == 2 ? S_NOTFOUND : st == 3 ? S_SCANFAIL : S_PRESSSCAN; lab(m, LB_BTNM, 160, 134, st == 3 ? T.hot : T.dim, AL_C); }
  chipArrow(9, 210, 50, 22, -1, btOff > 0); chipArrow(65, 210, 50, 22, 1, btOff + 4 < n);
  lab(S_TAPPICK, LB_TILE, 215, tby(225), T.dim, AL_C, 190);
}
static void btPick(int i) {                  // the user tapped a device: remember it, switch to OBD mode and connect
  BtDev d = btView[i];
  portENTER_CRITICAL(&btMux);
  bool same = !strcmp(d.addr, selAddr);                         // tapping the adapter that is already chosen keeps what we learned about its PIN
  memcpy(selAddr, d.addr, sizeof selAddr); strcpy(selName, d.name[0] ? d.name : d.addr);
  selBle = d.ble; selAType = d.atype; bleTry = 0; selGen = selGen + 1;
  if (!same) { pinIdx = 0; pinOk = false; }
  portEXIT_CRITICAL(&btMux);
  carBuf[0] = 0; vinBuf[0] = 0; linkState = 1; memset(psup, 0, sizeof psup); engineRan = false;
  scanStop = true; reconnectReq = true;
  savePrefs(); updateWants();
}
static void tapBt(int x, int y, bool first) {
  if (linkMode == 1) { tapKline(x, y, first); return; }
  if (!first) return;
  if (hitBack(x, y)) { nav(PG_SETUP); return; }
  if (hit(x, y, 211, 32, 100, 26)) { linkModeSet(1); return; }
  if (hit(x, y, 247, 4, 64, 20)) { btStartScan(); return; }
  if (hit(x, y, 9, 210, 50, 22)) { if (btOff > 0) btOff = max(0, btOff - 4); return; }
  if (hit(x, y, 65, 210, 50, 22)) { if (btOff + 4 < nBtView) btOff += 4; return; }
  for (int r = 0; r < 4; r++) if (hit(x, y, 9, 76 + r * 28, 302, 26) && btOff + r < nBtView) { btPick(btOff + r); return; }
}

// ---------- vehicle ----------
static int vehStep = 0, vehBrandSel = 0, vehPg = 0;
static const int VBX[2] = {9, 161};
static const int VPER = 12;                                          // models per page
static int brandCount(int b) { int n = 0; for (int i = 0; i < NMODEL; i++) if (MODELS[i].brand == b) n++; return n; }
static int brandPages(int b) { return max(1, (brandCount(b) + VPER - 1) / VPER); }
static void drawVeh() {
  subHeader(S_VEHICLE, S_COUNT, false);
  if (vehStep == 0) {
    for (int i = 0; i < NBRAND; i++) {
      int x = VBX[i & 1], y = 34 + (i >> 1) * 40; bool cur = vehModel >= 0 && MODELS[vehModel].brand == i;
      if (cur) halo(x, y, 150, 36, 10, T.acc, 3);
      if (cur) rr(x, y, 150, 36, 10, T.panel2, T.acc); else rrG(x, y, 150, 36, 10, TA.panel2, TA.panel, T.line);
      if (i == 6) { txt(F_UL, BRANDS[i], x + 75, y + 17, cur ? T.text : T.accL, AL_C); lab(S_BIKENOTE, LB_TILE, x + 75, y + (thai ? 32 : 30), T.dim, AL_C); }   // motorcycles: OBD-II bikes only
      else txt(F_UL, BRANDS[i], x + 75, y + 24, cur ? T.text : T.accL, AL_C);
    }
    chipId(S_AUTOVIN, VBX[1], 34 + 3 * 40, 150, 36, vehModel < 0);          // the 8th tile
    lab(S_PICKBRAND, LB_TILE, 160, tby(214), T.dim, AL_C);
  } else {
    int pages = brandPages(vehBrandSel); vehPg = constrain(vehPg, 0, pages - 1);
    int r = 0, skip = vehPg * VPER;
    for (int i = 0; i < NMODEL && r < VPER; i++) {
      if (MODELS[i].brand != vehBrandSel) continue;
      if (skip > 0) { skip--; continue; }
      int x = VBX[r & 1], y = 31 + (r >> 1) * 29; bool cur = i == vehModel;
      chipBox(x, y, 150, 26, cur);
      char t[32]; fitText(t, sizeof t, MODELS[i].name, F_UM, 136); txt(F_UM, t, x + 75, y + 18, cur ? T.onacc : T.text, AL_C);
      r++;
    }
    if (pages == 1) { chipArrow(9, 210, 50, 22, -1, false); chipId(S_AUTOVIN, 65, 210, 246, 22, vehModel < 0); }
    else {
      char pg[12]; snprintf(pg, sizeof pg, "%d/%d", vehPg + 1, pages);
      chipArrow(9, 210, 44, 22, -1, false); chipS("<", 57, 210, 34, 22, false); chipS(">", 95, 210, 34, 22, false);
      txt(F_UM, pg, 159, 226, T.dim, AL_C); chipId(S_AUTOVIN, 189, 210, 122, 22, vehModel < 0);
    }
  }
}
static int vehModelAt(int slot) {            // slot-th model on the shown page of the chosen brand
  int r = 0, want = vehPg * VPER + slot;
  for (int i = 0; i < NMODEL; i++) { if (MODELS[i].brand != vehBrandSel) continue; if (r == want) return i; r++; }
  return -1;
}
static void tapVeh(int x, int y, bool first) {
  if (!first) return;
  if (hitBack(x, y)) { nav(PG_SETUP); return; }
  if (vehStep == 0) {
    for (int i = 0; i < NBRAND; i++) if (hit(x, y, VBX[i & 1], 34 + (i >> 1) * 40, 150, 36)) {
      vehBrandSel = i; vehStep = 1; vehPg = 0;
      if (vehModel >= 0 && MODELS[vehModel].brand == i) { int r = 0; for (int k = 0; k < vehModel; k++) if (MODELS[k].brand == i) r++; vehPg = r / VPER; }   // open on the page of the current model
      return;
    }
    if (hit(x, y, VBX[1], 34 + 3 * 40, 150, 36)) applyVehicle(-1);
  } else {
    int pages = brandPages(vehBrandSel);
    if (pages == 1) {
      if (hit(x, y, 9, 210, 50, 22)) { vehStep = 0; return; }
      if (hit(x, y, 65, 210, 246, 22)) { applyVehicle(-1); return; }
    } else {
      if (hit(x, y, 9, 210, 44, 22)) { vehStep = 0; return; }
      if (hit(x, y, 57, 210, 34, 22)) { vehPg = (vehPg + pages - 1) % pages; return; }
      if (hit(x, y, 95, 210, 34, 22)) { vehPg = (vehPg + 1) % pages; return; }
      if (hit(x, y, 189, 210, 122, 22)) { applyVehicle(-1); return; }
    }
    for (int r = 0; r < VPER; r++) if (hit(x, y, VBX[r & 1], 31 + (r >> 1) * 29, 150, 26)) { int m = vehModelAt(r); if (m >= 0) applyVehicle(m); return; }
  }
}

// ---------- splash (logo) ----------
static uint32_t splashAt = 0;
static uint32_t splashMs() { return firstRun ? 7000u : 2300u; }
static void drawLogo() {                                       // 200 x 200 artwork at (60, 2)
  int y0 = max(2, bandY0), y1 = min(2 + LOGO_H, bandY1);
  for (int y = y0; y < y1; y++) {
    uint16_t* d = rowPtr(y) + 60; const uint16_t* s = LOGO_PX + (y - 2) * LOGO_W;
    for (int i = 0; i < LOGO_W; i++) d[i] = pgm_read_word(&s[i]);
  }
}
static void drawSplash() {
  fillBand(0);
  drawLogo();
  if (firstRun) {                                  // the language is not chosen yet: show the tip in Thai and in English
    bool t0 = thai;
    thai = true;  lab(S_FIRSTTIP, LB_BTNM, 160, 222, T.warn, AL_C);
    thai = false; lab(S_FIRSTTIP, LB_TILE, 160, 236, T.dim, AL_C);
    thai = t0;
  } else {
    char t[40]; snprintf(t, sizeof t, "%s  %s", EN[S_SPLASHHINT], FW_VERSION);
    txt(F_UM, t, 160, 226, T.dim, AL_C);
  }
  int px = (int)min<uint32_t>(100, (uiNow - splashAt) * 100 / splashMs());
  rr(110, 207, 100, 3, 1, T.dark);
  if (px >= 3) { halo(110, 207, px, 3, 1, T.acc, 2); rr(110, 207, px, 3, 1, T.acc); }
}

// ---------- alerts ----------
enum { AK_NONE = 0, AK_COOL, AK_OIL, AK_BATT, AK_MIL, AK_N };
static int alertOn = AK_NONE; static bool alertArmed[AK_N] = {true, true, true, true, true}; static uint32_t alertSnooze[AK_N]; static bool alertTest = false; static uint32_t alertTestAt = 0;
static int testNext = 0;
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
static void ringStroke(float cx, float cy, float R, float wd, uint16_t col) {
  int y0 = max((int)floorf(cy - R - wd - 1), bandY0), y1 = min((int)ceilf(cy + R + wd + 1), bandY1 - 1);
  for (int py = y0; py <= y1; py++) for (int px = (int)floorf(cx - R - wd - 1); px <= (int)ceilf(cx + R + wd + 1); px++) {
    if (px < 0 || px >= SCR_W) continue;
    float dx = px - cx, dy = py - cy, cv = clampf(wd * 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - R) + 0.5f, 0, 1);
    if (cv > 0.004f) { uint16_t* p = rowPtr(py) + px; *p = blendA(*p, col, (uint32_t)(cv * 255 + 0.5f)); }
  }
}
static void drawAlert() {
  int k = alertOn;
  fillBand(ALBG);
  halo(20, 26, 280, 188, 14, RED_AL, 4);
  rr(20, 26, 280, 188, 14, ALCARD);
  rrBorder(20, 26, 280, 188, 14, 1.5f, RED_AL);
  if (k == AK_MIL) lab(S_CHECKENG, LB_BTN, 160, thai ? 58 : 56, RED_AL, AL_C);
  else {
    StrId sid = k == AK_COOL ? S_COOLANT : k == AK_OIL ? S_OIL : S_BATT;
    int w1 = labW(S_WARNT, LB_BTN), w2 = labW(sid, LB_BTN), x0 = 160 - (w1 + 14 + w2) / 2;
    lab(S_WARNT, LB_BTN, x0, thai ? 58 : 56, RED_AL); txt(F_UM, "-", x0 + w1 + 5, 56, RED_AL); lab(sid, LB_BTN, x0 + w1 + 14, thai ? 58 : 56, RED_AL);
  }
  if (k == AK_MIL) {
    ringStroke(160, 98, 22, 2, RED_AL);
    rr(159, 86, 3, 14, 1, RED_AL); rr(159, 104, 3, 3, 1, RED_AL);
    lab(S_LAMPON2, LB_BTN, 160, thai ? 140 : 138, WHITE, AL_C, 130);
  } else {
    bool ok = false; float v = alertTest ? (k == AK_BATT ? 10.6f : k == AK_OIL ? 131.0f : 107.0f) : alertVal(k, ok); if (alertTest) ok = true;
    char b[16]; if (ok) fmtv(b, sizeof b, v, k == AK_BATT ? 1 : 0); else strcpy(b, "--");
    txtVU(F_NXL, b, F_UM, k == AK_BATT ? "V" : DEGC, 160, 118, WHITE, T.dim, AL_C);
  }
  lab(k == AK_COOL ? S_HOT : k == AK_OIL ? S_OILHOT : k == AK_BATT ? S_LOWV : S_NEWFAULT, LB_ROW, 160, thai ? 157 : 154, ALTXT, AL_C, 250);
  chipBox(36, 170, 116, 26, true); lab(S_OK, LB_BTNM, 94, thai ? 188 : 187, T.onacc, AL_C);
  chipBox(168, 170, 116, 26, false); lab(S_SNOOZE, LB_BTNM, 226, thai ? 188 : 187, T.text, AL_C);
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
static void tapAlert(int x, int y, bool first) {
  if (!first) return;
  if (hit(x, y, 36, 170, 116, 26)) alertClose(false); else if (hit(x, y, 168, 170, 116, 26)) alertClose(true);
}

// ---------- park screen ----------
static uint32_t parkAt = 0; static float parkBatt = 0;
static void drawPark() {
  uint32_t secs = (uiNow - parkAt) / 1000; char b[24], c[12];
  lab(S_PARKED, LB_BTNM, 160, thai ? 27 : 26, T.dim, AL_C);
  halo(110, 90, 100, 3, 1, T.acc, 2); rr(110, 90, 100, 3, 1, T.acc);
  clockStr(b, sizeof b, secs); txt(F_NXL, b, 160, 78, T.text, AL_C);
  tile(9, 100, 149, 52); lab(S_LASTBATT, LB_TILE, 83, tby(116), T.dim, AL_C);
  tile(162, 100, 149, 52); lab(S_RANGE, LB_TILE, 236, tby(116), T.dim, AL_C);
  tile(9, 158, 302, 50);
  static const StrId LB4[4] = {S_THISDRIVE, S_DTIME, S_AVG, S_TOPSPEED};
  for (int i = 0; i < 4; i++) lab(LB4[i], LB_TILE, 9 + 38 + i * 76, tby(173), T.dim, AL_C);
  float pb = parkBatt;
  if (pb > 1) { fmtv(b, sizeof b, pb, 1); txtVU(F_NHL, b, F_LS, "V", 83, 143, pb >= 12.3f ? T.good : T.warn, T.dim, AL_C); } else txt(F_NHL, "--", 83, 143, T.dim, AL_C);
  float rg = rangeKm();
  if (rg >= 0) { snprintf(b, sizeof b, "%d", (int)rg); txtVU(F_NHL, b, F_LS, "km", 236, 143, T.text, T.dim, AL_C); } else txt(F_NHL, "--", 236, 143, T.dim, AL_C);
  fmtv(b, sizeof b, tDist, 1); txtVU(F_NL, b, F_LS, "km", 47, 198, T.text, T.dim, AL_C);
  int mins = (int)(tSec / 60); snprintf(b, sizeof b, "%d:%02d", mins / 60, mins % 60); txtVU(F_NL, b, F_LS, "h", 123, 198, T.text, T.dim, AL_C);
  float al = tripAvgL100();
  if (al > 0) { fmtv(b, sizeof b, 100.0f / al, 1); txtVU(F_NL, b, F_LS, "km/L", 199, 198, T.text, T.dim, AL_C); } else txt(F_NL, "--", 199, 198, T.dim, AL_C);
  fmtv(b, sizeof b, pkD.spd, 0); txtVU(F_NL, b, F_LS, "km/h", 275, 198, T.text, T.dim, AL_C);
  uint32_t left = secs >= 180 ? 0 : 180 - secs; snprintf(c, sizeof c, "%u:%02u", (unsigned)(left / 60), (unsigned)(left % 60));
  int w1 = labW(S_SCROFF, LB_TILE), w2 = textW(F_LS, c), w3 = labW(S_TAPGAUGE, LB_TILE), tot = w1 + 4 + w2 + 12 + w3, x0 = 160 - tot / 2;
  lab(S_SCROFF, LB_TILE, x0, tby(231), T.dim); txt(F_LS, c, x0 + w1 + 4, tby(231), T.text); lab(S_TAPGAUGE, LB_TILE, x0 + w1 + 4 + w2 + 12, tby(231), T.dim);
}

// =====================================================================================
//  Update: Wi-Fi + GitHub release check + firmware install
//
//  The flash is 4 MB, too small for the Bluetooth stack AND Wi-Fi/TLS in one image, so the project is two images:
//    ota_0  the gauge (this file built normally)  - PG_UPD is only an info page whose button restarts into the updater
//    ota_1  the updater (-DUPD_BUILD)             - Wi-Fi list, password keyboard, release check, download -> ota_0
//  The updater writes the new gauge into ota_0 and only then switches the boot partition, so an interrupted update
//  never leaves the unit without a working updater (it simply boots into the updater again).
// =====================================================================================
#ifndef UPD_OWNER
#define UPD_OWNER "preay25715-a11y"          // GitHub account / organisation that owns the firmware repository (must be PUBLIC)
#endif
#ifndef UPD_REPO
#define UPD_REPO "JetSmartGaugeCAR"          // repository whose Releases hold firmware_<variant>.bin
#endif
#ifndef FW_VARIANT
#ifdef ST7789_DRIVER
#define FW_VARIANT "cyd_st7789"
#else
#define FW_VARIANT "cyd"
#endif
#endif

static bool updLeaving = false; static uint32_t updLeaveAt = 0;                  // restart requested (a short delay lets the last frame draw)
static uint8_t updErr = 0;
enum UpdErr : uint8_t { UE_NONE, UE_WIFI, UE_NET, UE_NOFW, UE_DL, UE_SHA, UE_BIG, UE_NOUPD, UE_NOGAUGE, UE_MOVING };
static const StrId UE_STR[] = {S_COUNT, S_ERRWIFI, S_ERRNET, S_ERRNOFW, S_ERRDL, S_ERRSHA, S_ERRBIG, S_ERRNOUPD, S_UPDNOGAUGE, S_ERRMOVING};

static void updNoteVersion() {                                                   // the gauge tells the updater which version is installed
  Preferences p;
  if (p.begin("upd", false)) { if (p.getString("gv", "") != FW_VERSION) p.putString("gv", FW_VERSION); p.end(); }
}
#ifdef UPD_BUILD
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
#define SHA_START(c) mbedtls_sha256_starts(c, 0)
#define SHA_UPD(c, b, n) mbedtls_sha256_update(c, b, n)
#define SHA_FIN(c, o) mbedtls_sha256_finish(c, o)
#else
#define SHA_START(c) mbedtls_sha256_starts_ret(c, 0)
#define SHA_UPD(c, b, n) mbedtls_sha256_update_ret(c, b, n)
#define SHA_FIN(c, o) mbedtls_sha256_finish_ret(c, o)
#endif
#endif
static bool updBootTo(bool updater) {                                            // choose the partition the next restart boots
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, updater ? ESP_PARTITION_SUBTYPE_APP_OTA_1 : ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  return p && esp_ota_set_boot_partition(p) == ESP_OK;
}
static void updLoop(uint32_t now) { if (updLeaving && now - updLeaveAt > 900) ESP.restart(); }
static void updRestartSoon() { updLeaving = true; updLeaveAt = millis(); redraw = true; }

// ---------- shared drawing ----------
static void drawLogoSm(int x0, int y0) {                                         // the 200 x 200 artwork at half size (2 x 2 average); pure black = the transparent corners
  int ys = max(y0, bandY0), ye = min(y0 + 100, bandY1);
  for (int y = ys; y < ye; y++) {
    uint16_t* d = rowPtr(y) + x0; const uint16_t* s0 = LOGO_PX + (y - y0) * 2 * LOGO_W;
    for (int i = 0; i < 100; i++) {
      uint16_t a = pgm_read_word(&s0[i * 2]), b = pgm_read_word(&s0[i * 2 + 1]), c = pgm_read_word(&s0[LOGO_W + i * 2]), e = pgm_read_word(&s0[LOGO_W + i * 2 + 1]);
      if (!(a | b | c | e)) continue;
      int r = ((a >> 11) + (b >> 11) + (c >> 11) + (e >> 11) + 2) >> 2;
      int g = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((e >> 5) & 63) + 2) >> 2;
      int bl = ((a & 31) + (b & 31) + (c & 31) + (e & 31) + 2) >> 2;
      uint16_t v = (uint16_t)((r << 11) | (g << 5) | bl);
      if (v) d[i] = v;
    }
  }
}
static void wifiIcon(int cx, int cy, uint16_t col, int lit) {                    // three arcs and a dot; cx, cy = the dot
  disc((float)cx, (float)cy, 1.7f, col);
  static const float RAD[3] = {5.0f, 9.0f, 13.0f};
  for (int k = 0; k < 3; k++) {
    uint16_t c = k < lit ? col : T.dark;
    for (int s = 0; s < 6; s++) {
      float a0 = (-0.80f + 1.60f * s / 6.0f), a1 = (-0.80f + 1.60f * (s + 1) / 6.0f);
      strokeSeg(cx + RAD[k] * sinf(a0), cy - RAD[k] * cosf(a0), cx + RAD[k] * sinf(a1), cy - RAD[k] * cosf(a1), 1.7f, c, true);
    }
  }
}
static void lockIcon(int x, int y, uint16_t col) {                               // 9 x 11 padlock
  rr(x, y + 5, 9, 7, 2, col);
  ln(x + 2, y + 5, x + 2, y + 3, col); ln(x + 2, y + 3, x + 4, y + 1, col); ln(x + 4, y + 1, x + 5, y + 1, col); ln(x + 5, y + 1, x + 7, y + 3, col); ln(x + 7, y + 3, x + 7, y + 5, col);
}
static void drawStatusLine(StrId id, uint16_t col, int by) { lab(id, LB_BTNM, 160, by, col, AL_C, 300); }

#ifndef UPD_BUILD
// =====================================================================================
//  Gauge image: info page + "start update mode"
// =====================================================================================
static void drawUpd() {
  subHeader(S_UPDTITLE, S_COUNT, false);
  drawLogoSm(10, 34);
  txt(F_UL, "Smart Gauge", 124, 52, T.text);
  lab(S_UPDINST, LB_TILE, 124, tby(72), T.dim);
  txt(F_UL, FW_VERSION, 311, 74, T.accL, AL_R);
  hl(124, 80, 187, T.sep);
  lab(S_UPDI1, LB_ROW, 124, 100, T.text); lab(S_UPDI2, LB_ROW, 124, 116, T.text); lab(S_UPDI3, LB_ROW, 124, 132, T.dim, AL_L, 187);
  if (updLeaving) { chipBox(9, 160, 302, 30, true); lab(S_UPDRESTART, LB_BTNM, 160, chipBy(160, 30), T.onacc, AL_C); }
  else { halo(9, 160, 302, 30, 8, T.acc, 3); chipId(S_UPDSTART, 9, 160, 302, 30, true); }
  if (updErr) lab(UE_STR[updErr], LB_BTNM, 160, 214, T.hot, AL_C, 300);
  else lab(S_UPDSRC, LB_TILE, 160, tby(214), T.dim, AL_C);
  txt(F_LS, UPD_OWNER "/" UPD_REPO, 160, 230, T.dim, AL_C);
}
static void tapUpd(int x, int y, bool first) {
  if (!first || updLeaving) return;
  if (hitBack(x, y)) { nav(PG_SETUP); return; }
  if (hit(x, y, 9, 160, 302, 30)) {
    bool ok; float spd = getVal(0x0D, ok);
    if (!demoMode && ok && spd > 3) { updErr = UE_MOVING; redraw = true; return; }       // never restart the gauge while the car is moving
    savePrefs();
    if (updBootTo(true)) { updErr = UE_NONE; updRestartSoon(); } else { updErr = UE_NOUPD; redraw = true; }
  }
}

#else
// =====================================================================================
//  Updater image
// =====================================================================================
enum UpdUi : uint8_t { UU_HOME, UU_WIFI, UU_KEY, UU_PROG };
enum UpdCmd : uint8_t { UC_NONE, UC_SCAN, UC_CONNECT, UC_CHECK, UC_INSTALL };
struct WfAp { char ssid[33]; int8_t rssi; bool lock; };
static const int WF_MAX = 16;
static uint8_t updUi = UU_HOME;
static volatile uint8_t updCmd = UC_NONE, updPhase = 0;                          // phase: 0 idle, 1 scanning, 2 connecting, 3 checking, 4 installing, 5 done
static char wfSsid[33] = "", wfPass[64] = "";                                    // saved network
static WfAp wfList[WF_MAX]; static volatile int wfN = 0; static int wfOff = 0; static volatile bool wfScanned = false;
static char updCur[20] = FW_VERSION, updLatest[20] = "", updUrl[256] = "", updSha[72] = "";
static volatile uint32_t updTotal = 0, updGot = 0; static volatile bool updHave = false;
// keyboard
static char kbSsid[33] = "", kbPass[64] = ""; static int kbLen = 0; static uint8_t kbMode = 0; static bool kbShow = false, kbLock = true;   // mode: 0 abc, 1 ABC, 2 symbols

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
static void verParse(const char* s, int* v) {                                    // "v1.2.3" -> 1,2,3
  v[0] = v[1] = v[2] = 0; int k = 0;
  for (; *s && k < 3; s++) { if (*s >= '0' && *s <= '9') v[k] = v[k] * 10 + (*s - '0'); else if (*s == '.') k++; else if (v[k] || k) break; }
}
static bool verNewer(const char* a, const char* b) { int x[3], y[3]; verParse(a, x); verParse(b, y); for (int i = 0; i < 3; i++) { if (x[i] != y[i]) return x[i] > y[i]; } return false; }
static bool jsonStr(const String& s, int from, int lim, const char* key, String& out) {    // first "key":"value" in s[from, lim)
  String k = String("\"") + key + "\":"; int p = s.indexOf(k, from); if (p < 0 || (lim >= 0 && p >= lim)) return false;
  p += k.length(); while (p < (int)s.length() && s[p] == ' ') p++;
  if (p >= (int)s.length() || s[p] != '"') return false;
  int q = s.indexOf('"', p + 1); if (q < 0) return false;
  out = s.substring(p + 1, q); return true;
}
static long jsonNum(const String& s, int from, int lim, const char* key) {
  String k = String("\"") + key + "\":"; int p = s.indexOf(k, from); if (p < 0 || (lim >= 0 && p >= lim)) return -1;
  p += k.length(); while (p < (int)s.length() && s[p] == ' ') p++;
  return s.substring(p, p + 12).toInt();
}
static bool httpBegin(HTTPClient& http, WiFiClientSecure& cli, const char* url, bool api) {
  cli.setInsecure();                                                              // no certificate store on the board: integrity rests on the SHA-256 check below
  http.setUserAgent("SmartGauge-OTA");
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(cli, url)) return false;
  if (api) http.addHeader("Accept", "application/vnd.github+json");
  return true;
}

// ---- worker task: scan, connect, check, install (the UI task keeps drawing while these block) ----
static void updDoScan() {
  updPhase = 1; wfScanned = false;
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, false);
  static WfAp tmp[WF_MAX]; int m = 0;
  for (int i = 0; i < n && n > 0; i++) {
    String s = WiFi.SSID(i); if (!s.length() || s.length() > 32) continue;
    int dup = -1; for (int j = 0; j < m; j++) if (!strcmp(tmp[j].ssid, s.c_str())) dup = j;
    int8_t rs = (int8_t)WiFi.RSSI(i);
    if (dup >= 0) { if (rs > tmp[dup].rssi) tmp[dup].rssi = rs; continue; }
    if (m < WF_MAX) { strncpy(tmp[m].ssid, s.c_str(), 32); tmp[m].ssid[32] = 0; tmp[m].rssi = rs; tmp[m].lock = WiFi.encryptionType(i) != WIFI_AUTH_OPEN; m++; }
  }
  WiFi.scanDelete();
  for (int i = 1; i < m; i++) { WfAp t = tmp[i]; int j = i - 1; while (j >= 0 && tmp[j].rssi < t.rssi) { tmp[j + 1] = tmp[j]; j--; } tmp[j + 1] = t; }
  memcpy(wfList, tmp, sizeof(WfAp) * m); wfN = m; wfOff = 0; wfScanned = true;
  updPhase = 0;
}
static bool updDoConnect() {
  updPhase = 2; updErr = UE_NONE;
  WiFi.mode(WIFI_STA); WiFi.disconnect(false); vTaskDelay(100);
  WiFi.begin(wfSsid, wfPass[0] ? wfPass : nullptr);
  uint32_t t0 = millis();
  while (!wifiUp() && millis() - t0 < 16000) vTaskDelay(100);
  bool ok = wifiUp();
  if (ok) { Preferences p; if (p.begin("upd", false)) { p.putString("ssid", wfSsid); p.putString("pass", wfPass); p.end(); } }
  else { updErr = UE_WIFI; WiFi.disconnect(false); }
  updPhase = 0; return ok;
}
static bool updDoCheck() {
  updPhase = 3; updErr = UE_NONE; updHave = false;
  if (!wifiUp()) { updErr = UE_WIFI; updPhase = 0; return false; }
  WiFiClientSecure cli; HTTPClient http;
  char url[160]; snprintf(url, sizeof url, "https://api.github.com/repos/%s/%s/releases/latest", UPD_OWNER, UPD_REPO);
  String body; int code = -1;
  if (httpBegin(http, cli, url, true)) {
    code = http.GET();
    if (code == 200 && http.getSize() < 98304) body = http.getString();
    http.end();
  }
  if (code != 200 || !body.length()) { updErr = UE_NET; updPhase = 0; return false; }
  String tag; if (!jsonStr(body, 0, -1, "tag_name", tag)) { updErr = UE_NOFW; updPhase = 0; return false; }
  String want = String("\"name\":\"firmware_") + FW_VARIANT + ".bin\"";
  int p = body.indexOf(want);
  if (p < 0) { updErr = UE_NOFW; updPhase = 0; return false; }
  int lim = body.indexOf("/releases/assets/", p + 10); if (lim < 0) lim = body.length();     // the next asset starts here
  String dl, dg; long sz = jsonNum(body, p, lim, "size");
  if (!jsonStr(body, p, lim, "browser_download_url", dl)) { updErr = UE_NOFW; updPhase = 0; return false; }
  if (jsonStr(body, p, lim, "digest", dg) && dg.startsWith("sha256:")) strncpy(updSha, dg.c_str() + 7, sizeof updSha - 1); else updSha[0] = 0;
  strncpy(updLatest, tag.c_str(), sizeof updLatest - 1); strncpy(updUrl, dl.c_str(), sizeof updUrl - 1);
  updTotal = sz > 0 ? (uint32_t)sz : 0; updHave = true;
  updPhase = 0; return true;
}
static void updDoInstall() {
  updPhase = 4; updErr = UE_NONE; updGot = 0;
  const esp_partition_t* tgt = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!tgt || !updHave || !wifiUp()) { updErr = tgt ? UE_NET : UE_NOGAUGE; updPhase = 0; return; }
  WiFiClientSecure cli; HTTPClient http;
  if (!httpBegin(http, cli, updUrl, false)) { updErr = UE_NET; updPhase = 0; return; }
  int code = http.GET();
  if (code != 200) { http.end(); updErr = UE_DL; updPhase = 0; return; }
  uint32_t total = http.getSize() > 0 ? (uint32_t)http.getSize() : updTotal;
  if (total < 100000) { http.end(); updErr = UE_DL; updPhase = 0; return; }
  if (total > tgt->size) { http.end(); updErr = UE_BIG; updPhase = 0; return; }
  updTotal = total;
  esp_ota_handle_t h = 0;
  if (esp_ota_begin(tgt, total, &h) != ESP_OK) { http.end(); updErr = UE_DL; updPhase = 0; return; }
  mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); SHA_START(&sha);
  static uint8_t buf[2048];
  WiFiClient* s = http.getStreamPtr(); uint32_t got = 0, last = millis(); bool fail = false;
  while (got < total && !fail) {
    int av = s->available();
    if (av > 0) {
      int n = s->readBytes(buf, (size_t)min<int>(av, (int)sizeof buf));
      if (n <= 0) continue;
      if (esp_ota_write(h, buf, n) != ESP_OK) { fail = true; break; }
      SHA_UPD(&sha, buf, n);
      got += n; updGot = got; last = millis();
    } else {
      if (!s->connected() || millis() - last > 15000) fail = true;
      vTaskDelay(2);
    }
  }
  http.end();
  uint8_t dig[32]; SHA_FIN(&sha, dig); mbedtls_sha256_free(&sha);
  if (fail || got != total) { esp_ota_abort(h); updErr = UE_DL; updPhase = 0; return; }
  if (updSha[0]) {
    char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dig[i]);
    if (strncasecmp(hex, updSha, 64) != 0) { esp_ota_abort(h); updErr = UE_SHA; updPhase = 0; return; }
  }
  if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(tgt) != ESP_OK) { updErr = UE_DL; updPhase = 0; return; }   // esp_ota_end also validates the image
  { Preferences p; if (p.begin("upd", false)) { p.putString("gv", updLatest); p.end(); } }
  updPhase = 5;
}
static void updTask(void*) {
  for (;;) {
    uint8_t c = updCmd;
    if (c == UC_NONE) { vTaskDelay(40); continue; }
    uint8_t next = UC_NONE;
    switch (c) {
      case UC_SCAN: updDoScan(); break;
      case UC_CONNECT: if (updDoConnect()) next = UC_CHECK; break;
      case UC_CHECK: updDoCheck(); break;
      case UC_INSTALL: updDoInstall(); break;
    }
    updCmd = next;
    if (updPhase == 5) updRestartSoon();
  }
}
static void updBegin() {
  Preferences p;
  if (p.begin("upd", true)) {
    String gv = p.getString("gv", ""), ss = p.getString("ssid", ""), pw = p.getString("pass", "");
    p.end();
    if (gv.length() && gv.length() < sizeof updCur) strncpy(updCur, gv.c_str(), sizeof updCur - 1);
    if (ss.length() && ss.length() < sizeof wfSsid) { strncpy(wfSsid, ss.c_str(), sizeof wfSsid - 1); strncpy(wfPass, pw.c_str(), sizeof wfPass - 1); }
  }
  WiFi.persistent(false); WiFi.mode(WIFI_STA);
  xTaskCreatePinnedToCore(updTask, "upd", 16384, nullptr, 1, nullptr, 0);
  if (wfSsid[0]) updCmd = UC_CONNECT;                                            // saved network: connect and look for a new version straight away
}
static bool updBusy() { return updPhase != 0; }
static void updExit() {                                                          // back to the gauge
  if (updBootTo(false)) { updErr = UE_NONE; updRestartSoon(); } else { updErr = UE_NOGAUGE; redraw = true; }
}
static bool updNewer() { return updHave && verNewer(updLatest, updCur); }

// ---------- home ----------
static void signalBars(int x, int y, int rssi) {
  int lv = !rssi ? 0 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
  for (int k = 0; k < 4; k++) { int h = 4 + k * 3; rr(x + k * 8, y + 16 - h, 5, h, 1, k < lv ? T.accL : T.dark); }
}
static void drawUpdHome() {
  subHeader(S_UPDTITLE, S_COUNT, false);
  drawLogoSm(10, 34);
  txt(F_UL, "Smart Gauge", 124, 52, T.text);
  lab(S_UPDINST, LB_TILE, 124, tby(72), T.dim); txt(F_UL, updCur, 311, 74, T.text, AL_R); hl(124, 80, 187, T.sep);
  bool nw = updNewer();
  lab(S_UPDLATEST, LB_TILE, 124, tby(94), T.dim); txt(F_UL, updHave ? updLatest : "--", 311, 96, nw ? T.accL : T.text, AL_R); hl(124, 102, 187, T.sep);
  lab(S_UPDSRC, LB_TILE, 124, tby(118), T.dim);
  // Wi-Fi row
  bool up = wifiUp();
  rrG(9, 140, 302, 26, 8, TA.panel2, TA.panel, T.line);
  wifiIcon(25, 158, up ? T.accL : T.dim, up ? 3 : 0);
  if (wfSsid[0]) { char t[40]; fitText(t, sizeof t, wfSsid, F_UM, 150); txt(F_UM, t, 46, 157, T.text); } else lab(S_NOSEL, LB_BTNM, 46, 157, T.dim);
  lab(up ? S_CONNECTED : S_UPDNOCONN, LB_TILE, 303, tby(156), up ? T.good : T.warn, AL_R);
  // status line
  uint8_t ph = updPhase;
  if (updLeaving) drawStatusLine(S_UPDRESTART, T.good, 186);
  else if (ph == 1) drawStatusLine(S_UPDSCANNING, T.dim, 186);
  else if (ph == 2) drawStatusLine(S_CONNECTING, T.dim, 186);
  else if (ph == 3) drawStatusLine(S_UPDCHECKING, T.dim, 186);
  else if (updErr) drawStatusLine(UE_STR[updErr], T.hot, 186);
  else if (!up) drawStatusLine(S_UPDNOCONN, T.warn, 186);
  else if (nw) drawStatusLine(S_UPDNEW, T.accL, 186);
  else if (updHave) drawStatusLine(S_UPDUPTODATE, T.good, 186);
  else drawStatusLine(S_CONNECTED, T.dim, 186);
  // buttons
  bool busy = ph != 0 || updLeaving;
  chipId(S_WIFI, 9, 200, 146, 26, false);
  if (nw && !busy) { halo(165, 200, 146, 26, 8, T.acc, 3); chipId(S_UPDINSTALL, 165, 200, 146, 26, true); }
  else chipId(S_UPDCHECK, 165, 200, 146, 26, false);
}
static void tapUpdHome(int x, int y, bool first) {
  if (!first) return;
  if (hitBack(x, y)) { if (updPhase != 4) updExit(); return; }
  if (updBusy() || updLeaving) return;
  if (hit(x, y, 9, 140, 302, 26) || hit(x, y, 9, 200, 146, 26)) { updUi = UU_WIFI; wfOff = 0; updErr = UE_NONE; updCmd = UC_SCAN; return; }
  if (hit(x, y, 165, 200, 146, 26)) {
    updErr = UE_NONE;
    if (updNewer()) { updUi = UU_PROG; updCmd = UC_INSTALL; }
    else if (wifiUp()) updCmd = UC_CHECK;
    else if (wfSsid[0]) updCmd = UC_CONNECT;
    else { updUi = UU_WIFI; wfOff = 0; updCmd = UC_SCAN; }
  }
}

// ---------- Wi-Fi list ----------
static void drawUpdWifi() {
  subHeader(S_WIFI, S_SCAN, true);
  int n = wfN; bool up = wifiUp(); char cur[40]; strncpy(cur, WiFi.SSID().c_str(), sizeof cur - 1); cur[sizeof cur - 1] = 0;
  for (int r = 0; r < 5 && updPhase != 1; r++) {
    int i = wfOff + r, y = 32 + r * 28; if (i >= n) break;
    const WfAp& a = wfList[i]; bool sel = up && !strcmp(a.ssid, cur);
    if (sel) rr(9, y, 302, 26, 8, T.panel2, T.acc); else rrG(9, y, 302, 26, 8, TA.panel2, TA.panel, T.line);
    int x = 19; if (a.lock) { lockIcon(18, y + 7, sel ? T.accL : T.dim); x = 36; }
    char t[40]; fitText(t, sizeof t, a.ssid, F_UM, 200 - x); txt(F_UM, t, x, y + 18, sel ? T.text : T.accL);
    if (sel) { ln(228, y + 13, 232, y + 17, T.good); ln(229, y + 13, 233, y + 17, T.good); ln(233, y + 17, 241, y + 7, T.good); ln(234, y + 17, 242, y + 7, T.good); }
    else if (!strcmp(a.ssid, wfSsid)) lab(S_UPDSAVED, LB_TILE, 252, tby(17), T.dim, AL_R);
    signalBars(268, y + 3, a.rssi);
  }
  chipArrow(9, 178, 70, 22, -1, false); chipArrow(85, 178, 70, 22, 1, false);
  if (updPhase == 1) lab(S_UPDSCANNING, LB_BTNM, 311, chipBy(178, 22), T.dim, AL_R);
  else if (updPhase == 2) lab(S_CONNECTING, LB_BTNM, 311, chipBy(178, 22), T.dim, AL_R);
  else if (!n && wfScanned) lab(S_NONET, LB_BTNM, 311, chipBy(178, 22), T.dim, AL_R);
  else if (updErr) lab(UE_STR[updErr], LB_TILE, 311, tby(212), T.hot, AL_R, 302);
}
static void kbOpen(const char* ssid, bool lock) {
  strncpy(kbSsid, ssid, sizeof kbSsid - 1); kbSsid[sizeof kbSsid - 1] = 0; kbLock = lock; kbMode = 0; kbShow = false;
  if (!strcmp(ssid, wfSsid)) { strncpy(kbPass, wfPass, sizeof kbPass - 1); kbPass[sizeof kbPass - 1] = 0; } else kbPass[0] = 0;
  kbLen = (int)strlen(kbPass); updUi = UU_KEY; updErr = UE_NONE;
}
static void tapUpdWifi(int x, int y, bool first) {
  if (first && hitBack(x, y)) { updUi = UU_HOME; return; }
  if (first && hit(x, y, 247, 4, 64, 20)) { if (!updBusy()) updCmd = UC_SCAN; return; }
  if (updBusy()) return;
  if (hit(x, y, 9, 178, 70, 22)) { if (wfOff > 0) wfOff--; return; }
  if (hit(x, y, 85, 178, 70, 22)) { if (wfOff < wfN - 5) wfOff++; return; }
  if (!first) return;
  for (int r = 0; r < 5; r++) if (hit(x, y, 9, 32 + r * 28, 302, 26)) {
    int i = wfOff + r; if (i >= wfN) return;
    const WfAp& a = wfList[i];
    if (a.lock) kbOpen(a.ssid, true);
    else { strncpy(wfSsid, a.ssid, 32); wfSsid[32] = 0; wfPass[0] = 0; updUi = UU_HOME; updCmd = UC_CONNECT; }
    return;
  }
}

// ---------- password keyboard ----------
struct KbKey { int16_t x, y, w; char c; uint8_t kind; };                          // kind: 0 character, 1 shift, 2 backspace, 3 symbols/letters, 4 space, 5 connect
static int kbBuild(KbKey* k) {
  const char* r1 = "1234567890";
  const char* r2 = kbMode == 2 ? "@#$%&*-+=_" : "qwertyuiop";
  const char* r3 = kbMode == 2 ? "!?:;()\"'~" : "asdfghjkl";
  const char* r4 = kbMode == 2 ? "/\\|[]{}" : "zxcvbnm";
  int n = 0;
  auto ch = [&](char c) { return (kbMode == 1 && c >= 'a' && c <= 'z') ? (char)(c - 32) : c; };
  for (int c = 0; c < 10; c++) k[n++] = {(int16_t)(6 + c * 31), 60, 29, r1[c], 0};
  for (int c = 0; c < 10; c++) k[n++] = {(int16_t)(6 + c * 31), 90, 29, ch(r2[c]), 0};
  for (int c = 0; c < 9; c++) k[n++] = {(int16_t)(21 + c * 31), 120, 29, ch(r3[c]), 0};
  k[n++] = {6, 150, 44, 0, 1};
  for (int c = 0; c < 7; c++) k[n++] = {(int16_t)(52 + c * 31), 150, 29, ch(r4[c]), 0};
  k[n++] = {270, 150, 44, 0, 2};
  k[n++] = {6, 180, 46, 0, 3}; k[n++] = {56, 180, 29, ',', 0}; k[n++] = {88, 180, 118, ' ', 4}; k[n++] = {209, 180, 29, '.', 0}; k[n++] = {242, 180, 72, 0, 5};
  return n;
}
static void drawUpdKey() {
  chipBox(9, 4, 64, 20, false); chevron(22, 14, -1, T.text); lab(S_BACK, LB_BTNM, 46, chipBy(4, 20), T.text, AL_C, 44);
  { char t[40]; fitText(t, sizeof t, kbSsid, F_UM, 160); txt(F_UM, t, 160, 18, T.text, AL_C); }
  chipId(kbShow ? S_HIDEPW : S_SHOWPW, 247, 4, 64, 20, kbShow);
  // password field: the tail of the text, masked unless "Show" is on
  rr(9, 30, 302, 24, 8, T.panel2, T.acc);
  if (!kbLen) lab(S_PWD, LB_BTNM, 19, chipBy(30, 24), T.dim);
  else {
    char shown[70]; int st = 0;
    for (;;) {
      int m = 0; for (int i = st; i < kbLen && m < 68; i++) shown[m++] = kbShow ? kbPass[i] : '*'; shown[m] = 0;
      if (textW(F_UM, shown) <= 276 || st >= kbLen - 1) break; st++;
    }
    txt(F_UM, shown, 19, 47, T.text);
  }
  KbKey k[48]; int n = kbBuild(k);
  for (int i = 0; i < n; i++) {
    const KbKey& q = k[i]; bool acc = (q.kind == 1 && kbMode == 1) || q.kind == 5;
    rr(q.x, q.y, q.w, 27, 6, acc ? T.acc : T.panel, acc ? T.acc : T.line);
    int cx = q.x + (q.w >> 1), cy = q.y + 13; uint16_t fg = acc ? T.onacc : T.text;
    switch (q.kind) {
      case 0: { char s[2] = {q.c, 0}; txt(F_UM, s, cx, q.y + 18, fg, AL_C); break; }
      case 1: ln(cx, cy - 6, cx - 5, cy, fg); ln(cx, cy - 6, cx + 5, cy, fg); ln(cx, cy - 6, cx, cy + 6, fg); ln(cx - 5, cy, cx - 2, cy, fg); ln(cx + 5, cy, cx + 2, cy, fg); break;
      case 2: ln(cx - 7, cy, cx + 6, cy, fg); ln(cx - 7, cy, cx - 3, cy - 4, fg); ln(cx - 7, cy, cx - 3, cy + 4, fg); break;
      case 3: txt(F_UM, kbMode == 2 ? "abc" : "?123", cx, q.y + 18, fg, AL_C); break;
      case 5: lab(S_CONNECTBTN, LB_BTNM, cx, chipBy(q.y, 27), fg, AL_C, q.w - 6); break;
    }
  }
  if (updPhase == 2) lab(S_CONNECTING, LB_BTNM, 160, 228, T.dim, AL_C);
  else if (updErr) lab(UE_STR[updErr], LB_BTNM, 160, 228, T.hot, AL_C, 300);
}
static void tapUpdKey(int x, int y, bool first) {
  if (first && hitBack(x, y)) { kbPass[0] = 0; kbLen = 0; updUi = UU_WIFI; return; }
  if (first && (hit(x, y, 247, 4, 64, 20) || hit(x, y, 9, 30, 302, 24))) { kbShow = !kbShow; return; }
  KbKey k[48]; int n = kbBuild(k);
  for (int i = 0; i < n; i++) {
    const KbKey& q = k[i];
    if (!hit(x, y, q.x - 1, q.y - 1, q.w + 2, 29)) continue;
    if (q.kind == 2) { if (kbLen > 0) kbPass[--kbLen] = 0; return; }                // backspace repeats while held
    if (!first) return;
    switch (q.kind) {
      case 0: case 4: if (kbLen < 63) { kbPass[kbLen++] = q.c; kbPass[kbLen] = 0; } if (kbMode == 1) kbMode = 0; break;
      case 1: kbMode = kbMode == 1 ? 0 : 1; break;
      case 3: kbMode = kbMode == 2 ? 0 : 2; break;
      case 5:
        if (kbLen >= 8 || !kbLock) { strncpy(wfSsid, kbSsid, 32); wfSsid[32] = 0; strncpy(wfPass, kbPass, 63); wfPass[63] = 0; updUi = UU_HOME; updCmd = UC_CONNECT; }
        break;
    }
    return;
  }
}

// ---------- install progress ----------
static void drawUpdProg() {
  drawLogoSm(110, 14);
  uint32_t tot = updTotal, got = updGot; int pct = tot ? (int)(got * 100ull / tot) : 0; if (pct > 100) pct = 100;
  bool done = updPhase == 5 || updLeaving, err = updErr != UE_NONE && updPhase == 0 && !updLeaving;
  { int w1 = labW(done ? S_UPDDONE : S_UPDBUSY, LB_BTNM), w2 = done ? 0 : textW(F_UL, updLatest) + 8, x0 = 160 - (w1 + w2) / 2;
    if (!err) { lab(done ? S_UPDDONE : S_UPDBUSY, LB_BTNM, x0, 142, done ? T.good : T.text); if (!done) txt(F_UL, updLatest, x0 + w1 + 8, 142, T.accL); } }
  rr(40, 154, 240, 10, 5, T.dark);
  int bw = done ? 240 : max(0, pct * 240 / 100); if (bw >= 10) { halo(40, 154, bw, 10, 5, err ? T.hot : T.acc, 2); rr(40, 154, bw, 10, 5, err ? T.hot : T.acc); }
  char t[48]; snprintf(t, sizeof t, "%d%%   %lu / %lu KB", pct, (unsigned long)(got / 1024), (unsigned long)(tot / 1024));
  if (!err) txt(F_UM, t, 160, 186, T.text, AL_C);
  if (err) {
    lab(UE_STR[updErr], LB_BTNM, 160, 186, T.hot, AL_C, 300);
    chipId(S_UPDRETRY, 9, 204, 146, 26, false); chipId(S_BACK, 165, 204, 146, 26, false);
  } else if (!done) lab(S_UPDNOOFF, LB_BTNM, 160, 218, T.warn, AL_C, 300);
}
static void tapUpdProg(int x, int y, bool first) {
  if (!first || updBusy() || updLeaving || !updErr) return;
  if (hit(x, y, 9, 204, 146, 26)) { updErr = UE_NONE; if (updHave && wifiUp()) updCmd = UC_INSTALL; else updUi = UU_HOME; }
  else if (hit(x, y, 165, 204, 146, 26)) { updUi = UU_HOME; }
}

static void drawUpd() {
  switch (updUi) {
    case UU_WIFI: drawUpdWifi(); break;
    case UU_KEY: drawUpdKey(); break;
    case UU_PROG: drawUpdProg(); break;
    default: drawUpdHome(); break;
  }
}
static void tapUpd(int x, int y, bool first) {
  switch (updUi) {
    case UU_WIFI: tapUpdWifi(x, y, first); break;
    case UU_KEY: tapUpdKey(x, y, first); break;
    case UU_PROG: tapUpdProg(x, y, first); break;
    default: tapUpdHome(x, y, first); break;
  }
}
#endif

// =====================================================================================
//  Navigation, frame composition, touch, main loop
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
    case PG_BT: btOff = 0; if (linkMode == 0 && !selAddr[0] && scanState != 1 && !demoMode) btStartScan(); break;
    case PG_PARK: parkAt = millis(); break;
    case PG_UPD: updErr = UE_NONE; break;
    case PG_CRP: crpMn = 1e9f; crpMx = -1e9f; break;
    case PG_LISTEN: listenMode = 0; listenGen = listenGen + 1; lsTotal = 0; lsRate = 0; lsTopN = 0; lsDistinct = 0; lsBuf = 0; lsLast.n = 0; lsStart = millis(); listenSt = 1; break;
    case PG_CRPCAL: { bool o; float v = metricVal(M_CRP, o); crpMeter = o ? clampi((int)lroundf(v * 100), 50, 450) : 125; crpMsg = 0; } break;
    default: break;
  }
}

static void drawScene() {
#ifdef UPD_BUILD
  drawUpd();
#else
  if (alertOn != AK_NONE) { drawAlert(); return; }
  switch (scr) {
    case PG_SPLASH: drawSplash(); break;
    case PG_DASH: drawDash(); break;
    case PG_DATA: drawData(); break;
    case PG_LOG: drawLog(); break;
    case PG_ALL: drawAll(); break;
    case PG_GRAPH: drawGraph(); break;
    case PG_TRIP: drawTrip(); break;
    case PG_PEAKS: drawPeaks(); break;
    case PG_CODES: drawCodes(); break;
    case PG_STATUS: drawStatus(); break;
    case PG_FREEZE: drawFreeze(); break;
    case PG_ECU: drawEcu(); break;
    case PG_SETUP: drawSetup(); break;
    case PG_SETUP2: drawSetup2(); break;
    case PG_BT: drawBt(); break;
    case PG_VEH: drawVeh(); break;
    case PG_PARK: drawPark(); break;
    case PG_UPD: drawUpd(); break;
    case PG_CRP: drawCrpCal(); break;
    case PG_CRPCAL: drawCrpCalib(); break;
    case PG_LISTEN: drawListen(); break;
  }
#endif
}
// ---------- RPM that follows the pedal (v1.0.11) ----------
// The car answers about every 70 ms (CAN) and a frame reaches the glass some 50 ms after it was composed. rpmFollow() continues the last trend over
// that gap, so the ring, the number and the shift strip are where the engine is now. Steady revs (idle, cruising) stay below the dead band, and then the
// number is exactly what the ECU said.
static const int RS_COMP = 60, RS_MAXH = 240, RS_CAP = 350;     // ms the picture is late by, longest look-ahead (ms), largest lead (rpm)
static bool rsOn = false; static float rsV1 = 0, rsSlope = 0, rsPrevS = 0; static uint32_t rsT1 = 0;     // last reply, its time, the trend (rpm per ms), and the step before
static float rpmFollow() {                                      // once per frame, from the frame's snapshot
  bool ok; float v = getVal(0x0C, ok);
  if (!ok) { rsOn = false; rsSlope = rsPrevS = 0; return 0; }
  uint32_t t = pageS[0x0C];
  if (!rsOn) { rsOn = true; rsV1 = v; rsT1 = t; rsSlope = rsPrevS = 0; return v; }
  if (t != rsT1) {                                              // a new reply
    float dt = (float)(int32_t)(t - rsT1), dv = v - rsV1, s = 0;
    if (dt >= 8 && dt <= 600 && fabsf(dv) >= 30 + 0.2f * dt) s = dv / dt;       // below the dead band it is noise (idle jitter), not the pedal
    float tgt = (s != 0 && rsPrevS != 0 && (s > 0) == (rsPrevS > 0)) ? (s + rsPrevS) * 0.5f : 0;   // a trend needs two steps the same way: noise changes sign
    rsSlope = tgt * 0.7f + rsSlope * 0.3f; rsPrevS = s; rsV1 = v; rsT1 = t;
  }
  float h = clampf((float)(int32_t)(uiNow - rsT1) + RS_COMP, 0, RS_MAXH);
  float r = v + clampf(rsSlope * h, -RS_CAP, RS_CAP);
  return r < 0 ? 0 : r;
}
static void renderFrame() {
  uiNow = snapNow = millis();                                   // one consistent snapshot of the car data for all bands
  memcpy(pvS, pv, sizeof pv); memcpy(psupS, psup, sizeof psup); memcpy(pageS, page, sizeof page);
  snapOn = true;
#ifndef UPD_BUILD
  rpmUi = rpmFollow();
  if (scr == PG_ALL) { int oN = allN, oP = allPg; allBuild(); if (allN != oN || allPg != oP) updateWants(); }
  if (scr == PG_BT) btBuildView();
#endif
  for (int b = 0; b < NBAND; b++) {
    bandY0 = b * BAND_H; bandY1 = bandY0 + BAND_H;
#ifdef UPD_BUILD
    fillBand(T.bg);
#else
    fillBand(alertOn != AK_NONE ? ALBG : (scr == PG_SPLASH ? C(0, 0, 0) : T.bg));
#endif
    drawScene();
    presentBand(b);
  }
  snapOn = false;
}

#ifndef UPD_BUILD
static void tapStatus(int x, int y, bool first) {
  if (first && scr == PG_ECU && hit(x, y, 215, 175, 96, 22)) { nav(PG_LISTEN); return; }
  if (first) codeTouch(x, y, scr);
}
#endif
static void handleTouch(int x, int y, bool first) {
#ifdef UPD_BUILD
  tapUpd(x, y, first);
#else
  if (alertOn != AK_NONE) { tapAlert(x, y, first); return; }
  switch (scr) {
    case PG_SPLASH:
      if (!first) return;
      if (firstRun) { invScreen = !invScreen; applyInvert(); splashAt = millis(); }
      else nav(PG_DASH);
      return;
    case PG_DASH: tapDash(x, y, first); return;
    case PG_DATA: tapData(x, y, first); return;
    case PG_LOG: tapLog(x, y, first); return;
    case PG_ALL: tapAll(x, y, first); return;
    case PG_GRAPH: tapGraph(x, y, first); return;
    case PG_TRIP: tapTrip(x, y, first); return;
    case PG_PEAKS: tapPeaks(x, y, first); return;
    case PG_CODES: tapCodes(x, y, first); return;
    case PG_STATUS: case PG_FREEZE: case PG_ECU: tapStatus(x, y, first); return;
    case PG_SETUP: tapSetup(x, y, first); return;
    case PG_SETUP2: tapSetup2(x, y, first); return;
    case PG_BT: tapBt(x, y, first); return;
    case PG_VEH: tapVeh(x, y, first); return;
    case PG_PARK: if (first) nav(PG_DASH); return;
    case PG_UPD: tapUpd(x, y, first); return;
    case PG_CRP: tapCrpCal(x, y, first); return;
    case PG_CRPCAL: tapCrpCalib(x, y, first); return;
    case PG_LISTEN: tapListen(x, y, first); return;
  }
#endif
}

// ---------- touch (shared by the gauge and the updater) ----------
static void pollTouch(uint32_t now) {
  static bool wasTouched = false; static uint32_t repAt = 0;
  bool t = ts.tirqTouched() && ts.touched();
  if (t) {
    TS_Point p = ts.getPoint();
    int x = constrain(map(p.x, 200, 3700, 1, 320), 0, 319), y = constrain(map(p.y, 240, 3800, 1, 240), 0, 239);
    if (flipScr) { x = 319 - x; y = 239 - y; }                       // the touch layer turns with the glass
    if (!wasTouched) {
      repAt = now + 450;
      if (screenOff) { screenOff = false; applyBright(); } else handleTouch(x, y, true);
    } else if ((int32_t)(now - repAt) >= 0) { repAt = now + 110; if (!screenOff) handleTouch(x, y, false); }
  }
  wasTouched = t;
}
static void initPanel() {
  tft.init(); tft.setRotation(flipScr ? 3 : 1); tft.setSwapBytes(true); applyInvert(); tft.fillScreen(0);
  ledcSetup(0, 5000, 8); ledcAttachPin(21, 0); applyBright();
  tsSPI.begin(25, 39, 32, 33); ts.begin(tsSPI); ts.setRotation(1);
}

#ifdef UPD_BUILD
// ---------- updater image: no Bluetooth and no OBD task, only the update pages ----------
void setup() {
  Serial.begin(115200);
  pinMode(4, OUTPUT); pinMode(16, OUTPUT); pinMode(17, OUTPUT);
  digitalWrite(4, HIGH); digitalWrite(16, HIGH); digitalWrite(17, HIGH);
  loadPrefs(); initPanel();
  scr = PG_UPD; redraw = true;
  updBegin();
}
void loop() {
  uint32_t now = millis(); static uint32_t tUI = 0;
  pollTouch(now);
  updLoop(now);
  if (redraw || now - tUI >= 80) { tUI = now; redraw = false; renderFrame(); }
  delay(5);
}
#else
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
    else if (now - rpmZeroAt > 6000) { engineRan = false; screenOff = false; rpmZeroAt = 0; nav(PG_PARK); }
  } else rpmZeroAt = 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(4, OUTPUT); pinMode(16, OUTPUT); pinMode(17, OUTPUT);              // CYD RGB LED off (active low)
  digitalWrite(4, HIGH); digitalWrite(16, HIGH); digitalWrite(17, HIGH);
  loadPrefs(); updNoteVersion();
  initPanel();
  linkState = demoMode ? 0 : ((linkMode == 1 || selAddr[0]) ? 1 : 3);
  if (demoMode && !slotsCustom) autoSlots();
  tripReset(); peaksClear(pkD); logClear();
  scr = PG_SPLASH; splashAt = millis(); redraw = true; updateWants();
  xTaskCreatePinnedToCore(obdTask, "obd", 16384, nullptr, 1, nullptr, 0);   // BLE service discovery runs on this stack too
}

void loop() {
  uint32_t now = millis();
  pollTouch(now);

  btBrowsing = (scr == PG_BT && linkMode == 0);
  if (vinNew) { vinNew = false; vinApply(); }
  if (pinDirty || protoDirty) { pinDirty = false; protoDirty = false; savePrefs(); }
  if (prefsDirty && now - prefsAt > 1500) { prefsDirty = false; savePrefs(); }
  if (pkDirty && now - pkSaved > 60000u) { pkDirty = false; pkSaved = now; savePrefs(); }
  if (uprotoAt && now - uprotoAt > 1500) { uprotoAt = 0; if (!demoMode) reconnectReq = true; }
  updLoop(now);

  static uint32_t tSim = 0, tUI = 0, tGraph = 0; static int lastLs = -1;
  if (now - tSim >= 100) {
    float dt = (now - tSim) / 1000.0f; tSim = now;
    tripStep(dt); peaksStep(); logStep(); alertsStep(); parkStep(now);
    if (linkState != lastLs) { lastLs = linkState; if (linkState == 2 && !slotsCustom) autoSlots(); }
    if (scr == PG_SPLASH && now - splashAt > splashMs()) { if (firstRun) savePrefs(); nav(PG_DASH); }
  }
  if (now - tGraph >= 1000) { tGraph = now; graphSample(); }
  if (!screenOff && (redraw || now - tUI >= 33)) { tUI = now; redraw = false; renderFrame(); }
  delay(1);
}
#endif
