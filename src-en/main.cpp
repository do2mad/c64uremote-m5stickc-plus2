// ============================================================================
//  C64uRemote  -  M5StickC Plus2  +  Unit RFID2 (WS1850S) on the Grove port
// ----------------------------------------------------------------------------
//  Remote control for the Commodore 64 Ultimate (c64u) / Ultimate64 Elite-II
//  via the ReST API of the Ultimate firmware (3.11 and newer).
//
//  Based on the original idea by Karl Prosser (@klumsy)
//      https://github.com/ReadyOS-C64/C64uRemote
//  extended by Martin Oswald (@mad) - https://1MHz.de
//
//  License: MIT - see LICENSE in the project root.
//    Copyright (c) 2026 Karl Prosser  (original project C64uRemote,
//                                      https://github.com/ReadyOS-C64/C64uRemote)
//    Copyright (c) 2026 Martin Oswald (port and extensions)
//
//  New compared to the first StickC version:
//    * Unit RFID2 (WS1850S, I2C 0x28) on the Grove port HY2.0-4P - optional.
//      The reader is detected at startup; if absent, everything stays as before.
//        - READ WiFi cards: SSID and password go straight into the device,
//          without reflashing. Format identical to M5Dial and Core.
//        - Read and run command cards (CMD): Reset, Reboot,
//          Ultimate menu, PowerOff (with prompt) and CPU switching.
//        - WRITE cards: command cards and WiFi cards.
//      Path cards (games) need a microSD and are therefore
//      reserved for Core / M5Dial - the Stick says so politely.
//    * Up to four WiFi credentials in internal memory (NVS). At startup
//      the device picks the network with the best signal.
//    * Setup portal: own access point, entry in the browser - the
//      way out when no WiFi card is at hand.
//    * build_env.h now only supplies the INITIAL VALUES for the first boot.
//
//  The MiniJoyC (HAT port, I2C G0/G26) stays supported unchanged and
//  runs on its own bus - joystick and RFID2 do not disturb each other.
//
//  Memory note: the ESP32-PICO-V3-02 has 2 MB of PSRAM in the package, and
//  that is where the two logo caches (240*135*2 = 64 kB each) live. While
//  the setup portal runs they are released and re-created afterwards - the
//  access point and the web server need the internal heap.
// ============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <MFRC522_I2C.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <strings.h>

#if __has_include("build_env.h")
#include "build_env.h"
#else
#define C64U_WIFI_SSID ""
#define C64U_WIFI_PASSWORD ""
#define C64U_TARGET_HOST "192.168.0.64"
#define C64U_TARGET_PASSWORD ""
#endif

//#include "commodore_logo_rgb565.h"   // commodore logo by Karl Prosser
#include "1MHz_logo_rgb565.h"     //1Mhz.de logo by mad

namespace {

// ------------------------------------------------------------
// Time and UI constants
// ------------------------------------------------------------
constexpr uint32_t kModalMs = 1400;
constexpr uint32_t kHttpTimeoutMs = 2500;
constexpr uint32_t kWiFiRetryMs = 10000;
constexpr uint32_t kConnectionProbeMs = 15000;
constexpr uint32_t kFrameMs = 33;
constexpr uint32_t kDoublePressMs = 300;
constexpr uint32_t kHomeEffectMs = 5000;
constexpr uint32_t kHomeLongEffectMs = 7000;
constexpr uint32_t kHomeStaticMs = 1000;
constexpr size_t kMaxCpuChoices = 16;

// MiniJoyC: treat as offline only after several read errors
constexpr uint8_t kJoyOfflineThreshold = 6;

// If the Stick button reacts inverted: set to true
constexpr bool kJoyButtonActiveLow = false;

// PowerOff must be confirmed a second time within this time window
constexpr uint32_t kPowerOffConfirmMs = 2000;

// ------------------------------------------------------------
// I2C pins for the MiniJoyC
// SDA -> G0
// SCL -> G26
// ------------------------------------------------------------
constexpr int kI2cSdaPin = 0;
constexpr int kI2cSclPin = 26;

// ------------------------------------------------------------
// Unit RFID2 (WS1850S) on the Grove port HY2.0-4P
//   SDA -> G32   SCL -> G33   (own bus: Wire1)
// This keeps the MiniJoyC undisturbed on Wire (G0/G26).
// ------------------------------------------------------------
constexpr int     kRfidSdaPin = 32;
constexpr int     kRfidSclPin = 33;
constexpr uint8_t kRfidAddr   = 0x28;

// Background polling interval and lockout time, so that the same card
// left on the reader is not triggered over and over.
constexpr uint32_t kRfidPollMs    = 250;
constexpr uint32_t kRfidRepeatMs  = 2500;

// ------------------------------------------------------------
// WiFi management
// ------------------------------------------------------------
constexpr size_t   kWifiProfileMax = 4;         // saved networks
constexpr uint32_t kPortalIdleMs   = 300000;    // Stop the portal after 5 min
constexpr uint32_t kPortalCloseMs  = 2500;      // Run-on time after saving
constexpr uint32_t kWifiCardConnectMs = 8000;   // Wait time after a WiFi card

constexpr const char* kPortalSsid    = "C64uRemote-Setup";
constexpr const char* kPortalPass    = "c64ultimate";
constexpr uint8_t     kPortalDnsPort = 53;

// ------------------------------------------------------------
// MiniJoyC I2C address and registers
// ------------------------------------------------------------
constexpr uint8_t kMiniJoyAddr = 0x54;
constexpr uint8_t kRegJoyX = 0x20;
constexpr uint8_t kRegJoyY = 0x21;
constexpr uint8_t kRegJoyButton = 0x30;
constexpr uint8_t kRegJoyRgbLed = 0x40;

// ------------------------------------------------------------
// Switching thresholds for all MiniJoyC directions 
// ------------------------------------------------------------
constexpr int kJoyThreshold = 100;

// ------------------------------------------------------------
// Menu entries
// ------------------------------------------------------------

// Order of the main menu. The names keep the handling in
// handleMenuSelect() from shifting when an entry is inserted.
enum MenuId : uint8_t {
  kMenuPowerOff = 0,
  kMenuCpuSpeed,
  kMenuNfc,
  kMenuWifi,
  kMenuConnTest,
  kMenuStatus,
  kMenuSettings,
  kMenuIdCount
};

constexpr const char* kMenuItems[] = {
    "PowerOff",
    "CPU Speed",
    "NFC / RFID",
    "WiFi",
    "Connection Test",
    "Status",
    "Settings",
};

// Setup list. Here too there are names instead of raw indices.
enum SettingsId : uint8_t {
  kSetAutoNfc = 0,
  kSetCardConfirm,     // Prompt time of the PowerOff command card (NFC cmd)
  kSetAnimations,
  kSetEffect,
  kSetJoyOverlay,
  kSetJoyLed,
  kSetJoyLedBright,
  kSetJoyThreshold,
  kSetAnimSpeed,
  kSetEffectTime,
  kSetStaticTime,
  kSetBrightness,
  kSetFactoryReset,
  kSetItemCount
};

constexpr const char* kDisplayMenuItems[] = {
    "Auto-NFC",
    "NFC-Cmd PowOff",
    "Animations",
    "Effect",
    "Joy Overlay",
    "Joy LED",
    "Joy LED Bright",
    "Joy Threshold",
    "Anim Speed",
    "Effect Time",
    "Static Time",
    "Brightness",
    "Factory Reset",
};
constexpr size_t kDisplayMenuCount = sizeof(kDisplayMenuItems) / sizeof(kDisplayMenuItems[0]);
static_assert(kDisplayMenuCount == static_cast<size_t>(kSetItemCount),
              "kDisplayMenuItems and SettingsId are out of sync");
constexpr size_t kMenuCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
static_assert(kMenuCount == static_cast<size_t>(kMenuIdCount),
              "kMenuItems and MenuId are out of sync");

// NFC / RFID submenu
enum NfcMenuId : uint8_t {
  kNfcRead = 0,        // Tap card and execute
  kNfcWriteCmd,        // Write command card
  kNfcWriteWifi,       // Write WiFi credentials to a card
  kNfcMenuCount
};

constexpr const char* kNfcMenuItems[kNfcMenuCount] = {
    "Read card",
    "CMD card",
    "WiFi to card",
};

// WiFi submenu
enum WifiMenuId : uint8_t {
  kWifiFromCard = 0,   // Read credentials from an NFC card
  kWifiPortal,         // Start the setup portal
  kWifiSavedList,      // saved networks: connect
  kWifiToCard,         // write the current credentials to a card
  kWifiDeleteOne,      // discard a single network
  kWifiDeleteAll,      // discard all networks
  kWifiMenuCount
};

constexpr const char* kWifiMenuItems[kWifiMenuCount] = {
    "From NFC card",
    "Setup portal",
    "Saved",
    "To NFC card",
    "Delete network",
    "Delete all",
};

enum class ScreenMode : uint8_t {
  Home,
  Menu,
  CpuMenu,
  Status,
  DisplaySettings,
  NfcMenu,        // NFC / RFID submenu
  NfcRead,        // Tap card -> run its content
  CmdPick,        // Select the command to put on the card
  NfcWrite,       // Tap card -> write text
  WifiMenu,       // WiFi submenu
  WifiCard,       // Tap card -> read credentials
  WifiSaved,      // saved networks
  WifiPortal,     // Setup access point is running
};

// What a card can hold when it is not a file path.
// The card then holds e.g. "CMD:RESET" or "CMD:CPU=10".
enum class CardCmd : uint8_t {
  None,
  Reset,
  Reboot,
  UltiMenu,
  PowerOff,        // Argument = confirmation time in seconds, 0 = immediately
  CpuSpeed,        // Argument = desired value, e.g. "10"
};

struct CardCommand {
  CardCmd cmd = CardCmd::None;
  String  arg;                 // PowerOff: seconds, CpuSpeed: MHz
  bool    hasArg = false;
};

// Card family - it decides how reading and writing are done.
enum class CardKind : uint8_t { None, Classic, Ultralight };

// Reader background polling on the home screen
enum class AutoNfcMode : uint8_t { Off, Slow, Normal, Fast };

// One stored WiFi profile
struct WifiProfile {
  String ssid;
  String pass;
};

enum class HomeMode : uint8_t {
  Static,
  Water,
  RotoZoom,
  SineWave,
  RippleBump,
  RasterBars,
};

enum class JoyDir : uint8_t {
  Center,
  Up,
  Down,
  Left,
  Right,
};

enum class DisplayEffectMode : uint8_t {
  Auto,
  Static,
  Water,
  RotoZoom,
  SineWave,
  RippleBump,
  RasterBars,
};

enum class AnimationSpeedMode : uint8_t {
  Slow,
  Normal,
  Fast,
};

enum class EffectDurationMode : uint8_t {
  Short,
  Normal,
  Long,
};

enum class StaticDurationMode : uint8_t {
  Short,
  Normal,
  Long,
};

enum class JoyThresholdMode : uint8_t {
  Threshold40,
  Threshold60,
  Threshold80,
  Threshold100,
};

// ------------------------------------------------------------
// Status / data structures
// ------------------------------------------------------------

struct ApiResponse {
  bool transportOk = false;
  bool jsonOk = false;
  bool apiOk = false;
  int httpCode = -1;
  String body;
  String errors;
};

struct ConnectionState {
  bool wifiConnected = false;
  bool targetReachable = false;
  bool authOk = false;
  String detail = "Not tested";
};

struct HomeDemoState {
  HomeMode mode = HomeMode::Static;
  uint8_t nextEffectIndex = 0;
  uint32_t startedAtMs = 0;
  uint32_t pausedAtMs = 0;
  bool frameDirty = true;
};

struct SettingsState {
  // Display/control options that are stored permanently in flash.
  bool animationsEnabled = true;
  DisplayEffectMode effectMode = DisplayEffectMode::Auto;
  bool joyOverlayEnabled = true;
  bool joyLedEnabled = true;
  uint8_t joyLedBrightness = 96;
  JoyThresholdMode joyThreshold = JoyThresholdMode::Threshold100;
  AnimationSpeedMode animationSpeed = AnimationSpeedMode::Normal;
  EffectDurationMode effectDuration = EffectDurationMode::Normal;
  StaticDurationMode staticDuration = StaticDurationMode::Normal;
  uint8_t brightness = 160;

  // Interval of the RFID reader background polling
  AutoNfcMode autoNfc = AutoNfcMode::Normal;
  // How long to wait for the confirmation after a PowerOff card
  // command (seconds). Within that time the card must be tapped again
  // or a button pressed.
  uint8_t cardConfirmS = 8;
};

struct JoyState {
  bool present = false;
  int8_t x = 0;
  int8_t y = 0;
  bool button = false;
  uint8_t failCount = 0;

  JoyDir dir = JoyDir::Center;
  JoyDir lastDir = JoyDir::Center;

  bool buttonLast = false;
};

struct AppState {
  ScreenMode screen = ScreenMode::Home;
  int menuIndex = 0;
  int cpuIndex = 0;
  int displaySettingsIndex = 0;
  int nfcMenuIndex = 0;
  int wifiMenuIndex = 0;
  int wifiSavedIndex = 0;
  int cmdIndex = 0;

  String cpuCategory;
  String cpuItem;
  String currentCpuValue = "Unknown";
  bool cpuPathKnown = false;
  String cpuWireOptions[kMaxCpuChoices];
  String cpuDisplayOptions[kMaxCpuChoices];
  size_t cpuChoiceCount = 0;

  bool configReady = false;
  ConnectionState connection = {};

  String modalText;
  uint16_t modalColor = TFT_WHITE;
  uint32_t modalUntilMs = 0;
  bool lastModalVisible = false;

  uint32_t lastWiFiAttemptMs = 0;
  uint32_t lastConnectionProbeMs = 0;

  bool pendingSoftReset = false;
  uint32_t pendingSoftResetAtMs = 0;

  bool pendingPowerOff = false;
  uint32_t pendingPowerOffAtMs = 0;

  // ---- RFID / NFC -------------------------------------------------------
  bool     rfidReady   = false;          // Reader found at startup
  String   rfidHint    = "";             // Text on the card screen
  String   pendingCardText;              // what should go on the card when writing
  String   lastCardUid;                  // last seen card
  uint32_t lastCardMs  = 0;
  uint32_t lastRfidPollMs = 0;

  // Pending prompt of a PowerOff card command
  bool     cardPowerOffPending = false;
  uint32_t cardPowerOffUntilMs = 0;
  String   cardPowerOffUid;

  // ---- WiFi -------------------------------------------------------------
  bool     wifiSavedDelete = false;      // List in delete mode
  bool     portalActive    = false;
  uint32_t portalStartedMs = 0;
  uint32_t portalSavedMs   = 0;
  String   wifiPendingSsid;              // Network whose connection we are waiting for
  uint32_t wifiPendingMs   = 0;

  HomeDemoState home = {};
  JoyState joy = {};
  SettingsState settings = {};
} app;

// ------------------------------------------------------------
// Graphics / logo
// ------------------------------------------------------------
M5Canvas canvas(&M5.Display);
Preferences prefs;

// RFID2 sits on the second I2C bus (Grove port). The driver gets its
// instance assigned in initRfid().
MFRC522_I2C rfid(kRfidAddr, -1, &Wire1);

// Setup portal
WebServer gPortal(80);
DNSServer gPortalDns;

// Saved networks and target data (NVS)
WifiProfile gWifiProfiles[kWifiProfileMax];
size_t      gWifiCount = 0;
size_t      gWifiTry   = 0;      // which network is tried next
String      gTargetHost;
String      gTargetPass;
bool miniJoyReady = false;
uint16_t* plainLogoPixels = nullptr;
uint16_t* boxedLogoPixels = nullptr;
uint16_t logoRow[240] = {};

int homeInnerX() { return 22; }
int homeInnerY() { return 20; }
int homeInnerW() { return 196; }
int homeInnerH() { return 95; }

// ------------------------------------------------------------
// String / config helpers
// ------------------------------------------------------------
String trimCopy(const String& value) {
  String result = value;
  result.trim();
  return result;
}

String configString(const char* value) {
  return trimCopy(value == nullptr ? "" : value);
}

// Since WiFi setup moved into the device, the build_env.h values are
// only INITIAL VALUES: they apply until something is stored in NVS
// for the first time (WiFi card, setup portal).
const String& buildWifiSsid() { static const String v = configString(C64U_WIFI_SSID);     return v; }
const String& buildWifiPass() { static const String v = configString(C64U_WIFI_PASSWORD); return v; }
const String& buildHost()     { static const String v = configString(C64U_TARGET_HOST);   return v; }
const String& buildHostPass() { static const String v = configString(C64U_TARGET_PASSWORD); return v; }

const String& targetHost()     { return gTargetHost; }
const String& targetPassword() { return gTargetPass; }

bool hasWiFiConfig()   { return gWifiCount > 0; }
bool hasTargetConfig() { return !gTargetHost.isEmpty(); }
bool configReady()     { return hasWiFiConfig() && hasTargetConfig(); }

// ------------------------------------------------------------
// Colors / graphics
// ------------------------------------------------------------
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

uint16_t blend565(uint16_t from, uint16_t to, float t) {
  t = std::max(0.0f, std::min(1.0f, t));
  const uint8_t fromR = ((from >> 11) & 0x1Fu) << 3;
  const uint8_t fromG = ((from >> 5) & 0x3Fu) << 2;
  const uint8_t fromB = (from & 0x1Fu) << 3;
  const uint8_t toR = ((to >> 11) & 0x1Fu) << 3;
  const uint8_t toG = ((to >> 5) & 0x3Fu) << 2;
  const uint8_t toB = (to & 0x1Fu) << 3;
  return rgb565(static_cast<uint8_t>(fromR + (toR - fromR) * t),
                static_cast<uint8_t>(fromG + (toG - fromG) * t),
                static_cast<uint8_t>(fromB + (toB - fromB) * t));
}

void fitHeightRect(int srcW, int srcH, int dstX, int dstY, int dstW, int dstH,
                   int* outX, int* outY, int* outW, int* outH) {
  *outH = dstH;
  *outW = std::max(1, static_cast<int>((static_cast<float>(srcW) * static_cast<float>(dstH)) /
                                       static_cast<float>(srcH)));
  *outX = dstX + (dstW - *outW) / 2;
  *outY = dstY;
}

uint16_t logoSourcePixel(int x, int y) {
  x = std::max(0, std::min(kLogoSrcW - 1, x));
  y = std::max(0, std::min(kLogoSrcH - 1, y));
  return pgm_read_word(&commodore_logo_rgb565[y * kLogoSrcW + x]);
}

void fillPixels(uint16_t* pixels, int width, int height, uint16_t color) {
  for (int i = 0; i < width * height; ++i) {
    pixels[i] = color;
  }
}

void fillRectPixels(uint16_t* pixels, int width, int height, int x, int y, int w, int h, uint16_t color) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(width, x + w);
  const int y1 = std::min(height, y + h);

  for (int yy = y0; yy < y1; ++yy) {
    uint16_t* row = pixels + yy * width;
    for (int xx = x0; xx < x1; ++xx) {
      row[xx] = color;
    }
  }
}

void drawLogoFitHeightToPixels(uint16_t* pixels, int width, int height, int dstX, int dstY, int dstW, int dstH) {
  int fitX = 0, fitY = 0, fitW = 0, fitH = 0;
  fitHeightRect(kLogoSrcW, kLogoSrcH, dstX, dstY, dstW, dstH, &fitX, &fitY, &fitW, &fitH);

  for (int y = 0; y < fitH; ++y) {
    const int dstRow = fitY + y;
    const int srcY = std::min(kLogoSrcH - 1, (y * kLogoSrcH) / fitH);
    uint16_t* row = pixels + dstRow * width;

    for (int x = 0; x < fitW; ++x) {
      const int srcX = std::min(kLogoSrcW - 1, (x * kLogoSrcW) / fitW);
      row[fitX + x] = logoSourcePixel(srcX, srcY);
    }
  }
}

// ------------------------------------------------------------
// Logo cache
//
// Two full frames of 240*135*2 = 64 kB each, in PSRAM where possible.
// They are released while the setup portal runs (access point and web
// server need the internal heap) and created again afterwards.
// ------------------------------------------------------------
void releaseLogoCache() {
  if (plainLogoPixels != nullptr) { free(plainLogoPixels); plainLogoPixels = nullptr; }
  if (boxedLogoPixels != nullptr) { free(boxedLogoPixels); boxedLogoPixels = nullptr; }
}

// Request a full frame: the ESP32-PICO-V3-02 in the StickC Plus2 has 2 MB
// PSRAM, the best place for the cache. If PSRAM is not active
// (BOARD_HAS_PSRAM missing in the build), internal memory is used.
uint16_t* allocFrameBuffer(size_t bytes) {
  uint16_t* buffer = nullptr;
  if (psramFound()) buffer = static_cast<uint16_t*>(ps_malloc(bytes));
  if (buffer == nullptr) buffer = static_cast<uint16_t*>(malloc(bytes));
  return buffer;
}

bool ensureLogoCache() {
  if (plainLogoPixels != nullptr && boxedLogoPixels != nullptr) return true;

  const int width  = M5.Display.width();
  const int height = M5.Display.height();
  const size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) *
                            sizeof(uint16_t);

  if (plainLogoPixels == nullptr) plainLogoPixels = allocFrameBuffer(frameBytes);
  if (boxedLogoPixels == nullptr) boxedLogoPixels = allocFrameBuffer(frameBytes);

  if (plainLogoPixels == nullptr || boxedLogoPixels == nullptr) {
    releaseLogoCache();
    Serial.printf("logo cache alloc failed (%u bytes each, free: %u)\n",
                  static_cast<unsigned>(frameBytes),
                  static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }

  fillPixels(plainLogoPixels, width, height, TFT_WHITE);
  drawLogoFitHeightToPixels(plainLogoPixels, width, height, 0, 0, width, height);

  fillPixels(boxedLogoPixels, width, height, TFT_WHITE);
  fillRectPixels(boxedLogoPixels, width, height,
                 homeInnerX(), homeInnerY(), homeInnerW(), homeInnerH(), TFT_WHITE);
  drawLogoFitHeightToPixels(boxedLogoPixels, width, height,
                            homeInnerX(), homeInnerY(), homeInnerW(), homeInnerH());
  return true;
}

// ------------------------------------------------------------
// Small state helpers
// ------------------------------------------------------------
void clearPendingPowerOff() {
  app.pendingPowerOff = false;
}

void setModal(const String& text, uint16_t color, uint32_t now, uint32_t durationMs = kModalMs) {
  app.modalText = text;
  app.modalColor = color;
  app.modalUntilMs = now + durationMs;
  app.home.frameDirty = true;
}

// Forward declarations
void setScreenMode(ScreenMode nextScreen, uint32_t now);
void handleMenuSelect(uint32_t now);
void requestPowerOff(uint32_t now);
void enterHomeMode(HomeMode mode, uint32_t now);
void updateMiniJoyStatusLed();
void moveSelection(int delta);
void goBack(uint32_t now);
void activateCurrent(uint32_t now);
void openNfcMenu(uint32_t now);
void openWifiMenu(uint32_t now);

const char* displayEffectModeLabel(DisplayEffectMode mode) {
  switch (mode) {
    case DisplayEffectMode::Auto: return "Auto";
    case DisplayEffectMode::Static: return "Static";
    case DisplayEffectMode::Water: return "Water";
    case DisplayEffectMode::RotoZoom: return "RotoZoom";
    case DisplayEffectMode::SineWave: return "SineWave";
    case DisplayEffectMode::RippleBump: return "Ripple";
    case DisplayEffectMode::RasterBars: return "RasterBars";
  }
  return "Auto";
}

const char* animationSpeedLabel(AnimationSpeedMode mode) {
  switch (mode) {
    case AnimationSpeedMode::Slow: return "Slow";
    case AnimationSpeedMode::Normal: return "Normal";
    case AnimationSpeedMode::Fast: return "Fast";
  }
  return "Normal";
}

const char* effectDurationLabel(EffectDurationMode mode) {
  switch (mode) {
    case EffectDurationMode::Short: return "Short";
    case EffectDurationMode::Normal: return "Normal";
    case EffectDurationMode::Long: return "Long";
  }
  return "Normal";
}

const char* staticDurationLabel(StaticDurationMode mode) {
  switch (mode) {
    case StaticDurationMode::Short: return "Short";
    case StaticDurationMode::Normal: return "Normal";
    case StaticDurationMode::Long: return "Long";
  }
  return "Normal";
}

const char* joyThresholdLabel(JoyThresholdMode mode) {
  switch (mode) {
    case JoyThresholdMode::Threshold40: return "40";
    case JoyThresholdMode::Threshold60: return "60";
    case JoyThresholdMode::Threshold80: return "80";
    case JoyThresholdMode::Threshold100: return "100";
  }
  return "100";
}

const char* autoNfcLabel(AutoNfcMode mode) {
  switch (mode) {
    case AutoNfcMode::Off:    return "Off";
    case AutoNfcMode::Slow:   return "1.5s";
    case AutoNfcMode::Normal: return "0.7s";
    case AutoNfcMode::Fast:   return "0.3s";
  }
  return "Off";
}

uint32_t autoNfcIntervalMs(AutoNfcMode mode) {
  switch (mode) {
    case AutoNfcMode::Slow:   return 1500;
    case AutoNfcMode::Normal: return 700;
    case AutoNfcMode::Fast:   return 300;
    case AutoNfcMode::Off:
    default:                  return 0;
  }
}

// Prompt time of a PowerOff command card
constexpr uint8_t kCardConfirmSteps[] = {3, 5, 8, 15};
constexpr size_t  kCardConfirmCount = sizeof(kCardConfirmSteps) / sizeof(kCardConfirmSteps[0]);

uint8_t nextCardConfirm(uint8_t current) {
  for (size_t i = 0; i < kCardConfirmCount; ++i) {
    if (current == kCardConfirmSteps[i]) return kCardConfirmSteps[(i + 1) % kCardConfirmCount];
  }
  return kCardConfirmSteps[0];
}

String cardConfirmLabel(uint8_t seconds) { return String(seconds) + "s"; }

int joyThresholdValue(JoyThresholdMode mode) {
  switch (mode) {
    case JoyThresholdMode::Threshold40: return 40;
    case JoyThresholdMode::Threshold60: return 60;
    case JoyThresholdMode::Threshold80: return 80;
    case JoyThresholdMode::Threshold100: return 100;
  }
  return 100;
}

float animationSpeedFactor(AnimationSpeedMode mode) {
  switch (mode) {
    case AnimationSpeedMode::Slow: return 0.65f;
    case AnimationSpeedMode::Fast: return 1.45f;
    case AnimationSpeedMode::Normal:
    default: return 1.0f;
  }
}

float effectDurationFactor(EffectDurationMode mode) {
  switch (mode) {
    case EffectDurationMode::Short: return 0.60f;
    case EffectDurationMode::Long: return 1.60f;
    case EffectDurationMode::Normal:
    default: return 1.0f;
  }
}
// Time settings for the static logo
float staticDurationFactor(StaticDurationMode mode) {
  switch (mode) {
    case StaticDurationMode::Short: return 0.60f;
    case StaticDurationMode::Long: return 3.60f;
    case StaticDurationMode::Normal:
    default: return 1.0f;
  }
}

HomeMode homeModeFromEffect(DisplayEffectMode mode) {
  switch (mode) {
    case DisplayEffectMode::Water: return HomeMode::Water;
    case DisplayEffectMode::RotoZoom: return HomeMode::RotoZoom;
    case DisplayEffectMode::SineWave: return HomeMode::SineWave;
    case DisplayEffectMode::RippleBump: return HomeMode::RippleBump;
    case DisplayEffectMode::RasterBars: return HomeMode::RasterBars;
    case DisplayEffectMode::Static:
    case DisplayEffectMode::Auto:
    default: return HomeMode::Static;
  }
}

bool displayUsesAutoCycle() {
  return app.settings.animationsEnabled && app.settings.effectMode == DisplayEffectMode::Auto;
}

bool displayUsesAlternatingCycle() {
  return app.settings.animationsEnabled && app.settings.effectMode != DisplayEffectMode::Static;
}

HomeMode selectedCycleEffect() {
  if (app.settings.effectMode == DisplayEffectMode::Auto) {
    switch (app.home.nextEffectIndex % 5u) {
      case 0: return HomeMode::Water;
      case 1: return HomeMode::RotoZoom;
      case 2: return HomeMode::SineWave;
      case 3: return HomeMode::RippleBump;
      default: return HomeMode::RasterBars;
    }
  }
  return homeModeFromEffect(app.settings.effectMode);
}

HomeMode currentConfiguredHomeMode() {
  if (!app.settings.animationsEnabled) return HomeMode::Static;
  if (app.settings.effectMode == DisplayEffectMode::Static) return HomeMode::Static;
  return app.home.mode;
}

uint8_t nextBrightnessValue(uint8_t current) {
  static const uint8_t kBrightnessLevels[] = {32, 64, 96, 128, 160, 192, 224, 255};
  const size_t count = sizeof(kBrightnessLevels) / sizeof(kBrightnessLevels[0]);
  for (size_t i = 0; i < count; ++i) {
    if (current < kBrightnessLevels[i]) return kBrightnessLevels[i];
    if (current == kBrightnessLevels[i]) {
      return kBrightnessLevels[(i + 1) % count];
    }
  }
  return kBrightnessLevels[0];
}

String brightnessLabel(uint8_t brightness) {
  return String(brightness);
}

uint8_t nextJoyLedBrightnessValue(uint8_t current) {
  static const uint8_t kLedBrightnessLevels[] = {16, 32, 48, 64, 96, 128, 160, 192, 255};
  const size_t count = sizeof(kLedBrightnessLevels) / sizeof(kLedBrightnessLevels[0]);
  for (size_t i = 0; i < count; ++i) {
    if (current < kLedBrightnessLevels[i]) return kLedBrightnessLevels[i];
    if (current == kLedBrightnessLevels[i]) {
      return kLedBrightnessLevels[(i + 1) % count];
    }
  }
  return kLedBrightnessLevels[0];
}

String joyLedBrightnessLabel(uint8_t brightness) {
  return String(brightness);
}

void applyBrightness() {
  M5.Display.setBrightness(app.settings.brightness);
}

void loadDefaultDisplaySettings() {
  app.settings.animationsEnabled = true;
  app.settings.effectMode = DisplayEffectMode::Auto;
  app.settings.joyOverlayEnabled = true;
  app.settings.joyLedEnabled = true;
  app.settings.joyLedBrightness = 96;
  app.settings.joyThreshold = JoyThresholdMode::Threshold100;
  app.settings.animationSpeed = AnimationSpeedMode::Normal;
  app.settings.effectDuration = EffectDurationMode::Normal;
  app.settings.staticDuration = StaticDurationMode::Normal;
  app.settings.brightness = 160;
  app.settings.autoNfc = AutoNfcMode::Normal;
  app.settings.cardConfirmS = 8;
}

void saveDisplaySettings() {
  prefs.begin("c64uremote", false);
  prefs.putBool("anim_on", app.settings.animationsEnabled);
  prefs.putUChar("fx_mode", static_cast<uint8_t>(app.settings.effectMode));
  prefs.putBool("joy_ovl", app.settings.joyOverlayEnabled);
  prefs.putBool("joy_led", app.settings.joyLedEnabled);
  prefs.putUChar("joy_led_br", app.settings.joyLedBrightness);
  prefs.putUChar("joy_thr", static_cast<uint8_t>(app.settings.joyThreshold));
  prefs.putUChar("anim_spd", static_cast<uint8_t>(app.settings.animationSpeed));
  prefs.putUChar("fx_time", static_cast<uint8_t>(app.settings.effectDuration));
  prefs.putUChar("st_time", static_cast<uint8_t>(app.settings.staticDuration));
  prefs.putUChar("bright", app.settings.brightness);
  prefs.putUChar("auto_nfc", static_cast<uint8_t>(app.settings.autoNfc));
  prefs.putUChar("card_cnf", app.settings.cardConfirmS);
  prefs.end();
}

void loadDisplaySettings() {
  loadDefaultDisplaySettings();

  prefs.begin("c64uremote", true);
  app.settings.animationsEnabled = prefs.getBool("anim_on", app.settings.animationsEnabled);
  const uint8_t rawEffect = prefs.getUChar("fx_mode", static_cast<uint8_t>(app.settings.effectMode));
  app.settings.joyOverlayEnabled = prefs.getBool("joy_ovl", app.settings.joyOverlayEnabled);
  app.settings.joyLedEnabled = prefs.getBool("joy_led", app.settings.joyLedEnabled);
  app.settings.joyLedBrightness = prefs.getUChar("joy_led_br", app.settings.joyLedBrightness);
  const uint8_t rawJoyThreshold = prefs.getUChar("joy_thr", static_cast<uint8_t>(app.settings.joyThreshold));
  const uint8_t rawAnimSpeed = prefs.getUChar("anim_spd", static_cast<uint8_t>(app.settings.animationSpeed));
  const uint8_t rawEffectTime = prefs.getUChar("fx_time", static_cast<uint8_t>(app.settings.effectDuration));
  const uint8_t rawStaticTime = prefs.getUChar("st_time", static_cast<uint8_t>(app.settings.staticDuration));
  const uint8_t rawBrightness = prefs.getUChar("bright", app.settings.brightness);
  const uint8_t rawAutoNfc = prefs.getUChar("auto_nfc", static_cast<uint8_t>(app.settings.autoNfc));
  app.settings.cardConfirmS = prefs.getUChar("card_cnf", app.settings.cardConfirmS);
  prefs.end();

  if (rawAutoNfc <= static_cast<uint8_t>(AutoNfcMode::Fast)) {
    app.settings.autoNfc = static_cast<AutoNfcMode>(rawAutoNfc);
  }
  if (app.settings.cardConfirmS < kCardConfirmSteps[0] ||
      app.settings.cardConfirmS > kCardConfirmSteps[kCardConfirmCount - 1]) {
    app.settings.cardConfirmS = 8;
  }

  if (rawEffect <= static_cast<uint8_t>(DisplayEffectMode::RasterBars)) {
    app.settings.effectMode = static_cast<DisplayEffectMode>(rawEffect);
  }
  if (rawJoyThreshold <= static_cast<uint8_t>(JoyThresholdMode::Threshold100)) {
    app.settings.joyThreshold = static_cast<JoyThresholdMode>(rawJoyThreshold);
  }
  if (rawAnimSpeed <= static_cast<uint8_t>(AnimationSpeedMode::Fast)) {
    app.settings.animationSpeed = static_cast<AnimationSpeedMode>(rawAnimSpeed);
  }
  if (rawEffectTime <= static_cast<uint8_t>(EffectDurationMode::Long)) {
    app.settings.effectDuration = static_cast<EffectDurationMode>(rawEffectTime);
  }
  if (rawStaticTime <= static_cast<uint8_t>(StaticDurationMode::Long)) {
    app.settings.staticDuration = static_cast<StaticDurationMode>(rawStaticTime);
  }

  app.settings.joyLedBrightness = std::max<uint8_t>(16, app.settings.joyLedBrightness);
  app.settings.brightness = std::max<uint8_t>(32, rawBrightness);
}

void resetHomeAnimation(uint32_t now) {
  app.home.nextEffectIndex = 0;
  enterHomeMode(HomeMode::Static, now);
}

void applyDisplaySettingChange(uint32_t now, const String& modalText) {
  saveDisplaySettings();
  resetHomeAnimation(now);
  setModal(modalText, rgb565(110, 230, 170), now, 1100);
  app.home.frameDirty = true;
}

void activateDisplaySetting(uint32_t now) {
  switch (app.displaySettingsIndex) {
    case kSetAutoNfc: {
      uint8_t next = static_cast<uint8_t>(app.settings.autoNfc) + 1;
      if (next > static_cast<uint8_t>(AutoNfcMode::Fast)) next = 0;
      app.settings.autoNfc = static_cast<AutoNfcMode>(next);
      applyDisplaySettingChange(now, String("AUTO-NFC ") + autoNfcLabel(app.settings.autoNfc));
      break;
    }

    case kSetCardConfirm:
      app.settings.cardConfirmS = nextCardConfirm(app.settings.cardConfirmS);
      applyDisplaySettingChange(now, "POWEROFF " + cardConfirmLabel(app.settings.cardConfirmS));
      break;

    case kSetAnimations:
      app.settings.animationsEnabled = !app.settings.animationsEnabled;
      applyDisplaySettingChange(now, app.settings.animationsEnabled ? "ANIMATION ON" : "ANIMATION OFF");
      break;

    case kSetEffect: {
      uint8_t next = static_cast<uint8_t>(app.settings.effectMode) + 1;
      if (next > static_cast<uint8_t>(DisplayEffectMode::RasterBars)) next = 0;
      app.settings.effectMode = static_cast<DisplayEffectMode>(next);
      applyDisplaySettingChange(now, String("EFFECT ") + displayEffectModeLabel(app.settings.effectMode));
      break;
    }

    case kSetJoyOverlay:
      app.settings.joyOverlayEnabled = !app.settings.joyOverlayEnabled;
      applyDisplaySettingChange(now, app.settings.joyOverlayEnabled ? "OVERLAY ON" : "OVERLAY OFF");
      break;

    case kSetJoyLed:
      app.settings.joyLedEnabled = !app.settings.joyLedEnabled;
      updateMiniJoyStatusLed();
      applyDisplaySettingChange(now, app.settings.joyLedEnabled ? "JOY LED ON" : "JOY LED OFF");
      break;

    case kSetJoyLedBright:
      app.settings.joyLedBrightness = nextJoyLedBrightnessValue(app.settings.joyLedBrightness);
      updateMiniJoyStatusLed();
      applyDisplaySettingChange(now, String("LED ") + joyLedBrightnessLabel(app.settings.joyLedBrightness));
      break;

    case kSetJoyThreshold: {
      uint8_t next = static_cast<uint8_t>(app.settings.joyThreshold) + 1;
      if (next > static_cast<uint8_t>(JoyThresholdMode::Threshold100)) next = 0;
      app.settings.joyThreshold = static_cast<JoyThresholdMode>(next);
      applyDisplaySettingChange(now, String("JOY ") + joyThresholdLabel(app.settings.joyThreshold));
      break;
    }

    case kSetAnimSpeed: {
      uint8_t next = static_cast<uint8_t>(app.settings.animationSpeed) + 1;
      if (next > static_cast<uint8_t>(AnimationSpeedMode::Fast)) next = 0;
      app.settings.animationSpeed = static_cast<AnimationSpeedMode>(next);
      applyDisplaySettingChange(now, String("SPEED ") + animationSpeedLabel(app.settings.animationSpeed));
      break;
    }

    case kSetEffectTime: {
      uint8_t next = static_cast<uint8_t>(app.settings.effectDuration) + 1;
      if (next > static_cast<uint8_t>(EffectDurationMode::Long)) next = 0;
      app.settings.effectDuration = static_cast<EffectDurationMode>(next);
      applyDisplaySettingChange(now, String("TIME ") + effectDurationLabel(app.settings.effectDuration));
      break;
    }

    case kSetStaticTime: {
      uint8_t next = static_cast<uint8_t>(app.settings.staticDuration) + 1;
      if (next > static_cast<uint8_t>(StaticDurationMode::Long)) next = 0;
      app.settings.staticDuration = static_cast<StaticDurationMode>(next);
      applyDisplaySettingChange(now, String("STATIC ") + staticDurationLabel(app.settings.staticDuration));
      break;
    }

    case kSetBrightness:
      app.settings.brightness = nextBrightnessValue(app.settings.brightness);
      applyBrightness();
      applyDisplaySettingChange(now, String("BRIGHT ") + brightnessLabel(app.settings.brightness));
      break;

    case kSetFactoryReset:
      loadDefaultDisplaySettings();
      applyBrightness();
      applyDisplaySettingChange(now, "FACTORY RESET");
      break;

    default:
      break;
  }
}

// ------------------------------------------------------------
// URL encoding / JSON helpers
// ------------------------------------------------------------
String urlEncode(const String& value) {
  static const char* hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);

  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

String apiBaseUrl() {
  return String("http://") + targetHost();
}

String extractErrors(DynamicJsonDocument& doc) {
  if (!doc.containsKey("errors")) {
    return "";
  }

  String text;
  JsonArray errors = doc["errors"].as<JsonArray>();
  for (JsonVariant value : errors) {
    if (!text.isEmpty()) {
      text += ", ";
    }
    text += value.as<const char*>();
  }
  return text;
}

String jsonValueToString(JsonVariantConst value) {
  if (value.is<const char*>()) return String(value.as<const char*>());
  if (value.is<String>()) return value.as<String>();
  if (value.is<long>()) return String(value.as<long>());
  if (value.is<int>()) return String(value.as<int>());
  if (value.is<bool>()) return value.as<bool>() ? "Yes" : "No";

  String text;
  serializeJson(value, text);
  return text;
}

String extractDigits(const String& value) {
  String digits;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      digits += c;
    }
  }
  return digits;
}

String cpuLabelFromValue(const String& rawValue) {
  const String trimmed = trimCopy(rawValue);
  const String digits = extractDigits(trimmed);

  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    if (trimCopy(app.cpuWireOptions[i]) == trimmed ||
        extractDigits(app.cpuWireOptions[i]) == digits ||
        trimCopy(app.cpuDisplayOptions[i]).equalsIgnoreCase(trimmed)) {
      return app.cpuDisplayOptions[i];
    }
  }

  if (!digits.isEmpty()) return digits + " MHz";
  return trimmed.isEmpty() ? "Unknown" : trimmed;
}

int cpuIndexFromValue(const String& rawValue) {
  const String trimmed = trimCopy(rawValue);
  const String digits = extractDigits(trimmed);

  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    if (trimCopy(app.cpuWireOptions[i]) == trimmed ||
        extractDigits(app.cpuWireOptions[i]) == digits ||
        trimCopy(app.cpuDisplayOptions[i]).equalsIgnoreCase(trimmed)) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void setFallbackCpuChoices() {
  static const char* fallback[] = {" 1", " 2", " 3", " 4", " 6", " 8", "10", "12",
                                   "14", "16", "20", "24", "32", "40", "48", "64"};
  app.cpuChoiceCount = std::min(kMaxCpuChoices, sizeof(fallback) / sizeof(fallback[0]));
  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    app.cpuWireOptions[i] = fallback[i];
    app.cpuDisplayOptions[i] = trimCopy(fallback[i]) + " MHz";
  }
}

// ------------------------------------------------------------
// REST request
// ------------------------------------------------------------
ApiResponse sendApiRequest(const char* method, const String& path, bool authenticated) {
  ApiResponse result;

  if (!hasTargetConfig()) {
    result.errors = "Target host missing";
    return result;
  }

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  const String url = apiBaseUrl() + path;

  if (!http.begin(url)) {
    result.errors = "HTTP begin failed";
    return result;
  }

  if (authenticated && !targetPassword().isEmpty()) {
    http.addHeader("X-Password", targetPassword());
  }

  if (strcmp(method, "GET") == 0) {
    result.httpCode = http.GET();
  } else if (strcmp(method, "PUT") == 0) {
    result.httpCode = http.sendRequest("PUT", "");
  } else {
    http.end();
    result.errors = "Unsupported method";
    return result;
  }

  result.transportOk = result.httpCode > 0;

  if (result.transportOk) {
    result.body = http.getString();

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, result.body) == DeserializationError::Ok) {
      result.jsonOk = true;
      result.errors = extractErrors(doc);
      result.apiOk = result.httpCode >= 200 && result.httpCode < 300 && result.errors.isEmpty();
    } else {
      result.apiOk = result.httpCode >= 200 && result.httpCode < 300;
    }
  } else {
    result.errors = http.errorToString(result.httpCode);
  }

  http.end();
  return result;
}

// ------------------------------------------------------------
// CPU speed query
// ------------------------------------------------------------
bool inspectCpuCategory(const String& category, String* itemOut, String* valueOut) {
  const ApiResponse response = sendApiRequest("GET", "/v1/configs/" + urlEncode(category), true);
  if (!response.apiOk) return false;

  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) return false;

  JsonVariant categoryObject = doc[category];
  if (categoryObject.isNull()) {
    for (JsonPair kv : doc.as<JsonObject>()) {
      if (String(kv.key().c_str()) != "errors" && kv.value().is<JsonObject>()) {
        categoryObject = kv.value();
        break;
      }
    }
  }

  if (categoryObject.isNull() || !categoryObject.is<JsonObject>()) return false;

  for (JsonPair kv : categoryObject.as<JsonObject>()) {
    const String key = kv.key().c_str();
    const String upper = key;
    if (upper.indexOf("CPU") >= 0 && upper.indexOf("Speed") >= 0) {
      *itemOut = key;
      *valueOut = jsonValueToString(kv.value());
      return true;
    }
  }

  return false;
}

bool refreshCpuChoices() {
  if (app.cpuCategory.isEmpty() || app.cpuItem.isEmpty()) {
    setFallbackCpuChoices();
    return false;
  }

  const ApiResponse response =
      sendApiRequest("GET", "/v1/configs/" + urlEncode(app.cpuCategory) + "/" + urlEncode(app.cpuItem), true);
  if (!response.apiOk) {
    setFallbackCpuChoices();
    return false;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) {
    setFallbackCpuChoices();
    return false;
  }

  JsonVariant itemObject = doc[app.cpuCategory][app.cpuItem];
  if (itemObject.isNull()) {
    setFallbackCpuChoices();
    return false;
  }

  app.cpuChoiceCount = 0;
  JsonArray values = itemObject["values"].as<JsonArray>();

  for (JsonVariant value : values) {
    if (app.cpuChoiceCount >= kMaxCpuChoices) break;

    const String wire = jsonValueToString(value);
    const String digits = extractDigits(wire);
    app.cpuWireOptions[app.cpuChoiceCount] = wire;
    app.cpuDisplayOptions[app.cpuChoiceCount] = digits.isEmpty() ? trimCopy(wire) : digits + " MHz";
    app.cpuChoiceCount += 1;
  }

  if (app.cpuChoiceCount == 0) {
    setFallbackCpuChoices();
    return false;
  }

  app.currentCpuValue = cpuLabelFromValue(jsonValueToString(itemObject["current"]));
  return true;
}

bool resolveCpuPath(String* detailOut = nullptr) {
  if (app.cpuPathKnown) {
    if (app.cpuChoiceCount == 0) refreshCpuChoices();
    return true;
  }

  String item;
  String value;

  if (inspectCpuCategory("U64 Specific Settings", &item, &value)) {
    app.cpuCategory = "U64 Specific Settings";
    app.cpuItem = item;
    app.currentCpuValue = cpuLabelFromValue(value);
    app.cpuPathKnown = true;
    refreshCpuChoices();
    return true;
  }

  const ApiResponse listResponse = sendApiRequest("GET", "/v1/configs", true);
  if (!listResponse.apiOk) {
    if (detailOut != nullptr) {
      *detailOut = listResponse.errors.isEmpty() ? "Config list failed" : listResponse.errors;
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, listResponse.body) != DeserializationError::Ok) {
    if (detailOut != nullptr) *detailOut = "Config list parse failed";
    return false;
  }

  JsonArray categories = doc["categories"].as<JsonArray>();
  for (JsonVariant valueVariant : categories) {
    const String category = valueVariant.as<const char*>();
    if (inspectCpuCategory(category, &item, &value)) {
      app.cpuCategory = category;
      app.cpuItem = item;
      app.currentCpuValue = cpuLabelFromValue(value);
      app.cpuPathKnown = true;
      refreshCpuChoices();
      return true;
    }
  }

  if (detailOut != nullptr) *detailOut = "CPU speed item not found";
  return false;
}

void refreshCpuValue() {
  String detail;
  if (!resolveCpuPath(&detail)) {
    app.currentCpuValue = detail;
    return;
  }
  refreshCpuChoices();
}


// ============================================================================
//  Network configuration in internal memory (NVS)
// ----------------------------------------------------------------------------
//  Up to four WiFi credentials plus address and password of the c64u. On the first
//  boot (nothing stored yet) the values come from build_env.h.
// ============================================================================
void beginWiFi(uint32_t now);
void refreshConnectionStatus(uint32_t now, bool force);

void saveNetConfig() {
  prefs.begin("c64unet", false);
  prefs.putString("host", gTargetHost);
  prefs.putString("hostpw", gTargetPass);
  prefs.putUChar("wcount", static_cast<uint8_t>(gWifiCount));
  for (size_t i = 0; i < kWifiProfileMax; ++i) {
    char keyS[8];
    char keyP[8];
    snprintf(keyS, sizeof(keyS), "s%u", static_cast<unsigned>(i));
    snprintf(keyP, sizeof(keyP), "p%u", static_cast<unsigned>(i));
    if (i < gWifiCount) {
      prefs.putString(keyS, gWifiProfiles[i].ssid);
      prefs.putString(keyP, gWifiProfiles[i].pass);
    } else {
      prefs.remove(keyS);
      prefs.remove(keyP);
    }
  }
  prefs.end();
}

void loadNetConfig() {
  prefs.begin("c64unet", true);
  const bool known = prefs.isKey("wcount");
  gTargetHost = trimCopy(prefs.getString("host", buildHost()));
  gTargetPass = prefs.getString("hostpw", buildHostPass());

  gWifiCount = 0;
  if (known) {
    const uint8_t count = std::min<uint8_t>(prefs.getUChar("wcount", 0),
                                            static_cast<uint8_t>(kWifiProfileMax));
    for (uint8_t i = 0; i < count; ++i) {
      char keyS[8];
      char keyP[8];
      snprintf(keyS, sizeof(keyS), "s%u", static_cast<unsigned>(i));
      snprintf(keyP, sizeof(keyP), "p%u", static_cast<unsigned>(i));
      const String ssid = trimCopy(prefs.getString(keyS, ""));
      if (ssid.isEmpty()) continue;
      gWifiProfiles[gWifiCount].ssid = ssid;
      gWifiProfiles[gWifiCount].pass = prefs.getString(keyP, "");
      ++gWifiCount;
    }
  }
  prefs.end();

  // Nothing stored yet -> take the initial values from build_env.h.
  if (!known && !buildWifiSsid().isEmpty()) {
    gWifiProfiles[0].ssid = buildWifiSsid();
    gWifiProfiles[0].pass = buildWifiPass();
    gWifiCount = 1;
  }
}

// Index of an already saved network, otherwise -1
int wifiProfileIndex(const String& ssid) {
  const String needle = trimCopy(ssid);
  for (size_t i = 0; i < gWifiCount; ++i) {
    if (gWifiProfiles[i].ssid.equalsIgnoreCase(needle)) return static_cast<int>(i);
  }
  return -1;
}

// Add or update a network. If the list is full, the oldest
// entry drops out - so the recently used networks stay in front.
bool wifiAddProfile(const String& ssidRaw, const String& pass, bool store = true) {
  const String ssid = trimCopy(ssidRaw);
  if (ssid.isEmpty()) return false;

  const int existing = wifiProfileIndex(ssid);
  if (existing >= 0) {
    gWifiProfiles[existing].pass = pass;
    // move to the front
    for (int i = existing; i > 0; --i) std::swap(gWifiProfiles[i], gWifiProfiles[i - 1]);
  } else {
    if (gWifiCount < kWifiProfileMax) ++gWifiCount;
    for (size_t i = gWifiCount - 1; i > 0; --i) gWifiProfiles[i] = gWifiProfiles[i - 1];
    gWifiProfiles[0].ssid = ssid;
    gWifiProfiles[0].pass = pass;
  }

  if (store) saveNetConfig();
  return true;
}

bool wifiRemoveProfile(size_t index) {
  if (index >= gWifiCount) return false;
  for (size_t i = index; i + 1 < gWifiCount; ++i) gWifiProfiles[i] = gWifiProfiles[i + 1];
  gWifiProfiles[gWifiCount - 1] = WifiProfile();
  --gWifiCount;
  saveNetConfig();
  return true;
}

void wifiClearProfiles() {
  for (size_t i = 0; i < kWifiProfileMax; ++i) gWifiProfiles[i] = WifiProfile();
  gWifiCount = 0;
  saveNetConfig();
}

// ----------------------------------------------------------------------------
//  WiFi cards
//
//  The card uses the same scheme as a WiFi QR code:
//      WIFI:S:MeinNetz;T:WPA;P:geheim;;
//  Special characters are escaped with a backslash. This exact format
//  write the same format - a card written there also
//  runs here unchanged.
// ----------------------------------------------------------------------------
bool parseWifiText(const String& raw, String* ssidOut, String* passOut) {
  String text = trimCopy(raw);
  if (text.length() < 6) return false;

  String head = text.substring(0, 5);
  head.toUpperCase();
  if (head != "WIFI:") return false;

  text = text.substring(5);

  String ssid;
  String pass;
  String field;
  char   key = 0;
  bool   escaped = false;

  auto storeField = [&]() {
    if (key == 'S') ssid = field;
    else if (key == 'P') pass = field;
    key = 0;
    field = "";
  };

  for (unsigned i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (escaped) {
      field += c;
      escaped = false;
      continue;
    }
    if (c == '\\') { escaped = true; continue; }

    if (c == ':' && key == 0 && field.length() == 1) {
      key = static_cast<char>(toupper(field[0]));
      field = "";
      continue;
    }
    if (c == ';') { storeField(); continue; }
    field += c;
  }
  storeField();

  ssid = trimCopy(ssid);
  if (ssid.isEmpty()) return false;
  if (ssidOut) *ssidOut = ssid;
  if (passOut) *passOut = pass;
  return true;
}

// Counterpart to parseWifiText.
String wifiCardText(const String& ssid, const String& pass) {
  auto escape = [](const String& in) {
    String out;
    for (unsigned i = 0; i < in.length(); ++i) {
      const char c = in[i];
      if (c == '\\' || c == ';' || c == ':' || c == ',' || c == '"') out += '\\';
      out += c;
    }
    return out;
  };
  String text = "WIFI:S:" + escape(ssid) + ";T:";
  text += pass.isEmpty() ? "nopass" : "WPA";
  text += ";P:" + escape(pass) + ";;";
  return text;
}

bool textLooksLikeWifi(const String& text) {
  String head = trimCopy(text).substring(0, 5);
  head.toUpperCase();
  return head == "WIFI:";
}

// Move the network with the best signal to the front so beginWiFi() tries
// it first. Without a hit the order stays as it is.
void wifiPickBestProfile() {
  if (gWifiCount < 2) return;

  const int found = WiFi.scanNetworks(false, true);
  if (found <= 0) {
    WiFi.scanDelete();
    return;
  }

  int bestProfile = -1;
  int bestRssi    = -999;
  for (int i = 0; i < found; ++i) {
    const int index = wifiProfileIndex(WiFi.SSID(i));
    if (index < 0) continue;
    const int rssi = WiFi.RSSI(i);
    if (rssi > bestRssi) { bestRssi = rssi; bestProfile = index; }
  }
  WiFi.scanDelete();

  if (bestProfile > 0) {
    for (int i = bestProfile; i > 0; --i) std::swap(gWifiProfiles[i], gWifiProfiles[i - 1]);
  }
}

// ============================================================================
//  Setup portal: own access point with a small web UI
// ----------------------------------------------------------------------------
//  While the portal runs, both logo caches are released: the access point
//  and the web server need the internal heap. On exit ensureLogoCache()
//  creates them again.
// ============================================================================
void releaseLogoCache();
bool ensureLogoCache();

String htmlEscape(const String& raw) {
  String out;
  out.reserve(raw.length() + 8);
  for (unsigned i = 0; i < raw.length(); ++i) {
    const char c = raw[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

void portalSendRoot() {
  String page = F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>C64uRemote Setup</title><style>"
                  "body{background:#0b1020;color:#dce6f8;font-family:system-ui,sans-serif;margin:0;padding:18px}"
                  "h1{font-size:20px;margin:0 0 14px}"
                  "form{max-width:420px}label{display:block;margin:12px 0 4px;font-size:14px;color:#9cbee4}"
                  "input{width:100%;box-sizing:border-box;padding:9px;border-radius:7px;border:1px solid #42607f;"
                  "background:#121c2d;color:#eaf1ff;font-size:15px}"
                  "button{margin-top:16px;padding:10px 18px;border:0;border-radius:7px;background:#2f6bb0;"
                  "color:#fff;font-size:15px}"
                  "p.note{font-size:13px;color:#8fa6c4;max-width:420px}"
                  "</style></head><body><h1>C64uRemote &ndash; Setup</h1><form method=\"POST\" action=\"/save\">");

  page += F("<label>Wi-Fi name (SSID)</label><input name=\"ssid\" maxlength=\"32\" value=\"");
  page += htmlEscape(gWifiCount > 0 ? gWifiProfiles[0].ssid : String());
  page += F("\">");
  page += F("<label>Wi-Fi password</label><input name=\"pass\" type=\"password\" maxlength=\"63\" value=\"\">");
  page += F("<label>Address of the c64u</label><input name=\"host\" maxlength=\"40\" value=\"");
  page += htmlEscape(gTargetHost);
  page += F("\">");
  page += F("<label>Password of the c64u (if set)</label><input name=\"hostpw\" type=\"password\" maxlength=\"40\" value=\"\">");
  page += F("<button type=\"submit\">Save</button></form>");
  page += F("<p class=\"note\">Password fields left empty keep the previous "
            "value. After saving the stick shuts the access point "
            "down and connects to the new network.</p>"
            "</body></html>");

  gPortal.send(200, "text/html; charset=utf-8", page);
}

void portalSendSaved(const String& ssid) {
  String page = F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>Saved</title><style>"
                  "body{background:#0b1020;color:#dce6f8;font-family:system-ui,sans-serif;margin:0;padding:18px}"
                  "h1{font-size:20px}p{max-width:420px;color:#9cbee4}"
                  "</style></head><body><h1>Saved</h1><p>");
  page += htmlEscape(ssid.isEmpty() ? String("Settings applied.")
                                    : ("The stick now connects to \"" + ssid + "\"."));
  page += F("</p><p>This page can be closed - the access point "
            "shuts down in a moment.</p></body></html>");
  gPortal.send(200, "text/html; charset=utf-8", page);
}

void portalHandleSave() {
  const String ssid   = trimCopy(gPortal.arg("ssid"));
  const String pass   = gPortal.arg("pass");
  const String host   = trimCopy(gPortal.arg("host"));
  const String hostpw = gPortal.arg("hostpw");

  if (!host.isEmpty()) gTargetHost = host;
  if (!hostpw.isEmpty()) gTargetPass = hostpw;

  if (!ssid.isEmpty()) {
    // Empty password field: a known network keeps its password.
    String usePass = pass;
    if (usePass.isEmpty()) {
      const int known = wifiProfileIndex(ssid);
      if (known >= 0) usePass = gWifiProfiles[known].pass;
    }
    wifiAddProfile(ssid, usePass, false);
  }
  saveNetConfig();

  portalSendSaved(ssid);
  app.portalSavedMs = millis();
}

void portalRedirect() {
  gPortal.sendHeader("Location", "http://192.168.4.1/", true);
  gPortal.send(302, "text/plain", "");
}

void startPortal(uint32_t now) {
  if (app.portalActive) return;

  releaseLogoCache();          // Free heap for AP + web server

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kPortalSsid, kPortalPass);
  delay(120);

  gPortalDns.setErrorReplyCode(DNSReplyCode::NoError);
  gPortalDns.start(kPortalDnsPort, "*", WiFi.softAPIP());

  gPortal.on("/", HTTP_GET, portalSendRoot);
  gPortal.on("/save", HTTP_POST, portalHandleSave);
  gPortal.onNotFound(portalRedirect);
  gPortal.begin();

  app.portalActive    = true;
  app.portalStartedMs = now;
  app.portalSavedMs   = 0;
}

void stopPortal(uint32_t now) {
  if (!app.portalActive) return;

  gPortal.stop();
  gPortalDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  app.portalActive  = false;
  app.portalSavedMs = 0;

  ensureLogoCache();
  gWifiTry = 0;                 // most recently saved network first
  app.lastWiFiAttemptMs = 0;
  beginWiFi(now);
}

void servicePortal(uint32_t now) {
  if (!app.portalActive) return;

  gPortalDns.processNextRequest();
  gPortal.handleClient();

  // Keep running briefly after saving so the response page
  // still reaches the browser.
  if (app.portalSavedMs != 0 && now - app.portalSavedMs >= kPortalCloseMs) {
    stopPortal(now);
    setModal("WIFI SAVED", rgb565(110, 230, 170), now, 1600);
    setScreenMode(ScreenMode::WifiMenu, now);
    return;
  }

  if (now - app.portalStartedMs >= kPortalIdleMs) {
    stopPortal(now);
    setModal("PORTAL CLOSED", rgb565(255, 170, 84), now, 1600);
    setScreenMode(ScreenMode::WifiMenu, now);
  }
}

// ============================================================================
//  NFC cards  (Unit RFID2 / WS1850S on the Grove port)
// ----------------------------------------------------------------------------
//  The card format is identical to M5Dial and M5Stack Core: a plain
//  ordinary NDEF text record. Supported are
//
//    A) NTAG213/215/216 and MIFARE Ultralight (SAK 0x00, 4 bytes per page,
//       no key): NDEF TLV from page 4 on. Pages 0..3 (UID, lock,
//       Capability Container) stay untouched.
//
//    B) MIFARE Classic 1K/4K/Mini (16 bytes per block): NDEF TLV in the
//       data blocks from block 4 on, sector trailers are skipped.
//       Authentication first tries the NDEF key D3F7D3F7D3F7,
//       then falls back to the factory key FFFFFFFFFFFF. Trailer and MAD
//       are never written - that way a card cannot be made
//       unusable.
//
//  Reading also recognizes the old raw format "C64UPATH", so that
//  already written cards keep working.
// ============================================================================
constexpr size_t  kMaxTextLen  = 246;   // like TeensyROM
constexpr size_t  kNdefBufSize = 288;   // TLV + record + text + reserve
constexpr uint8_t kUlDataPage  = 4;     // NDEF starts on page 4
constexpr uint8_t kMagic[8]    = {'C', '6', '4', 'U', 'P', 'A', 'T', 'H'};

MFRC522_I2C::MIFARE_Key kKeyNdef    = {{0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}};
MFRC522_I2C::MIFARE_Key kKeyFactory = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
bool gClassicNdefFormatted = false;     // Result of the last authentication
bool gClassicTryNdefFirst  = true;      // reset per card

uint8_t trailerForBlock(uint8_t block) { return static_cast<uint8_t>((block / 4) * 4 + 3); }

// Linear index -> data block, sector trailers are left out.
// 0->4, 1->5, 2->6, 3->8, 4->9, 5->10, 6->12 ...
uint8_t classicDataBlock(size_t index) {
  const size_t sector = 1 + index / 3;
  return static_cast<uint8_t>(sector * 4 + (index % 3));
}

// After a failed auth the card is in HALT state and
// only answers WUPA. So wake it explicitly and select it again.
bool reselectCard() {
  uint8_t atqa[2];
  uint8_t size = sizeof(atqa);
  if (rfid.PICC_WakeupA(atqa, &size) != MFRC522_I2C::STATUS_OK) return false;
  return rfid.PICC_Select(&(rfid.uid), 0) == MFRC522_I2C::STATUS_OK;
}

bool classicAuth(uint8_t block) {
  const uint8_t trailer = trailerForBlock(block);

  // Try the last successful key first, otherwise every
  // block an unnecessary failed attempt plus a re-select.
  MFRC522_I2C::MIFARE_Key* keys[2] = {
      gClassicTryNdefFirst ? &kKeyNdef : &kKeyFactory,
      gClassicTryNdefFirst ? &kKeyFactory : &kKeyNdef};

  for (int i = 0; i < 2; ++i) {
    if (rfid.PCD_Authenticate(MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A, trailer, keys[i],
                              &(rfid.uid)) == MFRC522_I2C::STATUS_OK) {
      gClassicNdefFormatted = (keys[i] == &kKeyNdef);
      gClassicTryNdefFirst  = gClassicNdefFormatted;
      return true;
    }
    rfid.PCD_StopCrypto1();
    if (!reselectCard()) return false;
  }
  return false;
}

bool rfidReadBlock(uint8_t block, uint8_t* out16) {
  if (!classicAuth(block)) return false;
  uint8_t buffer[18];
  uint8_t size = sizeof(buffer);
  if (rfid.MIFARE_Read(block, buffer, &size) != MFRC522_I2C::STATUS_OK) return false;
  memcpy(out16, buffer, 16);
  return true;
}

bool rfidWriteBlock(uint8_t block, const uint8_t* data16) {
  if (!classicAuth(block)) return false;
  return rfid.MIFARE_Write(block, const_cast<uint8_t*>(data16), 16) == MFRC522_I2C::STATUS_OK;
}

void rfidRelease() {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

bool cardPresent() {
  const bool found = rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial();
  if (found) gClassicTryNdefFirst = true;   // re-detected for every new card
  return found;
}

// ---------------------------------------------------------------------------
// Fast presence probe for the background polling
//
// With no card present the MFRC522 waits after the REQA command until its
// internal timer expires - PCD_Init() sets 0x03E8 = 1000 steps
// of 25 us each, i.e. 25 ms. That is how long the main loop would stall.
// A card answers far faster, 2 ms is enough for the bare probe.
// ---------------------------------------------------------------------------
uint16_t gRfidTimerReload = 0x03E8;         // read from the chip in initRfid()
constexpr uint16_t kRfidProbeReload = 80;   // 80 * 25 us = 2 ms

void setRfidTimerReload(uint16_t ticks) {
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegH, static_cast<byte>(ticks >> 8));
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegL, static_cast<byte>(ticks & 0xFF));
}

bool cardPresentQuick() {
  setRfidTimerReload(kRfidProbeReload);
  const bool present = rfid.PICC_IsNewCardPresent();
  setRfidTimerReload(gRfidTimerReload);     // restore before selecting
  if (!present) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;
  gClassicTryNdefFirst = true;
  return true;
}

CardKind cardKind() {
  const uint8_t type = rfid.PICC_GetType(rfid.uid.sak);
  if (type == MFRC522_I2C::PICC_TYPE_MIFARE_MINI ||
      type == MFRC522_I2C::PICC_TYPE_MIFARE_1K ||
      type == MFRC522_I2C::PICC_TYPE_MIFARE_4K) {
    return CardKind::Classic;
  }
  // NTAG213/215/216 and Ultralight all report SAK 0x00
  if (type == MFRC522_I2C::PICC_TYPE_MIFARE_UL) return CardKind::Ultralight;
  return CardKind::None;
}

const char* cardKindLabel(CardKind kind) {
  switch (kind) {
    case CardKind::Classic:    return "MIFARE Classic";
    case CardKind::Ultralight: return "NTAG / Ultralight";
    default:                   return "unknown";
  }
}

// ---- Ultralight / NTAG: 4 bytes per page ----------------------------------
// MIFARE_Read always returns 16 bytes, i.e. four pages at once.
bool ulRead16(uint8_t page, uint8_t* out16) {
  uint8_t buffer[18];
  uint8_t size = sizeof(buffer);
  if (rfid.MIFARE_Read(page, buffer, &size) != MFRC522_I2C::STATUS_OK) return false;
  memcpy(out16, buffer, 16);
  return true;
}

bool ulWritePage(uint8_t page, const uint8_t* data4) {
  return rfid.MIFARE_Ultralight_Write(page, const_cast<uint8_t*>(data4), 4) ==
         MFRC522_I2C::STATUS_OK;
}

bool ulWrite16(uint8_t firstPage, const uint8_t* data16) {
  for (uint8_t i = 0; i < 4; ++i) {
    if (!ulWritePage(static_cast<uint8_t>(firstPage + i), data16 + i * 4)) return false;
  }
  return true;
}

String cardUidString() {
  String uid;
  char buf[4];
  for (uint8_t i = 0; i < rfid.uid.size; ++i) {
    snprintf(buf, sizeof(buf), "%02X", rfid.uid.uidByte[i]);
    uid += buf;
  }
  return uid;
}

// ---- Access to the NDEF data area, 16 bytes at a time ----------------------
bool cardReadChunk(CardKind kind, size_t chunk, uint8_t* out16) {
  if (kind == CardKind::Classic) return rfidReadBlock(classicDataBlock(chunk), out16);
  return ulRead16(static_cast<uint8_t>(kUlDataPage + chunk * 4), out16);
}

bool cardWriteChunk(CardKind kind, size_t chunk, const uint8_t* data16) {
  if (kind == CardKind::Classic) return rfidWriteBlock(classicDataBlock(chunk), data16);
  return ulWrite16(static_cast<uint8_t>(kUlDataPage + chunk * 4), data16);
}

// ---------------------------------------------------------------------------
// NDEF: a single text record (Well Known, UTF-8, language "en")
//
//   TLV      : 03 <len> ... FE
//   Record   : D1 01 <plen> 54 | 02 'e' 'n' | <text>
//              D1 = MB|ME|SR|TNF=1 (Well Known), 54 = 'T'
// ---------------------------------------------------------------------------
size_t buildNdefText(const String& text, uint8_t* out, size_t cap) {
  const size_t textLen    = text.length();
  const size_t payloadLen = 3 + textLen;          // Status + "en" + text
  const size_t recordLen  = 4 + payloadLen;       // Header + type length + length + type
  const size_t total      = 2 + recordLen + 1;    // TLV header + record + terminator
  if (payloadLen > 255 || total > cap) return 0;

  size_t i = 0;
  out[i++] = 0x03;                                       // TLV: NDEF message
  out[i++] = static_cast<uint8_t>(recordLen);
  out[i++] = 0xD1;                                       // MB|ME|SR|TNF=Well Known
  out[i++] = 0x01;                                       // Type length
  out[i++] = static_cast<uint8_t>(payloadLen);
  out[i++] = 'T';                                        // Type "Text"
  out[i++] = 0x02;                                       // UTF-8, language code 2 chars
  out[i++] = 'e';
  out[i++] = 'n';
  for (size_t k = 0; k < textLen; ++k) out[i++] = static_cast<uint8_t>(text[k]);
  out[i++] = 0xFE;                                       // TLV: end

  // Pad to a multiple of 16 so that whole blocks are written
  while (i % 16 != 0 && i < cap) out[i++] = 0x00;
  return i;
}

// Finds the first text record in the data stream and returns its content.
bool parseNdefText(const uint8_t* data, size_t len, String* out) {
  size_t i = 0;

  // Walk the TLV chain until the NDEF message shows up
  size_t msgStart = 0;
  size_t msgLen   = 0;
  while (i < len) {
    const uint8_t tag = data[i++];
    if (tag == 0x00) continue;                     // NULL TLV
    if (tag == 0xFE) return false;                 // End without message
    if (i >= len) return false;

    size_t tlvLen = data[i++];
    if (tlvLen == 0xFF) {                          // 3-byte length
      if (i + 1 >= len) return false;
      tlvLen = (static_cast<size_t>(data[i]) << 8) | data[i + 1];
      i += 2;
    }
    if (tag == 0x03) { msgStart = i; msgLen = tlvLen; break; }
    i += tlvLen;                                   // skip other TLVs
  }
  if (msgLen == 0 || msgStart + msgLen > len) return false;

  // Walk through the records of the message
  size_t p = msgStart;
  const size_t end = msgStart + msgLen;
  while (p < end) {
    const uint8_t header = data[p++];
    const bool shortRec = (header & 0x10) != 0;
    const bool hasId    = (header & 0x08) != 0;
    const uint8_t tnf   = header & 0x07;
    if (p >= end) return false;

    const uint8_t typeLen = data[p++];
    size_t payloadLen = 0;
    if (shortRec) {
      if (p >= end) return false;
      payloadLen = data[p++];
    } else {
      if (p + 3 >= end) return false;
      payloadLen = (static_cast<size_t>(data[p]) << 24) | (static_cast<size_t>(data[p + 1]) << 16) |
                   (static_cast<size_t>(data[p + 2]) << 8) | data[p + 3];
      p += 4;
    }
    uint8_t idLen = 0;
    if (hasId) {
      if (p >= end) return false;
      idLen = data[p++];
    }

    const size_t typePos    = p;
    const size_t payloadPos = p + typeLen + idLen;

    // Some writers store a wrong payload length. On the last
    // record (ME flag) the TLV length therefore takes priority.
    if ((header & 0x40) != 0 && payloadPos < end) {
      const size_t fromTlv = end - payloadPos;
      if (fromTlv != payloadLen) payloadLen = fromTlv;
    }
    if (payloadPos + payloadLen > len) {
      if (payloadPos >= len) return false;
      payloadLen = len - payloadPos;          // better to truncate than to give up
    }

    // Well Known "T" = text record
    if (tnf == 0x01 && typeLen == 1 && data[typePos] == 'T' && payloadLen >= 1) {
      const uint8_t status  = data[payloadPos];
      const uint8_t langLen = status & 0x3F;
      if (payloadLen > static_cast<size_t>(1 + langLen)) {
        const size_t textPos = payloadPos + 1 + langLen;
        const size_t textLen = payloadLen - 1 - langLen;
        String text;
        text.reserve(textLen + 1);
        for (size_t k = 0; k < textLen; ++k) {
          const uint8_t b = data[textPos + k];
          if (b == 0x00 || b == 0xFE) break;   // Padding bytes / TLV end
          text += static_cast<char>(b);
        }
        *out = text;
        return true;
      }
    }

    p = payloadPos + payloadLen;
    if ((header & 0x40) != 0) break;               // ME: last record
  }
  return false;
}

// ---------------------------------------------------------------------------
// Read the card content: NDEF first, else the old raw format "C64UPATH"
// ---------------------------------------------------------------------------
struct CardContent {
  bool   ok       = false;
  bool   isNdef   = false;
  bool   isLegacy = false;
  String text;                 // Raw text from the card
  String error;
};

CardContent readCardContent(CardKind kind) {
  CardContent result;
  if (kind == CardKind::None) {
    result.error = "card type not supported";
    return result;
  }

  static uint8_t buffer[kNdefBufSize];
  memset(buffer, 0, sizeof(buffer));

  if (!cardReadChunk(kind, 0, buffer)) {
    result.error = (kind == CardKind::Classic) ? "block 4 unreadable (key?)"
                                               : "page 4 unreadable";
    return result;
  }
  cardReadChunk(kind, 1, buffer + 16);

  // Old raw format?
  if (memcmp(buffer, kMagic, sizeof(kMagic)) == 0) {
    const uint8_t len = buffer[9];
    if (len == 0 || len > 128) {
      result.error = "length invalid";
      return result;
    }
    String path;
    path.reserve(len + 1);
    static const uint8_t legacyBlocks[] = {5, 6, 8, 9, 10, 12, 13, 14};
    size_t remaining = len;
    for (size_t i = 0; i < 8 && remaining > 0; ++i) {
      uint8_t data[16] = {0};
      const bool ok = (kind == CardKind::Classic)
                          ? rfidReadBlock(legacyBlocks[i], data)
                          : ulRead16(static_cast<uint8_t>(8 + i * 4), data);
      if (!ok) {
        result.error = "read error (legacy format)";
        return result;
      }
      const size_t take = std::min<size_t>(16, remaining);
      for (size_t k = 0; k < take; ++k) path += static_cast<char>(data[k]);
      remaining -= take;
    }
    result.ok       = true;
    result.isLegacy = true;
    result.text     = path;
    return result;
  }

  // NDEF: take the length from the TLV and read only as much as needed
  size_t needed = sizeof(buffer);
  if (buffer[0] == 0x03) {
    needed = (buffer[1] == 0xFF)
                 ? 4 + ((static_cast<size_t>(buffer[2]) << 8) | buffer[3])
                 : 2 + static_cast<size_t>(buffer[1]) + 1;
    needed = std::min(needed, sizeof(buffer));
  }

  size_t have = 16;
  for (size_t chunk = 1; have < needed; ++chunk) {
    if (!cardReadChunk(kind, chunk, buffer + have)) break;
    have += 16;
    if (have + 16 > sizeof(buffer)) break;
  }

  String text;
  if (parseNdefText(buffer, have, &text)) {
    result.ok     = true;
    result.isNdef = true;
    result.text   = text;
    return result;
  }

  result.error = (buffer[0] == 0x03) ? "NDEF without text record" : "no NDEF text";
  return result;
}

// ---------------------------------------------------------------------------
// Write a card: always as an NDEF text record
// ---------------------------------------------------------------------------
bool writeCardText(CardKind kind, const String& text, String* errorOut) {
  if (kind == CardKind::None) {
    if (errorOut) *errorOut = "card type not supported";
    return false;
  }
  if (text.isEmpty() || text.length() > kMaxTextLen) {
    if (errorOut) *errorOut = "text too long (max " + String(kMaxTextLen) + ")";
    return false;
  }

  static uint8_t buffer[kNdefBufSize];
  const size_t total = buildNdefText(text, buffer, sizeof(buffer));
  if (total == 0) {
    if (errorOut) *errorOut = "NDEF does not fit";
    return false;
  }

  const size_t chunks = total / 16;
  for (size_t chunk = 0; chunk < chunks; ++chunk) {
    if (!cardWriteChunk(kind, chunk, buffer + chunk * 16)) {
      if (errorOut) {
        *errorOut = (chunk == 0) ? "card not writable"
                                 : "card too small from block " + String(chunk);
      }
      return false;
    }
  }

  // Check: read back and compare
  const CardContent check = readCardContent(kind);
  if (!check.ok || check.text != text) {
    if (errorOut) *errorOut = check.ok ? "verification mismatch" : check.error;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Command cards
//
//     CMD:RESET
//     CMD:REBOOT
//     CMD:MENU
//     CMD:POWEROFF=0      power off immediately
//     CMD:POWEROFF=8      prompt, 8 s to confirm
//     CMD:POWEROFF        prompt with the time configured on the device
//     CMD:CPU=10          set the CPU to 10 MHz
//
// Upper/lower case and spaces do not matter. M5Dial and Core use and
// M5Dial and Core - one card works on all devices.
// ---------------------------------------------------------------------------
constexpr const char* kCardCmdPrefix = "CMD:";

String sanitizeCardText(const String& text) {
  String out;
  out.reserve(text.length() + 1);
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (static_cast<uint8_t>(c) >= 0x20) out += c;
  }
  out.trim();
  return out;
}

bool parseCardCommand(const String& text, CardCommand* out) {
  String t = text;
  t.trim();
  String head = t.substring(0, 4);
  head.toUpperCase();
  if (head != kCardCmdPrefix) return false;

  String body = t.substring(4);
  body.trim();

  String arg;
  const int eq = body.indexOf('=');
  if (eq >= 0) {
    arg  = body.substring(eq + 1);
    body = body.substring(0, eq);
    arg.trim();
    body.trim();
  }
  body.toUpperCase();

  CardCommand cmd;
  cmd.arg    = arg;
  cmd.hasArg = (eq >= 0);

  if      (body == "RESET")    cmd.cmd = CardCmd::Reset;
  else if (body == "REBOOT")   cmd.cmd = CardCmd::Reboot;
  else if (body == "MENU")     cmd.cmd = CardCmd::UltiMenu;
  else if (body == "POWEROFF") cmd.cmd = CardCmd::PowerOff;
  else if (body == "CPU")      cmd.cmd = CardCmd::CpuSpeed;
  else return false;

  if (cmd.cmd == CardCmd::CpuSpeed && arg.isEmpty()) return false;

  *out = cmd;
  return true;
}

// Confirmation time of a PowerOff command in seconds.
// Without an argument the device setting applies, 0 means "no prompt".
uint8_t cardPowerOffSeconds(const CardCommand& c) {
  if (!c.hasArg) return app.settings.cardConfirmS;
  const long v = c.arg.toInt();
  if (v <= 0) return 0;
  return static_cast<uint8_t>(std::min<long>(v, 60));
}

String cardCommandText(const CardCommand& c) {
  switch (c.cmd) {
    case CardCmd::Reset:    return "CMD:RESET";
    case CardCmd::Reboot:   return "CMD:REBOOT";
    case CardCmd::UltiMenu: return "CMD:MENU";
    case CardCmd::PowerOff: return "CMD:POWEROFF=" + String(cardPowerOffSeconds(c));
    case CardCmd::CpuSpeed: return "CMD:CPU=" + c.arg;
    default:                return "";
  }
}

String cardCommandLabel(const CardCommand& c) {
  switch (c.cmd) {
    case CardCmd::Reset:    return "Reset";
    case CardCmd::Reboot:   return "Reboot";
    case CardCmd::UltiMenu: return "Ultimate Menu";
    case CardCmd::PowerOff: {
      const uint8_t sec = cardPowerOffSeconds(c);
      return sec == 0 ? String("PowerOff direct")
                      : ("PowerOff, " + String(sec) + "s prompt");
    }
    case CardCmd::CpuSpeed: return "CPU " + c.arg + " MHz";
    default:                return "?";
  }
}

// ---- Pick list for writing a card -----------------------------------------
// Fixed commands first, then all CPU steps the c64u offers.
constexpr size_t kCmdFixedCount = 5;

size_t cmdListCount() { return kCmdFixedCount + app.cpuChoiceCount; }

CardCommand cmdListAt(size_t index) {
  CardCommand c;
  switch (index) {
    case 0: c.cmd = CardCmd::Reset;    return c;
    case 1: c.cmd = CardCmd::Reboot;   return c;
    case 2: c.cmd = CardCmd::UltiMenu; return c;
    case 3: c.cmd = CardCmd::PowerOff; c.arg = "0"; c.hasArg = true; return c;
    case 4:
      c.cmd    = CardCmd::PowerOff;
      c.arg    = String(app.settings.cardConfirmS);
      c.hasArg = true;
      return c;
    default: break;
  }
  const size_t cpu = index - kCmdFixedCount;
  if (cpu < app.cpuChoiceCount) {
    c.cmd    = CardCmd::CpuSpeed;
    c.arg    = extractDigits(app.cpuDisplayOptions[cpu]);
    c.hasArg = true;
  }
  return c;
}

// ------------------------------------------------------------
// WiFi
// ------------------------------------------------------------
// Connects to the next saved network. With several entries it
// advances on every attempt until one answers.
void beginWiFi(uint32_t now) {
  if (!hasWiFiConfig()) return;
  if (gWifiTry >= gWifiCount) gWifiTry = 0;

  const WifiProfile& profile = gWifiProfiles[gWifiTry];

  // While the setup portal runs, the access point stays up.
  if (!app.portalActive) WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(profile.ssid.c_str(), profile.pass.c_str());
  app.lastWiFiAttemptMs = now;

  if (gWifiCount > 1) gWifiTry = (gWifiTry + 1) % gWifiCount;
}

void serviceWiFi(uint32_t now) {
  if (app.portalActive) return;          // Portal takes priority
  if (!hasWiFiConfig()) return;
  if (WiFi.status() == WL_CONNECTED) return;

  if (app.lastWiFiAttemptMs == 0 || now - app.lastWiFiAttemptMs >= kWiFiRetryMs) {
    beginWiFi(now);
  }
}

// Refreshes the known connection status in the background at coarse intervals.
// This lets the RGB LED show the real overall status without having to run a
// manual "Connection Test" every time.
void refreshConnectionStatus(uint32_t now, bool force = false) {
  app.connection.wifiConnected = WiFi.status() == WL_CONNECTED;

  if (!configReady()) {
    app.connection.targetReachable = false;
    app.connection.authOk = false;
    app.connection.detail = hasWiFiConfig() ? "Host not set" : "WiFi not configured";
    return;
  }

  if (!app.connection.wifiConnected) {
    app.connection.targetReachable = false;
    app.connection.authOk = false;
    app.connection.detail = "WiFi disconnected";
    return;
  }

  if (!force && app.lastConnectionProbeMs != 0 && now - app.lastConnectionProbeMs < kConnectionProbeMs) {
    return;
  }

  app.lastConnectionProbeMs = now;

  const ApiResponse reach = sendApiRequest("GET", "/v1/version", false);
  app.connection.targetReachable = reach.transportOk;

  if (!reach.transportOk) {
    app.connection.authOk = false;
    app.connection.detail = reach.errors.isEmpty() ? "Target unreachable" : reach.errors;
    return;
  }

  const ApiResponse auth = sendApiRequest("GET", "/v1/version", true);
  app.connection.authOk = auth.apiOk;
  app.connection.detail = auth.apiOk ? "Reachable + auth ok"
                                     : (auth.errors.isEmpty() ? "Auth failed" : auth.errors);
}

// ------------------------------------------------------------
// Connection test
// ------------------------------------------------------------
void runConnectionTest(uint32_t now) {
  clearPendingPowerOff();
  refreshConnectionStatus(now, true);

  if (!configReady()) {
    setModal("CONFIG MISSING", rgb565(255, 170, 84), now, 1700);
    setScreenMode(ScreenMode::Status, now);
    return;
  }

  if (!app.connection.wifiConnected) {
    beginWiFi(now);
    setModal("WIFI NOT READY", rgb565(255, 170, 84), now, 1700);
    setScreenMode(ScreenMode::Status, now);
    return;
  }

  if (app.connection.authOk) {
    refreshCpuValue();
    setModal("AUTH OK", rgb565(110, 230, 170), now, 1300);
  } else if (app.connection.targetReachable) {
    setModal("AUTH FAILED", rgb565(255, 200, 72), now, 1700);
  } else {
    setModal("TARGET OFFLINE", rgb565(255, 120, 96), now, 1700);
  }

  updateMiniJoyStatusLed();
  setScreenMode(ScreenMode::Status, now);
}

// ------------------------------------------------------------
// REST actions
// ------------------------------------------------------------
void performReset(uint32_t now) {
  clearPendingPowerOff();

  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  const ApiResponse response = sendApiRequest("PUT", "/v1/machine:reset", true);
  if (response.apiOk) {
    setModal("Resetting...", rgb565(110, 230, 170), now, 1500);
  } else {
    const String text = response.errors.isEmpty() ? "RESET FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}

void performPowerOff(uint32_t now) {
  //clearPendingPowerOff();

  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  const ApiResponse response = sendApiRequest("PUT", "/v1/machine:poweroff", true);
  if (response.apiOk) {
    setModal("PowerOff...", rgb565(110, 230, 170), now, 1500);
  } else {
    const String text = response.errors.isEmpty() ? "POWEROFF FAILED" : response.errors;
    setModal(text, rgb565(255, 170, 84), now, 1900);
  }
}

void requestPowerOff(uint32_t now) {
  if (app.pendingPowerOff && (now - app.pendingPowerOffAtMs <= kPowerOffConfirmMs)) {
    performPowerOff(now);
    return;
  }

  app.pendingPowerOff = true;
  app.pendingPowerOffAtMs = now;
  setModal("POWER OFF? AGAIN!", rgb565(255, 170, 84), now, 1600);
}

void performHardReset(uint32_t now) {
  clearPendingPowerOff();

  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  const ApiResponse response = sendApiRequest("PUT", "/v1/machine:reboot", true);
  if (response.apiOk) {
    setModal("HARD RESET", rgb565(255, 120, 96), now, 1600);
  } else {
    const String text = response.errors.isEmpty() ? "REBOOT FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}

void performMenuButton(uint32_t now) {
  clearPendingPowerOff();

  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  const ApiResponse response = sendApiRequest("PUT", "/v1/machine:menu_button", true);
  if (response.apiOk) {
    setModal("ULTIMATE MENU", rgb565(120, 220, 255), now, 1200);
  } else {
    const String text = response.errors.isEmpty() ? "MENU FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}

void setCpuSpeed(int cpuIndex, uint32_t now) {
  clearPendingPowerOff();

  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  String detail;
  if (!resolveCpuPath(&detail)) {
    setModal(detail, rgb565(255, 120, 96), now, 1900);
    return;
  }

  if (app.cpuChoiceCount == 0) {
    refreshCpuChoices();
  }

  const int clampedIndex = std::max(0, std::min(cpuIndex, static_cast<int>(app.cpuChoiceCount) - 1));
  const String displayValue = app.cpuDisplayOptions[clampedIndex];
  const String wireValue = app.cpuWireOptions[clampedIndex];
  const String path = "/v1/configs/" + urlEncode(app.cpuCategory) + "/" + urlEncode(app.cpuItem)
                    + "?value=" + urlEncode(wireValue);

  const ApiResponse response = sendApiRequest("PUT", path, true);
  if (response.apiOk) {
    refreshCpuValue();
    setModal(displayValue, rgb565(110, 230, 170), now, 1400);
  } else {
    const String text = response.errors.isEmpty() ? "CPU SET FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}


// ============================================================================
//  Evaluate and execute cards
// ============================================================================
bool render(uint32_t now);

// Runs a card command. The UID is needed so that a PowerOff
// can only be confirmed by the same card.
void runCardCommand(const CardCommand& cmd, const String& uid, uint32_t now) {
  app.rfidHint = cardCommandLabel(cmd);
  app.home.frameDirty = true;

  switch (cmd.cmd) {
    case CardCmd::Reset:
      performReset(now);
      return;

    case CardCmd::Reboot:
      performHardReset(now);
      return;

    case CardCmd::UltiMenu:
      performMenuButton(now);
      return;

    case CardCmd::PowerOff: {
      const uint8_t sec = cardPowerOffSeconds(cmd);

      // Tapping the same card again within the window confirms.
      if (app.cardPowerOffPending && uid == app.cardPowerOffUid &&
          static_cast<int32_t>(now - app.cardPowerOffUntilMs) < 0) {
        app.cardPowerOffPending = false;
        performPowerOff(now);
        return;
      }

      if (sec == 0) {                       // "CMD:POWEROFF=0" - no confirmation
        performPowerOff(now);
        return;
      }

      app.cardPowerOffPending = true;
      app.cardPowerOffUid     = uid;
      app.cardPowerOffUntilMs = now + sec * 1000u;
      app.rfidHint            = "present the card again";
      setModal("POWER OFF? AGAIN!", rgb565(255, 190, 84), now, sec * 1000u);
      return;
    }

    case CardCmd::CpuSpeed: {
      if (!app.cpuPathKnown) refreshCpuValue();
      const int index = cpuIndexFromValue(cmd.arg);
      setCpuSpeed(index, now);
      return;
    }

    default:
      setModal("UNKNOWN COMMAND", rgb565(255, 120, 96), now, 2000);
      return;
  }
}

// Takes WiFi credentials from a card and brings up the connection.
void applyWifiCard(const String& ssid, const String& pass, uint32_t now) {
  if (!wifiAddProfile(ssid, pass)) {
    app.rfidHint = "SSID or password too long";
    setModal("CARD INVALID", rgb565(255, 120, 96), now, 2200);
    return;
  }

  app.rfidHint = ssid;
  app.home.frameDirty = true;
  setModal("CONNECTING...", rgb565(120, 220, 255), now, kWifiCardConnectMs + 1500);
  render(now);

  // wifiAddProfile sorts the network to the front; from there only
  // this one network is tried instead of walking the whole list.
  const int index = wifiProfileIndex(ssid);
  gWifiTry = index >= 0 ? static_cast<size_t>(index) : 0;
  app.lastWiFiAttemptMs = 0;

  if (app.portalActive) stopPortal(now);   // stops the AP and connects on its own
  else                  beginWiFi(now);

  // Wait briefly for the result so the feedback is worth something. Longer
  // than kWifiCardConnectMs we do not wait - the rest runs through the
  // regular retries in serviceWiFi.
  const uint32_t deadline = millis() + kWifiCardConnectMs;
  while (millis() < deadline && WiFi.status() != WL_CONNECTED) delay(100);

  const bool connected = WiFi.status() == WL_CONNECTED;
  app.modalText = "";
  app.rfidHint  = connected ? (ssid + "  " + WiFi.localIP().toString())
                            : (ssid + " not reachable");
  app.home.frameDirty = true;
  setModal(connected ? "WIFI ACTIVE" : "NETWORK NOT THERE",
           connected ? rgb565(110, 230, 170) : rgb565(255, 190, 84), millis(), 2400);
  refreshConnectionStatus(millis(), true);
  updateMiniJoyStatusLed();
}

// Handles a selected card according to the current screen.
void processCard(uint32_t now) {
  const CardKind kind = cardKind();
  if (kind == CardKind::None) {
    rfidRelease();
    app.rfidHint = "card type not supported";
    app.home.frameDirty = true;
    return;
  }

  const String uid = cardUidString();

  // ---- Write --------------------------------------------------------------
  if (app.screen == ScreenMode::NfcWrite) {
    String error;
    const bool ok = writeCardText(kind, app.pendingCardText, &error);
    rfidRelease();
    app.rfidHint = ok ? (String(cardKindLabel(kind)) + "  " + uid) : error;
    app.home.frameDirty = true;
    setModal(ok ? "CARD OK" : "WRITE ERROR",
             ok ? rgb565(110, 230, 170) : rgb565(255, 120, 96), now, ok ? 1800 : 2200);
    return;
  }

  // ---- Read ---------------------------------------------------------------
  const CardContent content = readCardContent(kind);
  rfidRelease();

  if (!content.ok) {
    app.rfidHint = content.error;
    app.home.frameDirty = true;
    setModal("CARD EMPTY?", rgb565(255, 190, 84), now, 2000);
    return;
  }

  const String text = sanitizeCardText(content.text);

  // ---- WiFi card on the setup page ----------------------------------------
  if (app.screen == ScreenMode::WifiCard) {
    String ssid;
    String pass;
    if (!parseWifiText(text, &ssid, &pass) || ssid.isEmpty()) {
      app.rfidHint = "that is not a WiFi card";
      app.home.frameDirty = true;
      setModal("WRONG CARD", rgb565(255, 120, 96), now, 2200);
      return;
    }
    applyWifiCard(ssid, pass, now);
    setScreenMode(ScreenMode::WifiMenu, now);
    return;
  }

  // ---- WiFi card outside setup --------------------------------------------
  //
  // Just tap the card: the network is adopted and connected right away.
  if (textLooksLikeWifi(text)) {
    String ssid;
    String pass;
    if (!parseWifiText(text, &ssid, &pass) || ssid.isEmpty()) {
      app.rfidHint = "WiFi card without SSID";
      app.home.frameDirty = true;
      setModal("NO NETWORK", rgb565(255, 120, 96), now, 2200);
      return;
    }

    // If the connection is already up there is nothing to do. That also covers the
    // case where the card stays on the reader.
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
      app.rfidHint = ssid;
      app.home.frameDirty = true;
      setModal("ALREADY CONNECTED", rgb565(110, 230, 170), now, 1800);
      return;
    }

    applyWifiCard(ssid, pass, now);
    return;
  }

  // ---- Command card -------------------------------------------------------
  CardCommand command;
  if (parseCardCommand(text, &command)) {
    runCardCommand(command, uid, now);
    return;
  }

  // ---- Everything else: path card -----------------------------------------
  // The Stick would need a microSD with the program files for that. The
  // The card stays valid, it just belongs on an M5Dial or Core.
  Serial.printf("card read (%s): '%s'\n",
                content.isNdef ? "NDEF-Text" : "legacy format", text.c_str());
  app.rfidHint = "program card - no SD card here";
  app.home.frameDirty = true;
  setModal("NEEDS SD CARD", rgb565(255, 190, 84), now, 2400);
}

// These screens normally wait for a card.
bool onRfidScreen() {
  return app.screen == ScreenMode::NfcRead ||
         app.screen == ScreenMode::NfcWrite ||
         app.screen == ScreenMode::WifiCard;
}

void serviceRfid(uint32_t now) {
  if (!app.rfidReady) return;

  // Drop an expired PowerOff prompt of a card
  if (app.cardPowerOffPending &&
      static_cast<int32_t>(now - app.cardPowerOffUntilMs) >= 0) {
    app.cardPowerOffPending = false;
  }

  const bool onCardScreen = onRfidScreen();

  if (onCardScreen) {
    if (now - app.lastRfidPollMs < kRfidPollMs) return;
  } else {
    // Background polling only on the home screen
    if (app.settings.autoNfc == AutoNfcMode::Off) return;
    if (app.screen != ScreenMode::Home) return;
    if (now - app.lastRfidPollMs < autoNfcIntervalMs(app.settings.autoNfc)) return;
  }
  app.lastRfidPollMs = now;

  // On the card screens with the full time window, in the background with the
  // short probe - otherwise the main loop stalls 25 ms on every pass.
  if (!(onCardScreen ? cardPresent() : cardPresentQuick())) return;

  // Otherwise a card left on the reader would be executed again
  // every second. The prompt of a PowerOff command may in turn
  // be confirmed at any time by tapping the card again.
  const String uid = cardUidString();
  if (!app.cardPowerOffPending && uid == app.lastCardUid &&
      now - app.lastCardMs < kRfidRepeatMs) {
    rfidRelease();
    return;
  }
  app.lastCardUid = uid;
  app.lastCardMs  = now;

  if (!onCardScreen) app.rfidHint = "card detected";
  processCard(now);
}

// ---------------------------------------------------------------------------
// Reader detection
// ---------------------------------------------------------------------------
bool i2cDevicePresent(TwoWire& bus, uint8_t address) {
  bus.beginTransmission(address);
  return bus.endTransmission() == 0;
}

bool initRfid() {
  // If nothing answers at 0x28, no reader is on the Grove port.
  if (!i2cDevicePresent(Wire1, kRfidAddr)) return false;

  rfid.PCD_Init();
  delay(20);

  // For information only - the WS1850S does not always report the same
  // value as a real MFRC522, so no decision depends on it.
  const uint8_t version = rfid.PCD_ReadRegister(MFRC522_I2C::VersionReg);
  Serial.printf("RFID2 (I2C 0x%02X) VersionReg = 0x%02X\n", kRfidAddr, version);

  // Remember the timeout set by PCD_Init(), so that the fast probe
  // can restore it exactly afterwards.
  const uint16_t reload =
      static_cast<uint16_t>(rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegH) << 8) |
      rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegL);
  if (reload > kRfidProbeReload) gRfidTimerReload = reload;
  return true;
}

// ------------------------------------------------------------
// Read MiniJoyC over I2C
// ------------------------------------------------------------
// ------------------------------------------------------------
// MiniJoyC: start the bus and set the RGB LED
//
// The joystick is read directly over Wire further below anyway. For
// these two small steps no separate library is needed -
// that saves a dependency and the ambiguity warnings from
// its source (requestFrom(uint8_t, int)).
// ------------------------------------------------------------
bool miniJoyBegin() {
  Wire.begin(kI2cSdaPin, kI2cSclPin, 100000UL);
  delay(10);
  Wire.beginTransmission(kMiniJoyAddr);
  return Wire.endTransmission() == 0;
}

// Color in 0xRRGGBB format. After the register the chip expects
// three bytes in the order red, green, blue.
void miniJoySetLedColor(uint32_t rgb888color) {
  Wire.beginTransmission(kMiniJoyAddr);
  Wire.write(kRegJoyRgbLed);
  Wire.write(static_cast<uint8_t>((rgb888color >> 16) & 0xFF));
  Wire.write(static_cast<uint8_t>((rgb888color >> 8) & 0xFF));
  Wire.write(static_cast<uint8_t>(rgb888color & 0xFF));
  Wire.endTransmission();
}

bool joyReadRegister(uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(kMiniJoyAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom(static_cast<int>(kMiniJoyAddr), 1) != 1) return false;
  *outValue = Wire.read();
  return true;
}

bool joyReadI8(uint8_t reg, int8_t* outValue) {
  uint8_t raw = 0;
  if (!joyReadRegister(reg, &raw)) return false;
  *outValue = static_cast<int8_t>(raw);
  return true;
}

bool joyReadButton(bool* outPressed) {
  uint8_t raw = 0;
  if (!joyReadRegister(kRegJoyButton, &raw)) return false;

  if (kJoyButtonActiveLow) {
    *outPressed = (raw == 0);
  } else {
    *outPressed = (raw != 0);
  }
  return true;
}

// ------------------------------------------------------------
// MiniJoyC RGB LED
// ------------------------------------------------------------
// The status LED is driven through the official MiniJoyC library.
// This exact method already worked in the separate test program
// and is therefore the most reliable variant here.
void updateMiniJoyStatusLed() {
  if (!miniJoyReady) return;

  if (!app.settings.joyLedEnabled) {
    miniJoySetLedColor(0x000000);
    return;
  }

  const uint8_t level = app.settings.joyLedBrightness;
  auto scale = [level](uint8_t channel) -> uint8_t {
    return static_cast<uint8_t>((static_cast<uint16_t>(channel) * level) / 255u);
  };

  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;

  // Fine-grained LED status logic:
  // Red             = no WiFi
  // Blue            = target not reachable
  // Blue blinking   = target reachable, but auth failed
  // Green           = WiFi + target reachable + auth OK
  //
  // The blinking deliberately goes through the LED output only and changes no
  // other status data. The display stays calm while the LED
  // still shows the error state clearly.
  if (WiFi.status() != WL_CONNECTED) {
    red = 255;
  } else if (app.connection.targetReachable && app.connection.authOk) {
    green = 255;
  } else if (!app.connection.targetReachable) {
    blue = 255;
  } else {
    const bool blinkOn = ((millis() / 350u) % 2u) == 0u;
    blue = blinkOn ? 255 : 0;
  }

  const uint32_t color =
      (static_cast<uint32_t>(scale(red)) << 16) |
      (static_cast<uint32_t>(scale(green)) << 8) |
      static_cast<uint32_t>(scale(blue));
  miniJoySetLedColor(color);
}

// Direction mapping for your installation
JoyDir joyDirectionFromXY(int8_t x, int8_t y) {
  const int threshold = joyThresholdValue(app.settings.joyThreshold);
  if (y >= threshold)  return JoyDir::Right;
  if (y <= -threshold) return JoyDir::Left;
  if (x <= -threshold) return JoyDir::Up;
  if (x >= threshold)  return JoyDir::Down;
  return JoyDir::Center;
}

bool updateMiniJoy() {
  int8_t x = 0;
  int8_t y = 0;
  bool button = false;

  if (!joyReadI8(kRegJoyX, &x)) return false;
  if (!joyReadI8(kRegJoyY, &y)) return false;
  if (!joyReadButton(&button)) return false;

  app.joy.present = true;
  app.joy.x = x;
  app.joy.y = y;
  app.joy.button = button;
  app.joy.dir = joyDirectionFromXY(x, y);
  return true;
}

bool joyDirectionPressed(JoyDir dir) {
  return (app.joy.dir == dir && app.joy.lastDir != dir);
}

bool joyButtonPressed() {
  return (app.joy.button && !app.joy.buttonLast);
}

// ------------------------------------------------------------
// Joystick actions
// ------------------------------------------------------------

void applyJoyLeft(uint32_t now) {
  clearPendingPowerOff();
  app.pendingSoftReset = false;

  if (app.screen == ScreenMode::Home) {
    setModal("HOME", rgb565(120, 220, 255), now, 600);
    return;
  }
  goBack(now);
}

void applyJoyRight(uint32_t now) {
  app.pendingSoftReset = false;
  activateCurrent(now);
}

void applyJoyUp(uint32_t now) {
  if (app.screen == ScreenMode::Home) {
    app.pendingSoftReset = false;
    performReset(now);
    return;
  }
  clearPendingPowerOff();
  moveSelection(-1);
}

void applyJoyDown(uint32_t now) {
  if (app.screen == ScreenMode::Home) {
    app.pendingSoftReset = false;
    requestPowerOff(now);
    return;
  }
  clearPendingPowerOff();
  moveSelection(+1);
}

void applyJoyButton(uint32_t now) {
  app.pendingSoftReset = false;
  performMenuButton(now);
}

void processJoy(uint32_t now) {
  if (!updateMiniJoy()) {
    if (app.joy.failCount < 255) {
      app.joy.failCount++;
    }

    if (app.joy.failCount >= kJoyOfflineThreshold) {
      if (app.joy.present) {
        app.joy.present = false;
        setModal("MINIJOYC OFFLINE", rgb565(255, 170, 84), now, 900);
      }
      app.joy.lastDir = JoyDir::Center;
    }
    return;
  }

  app.joy.failCount = 0;

  if (!app.joy.present) {
    app.joy.present = true;
    setModal("MINIJOYC OK", rgb565(110, 230, 170), now, 700);
  }

  if (joyDirectionPressed(JoyDir::Left))  applyJoyLeft(now);
  if (joyDirectionPressed(JoyDir::Right)) applyJoyRight(now);
  if (joyDirectionPressed(JoyDir::Up))    applyJoyUp(now);
  if (joyDirectionPressed(JoyDir::Down))  applyJoyDown(now);

  if (joyButtonPressed()) {
    applyJoyButton(now);
  }

  app.joy.lastDir = app.joy.dir;
  app.joy.buttonLast = app.joy.button;
}

// ------------------------------------------------------------
// UI helpers
// ------------------------------------------------------------
void drawLabelValue(int x, int y, const char* label, const String& value, uint16_t valueColor = TFT_WHITE) {
  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString(label, x, y);
  canvas.setTextColor(valueColor);
  canvas.drawString(value, x, y + 12);
}

void drawWrappedText(int x, int y, int width, const String& text, int maxLines, int lineHeight, uint16_t color) {
  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(color);

  String remaining = trimCopy(text);

  for (int line = 0; line < maxLines && !remaining.isEmpty(); ++line) {
    String current;
    int split = -1;

    for (int i = 0; i < remaining.length(); ++i) {
      const char c = remaining[i];
      if (c == ' ') split = i;

      const String candidate = remaining.substring(0, i + 1);
      if (canvas.textWidth(candidate) > width) break;
      current = candidate;
    }

    if (current.isEmpty()) current = remaining;

    if (canvas.textWidth(current) > width) {
      if (split > 0) {
        current = remaining.substring(0, split);
      } else {
        current = remaining.substring(0, std::min(static_cast<int>(remaining.length()), 12));
      }
    }

    current.trim();
    String next = remaining.substring(current.length());
    next.trim();

    if (line == maxLines - 1 && !next.isEmpty()) {
      while (!current.isEmpty() && canvas.textWidth(current + "...") > width) {
        current.remove(current.length() - 1);
        current.trim();
      }
      current += "...";
      next = "";
    }

    canvas.drawString(current, x, y + line * lineHeight);
    remaining = next;
  }
}

void drawWrappedTextInfo(int x, int y, int width, const String& text, int maxLines, int lineHeight, uint16_t color) {
  drawWrappedText(x, y, width, text, maxLines, lineHeight, color);
}

uint16_t samplePlainLogoPixel(int x, int y) {
  if (x < 0 || x >= canvas.width() || y < 0 || y >= canvas.height()) return TFT_WHITE;
  return plainLogoPixels[y * canvas.width() + x];
}

int wrapCoord(int value, int limit) {
  if (limit <= 0) return 0;
  value %= limit;
  if (value < 0) value += limit;
  return value;
}

void drawBoxedLogoInner() {
  const int innerX = homeInnerX();
  const int innerY = homeInnerY();
  const int innerW = homeInnerW();
  const int innerH = homeInnerH();

  for (int y = 0; y < innerH; ++y) {
    canvas.pushImage(innerX, innerY + y, innerW, 1, boxedLogoPixels + (innerY + y) * canvas.width() + innerX);
  }
}

void drawPlainLogoFrame() {
  const int width = canvas.width();
  const int height = canvas.height();

  for (int y = 0; y < height; ++y) {
    canvas.pushImage(0, y, width, 1, plainLogoPixels + y * width);
  }
}

void drawLogoDistortedRows(uint32_t tickMs, bool waterMode) {
  const float t = static_cast<float>(tickMs) * 0.001f;
  const int width = canvas.width();
  const int height = canvas.height();
  const float cx = width * 0.5f;

  for (int y = 0; y < height; ++y) {
    const float baseWave = sinf(y * 0.10f + t * (waterMode ? 8.2f : 3.2f));
    const float secondWave = sinf(y * 0.035f - t * (waterMode ? 4.9f : 2.1f));
    const float xOffset = waterMode ? (baseWave * 10.8f + secondWave * 5.8f)
                                    : (baseWave * 7.0f + secondWave * 9.0f);
    const float yOffset = waterMode ? (secondWave * 3.0f) : (baseWave * 3.0f);
    const float shimmer = waterMode ? std::max(0.0f, sinf(y * 0.07f + t * 10.8f)) : 0.0f;
    const int srcY = std::max(0, std::min(height - 1, static_cast<int>(y + yOffset)));

    for (int x = 0; x < width; ++x) {
      const float sampleX = cx + (static_cast<float>(x) - cx) + xOffset;
      uint16_t color = samplePlainLogoPixel(static_cast<int>(sampleX), srcY);
      if (waterMode) {
        color = blend565(color, rgb565(180, 240, 255), shimmer * 0.28f);
        logoRow[x] = color;
      } else {
        logoRow[x] = blend565(color, rgb565(82, 180, 255), 0.08f);
      }
    }

    canvas.pushImage(0, y, width, 1, logoRow);
  }
}

void drawLogoRotoZoom(uint32_t tickMs) {
  const float t = static_cast<float>(tickMs) * 0.001f;
  const int width = canvas.width();
  const int height = canvas.height();
  const float srcCx = width * 0.5f;
  const float srcCy = height * 0.5f;
  const float dstCx = width * 0.5f;
  const float dstCy = height * 0.5f;
  const float angle = t * 1.8f;
  const float zoom = 1.33f + 0.65f * sinf(t * 1.25f);
  const float cs = cosf(angle) / zoom;
  const float sn = sinf(angle) / zoom;

  for (int y = 0; y < height; ++y) {
    const float py = static_cast<float>(y) - dstCy;
    for (int x = 0; x < width; ++x) {
      const float px = static_cast<float>(x) - dstCx;
      const int srcX = wrapCoord(static_cast<int>(srcCx + px * cs - py * sn), width);
      const int srcY = wrapCoord(static_cast<int>(srcCy + px * sn + py * cs), height);
      logoRow[x] = samplePlainLogoPixel(srcX, srcY);
    }
    canvas.pushImage(0, y, width, 1, logoRow);
  }
}

void drawLogoBumpRipple(uint32_t tickMs) {
  const float t = static_cast<float>(tickMs) * 0.001f;
  const int width = canvas.width();
  const int height = canvas.height();
  const float cx = width * 0.5f;
  const float cy = height * 0.5f;
  const float lightX = -0.58f;
  const float lightY = -0.42f;
  const float maxRadius = sqrtf(static_cast<float>(width * width + height * height)) * 0.5f;
  const float travel = t * 118.0f;
  const float span = maxRadius * 2.0f;
  const float waveFreq = 0.17f;
  const float timeFreq = 15.8f;
  const float displacementScale = 24.0f;
  const float sourceX[3] = {cx, cx - 40.0f, cx + 34.0f};
  const float sourceY[3] = {cy, cy + 28.0f, cy - 24.0f};
  const float sourceWeight[3] = {1.0f, 0.46f, 0.38f};

  for (int y = 0; y < height; ++y) {
    const float fy = static_cast<float>(y);
    for (int x = 0; x < width; ++x) {
      const float fx = static_cast<float>(x);
      float gradX = 0.0f;
      float gradY = 0.0f;
      float waveMix = 0.0f;

      for (int i = 0; i < 3; ++i) {
        const float dx = fx - sourceX[i];
        const float dy = fy - sourceY[i];
        const float radius = sqrtf(dx * dx + dy * dy) + 0.0001f;

        float reflected = fmodf(radius + travel * (0.94f + 0.08f * i), span);
        float bounceDir = 1.0f;
        if (reflected > maxRadius) {
          reflected = span - reflected;
          bounceDir = -1.0f;
        }

        const float normR = std::min(reflected / maxRadius, 1.0f);
        const float damping = 1.0f - normR * 0.55f;
        const float wave = reflected * waveFreq - t * (timeFreq + i * 0.8f);
        const float slope = cosf(wave) * waveFreq * damping * bounceDir * sourceWeight[i];

        gradX += slope * (dx / radius);
        gradY += slope * (dy / radius);
        waveMix += sinf(wave) * damping * sourceWeight[i];
      }

      const int srcX = std::max(0, std::min(width - 1, static_cast<int>(fx - gradX * displacementScale)));
      const int srcY = std::max(0, std::min(height - 1, static_cast<int>(fy - gradY * displacementScale)));

      uint16_t color = samplePlainLogoPixel(srcX, srcY);

      const float shade = std::max(0.0f, (-gradX * lightX - gradY * lightY) * 0.85f);
      const float highlight = std::min(0.48f, shade * 0.52f);
      const float shadow = std::min(0.26f, std::max(0.0f, (gradX * lightX + gradY * lightY) * 0.36f));

      color = blend565(color, rgb565(215, 244, 255), highlight);
      color = blend565(color, rgb565(18, 44, 76), shadow);
      color = blend565(color, rgb565(120, 196, 255), std::min(0.24f, fabsf(waveMix) * 0.10f));
      logoRow[x] = color;
    }

    canvas.pushImage(0, y, width, 1, logoRow);
  }
}

uint32_t homeModeDuration(HomeMode mode) {
  uint32_t baseMs = kHomeEffectMs;
  switch (mode) {
    case HomeMode::Static:
      baseMs = static_cast<uint32_t>(kHomeStaticMs * staticDurationFactor(app.settings.staticDuration));
      break;
    case HomeMode::RotoZoom:
    case HomeMode::RippleBump:
      baseMs = kHomeLongEffectMs;
      break;
    default:
      baseMs = kHomeEffectMs;
      break;
  }
  return static_cast<uint32_t>(baseMs * effectDurationFactor(app.settings.effectDuration));
}

void drawHomeTransitionFlash(uint32_t phaseTimeMs, HomeMode mode) {
  if (phaseTimeMs >= 150) return;

  if (phaseTimeMs < 40) {
    canvas.fillScreen(mode == HomeMode::Static ? rgb565(255, 252, 244) : rgb565(222, 236, 255));
    return;
  }

  const uint16_t flashColor = mode == HomeMode::Static ? rgb565(255, 246, 214) : rgb565(196, 224, 255);
  const int spacing = phaseTimeMs < 100 ? 3 : 5;

  for (int y = 0; y < canvas.height(); y += spacing) {
    canvas.drawFastHLine(0, y, canvas.width(), flashColor);
  }
  canvas.drawRect(0, 0, canvas.width(), canvas.height(), flashColor);
}

void drawRasterBarsBorder(uint32_t tickMs) {
  const float t = static_cast<float>(tickMs) * 0.0014f;
  const uint16_t palette[] = {
      rgb565(255, 92, 164), rgb565(255, 190, 94), rgb565(132, 255, 184), rgb565(96, 196, 255)};

  const int innerX = homeInnerX();
  const int innerY = homeInnerY();
  const int innerW = homeInnerW();
  const int innerH = homeInnerH();

  canvas.fillScreen(rgb565(100, 128, 214));

  for (int y = 0; y < canvas.height(); ++y) {
    const int band = static_cast<int>(fmodf(y + t * 120.0f, 56.0f) / 14.0f) & 3;
    const uint16_t color = palette[band];

    if (y < innerY || y >= innerY + innerH) {
      canvas.drawFastHLine(0, y, canvas.width(), color);
    } else {
      canvas.drawFastHLine(0, y, innerX, color);
      canvas.drawFastHLine(innerX + innerW, y, canvas.width() - (innerX + innerW), color);
    }
  }

  canvas.fillRect(innerX, innerY, innerW, innerH, rgb565(53, 75, 121));
  canvas.drawRect(innerX, innerY, innerW, innerH, rgb565(132, 170, 255));
  drawBoxedLogoInner();
}

HomeMode nextHomeEffect(uint8_t effectIndex) {
  switch (effectIndex % 5u) {
    case 0: return HomeMode::Water;
    case 1: return HomeMode::RotoZoom;
    case 2: return HomeMode::SineWave;
    case 3: return HomeMode::RippleBump;
    default: return HomeMode::RasterBars;
  }
}

void enterHomeMode(HomeMode mode, uint32_t now) {
  app.home.mode = mode;
  app.home.startedAtMs = now;
  app.home.frameDirty = true;
}

void updateHomeDemo(uint32_t now) {
  if (!app.settings.animationsEnabled || app.settings.effectMode == DisplayEffectMode::Static) {
    if (app.home.mode != HomeMode::Static || app.home.startedAtMs == 0) {
      enterHomeMode(HomeMode::Static, now);
    }
    return;
  }

  if (app.home.startedAtMs == 0) {
    enterHomeMode(HomeMode::Static, now);
    return;
  }

  const uint32_t durationMs = homeModeDuration(app.home.mode);
  if (now - app.home.startedAtMs < durationMs) return;

  if (app.home.mode == HomeMode::Static) {
    enterHomeMode(selectedCycleEffect(), now);
  } else {
    if (displayUsesAutoCycle()) {
      app.home.nextEffectIndex = static_cast<uint8_t>((app.home.nextEffectIndex + 1) % 5u);
    }
    enterHomeMode(HomeMode::Static, now);
  }
}

void drawHomeDemo(uint32_t now) {
  const HomeMode mode = currentConfiguredHomeMode();
  const uint32_t phaseTimeMs = now - app.home.startedAtMs;
  const uint32_t effectTickMs =
      static_cast<uint32_t>(static_cast<float>(phaseTimeMs) * animationSpeedFactor(app.settings.animationSpeed));

  switch (mode) {
    case HomeMode::Static:     drawPlainLogoFrame(); break;
    case HomeMode::Water:      drawLogoDistortedRows(effectTickMs, true); break;
    case HomeMode::RotoZoom:   drawLogoRotoZoom(effectTickMs); break;
    case HomeMode::SineWave:   drawLogoDistortedRows(effectTickMs, false); break;
    case HomeMode::RippleBump: drawLogoBumpRipple(effectTickMs); break;
    case HomeMode::RasterBars: drawRasterBarsBorder(effectTickMs); break;
  }

  drawHomeTransitionFlash(phaseTimeMs, mode);
}

void drawMenuTitle(const char* title) {
  canvas.setTextDatum(top_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(196, 224, 255));
  canvas.drawString(title, canvas.width() / 2, 8);
}

void drawModal(uint32_t now) {
  if (app.modalText.isEmpty() || now > app.modalUntilMs) return;

  const int boxW = 206;
  const int boxH = 72;
  const int x = (canvas.width() - boxW) / 2;
  const int y = (canvas.height() - boxH) / 2;
  const uint16_t shell = rgb565(6, 10, 18);
  const uint16_t inner = rgb565(24, 32, 54);

  canvas.fillRoundRect(x, y, boxW, boxH, 12, shell);
  canvas.drawRoundRect(x, y, boxW, boxH, 12, app.modalColor);
  canvas.fillRoundRect(x + 6, y + 6, boxW - 12, boxH - 12, 10, inner);
  canvas.fillRoundRect(x + 12, y + 12, boxW - 24, 6, 3, app.modalColor);

  canvas.setTextDatum(top_center);
  canvas.setFont(&fonts::Font4);
  canvas.setTextColor(app.modalColor);

  const bool longText = canvas.textWidth(app.modalText) > (boxW - 24);
  if (longText) {
    canvas.setFont(&fonts::Font2);
    drawWrappedText(x + 12, y + 19, boxW - 24, app.modalText, 3, 14, app.modalColor);
  } else {
    canvas.drawString(app.modalText, canvas.width() / 2, y + 24);
  }
}

void drawMenu() {
  drawMenuTitle("MENU");

  constexpr int visibleRows = 4;
  const int selectedIndex = std::max(0, std::min(app.menuIndex, static_cast<int>(kMenuCount) - 1));
  const int start = std::max(0, std::min(selectedIndex - (visibleRows - 1), static_cast<int>(kMenuCount) - visibleRows));
  const int end = std::min(static_cast<int>(kMenuCount), start + visibleRows);

  for (int index = start; index < end; ++index) {
    const int row = index - start;
    const int y = 24 + row * 24;
    const bool selected = index == selectedIndex;
    const uint16_t fill = selected ? rgb565(58, 102, 184) : rgb565(18, 30, 52);
    const uint16_t border = selected ? rgb565(184, 228, 255) : rgb565(74, 108, 148);
    const uint16_t text = selected ? TFT_WHITE : rgb565(212, 226, 248);

    canvas.fillRoundRect(12, y, 216, 20, 8, fill);
    canvas.drawRoundRect(12, y, 216, 20, 8, border);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(text);
    canvas.drawString(kMenuItems[index], canvas.width() / 2, y + 10);
  }

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(top_right);
  canvas.setTextColor(rgb565(156, 190, 228));
  char pageInfo[16];
  snprintf(pageInfo, sizeof(pageInfo), "%d/%d", selectedIndex + 1, static_cast<int>(kMenuCount));
  canvas.drawString(pageInfo, canvas.width() - 8, 8);

  if (start > 0) {
    canvas.setTextDatum(top_center);
    canvas.drawString("^", canvas.width() / 2, 20);
  }
  if (end < static_cast<int>(kMenuCount)) {
    canvas.setTextDatum(top_center);
    canvas.drawString("v", canvas.width() / 2, 121);
  }
}

void drawCpuMenu() {
  drawMenuTitle("CPU SPEED");
  drawLabelValue(12, 20, "Current", app.currentCpuValue, rgb565(110, 230, 170));

  const int choiceCount = static_cast<int>(app.cpuChoiceCount == 0 ? 1 : app.cpuChoiceCount);
  const int start = std::max(0, std::min(app.cpuIndex - 1, choiceCount - 3));
  const int end = std::min(choiceCount, start + 3);

  for (int index = start; index < end; ++index) {
    const int y = 52 + (index - start) * 24;
    const bool selected = index == app.cpuIndex;
    const uint16_t fill = selected ? rgb565(48, 94, 164) : rgb565(16, 25, 42);
    const uint16_t border = selected ? rgb565(122, 228, 174) : rgb565(66, 96, 132);

    canvas.fillRoundRect(18, y, 204, 20, 8, fill);
    canvas.drawRoundRect(18, y, 204, 20, 8, border);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(selected ? rgb565(220, 255, 232) : TFT_WHITE);
    canvas.drawString(app.cpuChoiceCount == 0 ? "?" : app.cpuDisplayOptions[index], canvas.width() / 2, y + 10);
  }
}

void drawDisplaySettings() {
  drawMenuTitle("SETUP");

  const String values[kDisplayMenuCount] = {
      app.rfidReady ? String(autoNfcLabel(app.settings.autoNfc)) : String("no NFC"),
      cardConfirmLabel(app.settings.cardConfirmS),
      app.settings.animationsEnabled ? "On" : "Off",
      String(displayEffectModeLabel(app.settings.effectMode)),
      app.settings.joyOverlayEnabled ? "On" : "Off",
      app.settings.joyLedEnabled ? "On" : "Off",
      joyLedBrightnessLabel(app.settings.joyLedBrightness),
      String(joyThresholdLabel(app.settings.joyThreshold)),
      String(animationSpeedLabel(app.settings.animationSpeed)),
      String(effectDurationLabel(app.settings.effectDuration)),
      String(staticDurationLabel(app.settings.staticDuration)),
      brightnessLabel(app.settings.brightness),
      "Now"};

  constexpr int visibleRows = 4;
  const int selectedIndex = std::max(0, std::min(app.displaySettingsIndex, static_cast<int>(kDisplayMenuCount) - 1));
  const int start = std::max(0, std::min(selectedIndex - (visibleRows - 1),
                                         static_cast<int>(kDisplayMenuCount) - visibleRows));
  const int end = std::min(static_cast<int>(kDisplayMenuCount), start + visibleRows);

  for (int index = start; index < end; ++index) {
    const int row = index - start;
    const int y = 24 + row * 24;
    const bool selected = index == selectedIndex;
    const uint16_t fill = selected ? rgb565(48, 94, 164) : rgb565(16, 25, 42);
    const uint16_t border = selected ? rgb565(184, 228, 255) : rgb565(66, 96, 132);

    canvas.fillRoundRect(10, y, 220, 20, 8, fill);
    canvas.drawRoundRect(10, y, 220, 20, 8, border);

    canvas.setTextDatum(middle_left);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(rgb565(212, 226, 248));
    canvas.drawString(kDisplayMenuItems[index], 16, y + 10);

    canvas.setTextDatum(middle_right);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(selected ? rgb565(220, 255, 232) : TFT_WHITE);
    canvas.drawString(values[index], 224, y + 10);
  }

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(top_right);
  canvas.setTextColor(rgb565(156, 190, 228));
  char pageInfo[16];
  snprintf(pageInfo, sizeof(pageInfo), "%d/%d", selectedIndex + 1, static_cast<int>(kDisplayMenuCount));
  canvas.drawString(pageInfo, canvas.width() - 8, 8);

  if (start > 0) {
    canvas.setTextDatum(top_center);
    canvas.drawString("^", canvas.width() / 2, 20);
  }
  if (end < static_cast<int>(kDisplayMenuCount)) {
    canvas.setTextDatum(top_center);
    canvas.drawString("v", canvas.width() / 2, 121);
  }
}

void drawStatus() {
  drawMenuTitle(app.rfidReady ? "STATUS  -  RFID2" : "STATUS");
  const bool wifiOk = WiFi.status() == WL_CONNECTED;

  drawLabelValue(12, 20, "WiFi", wifiOk ? WiFi.SSID() : String("Disconnected"),
                 wifiOk ? rgb565(110, 230, 170) : rgb565(255, 170, 84));
  drawLabelValue(124, 20, "Target",
                 app.connection.targetReachable ? "Reachable" : "Not reached",
                 app.connection.targetReachable ? rgb565(110, 230, 170) : rgb565(255, 170, 84));
  drawLabelValue(12, 56, "Auth", app.connection.authOk ? "OK" : "Not verified",
                 app.connection.authOk ? rgb565(110, 230, 170) : rgb565(255, 170, 84));
  drawLabelValue(124, 56, "CPU", app.currentCpuValue, TFT_WHITE);
  drawLabelValue(12, 92, "Host", targetHost(), rgb565(208, 224, 248));

  canvas.fillRoundRect(118, 88, 110, 38, 8, rgb565(16, 24, 40));
  canvas.drawRoundRect(118, 88, 110, 38, 8, rgb565(74, 108, 148));
  drawWrappedTextInfo(126, 96, 94, app.connection.detail, 2, 12, rgb565(182, 198, 220));
}

void drawJoyOverlay() {
  if (!app.settings.joyOverlayEnabled) return;
  if (!app.joy.present) return;

  char buf[40];
  snprintf(buf, sizeof(buf), "J %4d %4d %c",
           static_cast<int>(app.joy.x),
           static_cast<int>(app.joy.y),
           app.joy.button ? '*' : '-');

  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(120, 220, 255), rgb565(8, 14, 30));
  canvas.drawString(buf, 6, 118);
}


// ------------------------------------------------------------
// List screens (NFC, WiFi, command selection)
//
// All lists look the same: title on top, four rows, the value on the right,
// plus page number and paging arrows.
// ------------------------------------------------------------
constexpr int kListRows = 4;

int listWindowStart(int selected, int count) {
  int start = std::max(0, std::min(selected - (kListRows - 1), count - kListRows));
  return std::max(0, start);
}

void drawListFrame(const char* title, int count, int selected) {
  drawMenuTitle(title);

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(top_right);
  canvas.setTextColor(rgb565(156, 190, 228));
  char pageInfo[16];
  snprintf(pageInfo, sizeof(pageInfo), "%d/%d", selected + 1, std::max(1, count));
  canvas.drawString(pageInfo, canvas.width() - 8, 8);

  const int start = listWindowStart(selected, count);
  canvas.setTextDatum(top_center);
  if (start > 0)                    canvas.drawString("^", canvas.width() / 2, 20);
  if (start + kListRows < count)    canvas.drawString("v", canvas.width() / 2, 121);
}

void drawListEntry(int row, const String& label, const String& value, bool selected) {
  const int y = 24 + row * 24;
  const uint16_t fill   = selected ? rgb565(48, 94, 164) : rgb565(16, 25, 42);
  const uint16_t border = selected ? rgb565(184, 228, 255) : rgb565(66, 96, 132);

  canvas.fillRoundRect(10, y, 220, 20, 8, fill);
  canvas.drawRoundRect(10, y, 220, 20, 8, border);

  canvas.setTextDatum(middle_left);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(212, 226, 248));
  canvas.drawString(label, 16, y + 10);

  if (!value.isEmpty()) {
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(selected ? rgb565(220, 255, 232) : TFT_WHITE);
    canvas.drawString(value, 224, y + 10);
  }
}

// ---- NFC submenu ----------------------------------------------------------
String nfcMenuValue(size_t index) {
  if (!app.rfidReady) return "no NFC";
  switch (index) {
    case kNfcRead:      return "present";
    case kNfcWriteCmd:  return "command";
    case kNfcWriteWifi: return gWifiCount == 0 ? "empty" : "write";
    default:            return "";
  }
}

void drawNfcMenu() {
  const int count    = static_cast<int>(kNfcMenuCount);
  const int selected = std::max(0, std::min(app.nfcMenuIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  drawListFrame("NFC / RFID", count, selected);
  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListEntry(row, kNfcMenuItems[index], nfcMenuValue(index), index == selected);
  }
}

// ---- WiFi submenu ---------------------------------------------------------
String wifiMenuValue(size_t index) {
  switch (index) {
    case kWifiFromCard:  return app.rfidReady ? "present" : "no NFC";
    case kWifiPortal:    return app.portalActive ? "on" : "off";
    case kWifiSavedList: return String(static_cast<unsigned>(gWifiCount));
    case kWifiToCard:    return !app.rfidReady ? "no NFC"
                              : (gWifiCount == 0 ? "empty" : "write");
    case kWifiDeleteOne: return gWifiCount == 0 ? "empty" : "select";
    case kWifiDeleteAll: return "Reset";
    default:             return "";
  }
}

void drawWifiMenu() {
  const int count    = static_cast<int>(kWifiMenuCount);
  const int selected = std::max(0, std::min(app.wifiMenuIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  drawListFrame("WiFi", count, selected);
  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListEntry(row, kWifiMenuItems[index], wifiMenuValue(index), index == selected);
  }
}

// ---- Saved networks -------------------------------------------------------
void drawWifiSaved() {
  const int count = static_cast<int>(gWifiCount);
  drawMenuTitle(app.wifiSavedDelete ? "DELETE NETWORK" : "SAVED");

  if (count == 0) {
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(rgb565(255, 190, 84));
    canvas.drawString("no network stored", canvas.width() / 2, 60);
    return;
  }

  const int selected = std::max(0, std::min(app.wifiSavedIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  drawListFrame(app.wifiSavedDelete ? "DELETE NETWORK" : "SAVED", count, selected);
  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    const bool online = WiFi.status() == WL_CONNECTED &&
                        WiFi.SSID() == gWifiProfiles[index].ssid;
    drawListEntry(row, gWifiProfiles[index].ssid,
                  app.wifiSavedDelete ? "delete" : (online ? "active" : "connect"),
                  index == selected);
  }
}

// ---- Select a command for a card ------------------------------------------
void drawCmdPick() {
  const int count    = static_cast<int>(cmdListCount());
  const int selected = std::max(0, std::min(app.cmdIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  drawListFrame("COMMAND CARD", count, selected);
  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListEntry(row, cardCommandLabel(cmdListAt(index)), "", index == selected);
  }
}

// ---- "Tap card" -----------------------------------------------------------
void drawCardWait(const char* title, const String& hint) {
  drawMenuTitle(title);

  const int boxW = 208;
  const int boxH = 62;
  const int x = (canvas.width() - boxW) / 2;
  const int y = 26;
  canvas.fillRoundRect(x, y, boxW, boxH, 10, rgb565(16, 25, 42));
  canvas.drawRoundRect(x, y, boxW, boxH, 10, rgb565(74, 108, 148));

  canvas.setTextDatum(top_center);
  canvas.setFont(&fonts::Font4);
  canvas.setTextColor(rgb565(120, 220, 255));
  canvas.drawString("PRESENT CARD", canvas.width() / 2, y + 8);

  canvas.setFont(&fonts::Font2);
  drawWrappedText(x + 8, y + 32, boxW - 16, hint.isEmpty() ? String("waiting for a card") : hint,
                  2, 13, rgb565(212, 226, 248));

  canvas.setTextDatum(top_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("A = back", canvas.width() / 2, 100);
}

// ---- Setup portal ---------------------------------------------------------
void drawPortal() {
  drawMenuTitle("SETUP-PORTAL");

  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::Font2);

  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("Network", 12, 24);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(kPortalSsid, 12, 38);

  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("Password", 12, 56);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(kPortalPass, 12, 70);

  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("Browser", 12, 88);
  canvas.setTextColor(rgb565(110, 230, 170));
  canvas.drawString(WiFi.softAPIP().toString(), 12, 102);

  canvas.setTextDatum(top_right);
  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("A = close", canvas.width() - 8, 102);
}

// ============================================================================
//  Navigation
// ============================================================================
void openNfcMenu(uint32_t now);
void openWifiMenu(uint32_t now);

// Pointer to the selection index of the current list screen
int* listSelection(int* countOut) {
  switch (app.screen) {
    case ScreenMode::Menu:
      *countOut = static_cast<int>(kMenuCount);
      return &app.menuIndex;
    case ScreenMode::CpuMenu:
      *countOut = static_cast<int>(std::max<size_t>(1, app.cpuChoiceCount));
      return &app.cpuIndex;
    case ScreenMode::DisplaySettings:
      *countOut = static_cast<int>(kDisplayMenuCount);
      return &app.displaySettingsIndex;
    case ScreenMode::NfcMenu:
      *countOut = static_cast<int>(kNfcMenuCount);
      return &app.nfcMenuIndex;
    case ScreenMode::WifiMenu:
      *countOut = static_cast<int>(kWifiMenuCount);
      return &app.wifiMenuIndex;
    case ScreenMode::WifiSaved:
      *countOut = static_cast<int>(gWifiCount);
      return &app.wifiSavedIndex;
    case ScreenMode::CmdPick:
      *countOut = static_cast<int>(cmdListCount());
      return &app.cmdIndex;
    default:
      *countOut = 0;
      return nullptr;
  }
}

void moveSelection(int delta) {
  int count = 0;
  int* target = listSelection(&count);
  if (target == nullptr || count <= 0) return;

  *target = ((*target + delta) % count + count) % count;
  app.home.frameDirty = true;
}

// One level back
void goBack(uint32_t now) {
  clearPendingPowerOff();

  switch (app.screen) {
    case ScreenMode::Home:
      break;
    case ScreenMode::Menu:
      setScreenMode(ScreenMode::Home, now);
      break;
    case ScreenMode::NfcRead:
    case ScreenMode::NfcWrite:
    case ScreenMode::CmdPick:
      app.pendingCardText = "";
      setScreenMode(ScreenMode::NfcMenu, now);
      break;
    case ScreenMode::WifiCard:
    case ScreenMode::WifiSaved:
      app.wifiSavedDelete = false;
      setScreenMode(ScreenMode::WifiMenu, now);
      break;
    case ScreenMode::WifiPortal:
      stopPortal(now);
      setScreenMode(ScreenMode::WifiMenu, now);
      break;
    default:
      setScreenMode(ScreenMode::Menu, now);
      break;
  }
}

// ---- Selection in the NFC menu --------------------------------------------
void nfcMenuSelect(uint32_t now) {
  if (!app.rfidReady) {
    setModal("NO RFID2", rgb565(255, 120, 96), now, 1800);
    return;
  }

  switch (app.nfcMenuIndex) {
    case kNfcRead:
      app.pendingCardText = "";
      app.rfidHint = "waiting for a card";
      setScreenMode(ScreenMode::NfcRead, now);
      break;

    case kNfcWriteCmd:
      // The CPU steps come from the c64u - load them once so the list
      // offers the values that are actually available.
      if (!app.cpuPathKnown) refreshCpuValue();
      app.cmdIndex = 0;
      setScreenMode(ScreenMode::CmdPick, now);
      break;

    case kNfcWriteWifi:
      if (gWifiCount == 0) {
        setModal("NO NETWORK", rgb565(255, 190, 84), now, 1800);
        return;
      }
      app.pendingCardText = wifiCardText(gWifiProfiles[0].ssid, gWifiProfiles[0].pass);
      app.rfidHint = gWifiProfiles[0].ssid;
      setScreenMode(ScreenMode::NfcWrite, now);
      break;

    default:
      break;
  }
}

void cmdPickSelect(uint32_t now) {
  const CardCommand cmd = cmdListAt(static_cast<size_t>(app.cmdIndex));
  const String text = cardCommandText(cmd);
  if (text.isEmpty()) {
    setModal("UNKNOWN COMMAND", rgb565(255, 120, 96), now, 1600);
    return;
  }
  app.pendingCardText = text;
  app.rfidHint = cardCommandLabel(cmd);
  setScreenMode(ScreenMode::NfcWrite, now);
}

// ---- Selection in the WiFi menu -------------------------------------------
void wifiMenuSelect(uint32_t now) {
  switch (app.wifiMenuIndex) {
    case kWifiFromCard:
      if (!app.rfidReady) {
        setModal("NO RFID2", rgb565(255, 120, 96), now, 1800);
        return;
      }
      app.rfidHint = "present WiFi card";
      setScreenMode(ScreenMode::WifiCard, now);
      break;

    case kWifiPortal:
      if (app.portalActive) {
        stopPortal(now);
        setModal("PORTAL OFF", rgb565(255, 190, 84), now, 1400);
        return;
      }
      startPortal(now);
      setScreenMode(ScreenMode::WifiPortal, now);
      break;

    case kWifiSavedList:
      if (gWifiCount == 0) {
        setModal("NO NETWORK", rgb565(255, 190, 84), now, 1600);
        return;
      }
      app.wifiSavedIndex  = 0;
      app.wifiSavedDelete = false;
      setScreenMode(ScreenMode::WifiSaved, now);
      break;

    case kWifiToCard:
      if (!app.rfidReady) {
        setModal("NO RFID2", rgb565(255, 120, 96), now, 1800);
        return;
      }
      if (gWifiCount == 0) {
        setModal("NO NETWORK", rgb565(255, 190, 84), now, 1600);
        return;
      }
      app.pendingCardText = wifiCardText(gWifiProfiles[0].ssid, gWifiProfiles[0].pass);
      app.rfidHint = gWifiProfiles[0].ssid;
      setScreenMode(ScreenMode::NfcWrite, now);
      break;

    case kWifiDeleteOne:
      if (gWifiCount == 0) {
        setModal("NO NETWORK", rgb565(255, 190, 84), now, 1600);
        return;
      }
      app.wifiSavedIndex  = 0;
      app.wifiSavedDelete = true;
      setScreenMode(ScreenMode::WifiSaved, now);
      break;

    case kWifiDeleteAll:
      wifiClearProfiles();
      setModal("ALL DELETED", rgb565(255, 190, 84), now, 1800);
      break;

    default:
      break;
  }
}

// In the list of saved networks the side button toggles between
// "connect" and "delete"; here it is only executed.
void wifiSavedSelect(uint32_t now) {
  if (gWifiCount == 0) {
    setScreenMode(ScreenMode::WifiMenu, now);
    return;
  }
  const size_t index = static_cast<size_t>(
      std::max(0, std::min(app.wifiSavedIndex, static_cast<int>(gWifiCount) - 1)));

  if (app.wifiSavedDelete) {
    const String ssid = gWifiProfiles[index].ssid;
    wifiRemoveProfile(index);
    app.wifiSavedIndex  = 0;
    app.wifiSavedDelete = false;
    setModal(ssid + " GONE", rgb565(255, 190, 84), now, 1600);
    if (gWifiCount == 0) setScreenMode(ScreenMode::WifiMenu, now);
    return;
  }

  gWifiTry = index;
  app.lastWiFiAttemptMs = 0;
  beginWiFi(now);
  setModal("CONNECTING...", rgb565(120, 220, 255), now, 1800);
  setScreenMode(ScreenMode::WifiMenu, now);
}

void openNfcMenu(uint32_t now) {
  app.nfcMenuIndex = 0;
  setScreenMode(ScreenMode::NfcMenu, now);
}

void openWifiMenu(uint32_t now) {
  app.wifiMenuIndex  = 0;
  app.wifiSavedDelete = false;
  setScreenMode(ScreenMode::WifiMenu, now);
}

// Select / execute on the current screen
void activateCurrent(uint32_t now) {
  // If the prompt of a PowerOff command card is pending, the button
  // press confirms it - just like tapping the same card again.
  if (app.cardPowerOffPending) {
    app.cardPowerOffPending = false;
    if (static_cast<int32_t>(now - app.cardPowerOffUntilMs) < 0) {
      performPowerOff(now);
      return;
    }
  }

  switch (app.screen) {
    case ScreenMode::Home:
      clearPendingPowerOff();
      setScreenMode(ScreenMode::Menu, now);
      break;
    case ScreenMode::Menu:            handleMenuSelect(now); break;
    case ScreenMode::CpuMenu:         setCpuSpeed(app.cpuIndex, now); break;
    case ScreenMode::Status:          runConnectionTest(now); break;
    case ScreenMode::DisplaySettings: activateDisplaySetting(now); break;
    case ScreenMode::NfcMenu:         nfcMenuSelect(now); break;
    case ScreenMode::CmdPick:         cmdPickSelect(now); break;
    case ScreenMode::WifiMenu:        wifiMenuSelect(now); break;
    case ScreenMode::WifiSaved:       wifiSavedSelect(now); break;
    case ScreenMode::NfcRead:
    case ScreenMode::NfcWrite:
    case ScreenMode::WifiCard:
    case ScreenMode::WifiPortal:
      goBack(now);
      break;
  }
}

void drawAppUi() {
  canvas.fillScreen(rgb565(8, 14, 30));

  switch (app.screen) {
    case ScreenMode::Home:            break;
    case ScreenMode::Menu:            drawMenu(); break;
    case ScreenMode::CpuMenu:         drawCpuMenu(); break;
    case ScreenMode::Status:          drawStatus(); break;
    case ScreenMode::DisplaySettings: drawDisplaySettings(); break;
    case ScreenMode::NfcMenu:         drawNfcMenu(); break;
    case ScreenMode::CmdPick:         drawCmdPick(); break;
    case ScreenMode::NfcRead:         drawCardWait("READ CARD", app.rfidHint); break;
    case ScreenMode::NfcWrite:        drawCardWait("WRITE CARD", app.rfidHint); break;
    case ScreenMode::WifiMenu:        drawWifiMenu(); break;
    case ScreenMode::WifiSaved:       drawWifiSaved(); break;
    case ScreenMode::WifiCard:        drawCardWait("WIFI CARD", app.rfidHint); break;
    case ScreenMode::WifiPortal:      drawPortal(); break;
  }

  drawJoyOverlay();
}

void setScreenMode(ScreenMode nextScreen, uint32_t now) {
  if (app.screen == nextScreen) return;

  if (app.screen == ScreenMode::Home && nextScreen != ScreenMode::Home) {
    app.home.pausedAtMs = now;
  } else if (app.screen != ScreenMode::Home && nextScreen == ScreenMode::Home) {
    if (app.home.startedAtMs == 0) {
      enterHomeMode(HomeMode::Static, now);
    } else if (app.home.pausedAtMs != 0) {
      app.home.startedAtMs += now - app.home.pausedAtMs;
      app.home.pausedAtMs = 0;
    }
  }

  app.screen = nextScreen;
  app.home.frameDirty = true;
}

bool render(uint32_t now) {
  const bool modalVisible = !app.modalText.isEmpty() && now <= app.modalUntilMs;
  const HomeMode effectiveHomeMode = currentConfiguredHomeMode();

  // During the setup portal the logo cache is released - then there is
  // just an empty area on the home screen instead of a crash.
  if (app.screen == ScreenMode::Home && plainLogoPixels == nullptr) {
    if (!app.home.frameDirty && modalVisible == app.lastModalVisible) return false;
    canvas.fillScreen(rgb565(8, 14, 30));
    if (modalVisible) drawModal(now);
    canvas.pushSprite(0, 0);
    app.lastModalVisible = modalVisible;
    app.home.frameDirty = false;
    return true;
  }

  if (app.screen == ScreenMode::Home && effectiveHomeMode == HomeMode::Static) {
    const bool needsRedraw = app.home.frameDirty || modalVisible != app.lastModalVisible;
    if (!needsRedraw) return false;
    drawPlainLogoFrame();
  } else if (app.screen == ScreenMode::Home) {
    drawHomeDemo(now);
  } else {
    drawAppUi();
  }

  if (modalVisible) {
    drawModal(now);
  }

  canvas.pushSprite(0, 0);
  app.lastModalVisible = modalVisible;
  app.home.frameDirty = false;
  return true;
}

void handleMenuSelect(uint32_t now) {
  switch (app.menuIndex) {
    case kMenuPowerOff:
      requestPowerOff(now);
      break;

    case kMenuCpuSpeed:
      refreshCpuValue();
      app.cpuIndex = cpuIndexFromValue(app.currentCpuValue);
      setScreenMode(ScreenMode::CpuMenu, now);
      break;

    case kMenuNfc:
      openNfcMenu(now);
      break;

    case kMenuWifi:
      openWifiMenu(now);
      break;

    case kMenuConnTest:
      runConnectionTest(now);
      break;

    case kMenuStatus:
      setScreenMode(ScreenMode::Status, now);
      break;

    case kMenuSettings:
      app.displaySettingsIndex = 0;
      setScreenMode(ScreenMode::DisplaySettings, now);
      break;

    default:
      break;
  }
}

}  // namespace

// ------------------------------------------------------------
// setup()
// ------------------------------------------------------------

void setup() {
  auto cfg = M5.config();
  cfg.clear_display = true;
  // If auto detection ever gets it wrong, the Plus2 is assumed.
  // Otherwise the display stays dark because the wrong panel is driven.
  cfg.fallback_board = m5::board_t::board_M5StickCPlus2;
  M5.begin(cfg);

  Serial.begin(115200);
  M5.Display.setRotation(3);

  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setSwapBytes(true);

  // If memory is short, the device keeps running without logo effects.
  // It used to hang here - a dark screen without any hint at all.
  const bool logoReady = ensureLogoCache();

  setFallbackCpuChoices();
  loadDisplaySettings();
  loadNetConfig();
  applyBrightness();

  // Initialize the MiniJoyC on the HAT port.
  // Important: the bus runs at 100 kHz. That is how the separate
  // test program for the RGB LED worked reliably.
  miniJoyReady = miniJoyBegin();

  uint8_t dummy = 0;
  if (miniJoyReady && joyReadRegister(kRegJoyX, &dummy)) {
    app.joy.present = true;
    setModal("MINIJOYC OK", rgb565(110, 230, 170), millis(), 1000);
  } else {
    app.joy.present = false;
    setModal("MINIJOYC ?", rgb565(255, 170, 84), millis(), 1200);
  }

  // Apply the LED status right after initialization.
  updateMiniJoyStatusLed();

  // Unit RFID2 on the Grove port. Own I2C bus, so that the MiniJoyC on the
  // HAT port stays untouched.
  Wire1.begin(kRfidSdaPin, kRfidSclPin, 100000UL);
  app.rfidReady = initRfid();
  Serial.printf("RFID2 %s\n", app.rfidReady ? "found" : "not found");

  // With several saved networks, the one with the best signal comes first.
  if (gWifiCount > 1) {
    WiFi.mode(WIFI_STA);
    wifiPickBestProfile();
  }

  app.configReady = configReady();
  beginWiFi(millis());
  refreshConnectionStatus(millis(), true);
  updateMiniJoyStatusLed();

  if (app.rfidReady) {
    setModal("RFID2 OK", rgb565(110, 230, 170), millis(), 1000);
  }

  if (!logoReady) {
    setModal("LOW MEMORY", rgb565(255, 170, 84), millis(), 2400);
  } else if (!app.configReady) {
    setModal(hasWiFiConfig() ? "HOST MISSING" : "SET UP WIFI",
             rgb565(255, 170, 84), millis(), 2400);
  }
}

// ------------------------------------------------------------
// loop()
// ------------------------------------------------------------
void loop() {
  static uint32_t nextFrameMs = 0;

  M5.update();
  const uint32_t now = millis();

  serviceWiFi(now);
  servicePortal(now);
  refreshConnectionStatus(now);
  updateMiniJoyStatusLed();
  serviceRfid(now);

  // Drop the PowerOff confirmation after the timeout
  if (app.pendingPowerOff && (now - app.pendingPowerOffAtMs > kPowerOffConfirmMs)) {
    app.pendingPowerOff = false;
  }

  if (app.screen == ScreenMode::Home) {
    updateHomeDemo(now);
  }

  // Internal buttons
  //   A (big, front)  : one level back; on the home screen reset
  //                     (two short presses = reboot)
  //   B (side)        : next entry; on the home screen Ultimate menu
  //   Power (short)   : select / execute
  if (M5.BtnA.wasPressed()) {
    clearPendingPowerOff();

    if (app.screen == ScreenMode::Home) {
      if (app.pendingSoftReset && now - app.pendingSoftResetAtMs <= kDoublePressMs) {
        app.pendingSoftReset = false;
        performHardReset(now);
      } else {
        app.pendingSoftReset = true;
        app.pendingSoftResetAtMs = now;
      }
    } else {
      goBack(now);
    }
  }

  if (M5.BtnB.wasPressed()) {
    clearPendingPowerOff();

    if (app.screen == ScreenMode::Home) {
      app.pendingSoftReset = false;
      performMenuButton(now);
    } else {
      moveSelection(+1);
    }
  }

  if (M5.BtnPWR.wasPressed()) {
    app.pendingSoftReset = false;
    activateCurrent(now);
  }

  // MiniJoyC
  processJoy(now);

  // Delayed soft reset after a single click on BtnA
  if (app.pendingSoftReset && now - app.pendingSoftResetAtMs > kDoublePressMs) {
    app.pendingSoftReset = false;
    performReset(now);
  }

  // Reload the CPU value once WiFi is up
  if (WiFi.status() == WL_CONNECTED && app.currentCpuValue == "Unknown" && app.screen == ScreenMode::Home) {
    refreshCpuValue();
  }

  // Refresh the screen
  if (nextFrameMs == 0 || static_cast<int32_t>(now - nextFrameMs) >= 0) {
    render(now);
    nextFrameMs = now + kFrameMs;
  }

  delay(5);
}
