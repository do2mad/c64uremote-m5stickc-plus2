// ============================================================================
//  C64uRemote  -  M5StickC Plus2  +  Unit RFID2 (WS1850S) am Grove-Port
// ----------------------------------------------------------------------------
//  Fernbedienung fuer den Commodore 64 Ultimate (c64u) / Ultimate64 Elite-II
//  ueber die ReST-API der Ultimate-Firmware (ab 3.11).
//
//  Basiert auf der Originalidee von Karl Prosser (@klumsy)
//      https://github.com/ReadyOS-C64/C64uRemote
//  erweitert von Martin Oswald (@mad) - https://1MHz.de
//
//  Lizenz: MIT - siehe LICENSE im Projektstamm.
//    Copyright (c) 2026 Karl Prosser  (Originalprojekt C64uRemote,
//                                      https://github.com/ReadyOS-C64/C64uRemote)
//    Copyright (c) 2026 Martin Oswald (Portierung und Erweiterungen)
//
//  Neu gegenueber der ersten StickC-Fassung:
//    * Unit RFID2 (WS1850S, I2C 0x28) am Grove-Port HY2.0-4P - optional.
//      Erkannt wird der Leser beim Start; fehlt er, bleibt alles wie bisher.
//        - WLAN-Karten LESEN: SSID und Passwort landen direkt im Geraet,
//          ohne neu zu flashen. Format identisch zu M5Dial und Core.
//        - Befehlskarten (CMD) lesen und ausfuehren: Reset, Reboot,
//          Ultimate-Menue, PowerOff (mit Rueckfrage) und CPU-Umschaltung.
//        - Karten BESCHREIBEN: Befehlskarten und WLAN-Karten.
//      Pfadkarten (Spiele) brauchen eine microSD und bleiben deshalb dem
//      Core / M5Dial vorbehalten - der Stick meldet das freundlich.
//    * Bis zu vier WLAN-Zugaenge im internen Speicher (NVS). Beim Start
//      nimmt das Geraet das Netz mit dem besten Empfang.
//    * Setup-Portal: eigener Accesspoint, Eingabe im Browser - der
//      Rettungsweg, wenn gerade keine WLAN-Karte zur Hand ist.
//    * build_env.h liefert nur noch die STARTWERTE fuer den ersten Start.
//
//  Der MiniJoyC (HAT-Port, I2C G0/G26) bleibt unveraendert unterstuetzt und
//  laeuft auf einem eigenen Bus - Joystick und RFID2 stoeren sich nicht.
//
//  Speicherhinweis: der ESP32-PICO-V3-02 hat 2 MB PSRAM im Gehaeuse, dort
//  liegen die beiden Logo-Caches (je 240*135*2 = 64 kB). Solange das
//  Setup-Portal laeuft, werden sie freigegeben und danach wieder angelegt -
//  Accesspoint und Webserver brauchen den internen Heap.
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

//#include "commodore_logo_rgb565.h"   // commodore Logo von Karl Prosser
#include "1MHz_logo_rgb565.h"     //1Mhz.de Logo von mad

namespace {

// ------------------------------------------------------------
// Zeit- und UI-Konstanten
// ------------------------------------------------------------
constexpr uint32_t kModalMs = 1400;
constexpr uint32_t kApiRetryDelayMs  = 250;    // Pause vor dem zweiten Versuch
constexpr uint32_t kHttpTimeoutMs = 2500;
constexpr uint32_t kWiFiRetryMs = 10000;
constexpr uint32_t kConnectionProbeMs = 15000;
constexpr uint32_t kFrameMs = 33;
constexpr uint32_t kDoublePressMs = 300;
constexpr uint32_t kHomeEffectMs = 5000;
constexpr uint32_t kHomeLongEffectMs = 7000;
constexpr uint32_t kHomeStaticMs = 1000;
constexpr size_t kMaxCpuChoices = 16;
constexpr size_t kMaxJoyChoices = 6;

// MiniJoyC: erst nach mehreren Lesefehlern als offline behandeln
constexpr uint8_t kJoyOfflineThreshold = 6;

// Falls der Stick-Button invertiert reagiert: auf true setzen
constexpr bool kJoyButtonActiveLow = false;

// PowerOff muss innerhalb dieses Zeitfensters ein zweites Mal bestätigt werden
constexpr uint32_t kPowerOffConfirmMs = 2000;

// ------------------------------------------------------------
// I2C-Pins fuer den MiniJoyC
// SDA -> G0
// SCL -> G26
// ------------------------------------------------------------
constexpr int kI2cSdaPin = 0;
constexpr int kI2cSclPin = 26;

// ------------------------------------------------------------
// Unit RFID2 (WS1850S) am Grove-Port HY2.0-4P
//   SDA -> G32   SCL -> G33   (eigener Bus: Wire1)
// Der MiniJoyC bleibt dadurch ungestoert auf Wire (G0/G26).
// ------------------------------------------------------------
constexpr int     kRfidSdaPin = 32;
constexpr int     kRfidSclPin = 33;
constexpr uint8_t kRfidAddr   = 0x28;

// Abstand der Hintergrundabfrage und Sperrzeit, damit dieselbe liegen
// gebliebene Karte nicht dauernd erneut ausgeloest wird.
constexpr uint32_t kRfidPollMs    = 250;
constexpr uint32_t kRfidRepeatMs  = 2500;

// ------------------------------------------------------------
// WLAN-Verwaltung
// ------------------------------------------------------------
constexpr size_t   kWifiProfileMax = 4;         // gespeicherte Netze
constexpr uint32_t kPortalIdleMs   = 300000;    // Portal nach 5 min beenden
constexpr uint32_t kPortalCloseMs  = 2500;      // Nachlauf nach dem Speichern
constexpr uint32_t kWifiCardConnectMs = 8000;   // Wartezeit nach WLAN-Karte

constexpr const char* kPortalSsid    = "C64uRemote-Setup";
constexpr const char* kPortalPass    = "c64ultimate";
constexpr uint8_t     kPortalDnsPort = 53;

// ------------------------------------------------------------
// MiniJoyC I2C-Adresse und Register
// ------------------------------------------------------------
constexpr uint8_t kMiniJoyAddr = 0x54;
constexpr uint8_t kRegJoyX = 0x20;
constexpr uint8_t kRegJoyY = 0x21;
constexpr uint8_t kRegJoyButton = 0x30;
constexpr uint8_t kRegJoyRgbLed = 0x40;

// ------------------------------------------------------------
// Schaltschwellen fuer alle Richtungen MiniJoyC 
// ------------------------------------------------------------
constexpr int kJoyThreshold = 100;

// ------------------------------------------------------------
// Menüeinträge
// ------------------------------------------------------------

// Reihenfolge des Hauptmenues. Die Namen verhindern, dass beim Einfuegen
// eines Eintrags die Auswertung in handleMenuSelect() verrutscht.
enum MenuId : uint8_t {
  kMenuPowerOff = 0,
  kMenuCpuSpeed,
  kMenuNfc,
  kMenuWifi,
  kMenuJoySwap,
  kMenuStatus,
  kMenuSettings,
  kMenuIdCount
};

constexpr const char* kMenuItems[] = {
    "PowerOff",
    "CPU Speed",
    "NFC / RFID",
    "WLAN",
    "Joystick Swap",
    "Status",
    "Settings",
};

// Setup-Liste. Auch hier gibt es Namen statt roher Indizes.
enum SettingsId : uint8_t {
  kSetAutoNfc = 0,
  kSetCardConfirm,     // Abfragezeit der PowerOff-Befehlskarte (NFC-Cmd)
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
  kSetJoystick,        // Portbelegung im c64u (Config "Joystick Swapper")
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
    "c64u Joystick",
    "Factory Reset",
};
constexpr size_t kDisplayMenuCount = sizeof(kDisplayMenuItems) / sizeof(kDisplayMenuItems[0]);
static_assert(kDisplayMenuCount == static_cast<size_t>(kSetItemCount),
              "kDisplayMenuItems und SettingsId sind aus dem Tritt geraten");
constexpr size_t kMenuCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
static_assert(kMenuCount == static_cast<size_t>(kMenuIdCount),
              "kMenuItems und MenuId sind aus dem Tritt geraten");

// Untermenue NFC / RFID
enum NfcMenuId : uint8_t {
  kNfcRead = 0,        // Karte auflegen und ausfuehren
  kNfcWriteCmd,        // Befehlskarte schreiben
  kNfcWriteWifi,       // WLAN-Zugang auf eine Karte schreiben
  kNfcMenuCount
};

constexpr const char* kNfcMenuItems[kNfcMenuCount] = {
    "Karte lesen",
    "CMD-Karte",
    "WLAN auf Karte",
};

// Untermenue WLAN
enum WifiMenuId : uint8_t {
  kWifiFromCard = 0,   // Zugang von einer NFC-Karte lesen
  kWifiPortal,         // Setup-Portal starten
  kWifiSavedList,      // gespeicherte Netze: verbinden
  kWifiToCard,         // aktuellen Zugang auf eine Karte schreiben
  kWifiDeleteOne,      // ein einzelnes Netz verwerfen
  kWifiDeleteAll,      // alle Netze verwerfen
  kWifiMenuCount
};

constexpr const char* kWifiMenuItems[kWifiMenuCount] = {
    "Von NFC-Karte",
    "Setup-Portal",
    "Gespeichert",
    "Auf NFC-Karte",
    "Netz loeschen",
    "Alle loeschen",
};

enum class ScreenMode : uint8_t {
  Home,
  Menu,
  CpuMenu,
  Status,
  DisplaySettings,
  NfcMenu,        // Untermenue NFC / RFID
  NfcRead,        // Karte auflegen -> Inhalt ausfuehren
  CmdPick,        // Befehl auswaehlen, der auf die Karte soll
  NfcWrite,       // Karte auflegen -> Text schreiben
  WifiMenu,       // Untermenue WLAN
  WifiCard,       // Karte auflegen -> Zugangsdaten lesen
  WifiSaved,      // gespeicherte Netze
  WifiPortal,     // Setup-Accesspoint laeuft
};

// Was auf einer Karte stehen kann, wenn es kein Dateipfad ist.
// Auf der Karte steht dann z. B. "CMD:RESET" oder "CMD:CPU=10".
enum class CardCmd : uint8_t {
  None,
  Reset,
  Reboot,
  UltiMenu,
  PowerOff,        // Argument = Bestaetigungszeit in Sekunden, 0 = sofort
  CpuSpeed,        // Argument = gewuenschter Wert, z. B. "10"
  JoySwap,         // Argument = Zielwert; ohne Argument wird umgeschaltet
};

struct CardCommand {
  CardCmd cmd = CardCmd::None;
  String  arg;                 // PowerOff: Sekunden, CpuSpeed: MHz, JoySwap: Ziel
  bool    hasArg = false;
};

// Kartenfamilie - danach richtet sich, wie gelesen und geschrieben wird.
enum class CardKind : uint8_t { None, Classic, Ultralight };

// Hintergrundabfrage des Lesers auf dem Startbild
enum class AutoNfcMode : uint8_t { Off, Slow, Normal, Fast };

// Ein gespeicherter WLAN-Zugang
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
// Status-/Datenstrukturen
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
  // Anzeige-/Bedienoptionen, die dauerhaft im Flash gespeichert werden.
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

  // Abstand der Hintergrundabfrage des RFID-Lesers
  AutoNfcMode autoNfc = AutoNfcMode::Normal;
  // Wie lange nach einem PowerOff-Kartenbefehl auf die Bestaetigung
  // gewartet wird (Sekunden). Die Karte muss so lange erneut aufgelegt
  // oder eine Taste gedrueckt werden.
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

  String joyCategory;
  String joyItem;
  String joyValue;
  bool joyPathKnown = false;
  String joyOptions[kMaxJoyChoices];
  size_t joyChoiceCount = 0;

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
  bool     rfidReady   = false;          // Leser beim Start gefunden
  String   rfidHint    = "";             // Text auf dem Kartenbildschirm
  String   pendingCardText;              // was beim Schreiben auf die Karte soll
  String   lastCardUid;                  // zuletzt gesehene Karte
  uint32_t lastCardMs  = 0;
  uint32_t lastRfidPollMs = 0;

  // Offene Rueckfrage eines PowerOff-Kartenbefehls
  bool     cardPowerOffPending = false;
  uint32_t cardPowerOffUntilMs = 0;
  String   cardPowerOffUid;

  // ---- WLAN -------------------------------------------------------------
  bool     wifiSavedDelete = false;      // Liste im Loesch-Modus
  bool     portalActive    = false;
  uint32_t portalStartedMs = 0;
  uint32_t portalSavedMs   = 0;
  String   wifiPendingSsid;              // Netz, auf dessen Verbindung wir warten
  uint32_t wifiPendingMs   = 0;

  HomeDemoState home = {};
  JoyState joy = {};
  SettingsState settings = {};
} app;

// ------------------------------------------------------------
// Grafik / Logo
// ------------------------------------------------------------
M5Canvas canvas(&M5.Display);
Preferences prefs;

// RFID2 haengt am zweiten I2C-Bus (Grove-Port). Der Treiber bekommt die
// Instanz in initRfid() zugewiesen.
MFRC522_I2C rfid(kRfidAddr, -1, &Wire1);

// Setup-Portal
WebServer gPortal(80);
DNSServer gPortalDns;

// Gespeicherte Netze und Ziel-Daten (NVS)
WifiProfile gWifiProfiles[kWifiProfileMax];
size_t      gWifiCount = 0;
size_t      gWifiTry   = 0;      // welches Netz als naechstes probiert wird
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
// Hilfsfunktionen String / Config
// ------------------------------------------------------------
String trimCopy(const String& value) {
  String result = value;
  result.trim();
  return result;
}

String configString(const char* value) {
  return trimCopy(value == nullptr ? "" : value);
}

// Die Werte aus build_env.h sind seit der WLAN-Einrichtung im Geraet nur
// noch STARTWERTE: sie gelten so lange, bis zum ersten Mal etwas im NVS
// gespeichert wurde (WLAN-Karte, Setup-Portal).
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
// Farben / Grafik
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
// Logo-Cache
//
// Zwei Vollbilder zu je 240*135*2 = 64 kB, nach Moeglichkeit im PSRAM.
// Waehrend des Setup-Portals werden sie freigegeben (Accesspoint und
// Webserver brauchen den internen Heap) und danach wieder angelegt.
// ------------------------------------------------------------
void releaseLogoCache() {
  if (plainLogoPixels != nullptr) { free(plainLogoPixels); plainLogoPixels = nullptr; }
  if (boxedLogoPixels != nullptr) { free(boxedLogoPixels); boxedLogoPixels = nullptr; }
}

// Ein Vollbild anfordern: der ESP32-PICO-V3-02 im StickC Plus2 hat 2 MB
// PSRAM, dort ist der Cache am besten aufgehoben. Ist PSRAM nicht aktiv
// (BOARD_HAS_PSRAM fehlt im Build), wird der interne Speicher genommen.
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
    Serial.printf("logo cache alloc failed (%u Byte je Bild, frei: %u)\n",
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
// Kleine Zustands-Helfer
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

// Vorwärtsdeklarationen
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
void cycleJoystickValue(uint32_t now);

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

// Abfragezeit einer PowerOff-Befehlskarte
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
// Zeit Einstellungen für Statik Logo
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

    case kSetJoystick:
      cycleJoystickValue(now);
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
// URL-Encoding / JSON-Helfer
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
// REST-Request
// ------------------------------------------------------------
// ---------------------------------------------------------------------------
// Joystick-Ports des c64u
//
// Gemeint ist hier nicht der MiniJoyC am HAT-Port, sondern die Portbelegung
// im c64u. Einen eigenen "machine:"-Befehl gibt es dafuer in der ReST-API
// nicht: der c64u fuehrt sie als Konfigurationseintrag "Joystick Swapper" in
// der Kategorie "U64 Specific Settings". Gesetzt wird also ueber /v1/configs,
// genau wie die CPU-Stufe. Die Werteliste kommt vom Geraet und heisst je nach
// Firmware "Normal", "Swapped", "WASD Port 1", "WASD Port 2".
// ---------------------------------------------------------------------------

// Kurzform fuer die Karte: "Swapped" -> "SWAPPED", "WASD Port 1" -> "WASD1".
String joyTokenFromValue(const String& value) {
  String upper = trimCopy(value);
  upper.toUpperCase();
  if (upper.indexOf("WASD") >= 0) {
    const String digits = extractDigits(upper);
    return digits.isEmpty() ? String("WASD") : ("WASD" + digits);
  }
  if (upper.indexOf("SWAP")   >= 0) return "SWAPPED";
  if (upper.indexOf("NORMAL") >= 0) return "NORMAL";
  upper.replace(" ", "");
  return upper;
}

// Sucht zu einer Kurzform den Wert, den der c64u erwartet. Ist die Liste noch
// unbekannt, wird die Kurzform unveraendert durchgereicht.
String joyValueFromToken(const String& token) {
  const String want = joyTokenFromValue(token);
  for (size_t i = 0; i < app.joyChoiceCount; ++i) {
    if (joyTokenFromValue(app.joyOptions[i]) == want) return app.joyOptions[i];
  }
  return trimCopy(token);
}

// Kurzer Text fuer Liste und Meldung.
String joyLabelFromToken(const String& token) {
  const String t = joyTokenFromValue(token);
  if (t == "NORMAL")  return "Normal";
  if (t == "SWAPPED") return "Swapped";
  if (t == "WASD")    return "WASD";
  if (t.startsWith("WASD")) return "WASD P" + t.substring(4);
  return trimCopy(token);
}

ApiResponse sendApiRequestOnce(const char* method, const String& path, bool authenticated) {
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

// Der HTTP-Server im c64u nimmt jeweils nur eine Verbindung an. Haengt ein
// zweites Geraet im Netz und fragt zufaellig im selben Moment, kommt
// "connection refused" zurueck, obwohl mit Funk und Adresse alles stimmt.
// Ein Transportfehler heisst, dass beim c64u nichts angekommen ist - ein
// zweiter Versuch ist deshalb gefahrlos und rettet genau diesen Fall.
ApiResponse sendApiRequest(const char* method, const String& path, bool authenticated) {
  ApiResponse result = sendApiRequestOnce(method, path, authenticated);
  if (!result.transportOk && hasTargetConfig()) {
    delay(kApiRetryDelayMs);
    result = sendApiRequestOnce(method, path, authenticated);
  }
  return result;
}

// ------------------------------------------------------------
// CPU-Speed-Abfrage
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
//  Netz-Konfiguration im internen Speicher (NVS)
// ----------------------------------------------------------------------------
//  Bis zu vier WLAN-Zugaenge plus Adresse und Passwort des c64u. Beim ersten
//  Start (noch nichts gespeichert) kommen die Werte aus build_env.h.
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

  // Noch nie etwas gespeichert -> Startwerte aus build_env.h uebernehmen.
  if (!known && !buildWifiSsid().isEmpty()) {
    gWifiProfiles[0].ssid = buildWifiSsid();
    gWifiProfiles[0].pass = buildWifiPass();
    gWifiCount = 1;
  }
}

// Index eines bereits gespeicherten Netzes, sonst -1
int wifiProfileIndex(const String& ssid) {
  const String needle = trimCopy(ssid);
  for (size_t i = 0; i < gWifiCount; ++i) {
    if (gWifiProfiles[i].ssid.equalsIgnoreCase(needle)) return static_cast<int>(i);
  }
  return -1;
}

// Netz aufnehmen oder aktualisieren. Ist die Liste voll, fliegt der aelteste
// Eintrag raus - die zuletzt benutzten Netze stehen dadurch vorn.
bool wifiAddProfile(const String& ssidRaw, const String& pass, bool store = true) {
  const String ssid = trimCopy(ssidRaw);
  if (ssid.isEmpty()) return false;

  const int existing = wifiProfileIndex(ssid);
  if (existing >= 0) {
    gWifiProfiles[existing].pass = pass;
    // nach vorn holen
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
//  WLAN-Karten
//
//  Auf der Karte steht das gleiche Schema wie in einem WLAN-QR-Code:
//      WIFI:S:MeinNetz;T:WPA;P:geheim;;
//  Sonderzeichen sind mit Backslash geschuetzt. Genau dieses Format
//  schreiben auch M5Dial und Core - eine dort beschriebene Karte laeuft
//  hier unveraendert.
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

// Gegenstueck zu parseWifiText.
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

// Das Netz mit dem besten Empfang nach vorn holen, damit beginWiFi() es
// zuerst probiert. Ohne Treffer bleibt die Reihenfolge wie sie ist.
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
//  Setup-Portal: eigener Accesspoint mit kleiner Weboberflaeche
// ----------------------------------------------------------------------------
//  Waehrend das Portal laeuft, werden die beiden Logo-Caches freigegeben:
//  Accesspoint und Webserver brauchen den internen Heap. Beim Beenden legt
//  ensureLogoCache() sie wieder an.
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
  String page = F("<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
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

  page += F("<label>WLAN-Name (SSID)</label><input name=\"ssid\" maxlength=\"32\" value=\"");
  page += htmlEscape(gWifiCount > 0 ? gWifiProfiles[0].ssid : String());
  page += F("\">");
  page += F("<label>WLAN-Passwort</label><input name=\"pass\" type=\"password\" maxlength=\"63\" value=\"\">");
  page += F("<label>Adresse des c64u</label><input name=\"host\" maxlength=\"40\" value=\"");
  page += htmlEscape(gTargetHost);
  page += F("\">");
  page += F("<label>Passwort des c64u (falls gesetzt)</label><input name=\"hostpw\" type=\"password\" maxlength=\"40\" value=\"\">");
  page += F("<button type=\"submit\">Speichern</button></form>");
  page += F("<p class=\"note\">Leer gelassene Passwortfelder lassen den bisherigen "
            "Wert unveraendert. Nach dem Speichern schaltet der Stick den "
            "Accesspoint ab und verbindet sich mit dem neuen Netz.</p>"
            "</body></html>");

  gPortal.send(200, "text/html; charset=utf-8", page);
}

void portalSendSaved(const String& ssid) {
  String page = F("<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>Gespeichert</title><style>"
                  "body{background:#0b1020;color:#dce6f8;font-family:system-ui,sans-serif;margin:0;padding:18px}"
                  "h1{font-size:20px}p{max-width:420px;color:#9cbee4}"
                  "</style></head><body><h1>Gespeichert</h1><p>");
  page += htmlEscape(ssid.isEmpty() ? String("Einstellungen uebernommen.")
                                    : ("Der Stick verbindet sich jetzt mit \"" + ssid + "\"."));
  page += F("</p><p>Diese Seite kann geschlossen werden - der Accesspoint "
            "schaltet sich gleich ab.</p></body></html>");
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
    // Leeres Passwortfeld: bekanntes Netz behaelt sein Passwort.
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

  releaseLogoCache();          // Heap fuer AP + Webserver freimachen

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
  gWifiTry = 0;                 // zuletzt gespeichertes Netz zuerst
  app.lastWiFiAttemptMs = 0;
  beginWiFi(now);
}

void servicePortal(uint32_t now) {
  if (!app.portalActive) return;

  gPortalDns.processNextRequest();
  gPortal.handleClient();

  // Nach dem Speichern kurz nachlaufen lassen, damit die Antwortseite
  // noch beim Browser ankommt.
  if (app.portalSavedMs != 0 && now - app.portalSavedMs >= kPortalCloseMs) {
    stopPortal(now);
    setModal("WLAN GESPEICHERT", rgb565(110, 230, 170), now, 1600);
    setScreenMode(ScreenMode::WifiMenu, now);
    return;
  }

  if (now - app.portalStartedMs >= kPortalIdleMs) {
    stopPortal(now);
    setModal("PORTAL BEENDET", rgb565(255, 170, 84), now, 1600);
    setScreenMode(ScreenMode::WifiMenu, now);
  }
}

// ============================================================================
//  NFC-Karten  (Unit RFID2 / WS1850S am Grove-Port)
// ----------------------------------------------------------------------------
//  Das Kartenformat ist identisch zu M5Dial und M5Stack Core: ein ganz
//  gewoehnlicher NDEF-Textrecord. Unterstuetzt werden
//
//    A) NTAG213/215/216 und MIFARE Ultralight (SAK 0x00, 4 Byte je Seite,
//       kein Schluessel): NDEF-TLV ab Seite 4. Die Seiten 0..3 (UID, Lock,
//       Capability Container) bleiben unangetastet.
//
//    B) MIFARE Classic 1K/4K/Mini (16 Byte je Block): NDEF-TLV in den
//       Datenbloecken ab Block 4, Sektor-Trailer werden uebersprungen.
//       Authentifiziert wird erst mit dem NDEF-Schluessel D3F7D3F7D3F7,
//       ersatzweise mit dem Werksschluessel FFFFFFFFFFFF. Trailer und MAD
//       werden nie beschrieben - eine Karte kann so nicht unbrauchbar
//       werden.
//
//  Beim Lesen wird zusaetzlich das alte Rohformat "C64UPATH" erkannt, damit
//  bereits beschriebene Karten weiter funktionieren.
// ============================================================================
constexpr size_t  kMaxTextLen  = 246;   // wie TeensyROM
constexpr size_t  kNdefBufSize = 288;   // TLV + Record + Text + Reserve
constexpr uint8_t kUlDataPage  = 4;     // NDEF beginnt auf Seite 4
constexpr uint8_t kMagic[8]    = {'C', '6', '4', 'U', 'P', 'A', 'T', 'H'};

MFRC522_I2C::MIFARE_Key kKeyNdef    = {{0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}};
MFRC522_I2C::MIFARE_Key kKeyFactory = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
bool gClassicNdefFormatted = false;     // Ergebnis der letzten Authentifizierung
bool gClassicTryNdefFirst  = true;      // pro Karte zurueckgesetzt

uint8_t trailerForBlock(uint8_t block) { return static_cast<uint8_t>((block / 4) * 4 + 3); }

// Linearer Index -> Datenblock, Sektor-Trailer werden ausgelassen.
// 0->4, 1->5, 2->6, 3->8, 4->9, 5->10, 6->12 ...
uint8_t classicDataBlock(size_t index) {
  const size_t sector = 1 + index / 3;
  return static_cast<uint8_t>(sector * 4 + (index % 3));
}

// Nach einem fehlgeschlagenen Auth ist die Karte im HALT-Zustand und
// antwortet nur noch auf WUPA. Deshalb gezielt aufwecken und neu auswaehlen.
bool reselectCard() {
  uint8_t atqa[2];
  uint8_t size = sizeof(atqa);
  if (rfid.PICC_WakeupA(atqa, &size) != MFRC522_I2C::STATUS_OK) return false;
  return rfid.PICC_Select(&(rfid.uid), 0) == MFRC522_I2C::STATUS_OK;
}

bool classicAuth(uint8_t block) {
  const uint8_t trailer = trailerForBlock(block);

  // Den zuletzt erfolgreichen Schluessel zuerst probieren, sonst kostet jeder
  // Block einen unnoetigen Fehlversuch samt Neuauswahl.
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
  if (found) gClassicTryNdefFirst = true;   // bei jeder neuen Karte neu ermitteln
  return found;
}

// ---------------------------------------------------------------------------
// Schnelle Anwesenheitsprobe fuer die Hintergrundabfrage
//
// Liegt keine Karte auf, wartet der MFRC522 nach dem REQA-Kommando, bis sein
// interner Timer ablaeuft - PCD_Init() stellt dafuer 0x03E8 = 1000 Schritte
// zu je 25 us ein, also 25 ms. Genau so lange haengt sonst die Hauptschleife.
// Eine Karte antwortet weit schneller, fuer die reine Probe genuegen 2 ms.
// ---------------------------------------------------------------------------
uint16_t gRfidTimerReload = 0x03E8;         // in initRfid() vom Chip gelesen
constexpr uint16_t kRfidProbeReload = 80;   // 80 * 25 us = 2 ms

void setRfidTimerReload(uint16_t ticks) {
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegH, static_cast<byte>(ticks >> 8));
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegL, static_cast<byte>(ticks & 0xFF));
}

bool cardPresentQuick() {
  setRfidTimerReload(kRfidProbeReload);
  const bool present = rfid.PICC_IsNewCardPresent();
  setRfidTimerReload(gRfidTimerReload);     // vor der Auswahl zurueckstellen
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
  // NTAG213/215/216 und Ultralight melden sich alle mit SAK 0x00
  if (type == MFRC522_I2C::PICC_TYPE_MIFARE_UL) return CardKind::Ultralight;
  return CardKind::None;
}

const char* cardKindLabel(CardKind kind) {
  switch (kind) {
    case CardKind::Classic:    return "MIFARE Classic";
    case CardKind::Ultralight: return "NTAG / Ultralight";
    default:                   return "unbekannt";
  }
}

// ---- Ultralight / NTAG: 4 Byte pro Seite ----------------------------------
// MIFARE_Read liefert immer 16 Byte, also vier Seiten auf einmal.
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

// ---- Zugriff auf den NDEF-Datenbereich, 16 Byte am Stueck ------------------
bool cardReadChunk(CardKind kind, size_t chunk, uint8_t* out16) {
  if (kind == CardKind::Classic) return rfidReadBlock(classicDataBlock(chunk), out16);
  return ulRead16(static_cast<uint8_t>(kUlDataPage + chunk * 4), out16);
}

bool cardWriteChunk(CardKind kind, size_t chunk, const uint8_t* data16) {
  if (kind == CardKind::Classic) return rfidWriteBlock(classicDataBlock(chunk), data16);
  return ulWrite16(static_cast<uint8_t>(kUlDataPage + chunk * 4), data16);
}

// ---------------------------------------------------------------------------
// NDEF: ein einzelner Text-Record (Well Known, UTF-8, Sprache "en")
//
//   TLV      : 03 <len> ... FE
//   Record   : D1 01 <plen> 54 | 02 'e' 'n' | <text>
//              D1 = MB|ME|SR|TNF=1 (Well Known), 54 = 'T'
// ---------------------------------------------------------------------------
size_t buildNdefText(const String& text, uint8_t* out, size_t cap) {
  const size_t textLen    = text.length();
  const size_t payloadLen = 3 + textLen;          // Status + "en" + Text
  const size_t recordLen  = 4 + payloadLen;       // Header + Typlaenge + Laenge + Typ
  const size_t total      = 2 + recordLen + 1;    // TLV-Kopf + Record + Terminator
  if (payloadLen > 255 || total > cap) return 0;

  size_t i = 0;
  out[i++] = 0x03;                                       // TLV: NDEF-Nachricht
  out[i++] = static_cast<uint8_t>(recordLen);
  out[i++] = 0xD1;                                       // MB|ME|SR|TNF=Well Known
  out[i++] = 0x01;                                       // Typlaenge
  out[i++] = static_cast<uint8_t>(payloadLen);
  out[i++] = 'T';                                        // Typ "Text"
  out[i++] = 0x02;                                       // UTF-8, Sprachcode 2 Zeichen
  out[i++] = 'e';
  out[i++] = 'n';
  for (size_t k = 0; k < textLen; ++k) out[i++] = static_cast<uint8_t>(text[k]);
  out[i++] = 0xFE;                                       // TLV: Ende

  // Auf ein Vielfaches von 16 auffuellen, damit ganze Bloecke geschrieben werden
  while (i % 16 != 0 && i < cap) out[i++] = 0x00;
  return i;
}

// Sucht im Datenstrom den ersten Text-Record und gibt seinen Inhalt zurueck.
bool parseNdefText(const uint8_t* data, size_t len, String* out) {
  size_t i = 0;

  // TLV-Kette abklappern, bis die NDEF-Nachricht kommt
  size_t msgStart = 0;
  size_t msgLen   = 0;
  while (i < len) {
    const uint8_t tag = data[i++];
    if (tag == 0x00) continue;                     // NULL-TLV
    if (tag == 0xFE) return false;                 // Ende ohne Nachricht
    if (i >= len) return false;

    size_t tlvLen = data[i++];
    if (tlvLen == 0xFF) {                          // 3-Byte-Laenge
      if (i + 1 >= len) return false;
      tlvLen = (static_cast<size_t>(data[i]) << 8) | data[i + 1];
      i += 2;
    }
    if (tag == 0x03) { msgStart = i; msgLen = tlvLen; break; }
    i += tlvLen;                                   // anderes TLV ueberspringen
  }
  if (msgLen == 0 || msgStart + msgLen > len) return false;

  // Records der Nachricht durchgehen
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

    // Manche Schreiber tragen eine falsche Payload-Laenge ein. Beim letzten
    // Record (ME-Flag) hat deshalb die TLV-Laenge Vorrang.
    if ((header & 0x40) != 0 && payloadPos < end) {
      const size_t fromTlv = end - payloadPos;
      if (fromTlv != payloadLen) payloadLen = fromTlv;
    }
    if (payloadPos + payloadLen > len) {
      if (payloadPos >= len) return false;
      payloadLen = len - payloadPos;          // lieber kuerzen als aufgeben
    }

    // Well Known "T" = Text-Record
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
          if (b == 0x00 || b == 0xFE) break;   // Fuellbytes / TLV-Ende
          text += static_cast<char>(b);
        }
        *out = text;
        return true;
      }
    }

    p = payloadPos + payloadLen;
    if ((header & 0x40) != 0) break;               // ME: letzter Record
  }
  return false;
}

// ---------------------------------------------------------------------------
// Karteninhalt lesen: erst NDEF, ersatzweise das alte Rohformat "C64UPATH"
// ---------------------------------------------------------------------------
struct CardContent {
  bool   ok       = false;
  bool   isNdef   = false;
  bool   isLegacy = false;
  String text;                 // Rohtext von der Karte
  String error;
};

CardContent readCardContent(CardKind kind) {
  CardContent result;
  if (kind == CardKind::None) {
    result.error = "Kartentyp nicht unterstuetzt";
    return result;
  }

  static uint8_t buffer[kNdefBufSize];
  memset(buffer, 0, sizeof(buffer));

  if (!cardReadChunk(kind, 0, buffer)) {
    result.error = (kind == CardKind::Classic) ? "Block 4 nicht lesbar (Schluessel?)"
                                               : "Seite 4 nicht lesbar";
    return result;
  }
  cardReadChunk(kind, 1, buffer + 16);

  // Altes Rohformat?
  if (memcmp(buffer, kMagic, sizeof(kMagic)) == 0) {
    const uint8_t len = buffer[9];
    if (len == 0 || len > 128) {
      result.error = "Laenge ungueltig";
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
        result.error = "Lesefehler (Altformat)";
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

  // NDEF: Laenge aus dem TLV holen und nur so viel nachladen wie noetig
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

  result.error = (buffer[0] == 0x03) ? "NDEF ohne Text-Record" : "kein NDEF-Text";
  return result;
}

// ---------------------------------------------------------------------------
// Karte beschreiben: immer als NDEF-Text-Record
// ---------------------------------------------------------------------------
bool writeCardText(CardKind kind, const String& text, String* errorOut) {
  if (kind == CardKind::None) {
    if (errorOut) *errorOut = "Kartentyp nicht unterstuetzt";
    return false;
  }
  if (text.isEmpty() || text.length() > kMaxTextLen) {
    if (errorOut) *errorOut = "Text zu lang (max " + String(kMaxTextLen) + ")";
    return false;
  }

  static uint8_t buffer[kNdefBufSize];
  const size_t total = buildNdefText(text, buffer, sizeof(buffer));
  if (total == 0) {
    if (errorOut) *errorOut = "NDEF passt nicht";
    return false;
  }

  const size_t chunks = total / 16;
  for (size_t chunk = 0; chunk < chunks; ++chunk) {
    if (!cardWriteChunk(kind, chunk, buffer + chunk * 16)) {
      if (errorOut) {
        *errorOut = (chunk == 0) ? "Karte nicht beschreibbar"
                                 : "Karte zu klein ab Block " + String(chunk);
      }
      return false;
    }
  }

  // Kontrolle: zurueklesen und vergleichen
  const CardContent check = readCardContent(kind);
  if (!check.ok || check.text != text) {
    if (errorOut) *errorOut = check.ok ? "Kontrolle abweichend" : check.error;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Befehlskarten
//
//     CMD:RESET
//     CMD:REBOOT
//     CMD:MENU
//     CMD:POWEROFF=0      sofort ausschalten
//     CMD:POWEROFF=8      nachfragen, 8 s Zeit fuer die Bestaetigung
//     CMD:POWEROFF        nachfragen mit der am Geraet eingestellten Zeit
//     CMD:CPU=10          CPU auf 10 MHz stellen
//     CMD:JOY             Joystickports umschalten (Normal <-> Swapped)
//     CMD:JOY=SWAPPED     Ports fest setzen; auch NORMAL, WASD1, WASD2
//
// Gross-/Kleinschreibung und Leerzeichen sind egal. Dasselbe Format benutzen
// M5Dial und Core - eine Karte laeuft an allen Geraeten.
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
  else if (body == "JOY" || body == "JOYSTICK") cmd.cmd = CardCmd::JoySwap;
  else return false;

  if (cmd.cmd == CardCmd::CpuSpeed && arg.isEmpty()) return false;

  *out = cmd;
  return true;
}

// Bestaetigungszeit eines PowerOff-Befehls in Sekunden.
// Ohne Argument gilt die Geraeteeinstellung, 0 bedeutet "ohne Nachfrage".
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
    case CardCmd::JoySwap:  return (c.hasArg && !c.arg.isEmpty())
                                   ? ("CMD:JOY=" + joyTokenFromValue(c.arg))
                                   : String("CMD:JOY");
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
      return sec == 0 ? String("PowerOff direkt")
                      : ("PowerOff, " + String(sec) + "s Abfrage");
    }
    case CardCmd::CpuSpeed: return "CPU " + c.arg + " MHz";
    case CardCmd::JoySwap:  return (c.hasArg && !c.arg.isEmpty())
                                   ? ("Joystick " + joyLabelFromToken(c.arg))
                                   : String("Joystick tauschen");
    default:                return "?";
  }
}

// ---- Auswahlliste zum Beschreiben einer Karte -----------------------------
// Feste Befehle zuerst, danach die Joystickwerte und die CPU-Stufen, die
// der c64u anbietet.
constexpr size_t kCmdFixedCount = 6;

size_t cmdListCount() { return kCmdFixedCount + app.joyChoiceCount + app.cpuChoiceCount; }

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
    case 5: c.cmd = CardCmd::JoySwap; return c;   // umschalten, ohne Argument
    default: break;
  }
  size_t rest = index - kCmdFixedCount;
  if (rest < app.joyChoiceCount) {
    c.cmd    = CardCmd::JoySwap;
    c.arg    = joyTokenFromValue(app.joyOptions[rest]);
    c.hasArg = true;
    return c;
  }
  rest -= app.joyChoiceCount;
  if (rest < app.cpuChoiceCount) {
    c.cmd    = CardCmd::CpuSpeed;
    c.arg    = extractDigits(app.cpuDisplayOptions[rest]);
    c.hasArg = true;
  }
  return c;
}

// Kuerzel rechts neben dem Listeneintrag.
const char* cmdListTag(size_t index) {
  if (index < kCmdFixedCount) return "";
  if (index - kCmdFixedCount < app.joyChoiceCount) return "JOY";
  return "CPU";
}

// ------------------------------------------------------------
// WLAN
// ------------------------------------------------------------
// Verbindet mit dem naechsten gespeicherten Netz. Bei mehreren Eintraegen
// wird bei jedem Versuch weitergeschaltet, bis eines antwortet.
void beginWiFi(uint32_t now) {
  if (!hasWiFiConfig()) return;
  if (gWifiTry >= gWifiCount) gWifiTry = 0;

  const WifiProfile& profile = gWifiProfiles[gWifiTry];

  // Waehrend das Setup-Portal laeuft, bleibt der Accesspoint bestehen.
  if (!app.portalActive) WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                  // kein Modem-Sleep, sonst zaehe Antworten
  WiFi.begin(profile.ssid.c_str(), profile.pass.c_str());
  app.lastWiFiAttemptMs = now;

  if (gWifiCount > 1) gWifiTry = (gWifiTry + 1) % gWifiCount;
}

// Merkt sich, mit welchem Netz die Verbindung zustande kam. Nach einem
// Aussetzer wird dann zuerst wieder dieses versucht statt blind das naechste.
// Sind zwei Netze gespeichert und nur eines ist erreichbar, kostet das sonst
// jedes zweite Mal einen kompletten Wiederholungstakt.
bool gWifiNoted = false;

void serviceWiFi(uint32_t now) {
  if (app.portalActive) return;          // Portal hat Vorrang
  if (!hasWiFiConfig()) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (!gWifiNoted) {
      const int index = wifiProfileIndex(WiFi.SSID());
      if (index >= 0) gWifiTry = static_cast<size_t>(index);
      gWifiNoted = true;
    }
    return;
  }
  gWifiNoted = false;

  if (app.lastWiFiAttemptMs == 0 || now - app.lastWiFiAttemptMs >= kWiFiRetryMs) {
    beginWiFi(now);
  }
}

// Aktualisiert den bekannten Verbindungsstatus im Hintergrund in groben Abstaenden.
// Dadurch kann die RGB-LED den echten Gesamtstatus anzeigen, ohne dass jedes Mal
// manuell auf der Statusseite nachgesehen werden muss.
void refreshConnectionStatus(uint32_t now, bool force = false) {
  app.connection.wifiConnected = WiFi.status() == WL_CONNECTED;

  if (!configReady()) {
    app.connection.targetReachable = false;
    app.connection.authOk = false;
    app.connection.detail = hasWiFiConfig() ? "Host nicht gesetzt" : "WLAN nicht eingerichtet";
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

  // Ohne hinterlegtes Passwort waere die zweite Anfrage byte-gleich mit der
  // ersten - der Header X-Password wird ja nur gesetzt, wenn eines da ist.
  // Jede gesparte Anfrage macht auf dem c64u Platz fuer ein zweites Geraet.
  if (targetPassword().isEmpty()) {
    app.connection.authOk = reach.apiOk;
    app.connection.detail = reach.apiOk ? "Reachable + auth ok"
                                        : (reach.errors.isEmpty() ? "Auth failed" : reach.errors);
    return;
  }

  const ApiResponse auth = sendApiRequest("GET", "/v1/version", true);
  app.connection.authOk = auth.apiOk;
  app.connection.detail = auth.apiOk ? "Reachable + auth ok"
                                     : (auth.errors.isEmpty() ? "Auth failed" : auth.errors);
}

// ------------------------------------------------------------
// Verbindungstest
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
// REST-Aktionen
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
  setModal("POWER OFF? NOCHMAL!", rgb565(255, 170, 84), now, 1600);
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

// Sucht in einer Kategorie den Eintrag mit "Joystick" im Namen.
bool inspectJoyCategory(const String& category, String* itemOut, String* valueOut) {
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
    if (key.indexOf("Joystick") >= 0) {
      *itemOut  = key;
      *valueOut = jsonValueToString(kv.value());
      return true;
    }
  }
  return false;
}

// Holt Werteliste und aktuellen Stand des gefundenen Eintrags.
bool refreshJoyChoices() {
  if (app.joyCategory.isEmpty() || app.joyItem.isEmpty()) return false;

  const ApiResponse response = sendApiRequest(
      "GET", "/v1/configs/" + urlEncode(app.joyCategory) + "/" + urlEncode(app.joyItem), true);
  if (!response.apiOk) return false;

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) return false;

  JsonVariant itemObject = doc[app.joyCategory][app.joyItem];
  if (itemObject.isNull()) return false;

  app.joyChoiceCount = 0;
  JsonArray values = itemObject["values"].as<JsonArray>();
  for (JsonVariant value : values) {
    if (app.joyChoiceCount >= kMaxJoyChoices) break;
    app.joyOptions[app.joyChoiceCount] = trimCopy(jsonValueToString(value));
    app.joyChoiceCount += 1;
  }
  if (app.joyChoiceCount == 0) return false;

  app.joyValue = trimCopy(jsonValueToString(itemObject["current"]));
  return true;
}

// Findet Kategorie und Eintragsnamen einmalig heraus und merkt sie sich.
bool resolveJoyPath(String* detailOut = nullptr) {
  if (app.joyPathKnown) {
    if (app.joyChoiceCount == 0) refreshJoyChoices();
    return true;
  }

  String item, value;
  if (inspectJoyCategory("U64 Specific Settings", &item, &value)) {
    app.joyCategory  = "U64 Specific Settings";
    app.joyItem      = item;
    app.joyValue     = trimCopy(value);
    app.joyPathKnown = true;
    refreshJoyChoices();
    return true;
  }

  const ApiResponse listResponse = sendApiRequest("GET", "/v1/configs", true);
  if (!listResponse.apiOk) {
    if (detailOut) *detailOut = listResponse.errors.isEmpty() ? "Config list failed" : listResponse.errors;
    return false;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, listResponse.body) != DeserializationError::Ok) {
    if (detailOut) *detailOut = "Config list parse failed";
    return false;
  }

  JsonArray categories = doc["categories"].as<JsonArray>();
  for (JsonVariant valueVariant : categories) {
    const String category = valueVariant.as<const char*>();
    if (inspectJoyCategory(category, &item, &value)) {
      app.joyCategory  = category;
      app.joyItem      = item;
      app.joyValue     = trimCopy(value);
      app.joyPathKnown = true;
      refreshJoyChoices();
      return true;
    }
  }
  if (detailOut) *detailOut = "Joystick item not found";
  return false;
}

// Setzt die Portbelegung auf einen festen Wert.
void applyJoystickValue(const String& wanted, uint32_t now) {
  clearPendingPowerOff();

  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  String detail;
  if (!resolveJoyPath(&detail)) {
    setModal(detail, rgb565(255, 120, 96), now, 1900);
    return;
  }

  const String target = joyValueFromToken(wanted);
  const String path = "/v1/configs/" + urlEncode(app.joyCategory) + "/" + urlEncode(app.joyItem)
                    + "?value=" + urlEncode(target);

  const ApiResponse response = sendApiRequest("PUT", path, true);
  if (response.apiOk) {
    app.joyValue = target;
    setModal("JOY " + joyLabelFromToken(target), rgb565(110, 230, 170), now, 1400);
  } else {
    const String text = response.errors.isEmpty() ? "JOY SET FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
  app.home.frameDirty = true;
}

// Schaltet zwischen "Normal" und "Swapped" hin und her. Steht der c64u auf
// einem WASD-Modus, geht es zurueck auf "Normal".
void toggleJoystickSwap(uint32_t now) {
  clearPendingPowerOff();
  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  String detail;
  if (!resolveJoyPath(&detail)) {
    setModal(detail, rgb565(255, 120, 96), now, 1900);
    return;
  }
  refreshJoyChoices();

  applyJoystickValue(joyTokenFromValue(app.joyValue) == "NORMAL" ? "SWAPPED" : "NORMAL", now);
}

// Schaltet im Setup durch alle Werte, die der c64u anbietet.
void cycleJoystickValue(uint32_t now) {
  clearPendingPowerOff();
  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  String detail;
  if (!resolveJoyPath(&detail)) {
    setModal(detail, rgb565(255, 120, 96), now, 1900);
    return;
  }
  refreshJoyChoices();
  if (app.joyChoiceCount == 0) {
    setModal("JOY?", rgb565(255, 120, 96), now, 1900);
    return;
  }

  size_t index = 0;
  for (size_t i = 0; i < app.joyChoiceCount; ++i) {
    if (joyTokenFromValue(app.joyOptions[i]) == joyTokenFromValue(app.joyValue)) { index = i; break; }
  }
  index = (index + 1) % app.joyChoiceCount;
  applyJoystickValue(app.joyOptions[index], now);
}


// ============================================================================
//  Karten auswerten und ausfuehren
// ============================================================================
bool render(uint32_t now);

// Fuehrt einen Kartenbefehl aus. Die UID wird gebraucht, damit ein PowerOff
// nur von derselben Karte bestaetigt werden kann.
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

      // Zweites Auflegen derselben Karte innerhalb des Fensters bestaetigt.
      if (app.cardPowerOffPending && uid == app.cardPowerOffUid &&
          static_cast<int32_t>(now - app.cardPowerOffUntilMs) < 0) {
        app.cardPowerOffPending = false;
        performPowerOff(now);
        return;
      }

      if (sec == 0) {                       // "CMD:POWEROFF=0" - ohne Nachfrage
        performPowerOff(now);
        return;
      }

      app.cardPowerOffPending = true;
      app.cardPowerOffUid     = uid;
      app.cardPowerOffUntilMs = now + sec * 1000u;
      app.rfidHint            = "Karte nochmal auflegen";
      setModal("POWER OFF? NOCHMAL!", rgb565(255, 190, 84), now, sec * 1000u);
      return;
    }

    case CardCmd::JoySwap:
      if (cmd.hasArg && !cmd.arg.isEmpty()) applyJoystickValue(cmd.arg, now);
      else                                  toggleJoystickSwap(now);
      return;

    case CardCmd::CpuSpeed: {
      if (!app.cpuPathKnown) refreshCpuValue();
      const int index = cpuIndexFromValue(cmd.arg);
      setCpuSpeed(index, now);
      return;
    }

    default:
      setModal("BEFEHL UNBEKANNT", rgb565(255, 120, 96), now, 2000);
      return;
  }
}

// Uebernimmt einen WLAN-Zugang von einer Karte und baut die Verbindung auf.
void applyWifiCard(const String& ssid, const String& pass, uint32_t now) {
  if (!wifiAddProfile(ssid, pass)) {
    app.rfidHint = "SSID oder Passwort zu lang";
    setModal("KARTE UNGUELTIG", rgb565(255, 120, 96), now, 2200);
    return;
  }

  app.rfidHint = ssid;
  app.home.frameDirty = true;
  setModal("VERBINDE...", rgb565(120, 220, 255), now, kWifiCardConnectMs + 1500);
  render(now);

  // wifiAddProfile sortiert das Netz nach vorn; von dort aus wird gezielt
  // dieses eine Netz versucht und nicht die ganze Liste durchgegangen.
  const int index = wifiProfileIndex(ssid);
  gWifiTry = index >= 0 ? static_cast<size_t>(index) : 0;
  app.lastWiFiAttemptMs = 0;

  if (app.portalActive) stopPortal(now);   // beendet den AP und verbindet selbst
  else                  beginWiFi(now);

  // Kurz auf das Ergebnis warten, damit die Rueckmeldung etwas taugt. Laenger
  // als kWifiCardConnectMs wird nicht gewartet - der Rest laeuft ueber die
  // regelmaessigen Versuche in serviceWiFi weiter.
  const uint32_t deadline = millis() + kWifiCardConnectMs;
  while (millis() < deadline && WiFi.status() != WL_CONNECTED) delay(100);

  const bool connected = WiFi.status() == WL_CONNECTED;
  app.modalText = "";
  app.rfidHint  = connected ? (ssid + "  " + WiFi.localIP().toString())
                            : (ssid + " nicht erreichbar");
  app.home.frameDirty = true;
  setModal(connected ? "WLAN AKTIV" : "NETZ NICHT DA",
           connected ? rgb565(110, 230, 170) : rgb565(255, 190, 84), millis(), 2400);
  refreshConnectionStatus(millis(), true);
  updateMiniJoyStatusLed();
}

// Verarbeitet eine ausgewaehlte Karte passend zum aktuellen Bildschirm.
void processCard(uint32_t now) {
  const CardKind kind = cardKind();
  if (kind == CardKind::None) {
    rfidRelease();
    app.rfidHint = "Kartentyp nicht unterstuetzt";
    app.home.frameDirty = true;
    return;
  }

  const String uid = cardUidString();

  // ---- Schreiben ----------------------------------------------------------
  if (app.screen == ScreenMode::NfcWrite) {
    String error;
    const bool ok = writeCardText(kind, app.pendingCardText, &error);
    rfidRelease();
    app.rfidHint = ok ? (String(cardKindLabel(kind)) + "  " + uid) : error;
    app.home.frameDirty = true;
    setModal(ok ? "KARTE OK" : "SCHREIBFEHLER",
             ok ? rgb565(110, 230, 170) : rgb565(255, 120, 96), now, ok ? 1800 : 2200);
    return;
  }

  // ---- Lesen --------------------------------------------------------------
  const CardContent content = readCardContent(kind);
  rfidRelease();

  if (!content.ok) {
    app.rfidHint = content.error;
    app.home.frameDirty = true;
    setModal("KARTE LEER?", rgb565(255, 190, 84), now, 2000);
    return;
  }

  const String text = sanitizeCardText(content.text);

  // ---- WLAN-Karte auf der Einrichtungsseite -------------------------------
  if (app.screen == ScreenMode::WifiCard) {
    String ssid;
    String pass;
    if (!parseWifiText(text, &ssid, &pass) || ssid.isEmpty()) {
      app.rfidHint = "das ist keine WLAN-Karte";
      app.home.frameDirty = true;
      setModal("FALSCHE KARTE", rgb565(255, 120, 96), now, 2200);
      return;
    }
    applyWifiCard(ssid, pass, now);
    setScreenMode(ScreenMode::WifiMenu, now);
    return;
  }

  // ---- WLAN-Karte ausserhalb der Einrichtung ------------------------------
  //
  // Karte auflegen genuegt: das Netz wird uebernommen und gleich verbunden.
  if (textLooksLikeWifi(text)) {
    String ssid;
    String pass;
    if (!parseWifiText(text, &ssid, &pass) || ssid.isEmpty()) {
      app.rfidHint = "WLAN-Karte ohne SSID";
      app.home.frameDirty = true;
      setModal("KEIN NETZ", rgb565(255, 120, 96), now, 2200);
      return;
    }

    // Laeuft die Verbindung schon, ist nichts zu tun. Das faengt auch den
    // Fall ab, dass die Karte liegen bleibt.
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
      app.rfidHint = ssid;
      app.home.frameDirty = true;
      setModal("SCHON VERBUNDEN", rgb565(110, 230, 170), now, 1800);
      return;
    }

    applyWifiCard(ssid, pass, now);
    return;
  }

  // ---- Befehlskarte -------------------------------------------------------
  CardCommand command;
  if (parseCardCommand(text, &command)) {
    runCardCommand(command, uid, now);
    return;
  }

  // ---- Alles andere: Pfadkarte --------------------------------------------
  // Dafuer braeuchte der Stick eine microSD mit den Programmdateien. Die
  // Karte bleibt gueltig, sie gehoert nur an M5Dial oder Core.
  Serial.printf("Karte gelesen (%s): '%s'\n",
                content.isNdef ? "NDEF-Text" : "Altformat", text.c_str());
  app.rfidHint = "Programmkarte - dafuer fehlt die SD";
  app.home.frameDirty = true;
  setModal("BRAUCHT SD-KARTE", rgb565(255, 190, 84), now, 2400);
}

// Auf diesen Bildschirmen wird regulaer auf eine Karte gewartet.
bool onRfidScreen() {
  return app.screen == ScreenMode::NfcRead ||
         app.screen == ScreenMode::NfcWrite ||
         app.screen == ScreenMode::WifiCard;
}

void serviceRfid(uint32_t now) {
  if (!app.rfidReady) return;

  // Abgelaufene PowerOff-Rueckfrage einer Karte verwerfen
  if (app.cardPowerOffPending &&
      static_cast<int32_t>(now - app.cardPowerOffUntilMs) >= 0) {
    app.cardPowerOffPending = false;
  }

  const bool onCardScreen = onRfidScreen();

  if (onCardScreen) {
    if (now - app.lastRfidPollMs < kRfidPollMs) return;
  } else {
    // Hintergrundabfrage nur auf dem Startbild
    if (app.settings.autoNfc == AutoNfcMode::Off) return;
    if (app.screen != ScreenMode::Home) return;
    if (now - app.lastRfidPollMs < autoNfcIntervalMs(app.settings.autoNfc)) return;
  }
  app.lastRfidPollMs = now;

  // Auf den Kartenseiten mit vollem Zeitfenster, im Hintergrund mit der
  // kurzen Probe - sonst haengt die Hauptschleife bei jedem Durchlauf 25 ms.
  if (!(onCardScreen ? cardPresent() : cardPresentQuick())) return;

  // Eine liegen gebliebene Karte wird sonst im Sekundentakt erneut
  // ausgefuehrt. Die Rueckfrage eines PowerOff-Befehls darf dagegen
  // jederzeit durch erneutes Auflegen bestaetigt werden.
  const String uid = cardUidString();
  if (!app.cardPowerOffPending && uid == app.lastCardUid &&
      now - app.lastCardMs < kRfidRepeatMs) {
    rfidRelease();
    return;
  }
  app.lastCardUid = uid;
  app.lastCardMs  = now;

  if (!onCardScreen) app.rfidHint = "Karte erkannt";
  processCard(now);
}

// ---------------------------------------------------------------------------
// Erkennung des Lesers
// ---------------------------------------------------------------------------
bool i2cDevicePresent(TwoWire& bus, uint8_t address) {
  bus.beginTransmission(address);
  return bus.endTransmission() == 0;
}

bool initRfid() {
  // Meldet sich unter 0x28 nichts, steckt kein Leser am Grove-Port.
  if (!i2cDevicePresent(Wire1, kRfidAddr)) return false;

  rfid.PCD_Init();
  delay(20);

  // Nur zur Information - der WS1850S meldet hier nicht immer denselben
  // Wert wie ein echter MFRC522, deshalb haengt daran keine Entscheidung.
  const uint8_t version = rfid.PCD_ReadRegister(MFRC522_I2C::VersionReg);
  Serial.printf("RFID2 (I2C 0x%02X) VersionReg = 0x%02X\n", kRfidAddr, version);

  // Das von PCD_Init() gesetzte Zeitfenster merken, damit die schnelle Probe
  // es hinterher exakt wiederherstellen kann.
  const uint16_t reload =
      static_cast<uint16_t>(rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegH) << 8) |
      rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegL);
  if (reload > kRfidProbeReload) gRfidTimerReload = reload;
  return true;
}

// ------------------------------------------------------------
// MiniJoyC I2C lesen
// ------------------------------------------------------------
// ------------------------------------------------------------
// MiniJoyC: Bus starten und RGB-LED setzen
//
// Gelesen wird der Joystick weiter unten ohnehin direkt ueber Wire. Fuer
// diese beiden Handgriffe braucht es deshalb keine eigene Bibliothek -
// das spart eine Abhaengigkeit und die Mehrdeutigkeits-Warnungen aus
// deren Quelltext (requestFrom(uint8_t, int)).
// ------------------------------------------------------------
bool miniJoyBegin() {
  Wire.begin(kI2cSdaPin, kI2cSclPin, 100000UL);
  delay(10);
  Wire.beginTransmission(kMiniJoyAddr);
  return Wire.endTransmission() == 0;
}

// Farbe im Format 0xRRGGBB. Der Baustein erwartet hinter dem Register
// drei Bytes in der Reihenfolge Rot, Gruen, Blau.
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
// MiniJoyC RGB-LED
// ------------------------------------------------------------
// Die Status-LED wird ueber die offizielle MiniJoyC-Library angesteuert.
// Genau diese Methode hat bereits im separaten Testprogramm funktioniert
// und ist daher hier die zuverlaessigste Variante.
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

  // Feine Statuslogik der LED:
  // Rot             = kein WLAN
  // Blau            = Target nicht erreichbar
  // Blau blinkend   = Target erreichbar, aber Auth fehlgeschlagen
  // Gruen           = WLAN + Target erreichbar + Auth OK
  //
  // Das Blinken laeuft bewusst nur ueber die LED-Ausgabe und aendert keine
  // anderen Statusdaten. So bleibt die Anzeige ruhig, waehrend die LED den
  // Fehlerzustand trotzdem deutlich sichtbar macht.
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

// Richtungs-Mapping für deinen Einbau
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
// Joystick-Aktionen
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
// UI-Helfer
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
      app.rfidReady ? String(autoNfcLabel(app.settings.autoNfc)) : String("kein NFC"),
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
      app.joyValue.isEmpty() ? String("?") : joyLabelFromToken(app.joyValue),
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
// Listenbildschirme (NFC, WLAN, Befehlsauswahl)
//
// Alle Listen sehen gleich aus: Titel oben, vier Zeilen, rechts der Wert,
// dazu Seitenzahl und Blaetterpfeile.
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

// ---- NFC-Untermenue -------------------------------------------------------
String nfcMenuValue(size_t index) {
  if (!app.rfidReady) return "kein NFC";
  switch (index) {
    case kNfcRead:      return "auflegen";
    case kNfcWriteCmd:  return "Befehl";
    case kNfcWriteWifi: return gWifiCount == 0 ? "leer" : "schreiben";
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

// ---- WLAN-Untermenue ------------------------------------------------------
String wifiMenuValue(size_t index) {
  switch (index) {
    case kWifiFromCard:  return app.rfidReady ? "auflegen" : "kein NFC";
    case kWifiPortal:    return app.portalActive ? "an" : "aus";
    case kWifiSavedList: return String(static_cast<unsigned>(gWifiCount));
    case kWifiToCard:    return !app.rfidReady ? "kein NFC"
                              : (gWifiCount == 0 ? "leer" : "schreiben");
    case kWifiDeleteOne: return gWifiCount == 0 ? "leer" : "waehlen";
    case kWifiDeleteAll: return "Reset";
    default:             return "";
  }
}

void drawWifiMenu() {
  const int count    = static_cast<int>(kWifiMenuCount);
  const int selected = std::max(0, std::min(app.wifiMenuIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  drawListFrame("WLAN", count, selected);
  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListEntry(row, kWifiMenuItems[index], wifiMenuValue(index), index == selected);
  }
}

// ---- Gespeicherte Netze ---------------------------------------------------
void drawWifiSaved() {
  const int count = static_cast<int>(gWifiCount);
  drawMenuTitle(app.wifiSavedDelete ? "NETZ LOESCHEN" : "GESPEICHERT");

  if (count == 0) {
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(rgb565(255, 190, 84));
    canvas.drawString("kein Netz gespeichert", canvas.width() / 2, 60);
    return;
  }

  const int selected = std::max(0, std::min(app.wifiSavedIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  drawListFrame(app.wifiSavedDelete ? "NETZ LOESCHEN" : "GESPEICHERT", count, selected);
  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    const bool online = WiFi.status() == WL_CONNECTED &&
                        WiFi.SSID() == gWifiProfiles[index].ssid;
    drawListEntry(row, gWifiProfiles[index].ssid,
                  app.wifiSavedDelete ? "loeschen" : (online ? "aktiv" : "verbinden"),
                  index == selected);
  }
}

// ---- Befehl fuer eine Karte auswaehlen ------------------------------------
void drawCmdPick() {
  const int count    = static_cast<int>(cmdListCount());
  const int selected = std::max(0, std::min(app.cmdIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  drawListFrame("BEFEHLSKARTE", count, selected);
  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListEntry(row, cardCommandLabel(cmdListAt(index)),
                  cmdListTag(static_cast<size_t>(index)), index == selected);
  }
}

// ---- "Karte auflegen" -----------------------------------------------------
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
  canvas.drawString("KARTE AUFLEGEN", canvas.width() / 2, y + 8);

  canvas.setFont(&fonts::Font2);
  drawWrappedText(x + 8, y + 32, boxW - 16, hint.isEmpty() ? String("warte auf Karte") : hint,
                  2, 13, rgb565(212, 226, 248));

  canvas.setTextDatum(top_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("A = zurueck", canvas.width() / 2, 100);
}

// ---- Setup-Portal ---------------------------------------------------------
void drawPortal() {
  drawMenuTitle("SETUP-PORTAL");

  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::Font2);

  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("Netz", 12, 24);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(kPortalSsid, 12, 38);

  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("Passwort", 12, 56);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(kPortalPass, 12, 70);

  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("Browser", 12, 88);
  canvas.setTextColor(rgb565(110, 230, 170));
  canvas.drawString(WiFi.softAPIP().toString(), 12, 102);

  canvas.setTextDatum(top_right);
  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString("A = beenden", canvas.width() - 8, 102);
}

// ============================================================================
//  Navigation
// ============================================================================
void openNfcMenu(uint32_t now);
void openWifiMenu(uint32_t now);

// Zeiger auf den Auswahlindex des aktuellen Listenbildschirms
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

// Eine Ebene zurueck
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

// ---- Auswahl im NFC-Menue -------------------------------------------------
void nfcMenuSelect(uint32_t now) {
  if (!app.rfidReady) {
    setModal("KEIN RFID2", rgb565(255, 120, 96), now, 1800);
    return;
  }

  switch (app.nfcMenuIndex) {
    case kNfcRead:
      app.pendingCardText = "";
      app.rfidHint = "warte auf Karte";
      setScreenMode(ScreenMode::NfcRead, now);
      break;

    case kNfcWriteCmd:
      // Die CPU-Stufen kommen vom c64u - einmal nachladen, damit die Liste
      // die tatsaechlich moeglichen Werte anbietet.
      if (!app.cpuPathKnown) refreshCpuValue();
      if (!app.joyPathKnown) resolveJoyPath();
      app.cmdIndex = 0;
      setScreenMode(ScreenMode::CmdPick, now);
      break;

    case kNfcWriteWifi:
      if (gWifiCount == 0) {
        setModal("KEIN NETZ", rgb565(255, 190, 84), now, 1800);
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
    setModal("BEFEHL UNBEKANNT", rgb565(255, 120, 96), now, 1600);
    return;
  }
  app.pendingCardText = text;
  app.rfidHint = cardCommandLabel(cmd);
  setScreenMode(ScreenMode::NfcWrite, now);
}

// ---- Auswahl im WLAN-Menue ------------------------------------------------
void wifiMenuSelect(uint32_t now) {
  switch (app.wifiMenuIndex) {
    case kWifiFromCard:
      if (!app.rfidReady) {
        setModal("KEIN RFID2", rgb565(255, 120, 96), now, 1800);
        return;
      }
      app.rfidHint = "WLAN-Karte auflegen";
      setScreenMode(ScreenMode::WifiCard, now);
      break;

    case kWifiPortal:
      if (app.portalActive) {
        stopPortal(now);
        setModal("PORTAL AUS", rgb565(255, 190, 84), now, 1400);
        return;
      }
      startPortal(now);
      setScreenMode(ScreenMode::WifiPortal, now);
      break;

    case kWifiSavedList:
      if (gWifiCount == 0) {
        setModal("KEIN NETZ", rgb565(255, 190, 84), now, 1600);
        return;
      }
      app.wifiSavedIndex  = 0;
      app.wifiSavedDelete = false;
      setScreenMode(ScreenMode::WifiSaved, now);
      break;

    case kWifiToCard:
      if (!app.rfidReady) {
        setModal("KEIN RFID2", rgb565(255, 120, 96), now, 1800);
        return;
      }
      if (gWifiCount == 0) {
        setModal("KEIN NETZ", rgb565(255, 190, 84), now, 1600);
        return;
      }
      app.pendingCardText = wifiCardText(gWifiProfiles[0].ssid, gWifiProfiles[0].pass);
      app.rfidHint = gWifiProfiles[0].ssid;
      setScreenMode(ScreenMode::NfcWrite, now);
      break;

    case kWifiDeleteOne:
      if (gWifiCount == 0) {
        setModal("KEIN NETZ", rgb565(255, 190, 84), now, 1600);
        return;
      }
      app.wifiSavedIndex  = 0;
      app.wifiSavedDelete = true;
      setScreenMode(ScreenMode::WifiSaved, now);
      break;

    case kWifiDeleteAll:
      wifiClearProfiles();
      setModal("ALLE GELOESCHT", rgb565(255, 190, 84), now, 1800);
      break;

    default:
      break;
  }
}

// In der Liste der gespeicherten Netze schaltet die Seitentaste zwischen
// "verbinden" und "loeschen" um; hier wird nur ausgefuehrt.
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
    setModal(ssid + " WEG", rgb565(255, 190, 84), now, 1600);
    if (gWifiCount == 0) setScreenMode(ScreenMode::WifiMenu, now);
    return;
  }

  gWifiTry = index;
  app.lastWiFiAttemptMs = 0;
  beginWiFi(now);
  setModal("VERBINDE...", rgb565(120, 220, 255), now, 1800);
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

// Auswaehlen / ausfuehren auf dem aktuellen Bildschirm
void activateCurrent(uint32_t now) {
  // Laeuft gerade die Rueckfrage einer PowerOff-Befehlskarte, bestaetigt der
  // Tastendruck sie - genau wie ein zweites Auflegen derselben Karte.
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
    case ScreenMode::NfcRead:         drawCardWait("KARTE LESEN", app.rfidHint); break;
    case ScreenMode::NfcWrite:        drawCardWait("KARTE SCHREIBEN", app.rfidHint); break;
    case ScreenMode::WifiMenu:        drawWifiMenu(); break;
    case ScreenMode::WifiSaved:       drawWifiSaved(); break;
    case ScreenMode::WifiCard:        drawCardWait("WLAN-KARTE", app.rfidHint); break;
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

  // Waehrend des Setup-Portals ist der Logo-Cache freigegeben - dann gibt es
  // auf dem Startbild nur eine leere Flaeche statt eines Absturzes.
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

    case kMenuJoySwap:
      toggleJoystickSwap(now);
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
  // Falls die automatische Erkennung einmal danebengreift, gilt der Plus2.
  // Sonst bleibt das Display dunkel, weil ein falsches Panel angesteuert wird.
  cfg.fallback_board = m5::board_t::board_M5StickCPlus2;
  M5.begin(cfg);

  Serial.begin(115200);
  M5.Display.setRotation(3);

  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setSwapBytes(true);

  // Reicht der Speicher nicht, laeuft das Geraet ohne Logo-Effekte weiter.
  // Frueher blieb es hier haengen - ein dunkler Bildschirm ohne jeden Hinweis.
  const bool logoReady = ensureLogoCache();

  setFallbackCpuChoices();
  loadDisplaySettings();
  loadNetConfig();
  applyBrightness();

  // MiniJoyC am HAT-Port initialisieren.
  // Wichtig: der Bus laeuft mit 100 kHz. Genau so hat auch das separate
  // Testprogramm fuer die RGB-LED zuverlaessig funktioniert.
  miniJoyReady = miniJoyBegin();

  uint8_t dummy = 0;
  if (miniJoyReady && joyReadRegister(kRegJoyX, &dummy)) {
    app.joy.present = true;
    setModal("MINIJOYC OK", rgb565(110, 230, 170), millis(), 1000);
  } else {
    app.joy.present = false;
    setModal("MINIJOYC ?", rgb565(255, 170, 84), millis(), 1200);
  }

  // LED-Status direkt nach der Initialisierung uebernehmen.
  updateMiniJoyStatusLed();

  // Unit RFID2 am Grove-Port. Eigener I2C-Bus, damit der MiniJoyC am
  // HAT-Port unberuehrt bleibt.
  Wire1.begin(kRfidSdaPin, kRfidSclPin, 100000UL);
  app.rfidReady = initRfid();
  Serial.printf("RFID2 %s\n", app.rfidReady ? "gefunden" : "nicht gefunden");

  // Bei mehreren gespeicherten Netzen zuerst das mit dem besten Empfang.
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
    setModal("WENIG SPEICHER", rgb565(255, 170, 84), millis(), 2400);
  } else if (!app.configReady) {
    setModal(hasWiFiConfig() ? "HOST FEHLT" : "WLAN EINRICHTEN",
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

  // PowerOff-Bestätigung nach Timeout verwerfen
  if (app.pendingPowerOff && (now - app.pendingPowerOffAtMs > kPowerOffConfirmMs)) {
    app.pendingPowerOff = false;
  }

  if (app.screen == ScreenMode::Home) {
    updateHomeDemo(now);
  }

  // Interne Tasten
  //   A (gross, vorn) : eine Ebene zurueck; auf dem Startbild Reset
  //                     (zweimal kurz = Reboot)
  //   B (seitlich)    : naechster Eintrag; auf dem Startbild Ultimate-Menue
  //   Power (kurz)    : auswaehlen / ausfuehren
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

  // Verzögerter Soft-Reset nach Einzelklick auf BtnA
  if (app.pendingSoftReset && now - app.pendingSoftResetAtMs > kDoublePressMs) {
    app.pendingSoftReset = false;
    performReset(now);
  }

  // CPU-Wert nachladen, sobald WLAN steht
  if (WiFi.status() == WL_CONNECTED && app.currentCpuValue == "Unknown" && app.screen == ScreenMode::Home) {
    refreshCpuValue();
  }

  // Bildschirm aktualisieren
  if (nextFrameMs == 0 || static_cast<int32_t>(now - nextFrameMs) >= 0) {
    render(now);
    nextFrameMs = now + kFrameMs;
  }

  delay(5);
}
