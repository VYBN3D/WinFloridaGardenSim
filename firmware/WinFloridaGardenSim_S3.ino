/*
  Cannabis Garden — a small touchscreen farming game for the ESP32-2432S028R.

  Libraries:
    - TFT_eSPI by Bodmer
    - XPT2046_Touchscreen by Paul Stoffregen

  Configure TFT_eSPI's active User_Setup.h with the ESP32-2432S028R settings:
    ILI9341_2_DRIVER, 240x320, MOSI 13, MISO 12, SCLK 14, CS 15,
    DC 2, RST -1, BL 21, USE_HSPI_PORT.
  The touchscreen uses its separate CYD pins below.
*/

#include <SPI.h>
#include <TFT_eSPI.h>
#if defined(S3_CYD)
#include <Wire.h>  // FT6336G capacitive touch on I2C
// S3 CYD: capacitive FT6336G touch on I2C (SDA=16, SCL=15, INT=21). No
// XPT2046 library; a tiny compat class exposes the same API the game uses.
struct TS_Point { int x = 0, y = 0, z = 0; };
class CapTouch {
public:
  void begin() {}
  void setRotation(uint8_t) {}
  bool tirqTouched() { return true; }  // register poll decides touch state
  bool touched() { return ftTouchCount() > 0; }
  TS_Point getPoint() {
    TS_Point p;
    if (ftTouchCount() == 0) return p;
    uint8_t d[4] = {0, 0, 0, 0};
    ftRead(0x03, d, 4);
    int rawX = ((d[0] & 0x0F) << 8) | d[1];   // portrait 0..239
    int rawY = ((d[2] & 0x0F) << 8) | d[3];   // portrait 0..319
    p.x = rawY;            // rotation 1 landscape on the 480x320 panel
    p.y = 319 - rawX;
    p.z = 1000;
    return p;
  }
private:
  static void ftRead(uint8_t reg, uint8_t *buf, int n) {
    Wire.beginTransmission(0x38);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)0x38, (uint8_t)n);
    for (int i = 0; i < n && Wire.available(); ++i) buf[i] = Wire.read();
  }
  static uint8_t ftTouchCount() {
    uint8_t d = 0;
    ftRead(0x02, &d, 1);
    return (d >= 1 && d <= 5) ? d : 0;
  }
};
CapTouch touchscreen;
#else
#include <XPT2046_Touchscreen.h>
#endif
#include <Preferences.h>
// TFT_eSPI's SMOOTH_FONT support defines FS_NO_GLOBALS, which suppresses the
// global `File` alias when FS.h is (re)included by the SD library below.
#if defined(FS_NO_GLOBALS)
using fs::File;
#endif
#include <math.h>
#include <limits.h>
#include <ctype.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#if !defined(S3_CYD)
#define TOUCH_IRQ   36
#define TOUCH_MOSI  32
#define TOUCH_MISO  39
#define TOUCH_CLK   25
#define TOUCH_CS    33
#endif
#define BOOT_BUTTON_PIN 0

// On this CYD revision the microSD slot uses the default VSPI pins.
#if defined(S3_CYD)
// 4" ESP32-S3 board (lcdwiki 4.0inch ESP32-S3): microSD is native 4-bit
// SDIO; FT6336U touch reset/interrupt pins per the I/O allocation table.
#define SDMMC_CLK 38
#define SDMMC_CMD 40
#define SDMMC_D0  39
#define SDMMC_D1  41
#define SDMMC_D2  48
#define SDMMC_D3  47
#define FT_RST_PIN 18
#define FT_INT_PIN 17
#else
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCLK 18
#endif

// Harvest-ready indicator LED on the back of the CYD (RGB revision:
// red=GPIO 4, green=GPIO 16, blue=GPIO 17). Set singleColorLedPin to the
// pin of a single-color LED instead, or 255 to disable the LED entirely.
// Verified on hardware: driving 17 LOW lit BLUE, 16 lit GREEN, so the
// standard mapping is correct (red=4). The earlier "red dead on 4" result
// was the death-LED timing bug, not a dead pin.
#define LED_R_PIN 4
#define LED_G_PIN 16
#define LED_B_PIN 17
#define LED_SINGLE_PIN 255   // 255 = RGB LED mode; a pin number = single-color mode

// Draw directly to the TFT. The 320x240 RGB565 sprite used too much
// internal RAM on the CYD and caused unstable display/touch behavior.
#if defined(S3_CYD)
#include <Adafruit_NeoPixel.h>
// S3 CYD: single WS2812B NeoPixel on GPIO 42 replaces the 3-pin RGB LED.
Adafruit_NeoPixel harvestPixel(1, 42, NEO_GRB + NEO_KHZ800);
#endif
TFT_eSPI canvas = TFT_eSPI();

#if !defined(S3_CYD)
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(TOUCH_CS, TOUCH_IRQ);
#endif
Preferences preferences;

const int SCREEN_W = 320;
const int SCREEN_H = 240;
// True-scale drawing layer maps game space (320x240, where the layout is
// authored) onto the 4" board's 480x320 panel: 1.5x horizontally, 4/3
// vertically. Touch hitboxes stay in game space and never change.
const int PANEL_W = 480;
const int PANEL_H = 320;
const int ART_W = 178;   // garden art band width (layout + touch hitbox)
const int ART_H = 144;
unsigned long deathLedUntil = 0;  // red LED stays lit briefly after a kill

// Settings pages: scroll offset for the big-button list, and drag-gesture state.
int16_t settingsScroll = 0;
uint8_t characterSelectIndex = 0;  // grower shown on the giant select card


// Growth stage currently shown on screen; tick refreshes only redraw stats
// while the stage (and therefore the plant art) is unchanged.
uint8_t lastDrawnStage = 255;

enum Page : uint8_t { GARDEN, SUPPLIES, MARKET, STASH, OPTIONS, GROWER_MENU, HASH_ROOM, HARVEST_SELECT, TRIM_ROOM, PLANT_VIEW, PLANTING, NEARBY, NAME_EDIT, CHARACTER_SELECT, SEED_SHOP, OTA_PAGE, WIFI_LOGIN, CHEAT_CODE };

Page growerMenuReturn = GARDEN;    // where the leaf-menu BACK button returns
bool growerFromMenu = false;       // character select was opened from the leaf menu
Page growerNameReturn = OPTIONS;   // where the name editor returns
Page page = GARDEN;
Page pageAfterPlanting = GARDEN;
Page pageAfterSeedShop = SUPPLIES;

const int HEADER_H = 28;
const int NAV_Y = 207;
const int NAV_H = 33;
const int ACTION_Y = 174;
const int ACTION_H = 29;
const int PLANTS_PER_ROOM = 4;
const uint16_t HASH_ROOM_COST = 150;

const uint16_t BG = 0x1082;
const uint16_t PANEL = 0x2125;
const uint16_t PANEL_LIGHT = 0x3187;
const uint16_t TEXT_MAIN = 0xEF7D;
const uint16_t TEXT_DIM = 0x9CF3;
const uint16_t ACCENT = 0x6ECA;
const uint16_t ACCENT_DARK = 0x3D66;
const uint16_t GOLD = 0xFEA0;
const uint16_t RED = 0xE986;

// Small 24x24 alpha-cut mask of the WinFlorida leaf logo.
const uint8_t winLeafLogoMask[72] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0x80,0x01,0xFF,0xE0,
  0x07,0x80,0x78,0x0E,0x00,0x1C,0x1C,0x00,0x0C,0x18,0x00,0xC0,
  0x38,0x1F,0xC0,0x30,0x78,0x80,0x30,0xC1,0x80,0x31,0x81,0x80,
  0x31,0x01,0x80,0x31,0x21,0x00,0x32,0x43,0x00,0x1A,0xC6,0x00,
  0x18,0x8C,0x00,0x1C,0xF8,0x0C,0x0D,0xF0,0x1C,0x03,0x00,0x78,
  0x07,0xFF,0xE0,0x0E,0x7F,0x80,0x00,0x00,0x00,0x00,0x00,0x00
};

uint8_t growth = 0;
uint8_t hydration = 70;
uint8_t vigor = 78;
uint8_t waterPacks = 3;
uint8_t careKits = 2;
uint8_t pestDefenseKits = 2;
uint8_t seedStrain = 0;
uint8_t activeStrain = 0;
uint8_t remediationKits = 1;
uint16_t untrimmedUnits = 0;
uint8_t untrimmedStrain = 0;  // strain of the untrimmed batch waiting in the trim room
uint16_t credits = 18;
uint16_t gameMinutes = 0;
uint16_t harvestCount = 0;
uint8_t lastHarvestUnits = 0;
uint8_t moldLossUnits = 0;
uint8_t moldRescuedUnits = 0;  // units rescued from mold by remediation kits
// WiFi login state (buffers must exist before saveGame below).
char wifiSsid[33] = "";
char wifiPass[65] = "";
char wifiSsidEdit[33] = "";
char wifiPassEdit[65] = "";
// WiFi scan list for the login screen (stage 0 = pick your network).
char wifiScanSsid[12][33] = {""};
int8_t wifiScanRssi[12] = {0};
uint8_t wifiScanCount = 0;
uint8_t wifiScanPage = 0;          // first visible row (scroll offset)
bool wifiScanPending = false;
// Remembered networks: tap one on the scan list to connect without retyping
// the password. Updates connect on demand and log off when they finish.
char wifiSavedSsid[4][33] = {""};
char wifiSavedPass[4][65] = {""};
uint8_t wifiSavedCount = 0;
uint8_t wifiLoginStage = 0;            // 0 typing network, 1 typing password
char otaStatus[64] = "No WiFi yet - tap WIFI LOGIN";
char otaLatest[16] = "";               // version.txt when a check succeeded
bool otaBusy = false;                  // install running: button disabled
// Cheat console (Options > CHEATS): type a code, tap ACTIVATE.
char cheatEdit[16] = "";
char cheatFeedback[28] = "";
char growerName[12] = "Conor";
char growerNameEdit[12] = "";
uint8_t farmerCharacter = 0;
bool farmerCharacterChosen = false;
bool selectingFarmerFromOptions = false;
uint8_t bongs = 0;
uint8_t lighters = 0;
uint8_t rollingPapers = 0;
uint8_t growthBoostTicks = 0;
uint8_t smokeAccessoryUsed = 0;
uint8_t stonedLevel = 0;  // 0-100, decays one point per game tick
unsigned long rainbowUntil = 0;  // smoke-break LED flourish end
const char *stonedLevelNames[10] = {
  "Sober", "Barely Buzzed", "Nice Mellow", "Feeling Fuzzy", "Giggle Fit",
  "Munchies Soon", "Time Is Fake", "Couch Merged", "Deep Thoughts",
  "Giraffe Pu$$y"
};
uint8_t smokeProductUsed = 0;
uint8_t pestType = 0;
uint8_t pestSeverity = 0;
bool plantActive = true;
uint16_t plantVariant = 1;   // per-grow look seed, rolled fresh at each new planting
bool firstPlantingDone = true;
uint8_t plantedPotMask = 0;
bool seedDragging = false;
int seedDragX = 0;
int seedDragY = 0;
uint8_t gameSpeed = 0;  // 0 = RELAXED, 1 = QUICK, 2 = EXTREME (replaces quickPace/demoMode)
bool displayInverted = false;
bool hashRoomUnlocked = false;
bool resetArmed = false;
bool waterAnimationRequested = false;
bool waterAnimActive = false;           // non-blocking watering animation
int waterAnimFrame = 0;
unsigned long waterAnimNextAt = 0;
bool farmerOverlayActive = false;       // non-blocking farmer visit overlay
unsigned long farmerOverlayUntil = 0;
bool overlayDirty = false;              // garden repaint landed under the overlay
TFT_eSprite frameBuf = TFT_eSprite(&canvas);    // full-frame render buffer
bool frameBufferActive = false;                 // presentFrame pushes the buffer
bool smokeAnimationRequested = false;
bool hashAnimationRequested = false;
bool flowerHarvestAnimationRequested = false;
bool flowerTrimAnimationRequested = false;
bool moldAnimationRequested = false;
bool marketGearMode = false;
bool stashGearMode = false;

// Defoliation windows, over-defoliation and overwatering (per harvest).
const uint8_t DEFOLIATE_WINDOWS = 4;
const uint8_t defoliateWindowStart[DEFOLIATE_WINDOWS] = {20, 40, 60, 80};
const char *defoliateWindowNames[DEFOLIATE_WINDOWS] = {
  "FIRST DEFOLIATION", "SECOND DEFOLIATION", "THIRD DEFOLIATION", "FINAL DEFOLIATION"
};
uint8_t defoliateWindow = 255;      // window the click count belongs to (255 = none yet)
uint8_t defoliateClicks = 0;        // clicks used in the current window
uint8_t defoliateLimit = 2;         // hidden random limit for the current window
uint8_t defoliatePromptPending = 255;
// Defoliation pacing: after a window opens its button stays gray for this many
// game-minutes (1 per tick: 30 s real on normal speed, 4 s on fast) so the
// windows don't fire back-to-back. While a fresh window is open and untouched,
// growth pauses — defoliation is maintenance you must do, not a growth boost.
const uint8_t DEFOLIATE_COOLDOWN_MINUTES = 4;
uint16_t defoliateReadyAt = 0;      // gameMinutes when the window's button unlocks
uint8_t deathReason = 0;            // 1 = overwatered, 2 = over-defoliated, 3 = dried out
// Wilt timer: below 20% hydration the plant is on borrowed time. If it stays
// that dry for this many consecutive ticks it dies — even before the soil is
// fully bone-dry (the old 0% wither still applies). 20 ticks = 10 real
// minutes at normal speed, 80 s on fast.
const uint8_t DRY_TICKS_LIMIT = 20;
uint8_t dryStreakTicks = 0;         // consecutive ticks the plant has been <20% water
bool plantDeathAnimationRequested = false;
bool pestAnimationRequested = false;

const char DEVICE_BRAND[] = "Win Florida";
const uint16_t NEARBY_MAGIC = 0xC47A;
const uint8_t PACKET_HELLO = 1;
const uint8_t PACKET_MESSAGE = 2;
const uint8_t PACKET_GIFT = 3;
const uint8_t PACKET_CASH = 4;
const uint8_t MAX_NEARBY_PEERS = 4;
const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct __attribute__((packed)) NearbyPacket {
  uint16_t magic;
  uint8_t type;
  uint8_t item;
  char brand[12];
  char grower[12];
  uint8_t masteryLevel;
  char text[32];
};

struct NearbyPeer {
  bool active;
  uint8_t mac[6];
  char brand[12];
  char grower[12];
  uint8_t masteryLevel;
  unsigned long lastSeen;
};

struct FarmerSaying {
  const char *line1;
  const char *line2;
};

FarmerSaying farmerOverlaySaying;   // saying shown by the overlay (copyable)

const FarmerSaying farmerSayings[20] = {
  {"Fire!", ""},
  {"Dang, that looks", "Terpy."},
  {"Leaf me to it.", ""},
  {"Roots before shoots.", ""},
  {"Mites better scoot.", ""},
  {"Grow big or go home.", ""},
  {"That canopy is a", "real leaf-er."},
  {"Grower green team!", ""},
  {"Lookin' frosty-ish.", ""},
  {"Stay high on", "good vibes."},
  {"Bud-dy, we did it!", ""},
  {"Photosynthesis:", "nature's snack time."},
  {"This room's a", "total grow-mance."},
  {"More leaves, please.", ""},
  {"That stem has", "main character energy."},
  {"The plants look", "gorgeous today."},
  {"I could leaf here", "all day."},
  {"Buds of steel!", ""},
  {"Smells like a", "harvest party."},
  {"Terp there it is!", ""}
};

NearbyPeer nearbyPeers[MAX_NEARBY_PEERS] = {};
NearbyPacket inboundNearbyPacket = {};
uint8_t inboundNearbyMac[6] = {};
volatile bool inboundNearbyReady = false;
portMUX_TYPE nearbyMux = portMUX_INITIALIZER_UNLOCKED;
bool nearbyReady = false;
uint8_t selectedNearbyPeer = 255;
uint8_t nearbyMode = 0; // 0 list, 1 compose, 2 gift, 3 inbox, 4 cash
char outgoingMessage[28] = "";
char outgoingCash[6] = "";      // money amount being typed (digits only)
uint8_t msgKbMode = 0;           // compose keyboard: 0 lower, 1 upper, 2 symbols
bool fieldOnlyRedraw = false;    // a tap refreshed just the input field
uint8_t nameKbMode = 0;          // name-edit keyboard: 0 lower, 1 upper, 2 symbols
char lastNearbyMessage[52] = "No messages yet";
unsigned long lastNearbyHello = 0;

unsigned long lastGameTick = 0;
// Grow clock: wall seconds since power-on. The ESP32 RTC keeps counting
// through light AND deep sleep, so the grow advances while the screen sleeps.
uint32_t growEpoch = 0;
// Sleep dormancy: ticks that elapse while the screen sleeps are run "dormant"
// — water/vigor/pests/wilt-timer pause, so a plant put to sleep healthy can
// never die in its sleep — while growth keeps advancing on the wall clock.
bool ticksDuringSleep = false;
bool justWokeFromSleep = false;
unsigned long lastFarmerVisitAt = 0;
unsigned long lastSave = 0;
unsigned long resetArmedAt = 0;
bool previousTouch = false;

void render(bool presentNow = true);
void harvestLedRed();   // forward declaration (see LED section)
void applyDisplayInversion();  // panel-inversion-aware invert helper

bool wateringWouldDrown();
uint8_t defoliateButtonState();
void resetDefoliationForNewPlant();
void animatePlantDeath();
void showDefoliatePrompt();
uint8_t currentDefoliateWindow();
void animatePestInvasion();
void drawNearbyKeyboardRow(const char *keys, int count, int startX, int y);
void drawGardenActions();
void waitForCutsceneContinue();

// Firmware identity, shown in the boot log and on the Options page.
#define FW_VERSION "v41"
#define FW_BOARD    "S3"
#define FW_DATE    "Nov 20"
#define FW_VERSION_FULL FW_VERSION "-" FW_BOARD

// --- 8-bit soundtrack -------------------------------------------------------
// Two-voice square-wave chiptune. On this S3 board the audio path is digital:
// the ESP32-S3 streams I2S PCM to the onboard ES8311 codec (I2C addr 0x18 on
// the shared touch bus, SDA=16/SCL=15), whose analog out feeds the FM8002E
// speaker amp (GPIO1, LOW = enabled) driving the 2P speaker connector.
// There is no GPIO to tone() — the voices are synthesized as 16-bit PCM in
// audioPump() below, so the lead and the drums both play through the speaker.
uint8_t musicVolume = 40;  // 0..100% of max loudness; 0 = music off
uint8_t musicVolumeBeforeMute = 40;  // volume restored when the mute toggle is tapped
bool deathWarnModal = false;            // centered dying-plant popup, any page
bool deathWarnStamped = false;          // modal pixels currently in the buffer
unsigned long deathWarnNextAllowed = 0; // re-popup cooldown after dismissal

struct ChipNote { uint16_t freq; uint8_t steps; };  // freq 0 = rest; steps in 16ths
struct ChipSong { const ChipNote* notes; uint16_t count; uint16_t stepMs; bool loop; };
struct DrumLoop { const ChipNote* notes; uint16_t count; uint16_t stepMs; };

// The placeholder chiptune garden theme is gone; the in-game music is now a
// sampled loop streamed from flash through the PCM voice in audioSynthChunk().

// Plant lost: slow descending minor phrase.
const ChipNote deathJingleNotes[] PROGMEM = {
  {440,2},{330,2},{262,2},{220,3},{196,2},{165,5},{0,2},
};
const ChipSong deathJingle = {deathJingleNotes,
    sizeof(deathJingleNotes) / sizeof(ChipNote), 170, false};

// Harvest: rising fanfare.
const ChipNote harvestFanfareNotes[] PROGMEM = {
  {523,1},{659,1},{784,1},{1047,4},{0,1},{784,2},{1047,5},{0,2},
};
const ChipSong harvestFanfare = {harvestFanfareNotes,
    sizeof(harvestFanfareNotes) / sizeof(ChipNote), 90, false};

// Pest invasion: two-note siren.
const ChipNote pestAlarmNotes[] PROGMEM = {
  {880,2},{659,2},{880,2},{659,2},{880,2},{659,2},{0,2},
};
const ChipSong pestAlarm = {pestAlarmNotes,
    sizeof(pestAlarmNotes) / sizeof(ChipNote), 110, false};

// Buy / sell: classic coin blip.
const ChipNote coinBlipNotes[] PROGMEM = {
  {988,1},{1319,3},{0,1},
};
const ChipSong coinBlip = {coinBlipNotes,
    sizeof(coinBlipNotes) / sizeof(ChipNote), 70, false};

const ChipSong* activeSong = nullptr;
const ChipSong* backgroundSong = nullptr;
uint16_t songPos = 0;
unsigned long noteEnd = 0;

// Drum-voice state (declared early: startSong() silences the groove for jingles)
const DrumLoop* activeDrums = nullptr;
uint16_t drumPos = 0;
unsigned long drumEnd = 0;    // when the sounding hit stops
unsigned long drumNext = 0;   // when the next scheduled hit fires
bool drumPlaying = false;

// --- True-scale canvas -------------------------------------------------------
inline int SX(int x) { return (int)((int32_t)x * 3 / 2); }
inline int SY(int y) { return (int)((int32_t)y * 4 / 3); }
inline int SWD(int x, int w) { return SX(x + w) - SX(x); }
inline int SHD(int y, int h) { return SY(y + h) - SY(y); }
inline int scaleR(int r) { return (r * 17 + 6) / 12; }  // ~1.417x radius

struct GameCanvas {
  TFT_eSPI *tft;
  // NB: tft->fillScreen() uses the sprite's inherited 320x480 bounds and
  // would wipe only part of the 480-wide buffer - fill via the scaled path.
  void fillScreen(uint16_t c) { fillRect(0, 0, SCREEN_W, SCREEN_H, c); }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    tft->fillRect(SX(x), SY(y), SWD(x, w), SHD(y, h), c);
  }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    tft->fillRoundRect(SX(x), SY(y), SWD(x, w), SHD(y, h), scaleR(r), c);
  }
  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    tft->drawRoundRect(SX(x), SY(y), SWD(x, w), SHD(y, h), scaleR(r), c);
  }
  void drawFastHLine(int x, int y, int w, uint16_t c) {
    tft->drawFastHLine(SX(x), SY(y), SWD(x, w), c);
  }
  // Fills every PHYSICAL row of the logical band [y, y+1). SY() rounds down,
  // so on the 480x320 panel a logical band can span two panel rows; drawing
  // only SY(y) skips one physical row whenever the band is 2 rows tall —
  // which shows up as horizontal lines across gradients and bitmaps.
  void drawBandHLine(int x, int y, int w, uint16_t c) {
    int py1 = SY(y + 1);
    for (int py = SY(y); py < py1; ++py)
      tft->drawFastHLine(SX(x), py, SWD(x, w), c);
  }
  void drawFastVLine(int x, int y, int h, uint16_t c) {
    tft->drawFastVLine(SX(x), SY(y), SHD(y, h), c);
  }
  void drawLine(int x0, int y0, int x1, int y1, uint16_t c) {
    tft->drawLine(SX(x0), SY(y0), SX(x1), SY(y1), c);
  }
  void fillCircle(int x, int y, int r, uint16_t c) {
    tft->fillCircle(SX(x), SY(y), scaleR(r), c);
  }
  void drawCircle(int x, int y, int r, uint16_t c) {
    tft->drawCircle(SX(x), SY(y), scaleR(r), c);
  }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    tft->fillTriangle(SX(x0), SY(y0), SX(x1), SY(y1), SX(x2), SY(y2), c);
  }
  void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    tft->drawTriangle(SX(x0), SY(y0), SX(x1), SY(y1), SX(x2), SY(y2), c);
  }
  void drawPixel(int x, int y, uint16_t c) { tft->drawPixel(SX(x), SY(y), c); }
  void setTextColor(uint16_t c) { tft->setTextColor(c); }
  void setTextColor(uint16_t c, uint16_t bg) { tft->setTextColor(c, bg); }
  int16_t drawString(const char *s, int x, int y, uint8_t f) {
    return tft->drawString(s, SX(x), SY(y), f);
  }
  int16_t drawString(const String &s, int x, int y, uint8_t f) {
    return tft->drawString(s, SX(x), SY(y), f);
  }
  int16_t drawCentreString(const char *s, int x, int y, uint8_t f) {
    return tft->drawCentreString(s, SX(x), SY(y), f);
  }
  int16_t drawCentreString(const String &s, int x, int y, uint8_t f) {
    return tft->drawCentreString(s, SX(x), SY(y), f);
  }
  int16_t drawRightString(const char *s, int x, int y, uint8_t f) {
    return tft->drawRightString(s, SX(x), SY(y), f);
  }
  void setSwapBytes(bool b) { tft->setSwapBytes(b); }
  bool getSwapBytes() { return tft->getSwapBytes(); }
  void pushImage(int x, int y, int w, int h, uint16_t *d) {
    tft->pushImage(SX(x), SY(y), w, h, d);
  }
  void pushImage(int x, int y, int w, int h, uint16_t *d, uint8_t key) {
    tft->pushImage(SX(x), SY(y), w, h, d, key);
  }
};
GameCanvas game = { &canvas };

// Panel-resolution row blit used by the SD BMP loaders: writes into the
// frame buffer when it is active, straight to the panel otherwise.
void blitPanelRow(int px, int py, int w, uint16_t *row) {
  if (frameBufferActive) frameBuf.pushImage(px, py, w, 1, row);
  else canvas.pushImage(px, py, w, 1, row);
}

// --- Sound output: onboard ES8311 codec + FM8002E speaker amp ----------------
// ESP32-S3 I2S master -> ES8311 codec (I2C 0x18, shared touch bus) -> analog
// out -> FM8002E amp (GPIO1 LOW = on) -> 2P speaker connector. Voices are
// synthesized as 16-bit stereo PCM and pumped into the I2S DMA ring.
#include <driver/i2s_std.h>

#define AUDIO_MCLK_PIN  4
#define AUDIO_BCLK_PIN  5
#define AUDIO_WS_PIN    7
#define AUDIO_DOUT_PIN  8   // playback data — GPIO8 confirmed working on hardware
                             // (vendor table says 6, but GPIO6 gave no sound; LCD Wizard's
                             // schematic analysis was right)
#define AUDIO_AMP_PIN   1   // FM8002E enable, LOW = on
#define AUDIO_RATE      16000

static i2s_chan_handle_t audioTx = nullptr;
static i2s_std_gpio_config_t audioGpio;
uint8_t audioLastVol = 255;   // 255 forces a codec/amp refresh on the first pump

// PCM synth state (replaces the LEDC duty voices)
uint16_t pcmLeadFreq = 0;   // lead/bass square wave, 0 = rest
uint16_t pcmDrumFreq = 0;   // active drum blip, 0 = silent
uint32_t leadPhase = 0, drumPhase = 0;

static bool es8311Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t es8311Read(uint8_t reg) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)0x18, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

// ES8311 init for playback only: slave mode, 16-bit I2S, MCLK 256*fs from the
// MCLK pin, 16 kHz. Register sequence mirrors Espressif's canonical driver.
static void es8311Init() {
  es8311Write(0x01, 0x30); es8311Write(0x02, 0x00); es8311Write(0x03, 0x10);
  es8311Write(0x16, 0x24); es8311Write(0x04, 0x10); es8311Write(0x05, 0x00);
  es8311Write(0x0B, 0x00); es8311Write(0x0C, 0x00); es8311Write(0x10, 0x1F);
  es8311Write(0x11, 0x7F); es8311Write(0x00, 0x80);   // reset
  es8311Write(0x00, 0x80);   // slave mode (ESP32 drives the clocks)
  es8311Write(0x01, 0x3F);   // internal clock sourced from the MCLK pad
  // coeff entry for mclk 4096000 / rate 16000
  es8311Write(0x02, 0x00);   // pre-div 1, pre-multi x1
  es8311Write(0x05, 0x00);   // adc/dac div 1
  es8311Write(0x03, 0x10);   // adc osr 16, single speed
  es8311Write(0x04, 0x10);   // dac osr 16
  es8311Write(0x07, 0x00);   // lrck h
  es8311Write(0x08, 0xFF);   // lrck l
  es8311Write(0x06, 0x03);   // bclk div 4
  es8311Write(0x13, 0x10); es8311Write(0x1B, 0x0A); es8311Write(0x1C, 0x6A);
  // serial data ports: I2S format, 16-bit
  es8311Write(0x09, 0x0C);   // DAC data in
  es8311Write(0x0A, 0x0C);   // ADC data out
  // start(DECODE): DAC path on, analog section powered up
  es8311Write(0x0A, 0x4C);
  es8311Write(0x17, 0xBF); es8311Write(0x0E, 0x02); es8311Write(0x12, 0x00);
  es8311Write(0x14, 0x1A); es8311Write(0x0D, 0x01); es8311Write(0x15, 0x40);
  es8311Write(0x37, 0x48); es8311Write(0x45, 0x00);
}

static void es8311SetVolume(uint8_t percent) {
  // Logarithmic mapping from Espressif's driver for a natural taper.
  int vol = 0;
  if (percent > 0)
    vol = (int)(255.0 * log10(9.0 * percent / 100.0 + 1.0) / log10(10.0) + 0.5);
  es8311Write(0x32, (uint8_t)vol);
}

static void es8311Mute(bool m) {
  uint8_t regv = es8311Read(0x31) & 0x9F;
  if (m) {
    es8311Write(0x12, 0x02);
    es8311Write(0x31, regv | 0x60);
    es8311Write(0x32, 0x00);
    es8311Write(0x37, 0x08);
  } else {
    es8311Write(0x31, regv);
    es8311Write(0x12, 0x00);
  }
}

static void ampEnable(bool on) {
  pinMode(AUDIO_AMP_PIN, OUTPUT);
  digitalWrite(AUDIO_AMP_PIN, on ? LOW : HIGH);  // LOW = FM8002E enabled
}

static void audioInit() {
  es8311Init();
  i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ch.dma_desc_num = 8;
  ch.dma_frame_num = 480;   // 8 * 480 frames = ~240 ms of buffer
  if (i2s_new_channel(&ch, &audioTx, nullptr) != ESP_OK) { audioTx = nullptr; return; }
  i2s_std_config_t cfg = {};
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
  clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.clk_cfg = clk;
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  audioGpio.mclk = GPIO_NUM_4;
  audioGpio.bclk = GPIO_NUM_5;
  audioGpio.ws = GPIO_NUM_7;
  audioGpio.dout = GPIO_NUM_8;
  audioGpio.din = I2S_GPIO_UNUSED;
  audioGpio.invert_flags.mclk_inv = false;
  audioGpio.invert_flags.bclk_inv = false;
  audioGpio.invert_flags.ws_inv = false;
  cfg.gpio_cfg = audioGpio;
  if (i2s_channel_init_std_mode(audioTx, &cfg) != ESP_OK ||
      i2s_channel_enable(audioTx) != ESP_OK) {
    i2s_del_channel(audioTx);
    audioTx = nullptr;
    return;
  }
  ampEnable(musicVolume != 0);
}

// Fill the DMA ring: mix the lead and drum square voices into 16-bit stereo.

// --- PCM sample voices -------------------------------------------------------
// Boot chant (one-shot crowd chant) then the theme loop, both IMA ADPCM,
// 4-bit, 16 kHz mono, stored in flash. One decoder chain serves whichever
// stream is active; when the chant ends the theme takes over and loops.
#include "boot_chant.h"
#include "theme_loop.h"
#include "bong_hit.h"

static const uint8_t PCM_NONE = 0, PCM_CHANT = 1, PCM_BONG = 2, PCM_THEME = 3;
static uint8_t pcmStream = PCM_NONE;
static uint32_t pcmPos = 0;
static int32_t pcmPredict = 0;
static int pcmIndex = 0;
static bool pcmHighNib = false;

static const int16_t pcmStepTable[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,
  80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
  494,544,598,658,724,796,876,963,1060,1166,1282,1410,1552,1707,1878,2066,
  2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
  8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
  27086,29794,32767};
static const int8_t pcmIndexTable[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

static void pcmStart(uint8_t which) {
  pcmStream = which;
  pcmPos = 0;
  pcmPredict = 0;
  pcmIndex = 0;
  pcmHighNib = false;
}

void playBootChant() {
  if (!audioTx || musicVolume == 0) return;
  pcmStart(PCM_CHANT);
}

// Decode one ADPCM nibble of the active stream into a PCM sample.
static inline int32_t pcmNextSample() {
  for (;;) {
    const uint32_t frames =
        pcmStream == PCM_CHANT ? bootChantFrames
        : pcmStream == PCM_BONG ? bongHitFrames : themeFrames;
    if (pcmPos < frames) break;
    if (pcmStream == PCM_CHANT) {
      // Crowd chant, then the bong hit lands, then the theme drops.
      if (bongHitFrames) { pcmStart(PCM_BONG); continue; }
      if (themeFrames) { pcmStart(PCM_THEME); continue; }
      pcmStream = PCM_NONE;
      return 0;
    }
    if (pcmStream == PCM_BONG) {
      if (themeFrames) { pcmStart(PCM_THEME); continue; }
      pcmStream = PCM_NONE;
      return 0;
    }
    if (!frames) { pcmStream = PCM_NONE; return 0; }
    pcmPos = 0;                     // theme loops forever
    pcmPredict = 0;
    pcmIndex = 0;
    pcmHighNib = false;
    break;
  }
  const uint8_t *data =
      pcmStream == PCM_CHANT ? bootChantData
      : pcmStream == PCM_BONG ? bongHitData : themeData;
  uint8_t b = pgm_read_byte(&data[pcmPos >> 1]);
  uint8_t n = pcmHighNib ? (b >> 4) : (b & 0xF);
  pcmHighNib = !pcmHighNib;
  ++pcmPos;
  int step = pcmStepTable[pcmIndex];
  int32_t diff = step >> 3;
  if (n & 4) diff += step;
  if (n & 2) diff += step >> 2;
  if (n & 1) diff += step >> 3;
  if (n & 8) diff = -diff;
  pcmPredict += diff;
  if (pcmPredict > 32767) pcmPredict = 32767;
  if (pcmPredict < -32768) pcmPredict = -32768;
  pcmIndex += pcmIndexTable[n & 7];
  if (pcmIndex < 0) pcmIndex = 0;
  if (pcmIndex > 88) pcmIndex = 88;
  return pcmPredict;
}

static int16_t stereoBuf[480 * 2];
static void audioSynthChunk() {
  const uint32_t PHASE_STEP = (uint32_t)(4294967296.0 / AUDIO_RATE);
  // Refill pacing: the codec drains the DMA ring at exactly AUDIO_RATE, so
  // each pump replaces whatever drained since the previous one. The original
  // fixed-480-frame chunk with a non-blocking write let the main loop outrun
  // the codec (2x-fast music, glitchy button jingles), and a fixed blocking
  // chunk still starved the splash's 80 ms pump cadence (glitchy chant).
  // Covering the elapsed time keeps every cadence smooth.
  static uint32_t lastPumpMs = 0;
  uint32_t now = millis();
  uint32_t elapsedMs = lastPumpMs ? now - lastPumpMs : 80;
  lastPumpMs = now;
  if (elapsedMs > 200) elapsedMs = 200;   // never queue more than ~the ring
  int framesToFill = (int)elapsedMs * (AUDIO_RATE / 1000);
  while (framesToFill > 0) {
    int chunk = framesToFill > 480 ? 480 : framesToFill;
    for (int i = 0; i < chunk; ++i) {
      int32_t s = 0;
      if (pcmLeadFreq) {
        leadPhase += (uint32_t)pcmLeadFreq * PHASE_STEP;
        // Jingle voice rides ~6 dB under the theme's PCM level so sound
        // effects track the music volume knob without blasting over it.
        s += (leadPhase & 0x80000000UL) ? -6000 : 6000;
      }
      if (pcmDrumFreq) {
        drumPhase += (uint32_t)pcmDrumFreq * PHASE_STEP;
        int damp = pcmDrumFreq > 400 ? 6000 : (pcmDrumFreq <= 120 ? 12000 : 9000);
        s += (drumPhase & 0x80000000UL) ? -damp : damp;
      }
      if (pcmStream) s += (pcmNextSample() * 9) / 10;   // PCM sample voice
      if (s > 32767) s = 32767;
      else if (s < -32768) s = -32768;
      stereoBuf[2 * i] = stereoBuf[2 * i + 1] = (int16_t)s;
    }
    // Block until the DMA ring accepts the whole chunk: the decoder must not
    // advance past what the codec will actually consume.
    const uint8_t *p = reinterpret_cast<const uint8_t *>(stereoBuf);
    size_t remain = (size_t)chunk * 2 * sizeof(int16_t);
    while (remain) {
      size_t written = 0;
      if (i2s_channel_write(audioTx, p, remain, &written, 100) != ESP_OK ||
          written == 0)
        return;                    // channel stopped: don't spin forever
      p += written;
      remain -= written;
    }
    framesToFill -= chunk;
  }
}

void audioPump() {
  if (!audioTx) return;
  if (musicVolume != audioLastVol) {
    audioLastVol = musicVolume;
    if (musicVolume == 0) { es8311Mute(true); ampEnable(false); }
    // The options UI still runs 0..100, but the amp is capped: 100% on
    // screen drives the codec at 55% so the speaker stays comfortable.
    else { es8311SetVolume(musicVolume * 55 / 100); es8311Mute(false); ampEnable(true); }
  }
  audioSynthChunk();   // always pump (silence when idle) — DMA hates gaps
}

void chipToneOff() { pcmLeadFreq = 0; }

void chipToneOn(uint16_t freq) {
  if (freq == 0) { chipToneOff(); return; }
  pcmLeadFreq = freq;
}

void startSong(const ChipSong* song) {
  if (musicVolume == 0) return;
  chipToneOff();
  activeSong = song;
  songPos = 0;
  noteEnd = 0;
  if (song != backgroundSong) {
    drumToneOff();      // jingles play dry; the groove resumes after
    drumPlaying = false;
  }
}

void playCoin() { startSong(&coinBlip); }

// --- Drum track (second synth voice, mixed into the same PCM stream) ---------
// Drum hits are short square blips whose pitch implies the drum: ~65 Hz kick,
// ~180 Hz snare buzz, ~4.5 kHz hat tick. The pattern runs on the same
// 16th-note clock as the garden theme so the two voices stay in step.
// One 16-step bar of boom-bap, repeated with a bar-4 double-kick fill.
const ChipNote gardenDrumNotes[] PROGMEM = {
  {65,2},{4500,2},{180,2},{4500,2},{65,2},{4500,2},{180,2},{4500,2},   // bars 1-2
  {65,2},{4500,2},{180,2},{4500,2},{65,2},{4500,2},{180,2},{4500,2},
  {65,2},{4500,2},{180,2},{4500,2},{65,2},{4500,2},{180,2},{4500,2},   // bar 3
  {65,2},{4500,2},{180,2},{4500,2},{65,1},{0,1},{65,1},{4500,1},       // bar 4 fill
  {4500,2},{180,2},
};
const DrumLoop gardenDrums = {gardenDrumNotes,
    sizeof(gardenDrumNotes) / sizeof(ChipNote), 160};

void drumToneOff() { pcmDrumFreq = 0; }

void drumHit(uint16_t freq) {
  pcmDrumFreq = freq;
  drumPlaying = true;
  drumEnd = millis() + (freq > 400 ? 25 : (freq <= 120 ? 90 : 60));
}

void startDrums() {
  drumToneOff();
  drumPlaying = false;
  activeDrums = &gardenDrums;
  drumPos = 0;
  drumNext = millis();
}

void updateDrums() {
  if (musicVolume == 0) return;
  unsigned long now = millis();
  if (drumPlaying && (long)(drumEnd - now) <= 0) {
    drumToneOff();
    drumPlaying = false;
  }
  // Drums only run under the background theme; jingles play dry and the
  // groove picks up again (in step) when the theme returns.
  if (!activeSong || activeSong != backgroundSong) return;
  if (!activeDrums) startDrums();
  if ((long)(drumNext - now) > 0) return;
  if (drumPos >= activeDrums->count) drumPos = 0;
  uint16_t freq = pgm_read_word(&activeDrums->notes[drumPos].freq);
  uint8_t steps = pgm_read_byte(&activeDrums->notes[drumPos].steps);
  ++drumPos;
  if (freq) drumHit(freq);
  drumNext = now + (unsigned long)steps * activeDrums->stepMs;
}

void updateMusic() {
  updateDrums();
  audioPump();
  if (musicVolume == 0) return;
  if (!activeSong) {
    startSong(backgroundSong);  // auto-resume the theme whenever idle
    if (!activeSong) return;
  }
  unsigned long now = millis();
  if ((long)(noteEnd - now) > 0) return;
  if (songPos >= activeSong->count) {
    if (activeSong->loop) {
      songPos = 0;
    } else {
      // Jingle finished: hand the speaker back to the garden theme.
      const ChipSong* next = activeSong == backgroundSong ? nullptr : backgroundSong;
      activeSong = nullptr;
      chipToneOff();
      if (next) startSong(next);
      return;
    }
  }
  chipToneOff();
  uint16_t freq = pgm_read_word(&activeSong->notes[songPos].freq);
  uint8_t steps = pgm_read_byte(&activeSong->notes[songPos].steps);
  ++songPos;
  if (freq) chipToneOn(freq);
  noteEnd = now + (unsigned long)steps * activeSong->stepMs;
}

// Blocking-delay drop-in that keeps the sequencer playing during cutscenes.
void pumpDelay(unsigned long ms) {
  unsigned long end = millis() + ms;
  do {
    updateMusic();
    delay(ms < 12 ? ms : 12);
  } while ((long)(end - millis()) > 0);
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void drawLeafLogo(int x, int y, int scale, uint16_t color) {
  for (int row = 0; row < 24; ++row) {
    for (int col = 0; col < 24; ++col) {
      int bitIndex = row * 24 + col;
      uint8_t packed = pgm_read_byte(&winLeafLogoMask[bitIndex >> 3]);
      if (packed & (0x80 >> (bitIndex & 7))) {
        if (scale == 1) game.fillRect(x + col, y + row, 1, 1, color);
        else game.fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

int stageForGrowth() {
  if (growth < 12) return 0;
  if (growth < 32) return 1;
  if (growth < 58) return 2;
  if (growth < 82) return 3;
  return 4;
}

const char *stageName() {
  if (!plantActive) return "EMPTY ROOM";
  switch (stageForGrowth()) {
    case 0: return "SEEDLING";
    case 1: return "YOUNG PLANT";
    case 2: return "GROWING";
    case 3: return "FLOWERING";
    default: return "MATURE";
  }
}

const uint8_t STRAIN_COUNT = 16;
// Seeds are held per strain: buy (or receive) different strains and pick
// which one to plant on the planting screen.
uint8_t seedsByStrain[STRAIN_COUNT] = {0};
uint8_t plantStrain = 0;            // strain chosen on the planting screen
uint8_t sendSeedStrain = 0;         // strain picked in the nearby send-supplies flow
uint8_t sendStashStrain = 0;        // stash-jar strain picked on the same screen
uint8_t sendHashStrain = 0;         // hash-jar strain picked on the same screen
uint8_t sendConfirmItem = 0;        // product armed for the SEND IT? panel
uint8_t marketSellStrain = 0;   // strain picked in the sell submenu

// ---- Hash room wash picker: wash trimmed stash flower into hash ----
bool washOpen = false;
bool washConfirm = false;
uint8_t washStrain = 0;
uint8_t marketSellMode = 0;     // 0 none, 1/2 flower sell-1/sell-all, 3/4 hash
uint8_t marketSellCount = 0;    // stocked strains for the active mode
uint8_t marketSellList[STRAIN_COUNT];
bool marketSellConfirm = false; // one-tap ARE YOU SURE? before selling
bool smokeMenuOpen = false;     // stash page smoke-break chooser
bool smokeMenuConfirm = false;  // ARE YOU SURE? before lighting up
uint8_t smokeMenuPick = 0;      // 1 bong+flower, 2 bong+hash, 3 joint

uint16_t totalSeeds() {
  uint16_t t = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) t += seedsByStrain[i];
  return t;
}

uint8_t countSeededStrains() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) if (seedsByStrain[i]) ++n;
  return n;
}

// Next/prev strain (wrapping) that you actually hold seeds of.
uint8_t nextSeededStrain(uint8_t from, int dir) {
  for (uint8_t step = 0; step < STRAIN_COUNT; ++step) {
    from = (from + STRAIN_COUNT + dir) % STRAIN_COUNT;
    if (seedsByStrain[from]) return from;
  }
  return from;
}


// WinFlorida menu staples + cultivation names, easy -> hard display order.
const char *strainNames[STRAIN_COUNT] = {
  "Northern Lights", "Blue Dream", "Banana Papaya", "Superbuff Cherry", "Jack Herer",
  "Granddaddy Purple", "White Widow", "Blue Lobster", "OG Kush", "Sour Diesel",
  "Gelato", "Titan Express", "Zesty Garlic Cookies", "Moon Beam Gelato", "Toad Venom",
  "Foreign Kushmints"
};
const char *strainDifficulty[STRAIN_COUNT] = {
  "EASY", "MEDIUM", "MEDIUM", "MEDIUM", "MEDIUM", "MEDIUM", "MEDIUM",
  "HARD", "HARD", "HARD", "HARD", "HARD", "HARD", "HARD", "EXPERT", "EXPERT"
};
// Menu type + a few words of aroma/effects for the catalog card.
const char *strainType[STRAIN_COUNT] = {
  "INDICA", "SATIVA", "HYBRID", "HYBRID", "SATIVA", "INDICA", "HYBRID",
  "HYBRID", "INDICA", "SATIVA", "HYBRID", "HYBRID", "HYBRID", "HYBRID",
  "HYBRID", "INDICA"
};
const char *strainNotes[STRAIN_COUNT] = {
  "Earthy pine body calm", "Sweet berry heady lift", "Tropical banana cream",
  "Cherry gas social buzz", "Pine citrus bright focus", "Grape berry cozy body",
  "Pepper earth steady buzz", "Blue candy gas", "Fuel pine couch calm",
  "Diesel citrus daytime fuel", "Creamy dessert calm", "Fast gas express rush",
  "Garlic cookie funk", "Moonlit cream dessert", "Swamp funk mind melt",
  "Minty gas deep melt"
};
const int8_t strainPaceModifier[STRAIN_COUNT] = {1, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, -2, -2};
const uint8_t strainHydrationNeed[STRAIN_COUNT] = {14, 18, 18, 18, 18, 18, 18, 24, 24, 24, 24, 24, 24, 24, 32, 32};
const uint8_t strainPrices[STRAIN_COUNT] = {8, 12, 12, 12, 12, 12, 12, 18, 18, 18, 18, 18, 18, 18, 30, 30};
// Seed catalog runs easy -> hard: EASY first (the starting strain you can
// afford), then MEDIUM, HARD, EXPERT. Data stays keyed by strain index;
// these tables only order the catalog, buying, and unlock progression.
// Order: NL(3) $8 EASY | Blue Dream(1), White Widow(4), GDP(5), Jack(6) $12
// MEDIUM | Sour Diesel(0), OG Kush(2), GSC(7), Gelato(8) $18 HARD | Titan
// Express(11) & more $18 HARD | Toad Venom(14), Foreign Kushmints(15) $30
// EXPERT. Data is keyed in easy -> hard display order, so the catalog runs
// 0..15; the picker shows every card, locked ones grayed until unlocked.
const uint8_t strainCatalogOrder[STRAIN_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
const uint8_t strainCatalogPos[STRAIN_COUNT]  = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
// Market values scale with grow difficulty: EASY $8 seeds -> $10 flower per
// unit, MEDIUM $12 -> $14, HARD $18 -> $20, EXPERT $30 -> $32. Hash runs at
// roughly 2.5x the flower value of the strain it was made from.
const uint8_t strainFlowerValue[STRAIN_COUNT] = {10, 14, 14, 14, 14, 14, 14, 20, 20, 20, 20, 20, 20, 20, 32, 32};
const uint8_t strainHashValue[STRAIN_COUNT] = {25, 35, 35, 35, 35, 35, 35, 50, 50, 50, 50, 50, 50, 50, 80, 80};
// Stash and hash are tracked per strain so their sell prices match difficulty.
uint16_t stashByStrain[STRAIN_COUNT] = {0};
uint8_t nextStashStrain(uint8_t from, int dir) {
  for (uint8_t step = 0; step < STRAIN_COUNT; ++step) {
    from = (from + STRAIN_COUNT + dir) % STRAIN_COUNT;
    if (stashByStrain[from]) return from;
  }
  return from;
}

// Next unlocked strain (direction), walking catalog order — lets pickers
// scroll every strain you've unlocked, even ones you hold none of yet.
extern uint8_t strainUnlockedThrough;
uint8_t nextCatalogStrain(uint8_t from, int dir) {
  int8_t pos = strainCatalogPos[from];
  for (uint8_t step = 0; step < STRAIN_COUNT; ++step) {
    pos = (pos + STRAIN_COUNT + dir) % STRAIN_COUNT;
    if (pos <= strainUnlockedThrough) return strainCatalogOrder[pos];
  }
  return from;
}

uint16_t hashByStrain[STRAIN_COUNT] = {0};

// Next strain (direction) with hash units actually pressed.
uint8_t nextHashStrain(uint8_t from, int dir) {
  for (uint8_t step = 0; step < STRAIN_COUNT; ++step) {
    from = (from + STRAIN_COUNT + dir) % STRAIN_COUNT;
    if (hashByStrain[from]) return from;
  }
  return from;
}

// Next strain (direction) holding at least 2 trimmed jars — a wash run's
// minimum input.
uint8_t nextWashStrain(uint8_t from, int dir) {
  for (uint8_t step = 0; step < STRAIN_COUNT; ++step) {
    from = (from + STRAIN_COUNT + dir) % STRAIN_COUNT;
    if (stashByStrain[from] >= 2) return from;
  }
  return from;
}
// Harvest quality: the plant's vigor when harvested grades the batch and sets
// what it sells for. Quality (0-100 = vigor at harvest) is stored per stash
// jar and weighted-blended when batches of the same jar merge. Jars saved by
// older firmware default to 70 = full price.
uint8_t stashQualityByStrain[STRAIN_COUNT] = {70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70};
uint8_t hashQualityByStrain[STRAIN_COUNT]  = {70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70};
uint8_t untrimmedQuality = 70;  // vigor of the plant the untrimmed batch came from
// Defoliation clicks allowed per window: a random number in [min, max] each time.
// Going past it kills the plant. The button turns red once you reach the minimum.
const uint8_t strainDefoliateMin[STRAIN_COUNT] = {2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
const uint8_t strainDefoliateMax[STRAIN_COUNT] = {4, 3, 3, 3, 3, 3, 3, 2, 3, 3, 2, 2, 2, 2, 2, 2};
// Highest hydration a mature plant survives; younger stages tolerate less.
const uint8_t strainWaterTolerance[STRAIN_COUNT] = {98, 94, 94, 94, 94, 94, 94, 90, 90, 90, 90, 90, 90, 90, 86, 86};
const uint8_t stageWaterPenalty[5] = {14, 8, 4, 0, 0};
const uint8_t WATER_AMOUNT = 32;
const uint8_t WATER_DRAIN_PER_TICK = 3;        // was 1
const uint8_t THIRSTY_STRAIN_EXTRA_DRAIN = 2;  // was 1

uint16_t stashTotal() {
  uint16_t total = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) total += stashByStrain[i];
  return total;
}

uint16_t hashTotal() {
  uint16_t total = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) total += hashByStrain[i];
  return total;
}

// Cheapest non-empty bucket: smoking burns low-grade product first.
int16_t cheapestStashStrain() {
  int16_t best = -1;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (stashByStrain[i] && (best < 0 || strainFlowerValue[i] < strainFlowerValue[best])) best = i;
  return best;
}

int16_t cheapestHashStrain() {
  int16_t best = -1;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (hashByStrain[i] && (best < 0 || strainHashValue[i] < strainHashValue[best])) best = i;
  return best;
}

// Priciest non-empty bucket: selling one unit nets the top-shelf price.


// "$X each" when every bucket sells at the same price, "$X-$Y each" otherwise.
void stashPriceText(char *out, size_t len) {
  int16_t lo = -1, hi = -1;
  uint8_t loG = 100, hiG = 100;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    if (!stashByStrain[i]) continue;
    uint16_t v = flowerUnitPrice(i);
    uint8_t g = sellGradeForQuality(stashQualityByStrain[i]);
    if (lo < 0 || v < lo) { lo = v; loG = g; }
    if (hi < 0 || v > hi) { hi = v; hiG = g; }
  }
  if (lo < 0) snprintf(out, len, "-");
  else if (lo == hi) snprintf(out, len, "%s $%u each", gradeName(loG), lo);
  else snprintf(out, len, "%s $%u-$%u each", loG == hiG ? gradeName(loG) : "GRADED", lo, hi);
}

void hashPriceText(char *out, size_t len) {
  int16_t lo = -1, hi = -1;
  uint8_t loG = 100, hiG = 100;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    if (!hashByStrain[i]) continue;
    uint16_t v = hashUnitPrice(i);
    uint8_t g = sellGradeForQuality(hashQualityByStrain[i]);
    if (lo < 0 || v < lo) { lo = v; loG = g; }
    if (hi < 0 || v > hi) { hi = v; hiG = g; }
  }
  if (lo < 0) snprintf(out, len, "-");
  else if (lo == hi) snprintf(out, len, "%s $%u each", gradeName(loG), lo);
  else snprintf(out, len, "%s $%u-$%u each", loG == hiG ? gradeName(loG) : "GRADED", lo, hi);
}
uint16_t harvestedStrainMask = 0;
uint8_t strainUnlockedThrough = 0;

const char *currentStrainName() {
  return strainNames[min((int)activeStrain, (int)STRAIN_COUNT - 1)];
}

const char *masteryNicknameForLevel(uint8_t level) {
  static const char *names[10] = {
    "First Time", "Rookie Grower", "Green Thumb", "Bud Tender", "Harvest Hand",
    "Garden Guide", "Master Grower", "Hash Artisan", "High Roller", "Super Pothead"
  };
  return names[constrain((int)level - 1, 0, 9)];
}

uint8_t currentMasteryLevel() {
  return min(10, (int)(harvestCount / 5) + 1);
}

const char *farmerCharacterName(uint8_t character) {
  if (character == 1) return "Mathias";
  if (character == 2) return "Jay";
  if (character == 3) return "Lindsay";
  if (character == 4) return "Eric";
  if (character == 8) return "Pedro";
  if (character == 5) return "James";
  if (character == 6) return "Sergio";
  if (character == 7) return "Vicente";
  if (character == 9) return "Benji";
  return "Conor";
}

void saveGame() {
  preferences.putUChar("growth", growth);
  preferences.putUChar("water", hydration);
  preferences.putUChar("vigor", vigor);
  preferences.putUChar("waterPk", waterPacks);
  preferences.putUChar("careKit", careKits);
  preferences.putUChar("pestKit", pestDefenseKits);
  preferences.putBytes("seedArr", seedsByStrain, sizeof(seedsByStrain));
  preferences.putUChar("seedType", seedStrain);
  preferences.putUChar("activeType", activeStrain);
  preferences.putUShort("strainMask", harvestedStrainMask);
  preferences.putUShort("plantVar", plantVariant);
  preferences.putUChar("strainOpen", strainUnlockedThrough);
  preferences.putBytes("stashArr", stashByStrain, sizeof(stashByStrain));
  preferences.putBytes("hashArr", hashByStrain, sizeof(hashByStrain));
  preferences.putUShort("rawFlower", untrimmedUnits);
  preferences.putUChar("rawStrain", untrimmedStrain);
  preferences.putUChar("stoned", stonedLevel);
  preferences.putUShort("credits", credits);
  preferences.putUShort("minutes", gameMinutes);
  preferences.putUShort("harvests", harvestCount);
  preferences.putString("grower", growerName);
  preferences.putUChar("farmer", farmerCharacter);
  preferences.putBool("farmerChosen", farmerCharacterChosen);
  preferences.putUChar("bongs", bongs);
  preferences.putUChar("lighters", lighters);
  preferences.putUChar("papers", rollingPapers);
  preferences.putUChar("growBoost", growthBoostTicks);
  preferences.putBool("active", plantActive);
  preferences.putBool("plantDone", firstPlantingDone);
  preferences.putUChar("plantMask", plantedPotMask);
  preferences.putUChar("afterPlant", (uint8_t)pageAfterPlanting);
  preferences.putUChar("gspeed", gameSpeed);
  preferences.putBool("invert", displayInverted);
  preferences.putUChar("mvol", musicVolume);
  preferences.putBool("hashRoom", hashRoomUnlocked);
  preferences.putUChar("pest", pestType);
  preferences.putUChar("pestLevel", pestSeverity);
  preferences.putUChar("remKit", remediationKits);
  preferences.putUChar("defWin", defoliateWindow);
  preferences.putUChar("defClicks", defoliateClicks);
  preferences.putUChar("defLimit", defoliateLimit);
  preferences.putUShort("defReady", defoliateReadyAt);
  preferences.putUChar("dryTick", dryStreakTicks);  // wilt timer survives sleep/reboots
  preferences.putBytes("sqArr", stashQualityByStrain, sizeof(stashQualityByStrain));
  preferences.putBytes("hqArr", hashQualityByStrain, sizeof(hashQualityByStrain));
  preferences.putUChar("rawQual", untrimmedQuality);
  // Wall seconds since power-on at save time: on the next boot the grow
  // catches up on everything that happened while the screen slept.
  preferences.putUInt("epoch", (uint32_t)time(nullptr));
  preferences.putString("wSsid", wifiSsid);
  preferences.putString("wPass", wifiPass);
  preferences.putUChar("wSN", wifiSavedCount);
  for (uint8_t i = 0; i < wifiSavedCount; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "wS%u", i);
    preferences.putString(key, wifiSavedSsid[i]);
    snprintf(key, sizeof(key), "wP%u", i);
    preferences.putString(key, wifiSavedPass[i]);
  }
  preferences.putBool("started", true);
  lastSave = millis();
}

void newGame() {
  growth = 0;
  hydration = 70;
  dryStreakTicks = 0;
  vigor = 78;
  waterPacks = 3;
  careKits = 2;
  pestDefenseKits = 2;
  remediationKits = 1;
  // Start with no seeds and the stock strain pointed at Northern Lights so
  // the first seed-catalog visit opens with the EASY starter strain buyable.
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) seedsByStrain[i] = 0;
  plantStrain = strainCatalogOrder[0];
  seedStrain = strainCatalogOrder[0];
  activeStrain = 0;
  harvestedStrainMask = 0;
  strainUnlockedThrough = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    stashByStrain[i] = 0;
    hashByStrain[i] = 0;
  }
  untrimmedUnits = 0;
  untrimmedStrain = 0;
  stonedLevel = 0;
  credits = 18;
  gameMinutes = 0;
  harvestCount = 0;
  lastHarvestUnits = 0;
  moldLossUnits = 0;
  moldRescuedUnits = 0;
  moldAnimationRequested = false;
  strlcpy(growerName, "Conor", sizeof(growerName));
  farmerCharacter = 0;
  farmerCharacterChosen = false;
  selectingFarmerFromOptions = false;
  pageAfterPlanting = GARDEN;
  bongs = 0;
  lighters = 0;
  rollingPapers = 0;
  growthBoostTicks = 0;
  smokeAccessoryUsed = 0;
  smokeProductUsed = 0;
  pestType = 0;
  pestSeverity = 0;
  plantActive = false;
  firstPlantingDone = false;
  plantedPotMask = 0;
  seedDragging = false;
  page = CHARACTER_SELECT;
  gameSpeed = 0;
  displayInverted = false;
  musicVolume = 40;
  hashRoomUnlocked = false;
  saveGame();
}

void loadGame() {
  preferences.begin("cannagame", false);
  if (!preferences.getBool("started", false)) {
    newGame();
    return;
  }
  growth = preferences.getUChar("growth", 0);
  hydration = preferences.getUChar("water", 70);
  vigor = preferences.getUChar("vigor", 78);
  waterPacks = preferences.getUChar("waterPk", 3);
  careKits = preferences.getUChar("careKit", 2);
  pestDefenseKits = preferences.getUChar("pestKit", 2);
  seedStrain = preferences.getUChar("seedType", strainCatalogOrder[0]);
  activeStrain = preferences.getUChar("activeType", seedStrain);
  if (seedStrain >= STRAIN_COUNT) seedStrain = 0;
  if (activeStrain >= STRAIN_COUNT) activeStrain = seedStrain;
  // Per-strain seed pool (v15+). Older saves kept one seed pool tagged with
  // seedStrain: migrate it into the new per-strain slots.
  if (preferences.getBytesLength("seedArr") == sizeof(seedsByStrain)) {
    preferences.getBytes("seedArr", seedsByStrain, sizeof(seedsByStrain));
  } else {
    uint8_t oldSeeds = preferences.getUChar("seeds", 0);
    if (oldSeeds && seedStrain < STRAIN_COUNT) seedsByStrain[seedStrain] = oldSeeds;
  }
  plantStrain = seedStrain;
  if (!seedsByStrain[plantStrain]) plantStrain = nextSeededStrain(plantStrain, 1);
  // One-time migration: saves started before the easy-first catalog received
  // the old Sour Diesel starter pack. If those seeds were never planted,
  // swap the stock to Northern Lights so the EASY strain is buyable first.
  if (!preferences.getBool("migNL", false)) {
    if (seedsByStrain[seedStrain] > 0 && seedStrain == 0 && !plantActive) {
      seedStrain = strainCatalogOrder[0];
      activeStrain = seedStrain;
      preferences.putUChar("seedType", seedStrain);
      preferences.putUChar("activeType", activeStrain);
    }
    preferences.putBool("migNL", true);
  }
  harvestedStrainMask = preferences.getUShort("strainMask", 0) & 0xFFFF;
  strainUnlockedThrough = min((int)preferences.getUChar("strainOpen", 0), (int)STRAIN_COUNT - 1);
  if (preferences.getBytes("stashArr", stashByStrain, sizeof(stashByStrain)) !=
      sizeof(stashByStrain)) {
    // Migrate pre-per-strain saves: the old pooled stash keeps its value in
    // the cheapest strain so nothing is inflated.
    stashByStrain[3] = preferences.getUShort("stash", 0);
  }
  if (preferences.getBytes("hashArr", hashByStrain, sizeof(hashByStrain)) !=
      sizeof(hashByStrain)) {
    hashByStrain[3] = preferences.getUShort("hash", 0);
  }
  untrimmedUnits = preferences.getUShort("rawFlower", 0);
  untrimmedStrain = preferences.getUChar("rawStrain", 0);
  stonedLevel = preferences.getUChar("stoned", 0);
  credits = preferences.getUShort("credits", 18);
  gameMinutes = preferences.getUShort("minutes", 0);
  harvestCount = preferences.getUShort("harvests", 0);
  String savedGrowerName = preferences.getString("grower", "Conor");
  savedGrowerName.trim();
  if (savedGrowerName.length()) savedGrowerName.toCharArray(growerName, sizeof(growerName));
  bongs = preferences.getUChar("bongs", 0);
  lighters = preferences.getUChar("lighters", 0);
  rollingPapers = preferences.getUChar("papers", 0);
  growthBoostTicks = preferences.getUChar("growBoost", 0);
  plantActive = preferences.getBool("active", true);
  plantVariant = preferences.getUShort("plantVar", 1);
  firstPlantingDone = preferences.getBool("plantDone", true);
  farmerCharacter = preferences.getUChar("farmer", 0);
  if (farmerCharacter > 9) farmerCharacter = 0;
  farmerCharacterChosen = preferences.getBool("farmerChosen", firstPlantingDone);
  plantedPotMask = preferences.getUChar("plantMask", firstPlantingDone ? 0x0F : 0);
  uint8_t afterPlant = preferences.getUChar("afterPlant", (uint8_t)GARDEN);
  pageAfterPlanting = (afterPlant == HASH_ROOM || afterPlant == TRIM_ROOM)
                        ? (Page)afterPlant : GARDEN;
  if (!firstPlantingDone) {
    plantActive = false;
    // Route by what the grower actually has: no grower yet -> picker; grower
    // but no full seed pack -> seed catalog; pack in hand -> planting screen.
    page = !farmerCharacterChosen ? CHARACTER_SELECT
         : totalSeeds() >= PLANTS_PER_ROOM ? PLANTING : SEED_SHOP;
  }
  strlcpy(wifiSsid, preferences.getString("wSsid", "").c_str(), sizeof(wifiSsid));
  strlcpy(wifiPass, preferences.getString("wPass", "").c_str(), sizeof(wifiPass));
  wifiSavedCount = preferences.getUChar("wSN", 0);
  if (wifiSavedCount > 4) wifiSavedCount = 0;
  for (uint8_t i = 0; i < wifiSavedCount; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "wS%u", i);
    strlcpy(wifiSavedSsid[i], preferences.getString(key, "").c_str(), sizeof(wifiSavedSsid[0]));
    snprintf(key, sizeof(key), "wP%u", i);
    strlcpy(wifiSavedPass[i], preferences.getString(key, "").c_str(), sizeof(wifiSavedPass[0]));
  }
  gameSpeed = preferences.getUChar("gspeed", 0);
  if (gameSpeed > 2) gameSpeed = 0;
  displayInverted = preferences.getBool("invert", false);
  musicVolume = preferences.getUChar("mvol", 40);
  if (musicVolume > 100) musicVolume = 40;
  hashRoomUnlocked = preferences.getBool("hashRoom", false);
  pestType = preferences.getUChar("pest", 0);
  pestSeverity = preferences.getUChar("pestLevel", 0);
  remediationKits = preferences.getUChar("remKit", 1);
  if (pestType > 3) { pestType = 0; pestSeverity = 0; }
  defoliateWindow = preferences.getUChar("defWin", 255);
  defoliateClicks = preferences.getUChar("defClicks", 0);
  defoliateLimit = preferences.getUChar("defLimit", 2);
  defoliateReadyAt = preferences.getUShort("defReady", 0);
  dryStreakTicks = preferences.getUChar("dryTick", 0);
  if (preferences.getBytesLength("sqArr") == sizeof(stashQualityByStrain))
    preferences.getBytes("sqArr", stashQualityByStrain, sizeof(stashQualityByStrain));
  if (preferences.getBytesLength("hqArr") == sizeof(hashQualityByStrain))
    preferences.getBytes("hqArr", hashQualityByStrain, sizeof(hashQualityByStrain));
  untrimmedQuality = preferences.getUChar("rawQual", 70);
}

// Advances the grow by the wall time that passed since the last save — what
// keeps the grow running through sleep (the RTC clock never stops) and across
// deep-sleep wakes / reboots while the board stays powered.
void catchUpGrowTime() {
  uint32_t nowE = (uint32_t)time(nullptr);
  uint32_t savedE = preferences.getUInt("epoch", 0);
  if (savedE && savedE < nowE) {
    const uint8_t secPerTick = gameSpeed == 2 ? 4 : 30;
    uint32_t ticks = (nowE - savedE) / secPerTick;
    if (ticks > 2880) {
      ticks = 2880;         // bound pathological catch-ups (~24 h of growth)
      growEpoch = nowE;     // skip anything beyond the bound
    } else {
      growEpoch = savedE + ticks * secPerTick;
    }
    // Boot catch-up is sleep time too: run it dormant so a plant can never
    // die while the board was asleep/reset.
    ticksDuringSleep = true;
    while (ticks--) gameTick();
    ticksDuringSleep = false;
  } else {
    // No usable saved clock (first boot, or the clock restarted after a
    // power cycle): start counting from now.
    growEpoch = nowE;
  }
}

bool addNearbyPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 1;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void onNearbyData(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || len != (int)sizeof(NearbyPacket)) return;
  NearbyPacket packet;
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != NEARBY_MAGIC) return;
  portENTER_CRITICAL(&nearbyMux);
  inboundNearbyPacket = packet;
  memcpy(inboundNearbyMac, info->src_addr, sizeof(inboundNearbyMac));
  inboundNearbyReady = true;
  portEXIT_CRITICAL(&nearbyMux);
}

void startNearbyRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_err_t result = esp_now_init();
  if (result != ESP_OK && result != ESP_ERR_ESPNOW_EXIST) return;
  esp_now_register_recv_cb(onNearbyData);
  addNearbyPeer(BROADCAST_MAC);
  nearbyReady = true;
  lastNearbyHello = 0;
}

bool sendNearbyPacket(const uint8_t *mac, uint8_t type, uint8_t item,
                      const char *text) {
  if (!nearbyReady || !addNearbyPeer(mac)) return false;
  NearbyPacket packet = {};
  packet.magic = NEARBY_MAGIC;
  packet.type = type;
  packet.item = item;
  strncpy(packet.brand, DEVICE_BRAND, sizeof(packet.brand) - 1);
  strncpy(packet.grower, growerName, sizeof(packet.grower) - 1);
  packet.masteryLevel = currentMasteryLevel();
  if (text) strncpy(packet.text, text, sizeof(packet.text) - 1);
  return esp_now_send(mac, (const uint8_t *)&packet, sizeof(packet)) == ESP_OK;
}

int findOrAddNearbyPeer(const uint8_t *mac, const NearbyPacket &packet) {
  int slot = -1;
  unsigned long oldest = ULONG_MAX;
  int oldestSlot = 0;
  unsigned long now = millis();
  for (int i = 0; i < MAX_NEARBY_PEERS; ++i) {
    if (nearbyPeers[i].active && memcmp(nearbyPeers[i].mac, mac, 6) == 0) {
      slot = i;
      break;
    }
    if (!nearbyPeers[i].active || now - nearbyPeers[i].lastSeen > 20000UL) {
      slot = i;
      break;
    }
    if (nearbyPeers[i].lastSeen < oldest) {
      oldest = nearbyPeers[i].lastSeen;
      oldestSlot = i;
    }
  }
  if (slot < 0) slot = oldestSlot;
  NearbyPeer &peer = nearbyPeers[slot];
  peer.active = true;
  memcpy(peer.mac, mac, 6);
  strncpy(peer.brand, packet.brand[0] ? packet.brand : "Garden", sizeof(peer.brand) - 1);
  peer.brand[sizeof(peer.brand) - 1] = '\0';
  strncpy(peer.grower, packet.grower[0] ? packet.grower : "Grower", sizeof(peer.grower) - 1);
  peer.grower[sizeof(peer.grower) - 1] = '\0';
  peer.masteryLevel = constrain((int)packet.masteryLevel, 1, 10);
  peer.lastSeen = now;
  return slot;
}

// --- Nearby notifications ----------------------------------------------------
// A new message or supply delivery pops a small tappable card over whatever
// page is up: "NEW MESSAGE" / "SUPPLIES RECEIVED" + sender. Tapping it opens
// the NEARBY inbox; it also fades out on its own after 12 s. While the NEARBY
// page itself is open, incoming packets only repaint the grower list /
// inbox text, never the whole page.
bool nearbyPopupActive = false;
unsigned long nearbyPopupUntil = 0;
char nearbyPopupFrom[14] = "";
char nearbyPopupTitle[20] = "NEW MESSAGE";

void drawNearbyPopup() {
  game.fillRoundRect(168, 197, 147, 39, 8, ACCENT_DARK);
  game.drawRoundRect(168, 197, 147, 39, 8, PANEL_LIGHT);
  game.setTextColor(TFT_WHITE, ACCENT_DARK);
  game.drawCentreString(nearbyPopupTitle, 241, 204, 1);
  game.setTextColor(GOLD, ACCENT_DARK);
  game.drawCentreString(nearbyPopupFrom, 241, 216, 1);
}

// Repaints just the grower list band. No latest-message line here: messages
// and supply deliveries surface through the popup / inbox instead.
void drawNearbyPeerListOnly() {
  game.fillRect(0, 78, SCREEN_W, 100, BG);  // rows + empty-scan box
  int count = 0;
  for (int i = 0; i < MAX_NEARBY_PEERS; ++i) {
    NearbyPeer &peer = nearbyPeers[i];
    if (peer.active && millis() - peer.lastSeen < 20000UL) {
      ++count;
      int y = 80 + i * 23;
      uint16_t fill = (selectedNearbyPeer == i) ? ACCENT_DARK : PANEL;
      game.fillRoundRect(13, y, 294, 21, 4, fill);
      game.drawRoundRect(13, y, 294, 21, 4, PANEL_LIGHT);
      game.setTextColor(TEXT_MAIN, fill);
      game.drawString(peer.grower, 20, y + 6, 1);   // just the typed name
      game.setTextColor(TEXT_DIM, fill);
      game.drawRightString("ONLINE", 298, y + 6, 1);
    } else {
      peer.active = false;
      if (selectedNearbyPeer == i) selectedNearbyPeer = 255;
    }
  }
  if (!count) {
    game.fillRoundRect(14, 85, 292, 63, 8, PANEL);
    game.drawRoundRect(14, 85, 292, 63, 8, PANEL_LIGHT);
    game.setTextColor(TEXT_MAIN, PANEL);
    game.drawCentreString(nearbyReady ? "Scanning nearby Wi-Fi devices..." : "Nearby radio unavailable", 160, 103, 2);
    game.setTextColor(TEXT_DIM, PANEL);
    game.drawCentreString("No router needed; both boards scan together", 160, 127, 1);
  }
}

// Repaints just the inbox message box interior.
void drawNearbyInboxTextOnly() {
  game.fillRect(16, 84, 288, 74, PANEL);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(lastNearbyMessage, 160, 105, 2);
}

// The popup card sits over content that gets partial repaints (garden stats,
// farmer visits, animations) — re-stamp it at the end of each loop pass so it
// always reads as the topmost layer.
void drawFarmerInspector(int pose);
void drawFarmerSpeechBubble(const FarmerSaying &saying);

void refreshNearbyPopup() {
  // The dying-plant modal rides inside the buffer. If it left without a tap
  // (the plant recovered or died on its own), redraw the page under it so no
  // bar of the popup stays behind the plant art.
  if (deathWarnStamped && !deathWarnModal) {
    deathWarnStamped = false;
    render();                     // clean repaint; presentFrame included
  }
  if (farmerOverlayActive) {
    if ((long)(millis() - farmerOverlayUntil) >= 0 && !waterAnimActive) {
      farmerOverlayActive = false;
      restoreFarmerVisitArt();      // repaints the covered regions only
    } else if (overlayDirty && (page == GARDEN || page == PLANT_VIEW)) {
      // The farmer and bubble are only re-stamped when something actually
      // repainted underneath them. Quiet passes draw nothing, so the figure
      // stops flooding the display bus with continuous redraw traffic.
      drawFarmerInspector(2);
      drawFarmerSpeechBubble(farmerOverlaySaying);
      if (waterAnimActive) farmerOverlayUntil = millis() + 3000UL;
    }
  }
  overlayDirty = false;
  if (nearbyPopupActive) drawNearbyPopup();
  // The dying-plant modal is stamped LAST so it sits on top of everything:
  // stats, animations, the farmer overlay and the nearby popup alike.
  if (deathWarnModal) {
    drawDeathWarnPopup();
    deathWarnStamped = true;
  }
}

void applyNearbyGift(uint8_t item, uint8_t strain) {
  switch (item) {
    case 1:
      strain = min((int)strain, (int)STRAIN_COUNT - 1);
      seedsByStrain[strain] = min(255, (int)seedsByStrain[strain] + 4);
      seedStrain = strain;
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received 4 %s seeds", strainNames[strain]);
      break;
    case 2: pestDefenseKits = min(255, (int)pestDefenseKits + 1); snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received a pest kit"); break;
    case 3: waterPacks = min(255, (int)waterPacks + 2); snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received 2 water packs"); break;
    case 4: careKits = min(255, (int)careKits + 2); snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received 2 care kits"); break;
    case 5:
      strain = min((int)strain, (int)STRAIN_COUNT - 1);
      stashByStrain[strain] = min(65535, (int)stashByStrain[strain] + 1);
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received a %s jar", strainNames[strain]);
      break;
    case 6:
      strain = min((int)strain, (int)STRAIN_COUNT - 1);
      hashByStrain[strain] = min(65535, (int)hashByStrain[strain] + 1);
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received a %s hash jar", strainNames[strain]);
      break;
    default: return;
  }
  saveGame();
}

void serviceNearby() {
  if (!nearbyReady) return;
  unsigned long now = millis();
  if (now - lastNearbyHello >= 2200UL) {
    sendNearbyPacket(BROADCAST_MAC, PACKET_HELLO, 0, "");
    lastNearbyHello = now;
  }

  NearbyPacket packet;
  uint8_t mac[6];
  bool hasPacket = false;
  portENTER_CRITICAL(&nearbyMux);
  if (inboundNearbyReady) {
    packet = inboundNearbyPacket;
    memcpy(mac, inboundNearbyMac, sizeof(mac));
    inboundNearbyReady = false;
    hasPacket = true;
  }
  portEXIT_CRITICAL(&nearbyMux);
  if (!hasPacket) return;

  int peerIndex = findOrAddNearbyPeer(mac, packet);
  // Seed gifts carry the sender's strain in text (one digit). Older senders
  // leave it empty: the receiver then keeps their current strain.
  uint8_t giftStrain = seedStrain;
  if (packet.text[0] >= '0' && packet.text[0] < '0' + STRAIN_COUNT) {
    giftStrain = packet.text[0] - '0';
  }
  if (packet.type == PACKET_MESSAGE) {
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "%.25s: %.25s",
             nearbyPeers[peerIndex].grower, packet.text);
  } else if (packet.type == PACKET_GIFT) {
    applyNearbyGift(packet.item, giftStrain);
  } else if (packet.type == PACKET_CASH) {
    uint16_t amount = (uint16_t)atoi(packet.text);
    if (amount) {
      credits = (uint16_t)min(65535, (int)credits + amount);
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Received $%u", amount);
      saveGame();
    }
  }
  if (page == NEARBY) {
    // Targeted refresh only: keep the header, buttons and layout untouched.
    if (nearbyMode == 0) {
      drawNearbyPeerListOnly();
      presentFrame();
    } else if (nearbyMode == 3) {
      drawNearbyInboxTextOnly();
      presentFrame();
    }
    return;
  }
  // Popup only for real events: a message or a supply delivery. HELLO
  // beacons (peers announcing themselves) and other traffic never pop up.
  if (packet.type == PACKET_MESSAGE) {
    strlcpy(nearbyPopupTitle, "NEW MESSAGE", sizeof(nearbyPopupTitle));
  } else if (packet.type == PACKET_GIFT) {
    strlcpy(nearbyPopupTitle, "SUPPLIES RECEIVED", sizeof(nearbyPopupTitle));
  } else if (packet.type == PACKET_CASH) {
    strlcpy(nearbyPopupTitle, "MONEY RECEIVED", sizeof(nearbyPopupTitle));
  } else {
    return;
  }
  strlcpy(nearbyPopupFrom, nearbyPeers[peerIndex].grower, sizeof(nearbyPopupFrom));
  nearbyPopupUntil = millis() + 12000UL;
  nearbyPopupActive = true;
  drawNearbyPopup();
  presentWithOverlays();
}


void drawButton(int x, int y, int w, int h, const char *label,
                uint16_t fill, uint16_t textColor = TEXT_MAIN) {
  game.fillRoundRect(x, y, w, h, 6, fill);
  game.drawRoundRect(x, y, w, h, 6, PANEL_LIGHT);
  game.setTextColor(textColor, fill);
  int font = (w < 90 || h < 25) ? 1 : 2;
  int textHeight = font == 2 ? 16 : 8;
  game.drawCentreString(label, x + w / 2, y + (h - textHeight) / 2, font);
}


void drawFarmerHeadLarge(int cx, int cy, uint8_t character) {
  uint16_t hair = character == 3 ? rgb565(210, 176, 104) : rgb565(35, 27, 26);
  uint16_t skin = character == 2 ? rgb565(133, 84, 61)
                : character == 6 ? rgb565(172, 112, 78)
                : character == 7 ? rgb565(216, 156, 116)
                : rgb565(230, 184, 143);
  uint16_t beard = rgb565(52, 37, 33);
  uint16_t black = rgb565(16, 19, 22);

  if (character == 5) {
    // James: normal mid-length brown-gray hair with a gray goatee.
    uint16_t jHair = rgb565(124, 110, 97), jLight = rgb565(168, 156, 142), jDark = rgb565(88, 76, 66);
    game.fillRoundRect(cx - 14, cy - 14, 28, 34, 10, jHair);      // back + sides past the shoulders
    game.fillCircle(cx, cy, 10, skin);
    game.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, jHair);        // flat fringe
    game.fillRect(cx - 14, cy - 6, 5, 20, jHair);                 // mid-length sides to the jaw
    game.fillRect(cx + 9, cy - 6, 5, 20, jHair);
    game.drawLine(cx - 9, cy - 11, cx + 9, cy - 13, jLight);      // subtle gray highlights
    game.drawLine(cx - 10, cy - 3, cx - 10, cy + 9, jLight);
    game.drawLine(cx + 10, cy - 3, cx + 10, cy + 9, jDark);
    game.drawFastHLine(cx - 6, cy - 4, 4, jDark);                 // brows
    game.drawFastHLine(cx + 2, cy - 4, 4, jDark);
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);   // dark grow glasses
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);          // bridge
    game.drawFastHLine(cx - 14, cy - 5, 4, black);         // arms into hair
    game.drawFastHLine(cx + 10, cy - 5, 4, black);
    game.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // glare
    game.drawFastHLine(cx - 4, cy + 5, 8, rgb565(150, 90, 80));
    // Grayish goatee: mustache, sides around the mouth, and chin patch.
    uint16_t jGoatee = rgb565(152, 146, 140);
    game.drawFastHLine(cx - 4, cy + 3, 8, jGoatee);
    game.drawFastHLine(cx - 3, cy + 2, 6, jGoatee);
    game.drawFastVLine(cx - 5, cy + 3, 4, jGoatee);
    game.drawFastVLine(cx + 4, cy + 3, 4, jGoatee);
    game.fillRoundRect(cx - 3, cy + 6, 7, 6, 2, jGoatee);
    return;
  }

  if (character == 1) {
    // Mathias: shoulder-length wavy brown hair, square shades, thick mustache.
    uint16_t mHair = rgb565(96, 64, 40), mHairLight = rgb565(134, 95, 60);
    game.fillRoundRect(cx - 14, cy - 14, 28, 31, 10, mHair);      // hair past the jaw
    game.fillCircle(cx, cy, 10, skin);
    game.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, mHair);        // swept fringe
    game.fillRect(cx - 14, cy - 6, 5, 23, mHair);                // long sides
    game.fillRect(cx + 9, cy - 6, 5, 23, mHair);
    game.drawFastVLine(cx - 12, cy - 2, 18, mHairLight);          // wavy strands
    game.drawFastVLine(cx - 11, cy + 2, 15, mHairLight);
    game.drawFastVLine(cx + 12, cy - 1, 16, mHairLight);
    game.drawFastVLine(cx + 11, cy + 4, 13, mHairLight);
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);          // square shades
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);                 // bridge
    game.drawFastHLine(cx - 14, cy - 5, 4, black);                // arms into hair
    game.drawFastHLine(cx + 10, cy - 5, 4, black);
    game.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // lens glare
    game.fillRoundRect(cx - 7, cy + 4, 14, 5, 2, rgb565(110, 74, 48));   // thick mustache
    game.fillRect(cx - 1, cy + 4, 2, 2, skin);                    // philtrum gap
    return;
  }

  if (character == 6) {
    // Sergio: buzz cut, clear glasses, short goatee.
    uint16_t buzz = rgb565(45, 32, 26);
    game.fillCircle(cx, cy - 2, 11, buzz);                      // buzz: thin crescent
    game.fillCircle(cx, cy, 10, skin);                          // face carves it back
    for (int i = -8; i <= 8; i += 4)                              // stubble texture
      game.drawFastVLine(cx + i, cy - 12, 3, rgb565(74, 55, 42));
    game.fillRect(cx - 10, cy - 5, 2, 2, buzz);                 // sideburn hints
    game.fillRect(cx + 8, cy - 5, 2, 2, buzz);
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, rgb565(206, 226, 232));  // clear lenses
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, rgb565(206, 226, 232));
    game.drawRoundRect(cx - 10, cy - 6, 9, 8, 1, black);        // thin frames
    game.drawRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);
    game.drawFastHLine(cx - 14, cy - 5, 4, black);
    game.drawFastHLine(cx + 10, cy - 5, 4, black);
    game.drawFastHLine(cx - 4, cy + 4, 8, beard);               // short goatee
    game.fillRect(cx - 2, cy + 7, 4, 3, beard);
    return;
  }

  if (character == 7) {
    // Vicente: jerry curl, medium length, dark brown.
    uint16_t vHair = rgb565(58, 38, 26), vCurl = rgb565(94, 64, 42);
    game.fillRoundRect(cx - 14, cy - 14, 28, 30, 10, vHair);
    game.fillCircle(cx, cy, 10, skin);
    game.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, vHair);
    game.fillRect(cx - 14, cy - 6, 5, 21, vHair);
    game.fillRect(cx + 9, cy - 6, 5, 21, vHair);
    for (int i = -12; i <= 12; i += 6)                            // curl texture
      game.fillCircle(cx + i, cy - 15 + abs(i) / 3, 2, vCurl);
    game.fillCircle(cx - 12, cy - 2, 2, vCurl);
    game.fillCircle(cx + 12, cy + 1, 2, vCurl);
    game.fillCircle(cx - 12, cy + 5, 2, vCurl);
    game.fillCircle(cx + 12, cy + 7, 2, vCurl);
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);        // dark grow glasses
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);
    game.drawFastHLine(cx - 14, cy - 5, 4, black);
    game.drawFastHLine(cx + 10, cy - 5, 4, black);
    game.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));
    return;
  }

  if (character == 9) {
    // Benji: shoulder-length blonde-brown dreads, Ray-Bans, chin beard.
    uint16_t bHair = rgb565(146, 114, 70), bLight = rgb565(184, 152, 100),
             bDark = rgb565(112, 84, 52), bBeard = rgb565(66, 46, 32);
    game.fillRoundRect(cx - 14, cy - 14, 28, 32, 10, bHair);   // dread mass behind
    game.fillCircle(cx, cy, 10, skin);
    game.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, bHair);     // hairline
    // Individual dreads framing the face, alternating light/dark.
    for (int i = 0; i < 3; ++i) {
      int y = cy - 8 + i * 9;
      game.fillRoundRect(cx - 16, y, 4, 9, 2, i % 2 ? bLight : bDark);     // left dreads
      game.fillRoundRect(cx + 12, y + 3, 4, 9, 2, i % 2 ? bDark : bLight); // right dreads
    }
    game.fillCircle(cx - 12, cy + 19, 2, bLight);              // loose dread tips
    game.fillCircle(cx + 12, cy + 20, 2, bDark);
    // Ray-Bans: square dark lenses, bridge and temple arms.
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);
    game.drawFastHLine(cx - 14, cy - 5, 4, black);
    game.drawFastHLine(cx + 10, cy - 5, 4, black);
    game.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // lens glare
    // Mustache and chin patch — the bandana stays home.
    game.fillRoundRect(cx - 7, cy + 4, 14, 4, 2, bBeard);
    game.fillRect(cx - 1, cy + 4, 2, 2, skin);
    game.fillRect(cx - 3, cy + 8, 7, 3, bBeard);              // short stubble patch
    for (int i = -2; i <= 2; i += 2)                          // stubble specks
      game.drawPixel(cx + i, cy + 7, bBeard);
    return;
  }

  game.fillRoundRect(cx - 14, cy - 14, 28, 36, 10, hair);
  if (character == 2) {
    for (int i = -2; i <= 2; ++i) {
      game.drawFastVLine(cx + i * 5, cy - 12, 36, rgb565(65, 44, 35));
      game.drawFastVLine(cx + i * 5 + 2, cy - 10, 33, hair);
    }
  }
  game.fillCircle(cx, cy, 10, skin);

  if (character == 0) {
    game.fillRoundRect(cx - 12, cy - 14, 24, 8, 6, hair);
    game.fillRect(cx - 13, cy - 6, 5, 28, hair);
    game.fillRect(cx + 8, cy - 6, 5, 28, hair);
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);   // dark grow glasses
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);          // bridge
    game.drawFastHLine(cx - 14, cy - 5, 4, black);         // arms into hair
    game.drawFastHLine(cx + 10, cy - 5, 4, black);
    game.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // glare
    game.drawFastHLine(cx - 7, cy + 4, 14, beard);
    game.drawFastHLine(cx - 5, cy + 7, 10, TFT_WHITE);
    game.fillTriangle(cx - 6, cy + 9, cx + 6, cy + 9, cx, cy + 16, beard);
  } else if (character == 2) {
    game.fillRoundRect(cx - 10, cy - 5, 9, 7, 2, TFT_BLACK);
    game.fillRoundRect(cx + 1, cy - 5, 9, 7, 2, TFT_BLACK);
    game.drawLine(cx - 1, cy - 2, cx + 1, cy - 2, black);
    game.drawLine(cx - 10, cy - 5, cx - 1, cy - 5, black);
    game.drawLine(cx + 1, cy - 5, cx + 10, cy - 5, black);
    game.fillTriangle(cx - 8, cy + 4, cx + 8, cy + 4, cx, cy + 17, beard);
    game.drawFastHLine(cx - 4, cy + 6, 8, TFT_WHITE);
  } else if (character == 8) {
    // Pedro (photo reference for the head only): short black fade, dark
    // wayfarers, full beard. Same gardener outfit as the other growers.
    uint16_t pHair = rgb565(30, 26, 24);
    uint16_t pSkin = rgb565(212, 168, 138);
    uint16_t pBeard = rgb565(62, 52, 47);
    game.fillRoundRect(cx - 12, cy - 12, 24, 24, 8, pHair);   // short hair cap
    game.fillCircle(cx, cy, 10, pSkin);
    game.fillRoundRect(cx - 11, cy - 11, 22, 5, 4, pHair);    // flat fringe
    game.fillRect(cx - 11, cy - 6, 2, 6, pHair);              // tight fade sides
    game.fillRect(cx + 9, cy - 6, 2, 6, pHair);
    game.fillRoundRect(cx - 9, cy + 2, 18, 13, 6, pBeard);    // full beard
    game.fillRoundRect(cx - 2, cy + 3, 4, 2, 1, pSkin);       // mouth peeking
    game.drawFastHLine(cx - 8, cy + 4, 5, rgb565(122, 114, 106));  // gray flecks
    game.drawFastHLine(cx + 3, cy + 5, 5, rgb565(122, 114, 106));
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 2, black);      // dark wayfarers
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 2, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);
    game.drawLine(cx - 8, cy - 4, cx - 5, cy - 2, rgb565(95, 95, 102));  // lens glare
  } else if (character == 4) {
    // Eric: long dark hair tucked behind both ears and thick black glasses.
    game.fillRoundRect(cx - 12, cy - 14, 24, 9, 6, hair);
    game.fillRoundRect(cx - 15, cy - 4, 5, 22, 3, hair);
    game.fillRoundRect(cx + 10, cy - 4, 5, 22, 3, hair);
    game.fillRoundRect(cx - 10, cy - 5, 9, 7, 2, rgb565(10, 13, 16));
    game.fillRoundRect(cx + 1, cy - 5, 9, 7, 2, rgb565(10, 13, 16));
    game.drawFastHLine(cx - 1, cy - 2, 2, rgb565(10, 13, 16));
    game.drawFastHLine(cx - 9, cy - 4, 7, rgb565(96, 107, 111));
    game.drawFastHLine(cx + 2, cy - 4, 7, rgb565(96, 107, 111));
    game.drawFastHLine(cx - 4, cy + 6, 8, rgb565(112, 65, 52));
  } else {
    game.fillRoundRect(cx - 12, cy - 14, 24, 8, 6, hair);
    game.fillRect(cx - 13, cy - 6, 5, 29, hair);
    game.fillRect(cx + 8, cy - 6, 5, 29, hair);
    game.fillRoundRect(cx - 10, cy - 6, 9, 8, 1, black);   // dark grow glasses
    game.fillRoundRect(cx + 1, cy - 6, 9, 8, 1, black);
    game.drawFastHLine(cx - 1, cy - 4, 2, black);          // bridge
    game.drawFastHLine(cx - 14, cy - 5, 4, black);         // arms into hair
    game.drawFastHLine(cx + 10, cy - 5, 4, black);
    game.drawLine(cx - 8, cy - 3, cx - 5, cy - 1, rgb565(70, 90, 100));  // glare
    game.drawFastVLine(cx - 11, cy + 2, 15, rgb565(239, 216, 151));
    game.drawFastVLine(cx + 11, cy + 2, 15, rgb565(239, 216, 151));
    game.drawFastHLine(cx - 4, cy + 6, 8, rgb565(167, 83, 78));
    game.drawFastHLine(cx - 3, cy + 8, 6, TFT_WHITE);
  }
}

void drawCharacterMini(int cx, int top, uint8_t character) {
  uint16_t hair = character == 3 ? rgb565(210, 176, 104)
                : character == 5 ? rgb565(124, 110, 97)
                : character == 6 ? rgb565(45, 32, 26)
                : character == 7 ? rgb565(58, 38, 26)
                : character == 1 ? rgb565(96, 64, 40)
                : character == 9 ? rgb565(146, 114, 70)
                : rgb565(35, 27, 26);
  uint16_t skin = character == 2 ? rgb565(133, 84, 61)
                : character == 6 ? rgb565(172, 112, 78)
                : character == 7 ? rgb565(216, 156, 116)
                : rgb565(230, 184, 143);
  const uint16_t black = rgb565(13, 16, 19);
  int faceY = top + 8;
  game.fillRoundRect(cx - 9, top, 18, character == 6 ? 10 : 15, 7, hair);
  game.fillCircle(cx, faceY, 6, skin);
  if (character == 6) {
    game.drawFastHLine(cx - 2, faceY + 3, 5, rgb565(52, 37, 33));   // short goatee
    game.fillRect(cx - 1, faceY + 4, 2, 2, rgb565(52, 37, 33));
  }
  if (character == 7) {
    game.drawFastVLine(cx - 8, top + 6, 8, hair);                  // medium sides
    game.drawFastVLine(cx + 7, top + 6, 8, hair);
    for (int i = -6; i <= 6; i += 4)                                 // curl texture
      game.drawPixel(cx + i, top + abs(i) / 3, rgb565(94, 64, 42));
  }
  if (character == 5) {
    game.drawFastVLine(cx - 8, top + 6, 7, hair);                 // mid-length sides
    game.drawFastVLine(cx + 7, top + 6, 7, hair);
    game.drawFastHLine(cx - 2, faceY + 3, 5, rgb565(152, 146, 140));  // goatee
    game.fillRect(cx - 1, faceY + 4, 3, 2, rgb565(152, 146, 140));
  }
  if (character == 1) {
    game.drawFastVLine(cx - 8, top + 6, 8, hair);                 // long sides
    game.drawFastVLine(cx + 7, top + 6, 8, hair);
    game.drawFastHLine(cx - 3, faceY + 2, 6, rgb565(110, 74, 48));  // mustache
  }
  game.fillRoundRect(cx - 6, faceY - 2, 5, 4, 1, black);   // dark grow glasses
  game.fillRoundRect(cx + 1, faceY - 2, 5, 4, 1, black);
  game.drawFastHLine(cx - 1, faceY, 2, black);
  if (character == 0) game.drawFastHLine(cx - 3, faceY + 3, 6, rgb565(52, 37, 33));
  if (character == 8) {
    game.drawFastHLine(cx - 4, faceY + 3, 9, rgb565(62, 52, 47));   // full beard
    game.fillRoundRect(cx - 5, faceY + 4, 10, 5, 2, rgb565(62, 52, 47));
  }
  if (character == 3) {
    game.drawFastVLine(cx - 8, top + 8, 10, rgb565(239, 216, 151));
    game.drawFastVLine(cx + 7, top + 8, 10, rgb565(239, 216, 151));
  } else if (character == 4 || character == 0) {
    game.drawFastVLine(cx - 8, top + 6, 11, hair);
    game.drawFastVLine(cx + 7, top + 6, 11, hair);
  }
  game.fillRoundRect(cx - 7, top + 15, 14, 8, 3, rgb565(39, 132, 71));
  game.fillRect(cx - 5, top + 23, 4, 4, black);
  game.fillRect(cx + 1, top + 23, 4, 4, black);
}

// Cog gear icon for the settings corner (compact, to fit the header).
void drawGearIcon(int cx, int cy) {
  uint16_t metal = rgb565(178, 186, 194);
  uint16_t bg = 0x0861;
  for (int a = 0; a < 8; ++a) {
    float ang = a * PI / 4.0f;
    game.fillCircle(cx + (int)(cosf(ang) * 7.0f), cy + (int)(sinf(ang) * 7.0f), 2, metal);
  }
  game.fillCircle(cx, cy, 7, metal);
  game.fillCircle(cx, cy, 4, bg);
  game.fillCircle(cx, cy, 1, metal);
}

// Battery gauge + USB-charge indicator from IO9 (battery ADC pin).
// BATT_DIV assumes a 2:1 resistor divider on the board; tweak if a
// multimeter shows the reported voltage is off.
#define BATT_DIV 2.0f
void drawBatteryStatus(int x, int cy) {
  bool charging = Serial.isConnected();
  float v = analogReadMilliVolts(9) / 1000.0f * BATT_DIV;
  uint16_t shell = rgb565(178, 186, 194);
  game.drawRoundRect(x, cy - 3, 11, 7, 1, shell);
  game.fillRect(x + 11, cy - 1, 2, 3, shell);           // nub
  if (v < 2.5f) return;                                  // no battery connected
  uint8_t pct = (uint8_t)constrain((v - 3.30f) / 0.90f * 100.0f, 0, 100);
  int fillw = (pct * 9) / 100;
  if (fillw) game.fillRect(x + 1, cy - 2, fillw, 5, charging ? GOLD : rgb565(70, 180, 90));
  if (charging) {
    // tiny bolt over the bars
    game.fillTriangle(x + 4, cy - 3, x + 7, cy - 1, x + 4, cy + 2, 0x0000);
    game.fillTriangle(x + 6, cy + 2, x + 6, cy, x + 9, cy - 2, 0x0000);
    game.fillRect(x + 3, cy, 2, 3, 0x0000);
  }
}

void drawHeader() {
  game.fillRect(0, 0, SCREEN_W, HEADER_H, 0x0861);
  game.drawFastHLine(0, HEADER_H - 1, SCREEN_W, 0x4D89);
  drawLeafLogo(5, 2, 1, TFT_WHITE);
  game.setTextColor(TEXT_MAIN, 0x0861);
  game.drawString("WIN FLORIDA", 31, 4, 2);   // 81px wide in font 2
  game.setTextColor(TEXT_DIM, 0x0861);
  game.drawString(growerName, 32, 18, 1);

  char money[14];
  snprintf(money, sizeof(money), "$%u", credits);
  game.setTextColor(GOLD, 0x0861);
  game.drawRightString(money, 258, 6, 2);

  drawBatteryStatus(264, 14);
  drawGearIcon(301, 14);

  // Stoned meter: 10 levels, from Sober to Stoned as FUCK.
  uint8_t sIdx = min(9, (int)(stonedLevel / 10));
  uint8_t lit = min(10, (int)((stonedLevel + 9) / 10));
  // Mini head + stoned meter as one centered block: head spans cx-9..cx+9,
  // meter track cx+10..cx+89, so the block's center is cx+40 — cx=121 puts it
  // right of the WIN FLORIDA wordmark (real font-2 width 81px, ending x=112)
  // and clear of the money readout (starts near x=218).
  drawCharacterMini(121, 0, farmerCharacter);   // mini head: current grower
  game.setTextColor(TEXT_DIM, 0x0861);
  game.drawString(stonedLevelNames[sIdx], 132, 3, 1);
  game.fillRoundRect(131, 14, 80, 7, 3, 0x0630);
  for (uint8_t i = 0; i < lit; ++i) {
    uint16_t seg = i < 3 ? rgb565(70, 180, 90)
                 : i < 6 ? rgb565(230, 200, 60)
                 : i < 8 ? rgb565(240, 140, 40)
                 : rgb565(235, 60, 60);
    game.fillRect(131 + i * 8, 15, 7, 5, seg);
  }

  drawGearIcon(301, 14);
}

void drawNav() {
  const char *labels[] = {"GARDEN", "SUPPLY", "MARKET", "STASH", "NEARBY"};
  const Page pages[] = {GARDEN, SUPPLIES, MARKET, STASH, NEARBY};
  game.fillRect(0, NAV_Y, SCREEN_W, NAV_H, 0x0861);
  for (int i = 0; i < 5; ++i) {
    int x = i * 64;
    Page tabPage = pages[i];
    uint16_t fill = (page == tabPage) ? ACCENT_DARK : 0x18E3;
    game.fillRoundRect(x + 2, NAV_Y + 3, 60, 27, 5, fill);
    game.setTextColor((page == tabPage) ? TFT_WHITE : TEXT_DIM, fill);
    game.drawCentreString(labels[i], x + 32, NAV_Y + 11, 1);
  }
}

void drawStatBar(int x, int y, const char *label, int value, uint16_t color) {
  char valueText[8];
  snprintf(valueText, sizeof(valueText), "%d%%", value);
  game.setTextColor(TEXT_DIM, BG);
  game.drawString(label, x, y, 1);
  game.setTextColor(TEXT_MAIN, BG);
  game.drawRightString(valueText, x + 124, y, 1);
  game.fillRoundRect(x, y + 12, 124, 9, 4, 0x0841);
  int fillWidth = (124 * value) / 100;
  if (fillWidth > 0) game.fillRoundRect(x, y + 12, fillWidth, 9, 4, color);
}

void drawLeaflet(int bx, int by, int tx, int ty, int width,
                 uint16_t darkColor, uint16_t lightColor, uint16_t veinColor) {
  float dx = (float)(tx - bx);
  float dy = (float)(ty - by);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) return;
  float px = -dy / len;
  float py = dx / len;
  int mx = (bx + tx) / 2;
  int my = (by + ty) / 2;
  int lx = mx + (int)(px * width * 0.5f);
  int ly = my + (int)(py * width * 0.5f);
  int rx = mx - (int)(px * width * 0.5f);
  int ry = my - (int)(py * width * 0.5f);

  game.fillTriangle(bx, by, lx, ly, tx, ty, darkColor);
  game.fillTriangle(bx, by, tx, ty, rx, ry, lightColor);
  game.drawLine(bx, by, tx, ty, veinColor);

  // Tiny edge notches suggest the serrated edge of a cannabis leaflet.
  for (int n = 1; n <= 2; ++n) {
    float t = 0.34f + n * 0.19f;
    int ex = bx + (int)(dx * t) + (int)(px * width * 0.34f);
    int ey = by + (int)(dy * t) + (int)(py * width * 0.34f);
    game.drawLine(ex, ey, ex + (int)(px * 2), ey + (int)(py * 2), veinColor);
    ex = bx + (int)(dx * t) - (int)(px * width * 0.34f);
    ey = by + (int)(dy * t) - (int)(py * width * 0.34f);
    game.drawLine(ex, ey, ex - (int)(px * 2), ey - (int)(py * 2), veinColor);
  }
}

void drawFanLeafTinted(int x, int y, float scale, int tiltDegrees,
                       int fingers, uint8_t tint) {
  // Four hand-tuned palettes so each plant's canopy leans a slightly
  // different shade of green (classic emerald, forest, lime, blue-green).
  uint16_t leafDark, leafMid, leafLight, vein;
  switch (tint & 3) {
    case 1:  leafDark = rgb565(14, 64, 40);  leafMid = rgb565(30, 118, 66);
             leafLight = rgb565(66, 150, 86); vein = rgb565(136, 184, 124); break;
    case 2:  leafDark = rgb565(26, 84, 28);  leafMid = rgb565(52, 142, 42);
             leafLight = rgb565(96, 180, 62); vein = rgb565(168, 204, 102); break;
    case 3:  leafDark = rgb565(16, 74, 58);  leafMid = rgb565(34, 124, 80);
             leafLight = rgb565(70, 158, 102); vein = rgb565(140, 190, 132); break;
    default: leafDark = rgb565(20, 76, 35);  leafMid = rgb565(38, 130, 58);
             leafLight = rgb565(78, 166, 75); vein = rgb565(148, 194, 111); break;
  }
  // Seedlings fan 3 fingers, young veg 5, mature plants the full 7.
  static const int offs7[] = {-66, -44, -22, 0, 22, 44, 66};
  static const int lens7[] = {19, 27, 34, 43, 34, 27, 19};
  static const int offs5[] = {-44, -22, 0, 22, 44};
  static const int lens5[] = {27, 34, 43, 34, 27};
  static const int offs3[] = {-22, 0, 22};
  static const int lens3[] = {34, 43, 34};
  const int *offs = fingers <= 3 ? offs3 : fingers == 5 ? offs5 : offs7;
  const int *lens = fingers <= 3 ? lens3 : fingers == 5 ? lens5 : lens7;
  int count = fingers <= 3 ? 3 : fingers == 5 ? 5 : 7;
  for (int i = 0; i < count; ++i) {
    float angle = (tiltDegrees + offs[i]) * 0.0174532925f;
    int length = (int)(lens[i] * scale);
    int tx = x + (int)(sinf(angle) * length);
    int ty = y - (int)(cosf(angle) * length);
    int mid = count / 2;
    int width = (int)((i == mid ? 9 : (i == mid - 1 || i == mid + 1 ? 7 : 5)) * scale);
    if (width < 3) width = 3;
    drawLeaflet(x, y, tx, ty, width, leafDark, leafMid, vein);
    if (i == mid) {
      // Highlight the central leaflet for depth.
      game.drawLine(x, y, tx, ty, leafLight);
    }
  }
}

void drawFanLeaf(int x, int y, float scale, int tiltDegrees) {
  drawFanLeafTinted(x, y, scale, tiltDegrees, 7, 0);
}

void drawPot() {
  uint16_t potDark = rgb565(91, 49, 33);
  uint16_t potMid = rgb565(157, 82, 49);
  uint16_t potLight = rgb565(205, 125, 76);
  game.fillRoundRect(62, 141, 54, 26, 5, potDark);
  game.fillRoundRect(65, 140, 48, 23, 4, potMid);
  game.fillRect(68, 142, 8, 18, potLight);
  game.drawFastHLine(65, 141, 48, rgb565(231, 159, 102));
  game.fillRoundRect(59, 137, 60, 8, 3, potDark);
  game.fillRoundRect(61, 137, 56, 5, 2, potMid);
  game.drawFastHLine(66, 138, 46, potLight);
  game.fillRect(65, 139, 48, 3, rgb565(74, 47, 29));
  game.fillCircle(73, 140, 2, rgb565(137, 94, 57));
  game.fillCircle(92, 139, 2, rgb565(137, 94, 57));
  game.fillCircle(106, 140, 2, rgb565(137, 94, 57));
  // Show the stem emerging from the soil after the pot is drawn in front.
  if (plantActive) {
    game.drawFastVLine(89, 135, 7, rgb565(35, 104, 48));
    game.drawPixel(90, 137, rgb565(111, 163, 78));
  }
}

void drawGardenBackdrop() {
  // Faithful vector rebuild of the SD art's grow room: gradient, two grid
  // systems, trellis wires, beam, four hanging grow lights and the floor.
  for (int y = HEADER_H; y < HEADER_H + 137; ++y) {
    int ay = y - HEADER_H;
    uint8_t r = 7 + (uint8_t)((ay * 3) / 136);
    uint8_t g = 23 + (uint8_t)((ay * 14) / 136);
    uint8_t b = 25 + (uint8_t)((ay * 9) / 136);
    game.drawBandHLine(0, y, 178, rgb565(r, g, b));
  }
  // Dark background grid.
  static const int darkCols[5] = {7, 43, 88, 133, 170};
  for (int i = 0; i < 5; ++i)
    game.drawFastVLine(darkCols[i], 23 + HEADER_H, 114, rgb565(27, 50, 48));
  game.drawFastHLine(0, 41 + HEADER_H, 178, rgb565(27, 47, 43));
  // Light foreground grid.
  static const int lightCols[8] = {15, 38, 61, 84, 107, 130, 153, 175};
  for (int i = 0; i < 8; ++i)
    game.drawFastVLine(lightCols[i], 41 + HEADER_H, 96, rgb565(43, 66, 58));
  static const int lightRows[4] = {52, 73, 94, 115};
  for (int i = 0; i < 4; ++i)
    game.drawFastHLine(0, lightRows[i] + HEADER_H, 178, rgb565(43, 66, 58));
  // Sage trellis wires.
  static const int wires[3] = {66, 91, 116};
  for (int i = 0; i < 3; ++i)
    game.drawFastHLine(0, wires[i] + HEADER_H, 178, rgb565(99, 116, 97));
  // Structural beam just under the light rail.
  game.fillRect(0, 20 + HEADER_H, 178, 3, rgb565(43, 59, 53));
  // Four hanging grow lights: rail, glow and lamp body.
  for (int i = 0; i < 4; ++i) {
    int lx = 10 + i * 44;
    game.fillRect(lx, 5 + HEADER_H, 33, 4, rgb565(57, 66, 60));
    game.fillRect(lx + 2, 10 + HEADER_H, 29, 3, rgb565(224, 220, 183));
    game.fillRect(lx + 2, 13 + HEADER_H, 29, 2, rgb565(170, 176, 151));
  }
  // Dark floor with a pale contact line and sparse dither.
  game.drawFastHLine(0, 137 + HEADER_H, 178, rgb565(69, 85, 60));
  game.fillRect(0, 138 + HEADER_H, 178, 6, rgb565(12, 25, 22));
  static const int dith[7][2] = {{20, 139}, {55, 140}, {90, 139}, {130, 141},
                                 {160, 140}, {40, 142}, {110, 142}};
  for (int i = 0; i < 7; ++i)
    game.drawPixel(dith[i][0], dith[i][1] + HEADER_H, rgb565(24, 39, 29));
}

// ---- Vector replicas of the PLANT<n>.BMP art (used when no SD card) ----

void drawArtPot(int cx) {
  const uint16_t rim = rgb565(204, 131, 77);
  const uint16_t out = rgb565(78, 43, 30);
  const uint16_t band = rgb565(73, 48, 31);
  const uint16_t body = rgb565(147, 75, 45);
  const uint16_t hi = rgb565(190, 112, 66);
  game.fillRect(cx - 12, 129 + HEADER_H, 25, 2, rim);
  game.drawPixel(cx - 12, 129 + HEADER_H, out);
  game.drawPixel(cx + 12, 129 + HEADER_H, out);
  game.fillRect(cx - 10, 131 + HEADER_H, 21, 10, out);
  game.fillRect(cx - 9, 132 + HEADER_H, 19, 2, band);
  game.fillTriangle(cx - 9, 134 + HEADER_H, cx + 9, 134 + HEADER_H,
                    cx + 6, 140 + HEADER_H, body);
  game.fillTriangle(cx - 9, 134 + HEADER_H, cx + 6, 140 + HEADER_H,
                    cx - 7, 140 + HEADER_H, body);
  game.fillRect(cx - 8, 139 + HEADER_H, 15, 1, out);
  game.drawLine(cx - 6, 134 + HEADER_H, cx - 5, 139 + HEADER_H, hi);
}

void drawArtBranch(int x1, int y1, int x2, int y2, uint16_t c) {
  game.drawLine(x1, y1 + HEADER_H, x2, y2 + HEADER_H, c);
  game.drawLine(x1 + 1, y1 + HEADER_H, x2 + 1, y2 + HEADER_H, c);
}

void drawArtBud(int x, int y, float s, int pistils) {
  const uint16_t calyx = rgb565(31, 78, 39);
  const uint16_t mid = rgb565(94, 128, 66);
  const uint16_t lite = rgb565(112, 159, 91);
  const uint16_t fuzz = rgb565(119, 131, 108);
  const uint16_t gold = rgb565(176, 196, 124);
  const uint16_t pil = rgb565(205, 201, 157);
  const uint16_t pilW = rgb565(224, 220, 183);
  int r1 = (int)(3.4f * s);
  if (r1 < 2) r1 = 2;
  game.fillCircle(x, y + HEADER_H, r1, mid);
  game.fillCircle(x - r1 + 1, y - (int)(1.5f * s) + HEADER_H,
                  max(1, (int)(2.2f * s)), lite);
  game.fillCircle(x + (int)(1.5f * s), y - (int)(2 * s) + HEADER_H,
                  max(1, (int)(1.6f * s)), mid);
  game.fillCircle(x + (int)s, y + (int)(1.5f * s) + HEADER_H,
                  max(1, (int)(1.8f * s)), calyx);
  game.fillCircle(x - (int)(2 * s), y + (int)(2.2f * s) + HEADER_H,
                  max(1, (int)(1.4f * s)), calyx);
  // Fuzzy sage/gold speckles so the lump reads as frosty cannabis.
  static const int8_t fz[12][2] = {{-3, -2}, {2, -3}, {4, 0},  {-4, 1},
                                   {1, 2},   {-2, 3}, {3, 3},  {0, -4},
                                   {-5, -1}, {5, 2},  {-1, -3}, {2, 4}};
  for (int i = 0; i < pistils * 2 && i < 12; ++i) {
    uint16_t c = (i % 3) ? fuzz : ((i % 2) ? gold : pil);
    game.drawPixel(x + (int)(fz[i][0] * s / 1.2f),
                   y + (int)(fz[i][1] * s / 1.2f) + HEADER_H, c);
    if (i % 4 == 0)
      game.drawPixel(x + (int)(fz[i][0] * s / 1.2f) + 1,
                     y + (int)(fz[i][1] * s / 1.2f) - 1 + HEADER_H, pilW);
  }
  for (int i = 0; i < pistils; ++i) {
    int px = x + ((i * 7) % (2 * r1)) - r1;
    int py = y - r1 + 1 + (i % 4);
    game.drawPixel(px, py + HEADER_H, (i % 2) ? pil : gold);
    game.drawPixel(px + 1, py - 1 + HEADER_H, (i % 3 == 0) ? pilW : lite);
  }
}

static uint32_t variantNext(uint32_t &rs) {
  rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
  return rs;
}

// ---- seeded plant transform -------------------------------------------------
// One code path draws the seeded pheno everywhere: the small garden (scale 1),
// the fullscreen room (magnified) and the harvest cutscene (relocated, slightly
// scaled to fit). All views share the same per-pot seed and geometry, so the
// plants always look exactly the same.
static int zRefCx, zX0, zSoilY;
static float zSc;
static inline int zX(int x) { return zX0 + (int)((x - zRefCx) * zSc); }
static inline int zY(int y) { return zSoilY + (int)((y - 129) * zSc); }

static void zBranch(int x1, int y1, int x2, int y2, uint16_t c) {
  const int ax = zX(x1), ay = zY(y1), bx = zX(x2), by = zY(y2);
  game.drawLine(ax, ay, bx, by, c);
  game.drawLine(ax + 1, ay, bx + 1, by, c);
  if (zSc > 1.2f) {   // thicker stroke so zoomed stems keep their weight
    game.drawLine(ax, ay + 1, bx, by + 1, c);
    game.drawLine(ax, ay - 1, bx, by - 1, c);
  }
}

static void zPot(int cx) {
  const int x0 = zX(cx - 12), w = (int)(25 * zSc);
  const int y0 = zSoilY, hRim = (int)(2 * zSc), hBody = (int)(11 * zSc);
  const uint16_t rim = rgb565(204, 131, 77), out = rgb565(78, 43, 30),
                 body = rgb565(147, 75, 45), hi = rgb565(190, 112, 66);
  game.fillRect(x0, y0, w, hRim, rim);
  game.drawPixel(x0, y0, out);
  game.drawPixel(x0 + w - 1, y0, out);
  const int bw = w * 3 / 5, ins = (w - bw) / 2;
  game.fillTriangle(x0, y0 + hRim, x0 + w, y0 + hRim, x0 + w - ins, y0 + hBody, body);
  game.fillTriangle(x0, y0 + hRim, x0 + w - ins, y0 + hBody, x0 + ins, y0 + hBody, body);
  game.fillRect(x0 + ins, y0 + hBody - 1, bw, 1 + (int)zSc, out);
  game.drawLine(x0 + (int)(6 * zSc), y0 + hRim + (int)(4 * zSc),
                x0 + (int)(7 * zSc), y0 + hBody - 1, hi);
}

static void drawArtPlantT(int cx, int stage, int screenX, int soilScreenY,
                          float sc, bool withPot) {
  // Every draw is seeded from plantVariant + this pot's position, so the four
  // roommates grow as distinct phenos and every new planting rolls a fresh
  // look for the whole grow. Seeding is deterministic per frame: no flicker.
  uint32_t rs = (uint32_t)plantVariant * 2654435761u ^ ((uint32_t)cx * 97u + 7u);
  variantNext(rs); variantNext(rs); variantNext(rs);
  const int lean = (int)(variantNext(rs) % 9) - 4;           // stem lean -4..+4
  const uint8_t tint = (variantNext(rs) >> 6) & 3;           // canopy palette
  // Vigor: each roommate reaches 0.78..1.24 of the stage's nominal height,
  // so some plants stay short and bushy while others stretch tall and lanky.
  const int vigorX100 = 78 + (int)(variantNext(rs) % 47);    // 78..124
  const int stretch = (int)(variantNext(rs) % 5);            // internode drift

  zRefCx = cx; zX0 = screenX; zSoilY = soilScreenY; zSc = sc;

  const uint16_t stemThin = rgb565(99, 116, 97);
  const uint16_t stemA = rgb565(119, 131, 108);
  const uint16_t stemB = rgb565(88, 132, 60);

  const int soil = 129;
  const int nominal = stage == 0 ? 16 : stage == 1 ? 28 : stage == 2 ? 42
                      : stage == 3 ? 60 : 80;
  const int topY = soil - nominal * vigorX100 / 100 +
                   (int)(variantNext(rs) % 5) - 2;

  // Stem: a gentle bow toward the plant's lean, drawn in two segments.
  const int midY = (soil + topY) / 2;
  const int midX = cx + lean / 3;
  zBranch(cx, soil, midX, midY, stemThin);
  zBranch(midX, midY, cx + lean, topY, stemThin);
  zBranch(cx + 1, soil, midX + 1, midY, stemA);
  zBranch(midX + 1, midY, cx + 1 + lean, topY, stemB);
  if (withPot) zPot(cx + 1);

  // Side branches alternate up the stem and the lower ones run longer — the
  // real cannabis Christmas-tree habit. Each branch carries a crown fan leaf,
  // and from flowering on, its own bud.
  const int fingers = stage == 0 ? 3 : stage == 1 ? 5 : 7;
  const int branches = stage == 0 ? 0 : stage == 1 ? 2
                       : 3 + (stretch >= 2 ? 1 : 0);
  for (int i = 0; i < branches; ++i) {
    const int sgn = (i & 1) ? 1 : -1;              // alternate sides
    const int ys = soil - (i + 1) * (soil - topY) / (branches + 1);
    const int rise = 3 + (int)(variantNext(rs) % 5);
    int len = (stage == 1 ? 9 : stage == 2 ? 13 : stage == 3 ? 15 : 17)
              - i * 2 + (int)(variantNext(rs) % 4) - 1;
    if (len < 5) len = 5;
    const int tipX = cx + sgn * len + lean / 4;
    const int tipY = ys - rise;
    zBranch(cx + lean / 3, ys, tipX, tipY, stemA);
    zBranch(cx + 1 + lean / 3, ys, tipX + 1, tipY, stemB);
    // Fan leaf crowning the branch, angled outward with its own jitter.
    drawFanLeafTinted(zX(tipX), zY(tipY - 1),
                      (stage >= 3 ? 0.52f : 0.46f) *
                          (0.90f + (variantNext(rs) % 25) / 100.0f) * sc,
                      sgn * (28 + (int)(variantNext(rs) % 15)),
                      fingers, tint);
    if (stage >= 3) {
      float bs = (stage == 4 ? 1.05f : 0.75f) *
                 (0.85f + (variantNext(rs) % 30) / 100.0f);
      drawArtBud(zX(tipX), zY(tipY - 2) - HEADER_H, bs * sc, stage == 4 ? 6 : 4);
      if (stage == 4)
        drawArtBud(zX(tipX - sgn * 3), zY(tipY + 3) - HEADER_H, bs * 0.75f * sc, 5);
    }
  }

  // Crown just under the stem tip: staggered leaves, never mirrored.
  if (stage == 0) {
    drawFanLeafTinted(zX(cx + lean), zY(topY), 0.45f * sc,
                      (int)(variantNext(rs) % 13) - 6, 3, tint);
  } else {
    drawFanLeafTinted(zX(cx + lean - 5), zY(topY + 5), 0.50f * sc,
                      -38 + (int)(variantNext(rs) % 9), fingers, tint);
    drawFanLeafTinted(zX(cx + lean + 5), zY(topY + 7), 0.50f * sc,
                      38 - (int)(variantNext(rs) % 9), fingers, tint);
    drawFanLeafTinted(zX(cx + lean), zY(topY), 0.58f * sc,
                      (int)(variantNext(rs) % 9) - 4, fingers, tint);
  }
  if (stage >= 3) {
    // Dominant main cola: stacked buds tapering up the stem tip.
    const int mj = (int)(variantNext(rs) % 3);
    drawArtBud(zX(cx + lean), zY(topY + 8 + mj) - HEADER_H,
               (stage == 4 ? 1.45f : 1.0f) * sc, 7);
    drawArtBud(zX(cx + lean - 1), zY(topY + 2 + mj) - HEADER_H,
               (stage == 4 ? 1.20f : 0.85f) * sc, 6);
    drawArtBud(zX(cx + lean + 1), zY(topY - 4 + mj) - HEADER_H,
               (stage == 4 ? 0.95f : 0.70f) * sc, 6);
  }
  if (stage == 4) {
    // White pistil flecks scattered across the whole canopy.
    static const int8_t sp[16][2] = {{16, 66}, {30, 66}, {12, 78}, {36, 78},
                                     {20, 58}, {27, 58}, {16, 90}, {31, 90},
                                     {12, 102}, {35, 102}, {18, 72}, {29, 72},
                                     {14, 84}, {33, 84}, {20, 96}, {27, 96}};
    for (int i = 0; i < 16; ++i) {
      game.drawPixel(zX(cx - 23 + sp[i][0]), zY(sp[i][1]),
                     rgb565(224, 220, 183));
      game.drawPixel(zX(cx - 23 + sp[i][0]) + 1, zY(sp[i][1]) - 1,
                     rgb565(205, 201, 157));
    }
  }
}

void drawArtPlant(int cx, int stage) {   // small garden: 1:1, identical view
  drawArtPlantT(cx, stage, cx, 129 + HEADER_H, 1.0f, true);
}

// Fullscreen room: the same seeded phenos, magnified around their soil line.
void drawArtPlantZoomed(int cx, int stage) {
  if (!plantActive) {                    // empty room: bare pots only
    zRefCx = cx; zX0 = (int)(cx * 1.78f); zSoilY = 205; zSc = 1.78f;
    zPot(cx + 1);
    return;
  }
  drawArtPlantT(cx, stage, (int)(cx * 1.78f), 205, 1.78f, true);
}







void drawMiniRoomPlant(int cx, int potBase, float scale) {
  uint16_t potDark = rgb565(92, 49, 34);
  uint16_t potMid = rgb565(164, 86, 55);
  uint16_t potLight = rgb565(203, 126, 78);
  int potW = (int)(25 * scale);
  int potH = (int)(15 * scale);
  game.fillRoundRect(cx - potW / 2, potBase - potH, potW, potH, 3, potDark);
  game.fillRoundRect(cx - potW / 2 + 2, potBase - potH, potW - 4, potH - 2, 2, potMid);
  game.drawFastVLine(cx - potW / 3, potBase - potH + 2, potH - 5, potLight);
  game.fillRoundRect(cx - potW / 2 - 2, potBase - potH - 3, potW + 4, 5, 2, potLight);
  if (!plantActive) return;

  int stage = stageForGrowth();
  int top = potBase - potH - (int)((20 + growth * 0.42f) * scale);
  game.drawLine(cx, potBase - potH, cx, top + 4, rgb565(35, 104, 48));
  if (stage >= 1) {
    int y = potBase - potH - (int)(15 * scale);
    drawFanLeaf(cx - (int)(8 * scale), y, scale * 0.58f, -35);
    drawFanLeaf(cx + (int)(8 * scale), y, scale * 0.58f, 35);
  }
  if (stage >= 2) {
    int y = potBase - potH - (int)(26 * scale);
    drawFanLeaf(cx - (int)(7 * scale), y, scale * 0.66f, -34);
    drawFanLeaf(cx + (int)(7 * scale), y, scale * 0.66f, 34);
  }
  drawFanLeaf(cx, top + 4, scale * 0.72f, 0);

  // Repaint the front of the pot after its foliage so the stalk visibly meets
  // the soil instead of sitting on top of the pot wall.
  game.fillRoundRect(cx - potW / 2, potBase - potH, potW, potH, 3, potDark);
  game.fillRoundRect(cx - potW / 2 + 2, potBase - potH, potW - 4, potH - 2, 2, potMid);
  game.drawFastVLine(cx - potW / 3, potBase - potH + 2, potH - 5, potLight);
  game.fillRoundRect(cx - potW / 2 - 2, potBase - potH - 3, potW + 4, 5, 2, potLight);
  game.fillRoundRect(cx - potW / 2 + 4, potBase - potH - 1, potW - 8, 3, 2,
                    rgb565(67, 44, 30));
  game.drawFastVLine(cx, potBase - potH - 3, 6, rgb565(35, 104, 48));
  game.drawPixel(cx + 1, potBase - potH - 1, rgb565(111, 163, 78));
}

void drawFullScreenGardenFallback() {
  for (int y = 0; y < SCREEN_H; ++y) {
    float blend = (float)y / SCREEN_H;
    uint8_t r = 5 + (uint8_t)(blend * 10);
    uint8_t g = 19 + (uint8_t)(blend * 30);
    uint8_t b = 21 + (uint8_t)(blend * 20);
    game.drawBandHLine(0, y, SCREEN_W, rgb565(r, g, b));
  }
  game.drawFastVLine(13, 0, SCREEN_H - 14, rgb565(29, 53, 51));
  game.drawFastVLine(306, 0, SCREEN_H - 14, rgb565(29, 53, 51));
  game.drawFastHLine(13, 53, 294, rgb565(23, 44, 42));
  game.drawFastHLine(13, 106, 294, rgb565(23, 44, 42));
  // Sage trellis wires carried over from the small garden view, with the
  // posts between them, so the zoomed room keeps its structure.
  static const int wires[4] = {60, 92, 124, 156};
  for (int i = 0; i < 4; ++i)
    game.drawFastHLine(13, wires[i], 294, rgb565(99, 116, 97));
  for (int i = 0; i < 5; ++i)
    game.drawFastVLine(64 + i * 48, 53, 110, rgb565(43, 66, 58));
  game.fillRect(0, 224, SCREEN_W, 8, rgb565(32, 49, 39));
  game.drawFastHLine(0, 224, SCREEN_W, rgb565(76, 91, 61));
  // The same seeded phenos as the small garden, magnified.
  const int stage = stageForGrowth();
  drawArtPlantZoomed(24, stage);
  drawArtPlantZoomed(68, stage);
  drawArtPlantZoomed(112, stage);
  drawArtPlantZoomed(156, stage);
}

void restoreFullScreenFallbackRegion() {
  const int left = 178;
  const int right = SCREEN_W;
  for (int y = 80; y < 210; ++y) {
    float blend = (float)y / SCREEN_H;
    uint8_t r = 5 + (uint8_t)(blend * 10);
    uint8_t g = 19 + (uint8_t)(blend * 30);
    uint8_t b = 21 + (uint8_t)(blend * 20);
    game.drawBandHLine(left, y, right - left, rgb565(r, g, b));
  }
  game.drawFastVLine(306, 80, 130, rgb565(29, 53, 51));
  game.drawFastHLine(left, 106, right - left, rgb565(23, 44, 42));
  // Trellis wires + posts continue through the restored band so the farmer
  // overlay repair never erases the structure.
  static const int rw[3] = {92, 124, 156};
  for (int i = 0; i < 3; ++i)
    game.drawFastHLine(left, rw[i], right - left, rgb565(99, 116, 97));
  for (int i = 0; i < 2; ++i)
    game.drawFastVLine(208 + i * 48, 80, 83, rgb565(43, 66, 58));
  {
    const int stage = stageForGrowth();
    drawArtPlantZoomed(112, stage);   // same phenos, redrawn over the overlay
    drawArtPlantZoomed(156, stage);
  }
}

void drawPlantFullScreenPage() {
  drawFullScreenGardenFallback();  // vector room: trellis + mini plants
  drawButton(264, 5, 48, 23, "CLOSE", ACCENT_DARK);
  game.fillRoundRect(87, 219, 146, 17, 4, 0x0861);
  game.setTextColor(TEXT_MAIN, 0x0861);
  game.drawCentreString("TAP ANYWHERE TO RETURN", 160, 223, 1);
  lastDrawnStage = stageForGrowth();
}

uint8_t plantedPotCount() {
  uint8_t count = 0;
  for (int i = 0; i < PLANTS_PER_ROOM; ++i) {
    if (plantedPotMask & (1 << i)) ++count;
  }
  return count;
}

void drawPlantingPage() {
  game.fillScreen(BG);
  game.setTextColor(TEXT_MAIN, BG);
  game.drawCentreString("START YOUR GARDEN", 160, 12, 2);
  game.setTextColor(TEXT_DIM, BG);
  game.drawCentreString("Tap each seed; it plants in the next open pot", 160, 34, 1);
  // Strain picker: shown while the batch is untouched and you hold seeds of
  // more than one strain — the whole room grows the picked strain.
  if (plantedPotCount() == 0 && countSeededStrains() > 1) {
    game.fillRoundRect(58, 56, 204, 22, 5, PANEL);
    game.drawRoundRect(58, 56, 204, 22, 5, PANEL_LIGHT);
    game.setTextColor(ACCENT, PANEL);
    game.drawCentreString(strainNames[plantStrain], 160, 62, 1);
    drawButton(8, 57, 42, 20, "<", PANEL_LIGHT);
    drawButton(270, 57, 42, 20, ">", PANEL_LIGHT);
  }
  for (int i = 0; i < PLANTS_PER_ROOM; ++i) drawPlantingPot(i);
  drawPlantingTray();
}

void drawPlantingPot(int index) {
  const int potX[] = {48, 122, 198, 272};
  uint16_t potDark = rgb565(91, 49, 33);
  uint16_t potMid = rgb565(157, 82, 49);
  uint16_t potLight = rgb565(205, 125, 76);
  uint16_t soil = rgb565(67, 44, 30);
  int cx = potX[index];
  bool planted = plantedPotMask & (1 << index);
  game.fillRect(cx - 24, 115, 48, 56, BG);
  game.fillRoundRect(cx - 18, 125, 36, 39, 5, potDark);
  game.fillRoundRect(cx - 15, 127, 30, 34, 4, potMid);
  game.drawFastVLine(cx - 10, 131, 26, potLight);
  game.fillRoundRect(cx - 22, 120, 44, 9, 4, potDark);
  game.fillRoundRect(cx - 19, 121, 38, 5, 3, potMid);
  game.fillRoundRect(cx - 16, 122, 32, 3, 2, soil);
  if (planted) {
    game.drawFastVLine(cx, 116, 8, rgb565(35, 104, 48));
    drawFanLeaf(cx, 116, 0.28f, 0);
  } else {
    game.drawCircle(cx, 124, 2, rgb565(125, 87, 55));
  }
  char label[12];
  snprintf(label, sizeof(label), "POT %u", (unsigned)(index + 1));
  game.fillRect(cx - 25, 165, 50, 12, BG);
  game.setTextColor(planted ? ACCENT : TEXT_DIM, BG);
  game.drawCentreString(planted ? "PLANTED" : label, cx, 168, 1);
}

void drawPlantingTray() {
  game.fillRect(10, 184, 300, 52, BG);
  game.fillRoundRect(13, 187, 294, 39, 7, PANEL);
  game.drawRoundRect(13, 187, 294, 39, 7, PANEL_LIGHT);
  uint8_t remaining = min((int)(PLANTS_PER_ROOM - plantedPotCount()), (int)seedsByStrain[plantStrain]);
  const int seedX[] = {43, 121, 199, 277};
  for (int i = 0; i < remaining; ++i) {
    game.fillCircle(seedX[i], 205, 7, rgb565(152, 111, 67));
    game.drawPixel(seedX[i] - 2, 202, rgb565(222, 191, 126));
  }
  char progress[24];
  snprintf(progress, sizeof(progress), "%u OF 4 PLANTED", plantedPotCount());
  game.setTextColor(GOLD, BG);
  game.drawCentreString(progress, 160, 229, 1);
}

void plantSeedFromTap(int x, int y) {
  if (y < 188 || y > 228 || plantedPotCount() >= PLANTS_PER_ROOM || seedsByStrain[plantStrain] == 0) return;
  const int seedX[] = {43, 121, 199, 277};
  uint8_t remaining = min((int)(PLANTS_PER_ROOM - plantedPotCount()), (int)seedsByStrain[plantStrain]);
  bool tappedSeed = false;
  for (int i = 0; i < remaining; ++i) {
    if (abs(x - seedX[i]) <= 18) {
      tappedSeed = true;
      break;
    }
  }
  if (!tappedSeed) return;

  int plantedPot = -1;
  for (int pot = 0; pot < PLANTS_PER_ROOM; ++pot) {
    if ((plantedPotMask & (1 << pot)) == 0) {
      plantedPotMask |= (1 << pot);
      --seedsByStrain[plantStrain];
      plantedPot = pot;
      break;
    }
  }
  if (plantedPotMask == 0x0F) {
    firstPlantingDone = true;
    plantActive = true;
    plantVariant = (uint16_t)random(65536);  // every grow grows differently
    activeStrain = plantStrain;
    growth = 0;
    hydration = 65;
    vigor = 80;
    pestType = 0;
    pestSeverity = 0;
    resetDefoliationForNewPlant();
    page = pageAfterPlanting;
  }
  saveGame();
  if (page == PLANTING) {
    if (plantedPot >= 0) drawPlantingPot(plantedPot);
    drawPlantingTray();
    presentFrame();
  } else render();
}

void drawPlant() {
  drawGardenBackdrop();
  if (!plantActive) {
    // Empty room: four bare pots and the invite to plant.
    drawArtPot(24);
    drawArtPot(68);
    drawArtPot(112);
    drawArtPot(156);
    game.setTextColor(TEXT_DIM, rgb565(12, 33, 35));
    game.drawCentreString("Plant a seed to begin", 89, 91, 1);
    return;
  }
  // Vector fallback matches the SD art: four pots, all at the growth stage.
  int stage = stageForGrowth();
  drawArtPlant(23, stage);
  drawArtPlant(67, stage);
  drawArtPlant(111, stage);
  drawArtPlant(155, stage);
}

void drawGardenSidePanel() {
  overlayDirty = true;   // repaints the region the farmer overlay sits over
  game.fillRect(178, HEADER_H, 142, 144, BG);
  game.setTextColor(TEXT_MAIN, BG);
  game.drawString(stageName(), 188, 36, 2);
  char dayText[18];
  snprintf(dayText, sizeof(dayText), "DAY %02u", (unsigned)(1 + gameMinutes / 20));
  game.setTextColor(TEXT_DIM, BG);
  game.drawString(dayText, 188, 54, 1);
  game.setTextColor(TEXT_DIM, BG);
  game.drawRightString(currentStrainName(), 312, 60, 1);
  drawStatBar(188, 69, "GROWTH", plantActive ? growth : 0, ACCENT);
  drawStatBar(188, 101, "HYDRATION", plantActive ? hydration : 0, 0x3DDF);
  drawStatBar(188, 133, "VIGOR", plantActive ? vigor : 0, GOLD);

  game.fillRoundRect(186, 157, 132, 13, 4, pestType ? 0x6000 : 0x10A3);
  game.setTextColor(pestType ? TFT_YELLOW : 0x9EFB, pestType ? 0x6000 : 0x10A3);
  if (pestType) {
    const char *pestNames[] = {"", "MITES", "APHIDS", "GNATS"};
    char pestText[22];
    snprintf(pestText, sizeof(pestText), "%s  %u%%", pestNames[pestType], pestSeverity);
    game.drawCentreString(pestText, 252, 159, 1);
  } else {
    game.drawCentreString("NO PESTS", 252, 159, 1);
  }
}

void drawGardenZoomChip() {
  game.fillRoundRect(7, 31, 70, 13, 4, 0x0861);
  game.setTextColor(TEXT_DIM, 0x0861);
  game.drawCentreString("TAP TO ZOOM", 42, 34, 1);
}

// Centered dying-plant modal, shown once per crisis on any page. Tap
// dismisses and jumps to the garden where the plant can be treated.
void drawDeathWarnPopup() {
  if (!plantActive) { deathWarnModal = false; return; }
  game.fillRoundRect(30, 86, 260, 96, 10, 0xA000);
  game.drawRoundRect(30, 86, 260, 96, 10, TFT_YELLOW);
  game.setTextColor(TFT_YELLOW, 0xA000);
  if (hydration < 20) {
    game.drawCentreString("YOUR PLANT IS DYING!", 160, 96, 2);
    game.setTextColor(GOLD, 0xA000);
    game.drawCentreString(waterPacks ? "USE A WATER PACK NOW" : "BUY WATER PACKS",
                        160, 120, 1);
  } else {
    game.drawCentreString("PEST CRISIS!", 160, 96, 2);
    game.setTextColor(GOLD, 0xA000);
    game.drawCentreString(pestDefenseKits ? "USE PEST DEFENSE NOW" : "BUY PEST DEFENSE",
                        160, 120, 1);
  }
  game.setTextColor(TFT_WHITE, 0xA000);
  game.drawCentreString("TAP TO GO HELP YOUR PLANT", 160, 140, 1);
}

void drawGardenPage() {
  drawPlant();
  drawGardenZoomChip();
  drawGardenSidePanel();
  drawGardenActions();
  lastDrawnStage = stageForGrowth();
}

void drawGardenActions() {
  overlayDirty = true;
  game.fillRect(0, ACTION_Y, SCREEN_W, ACTION_H, BG);
  if (plantActive) {
    // Green = good to use, red = using it now can kill the plant,
    // gray = no supplies / not the right time.
    const uint16_t BTN_GREEN = 0x2C64;
    const uint16_t BTN_RED = 0xA000;
    // While growing, the three buttons stretch across the full row.
    // When HARVEST is available it takes the fourth slot again.
    bool harvestReady = growth >= 100;
    int waterX = 3,   waterW = harvestReady ? 76 : 103;
    int defolX = harvestReady ? 82 : 109,  defolW = harvestReady ? 76 : 102;
    int treatX = harvestReady ? 161 : 214, treatW = harvestReady ? 76 : 103;
    if (waterPacks == 0) drawButton(waterX, ACTION_Y + 1, waterW, 26, "WATER", PANEL_LIGHT, TEXT_DIM);
    else drawButton(waterX, ACTION_Y + 1, waterW, 26, "WATER", wateringWouldDrown() ? BTN_RED : BTN_GREEN, TFT_WHITE);
    uint8_t defState = defoliateButtonState();
    if (defState == 0) drawButton(defolX, ACTION_Y + 1, defolW, 26, "DEFOLIATE", PANEL_LIGHT, TEXT_DIM);
    else drawButton(defolX, ACTION_Y + 1, defolW, 26, "DEFOLIATE", defState == 2 ? BTN_RED : BTN_GREEN, TFT_WHITE);
    drawButton(treatX, ACTION_Y + 1, treatW, 26, "TREAT", pestType ? 0x6800 : PANEL);
    if (growth >= 100) {
      drawButton(240, ACTION_Y + 1, 77, 26, "HARVEST", ACCENT_DARK);
    }
  } else {
    drawButton(80, ACTION_Y + 1, 160, 26, totalSeeds() >= PLANTS_PER_ROOM ? "PLANT ROOM" : "BUY SEEDS", ACCENT_DARK);
  }
}

void drawPageTitle(const char *title, const char *subtitle) {
  game.setTextColor(TEXT_MAIN, BG);
  game.drawString(title, 12, 40, 2);
  game.setTextColor(TEXT_DIM, BG);
  game.drawString(subtitle, 12, 60, 1);
}


// Big-card grid style shared with the seed catalog: the whole card is the
// tap target, an ACCENT border means "affordable right now".
void drawSupplyCard(int x, int y, const char *label, const char *value, bool affordable) {
  uint16_t fill = PANEL;
  game.fillRoundRect(x, y, 152, 40, 5, fill);
  game.drawRoundRect(x, y, 152, 40, 5, affordable ? ACCENT : PANEL_LIGHT);
  game.setTextColor(affordable ? TEXT_MAIN : TEXT_DIM, fill);
  game.drawCentreString(label, x + 76, y + 4, 2);
  game.setTextColor(affordable ? GOLD : TEXT_DIM, fill);
  game.drawCentreString(value, x + 76, y + 25, 1);
}

// Card for the send-supplies grid: same look as the supply shop cards.
void drawSendCard(int x, int y, int w, int h, const char *label,
                  const char *value, bool enabled) {
  game.fillRoundRect(x, y, w, h, 5, PANEL);
  game.drawRoundRect(x, y, w, h, 5, enabled ? ACCENT : PANEL_LIGHT);
  game.setTextColor(enabled ? TEXT_MAIN : TEXT_DIM, PANEL);
  game.drawCentreString(label, x + w / 2, y + h / 2 - 12, 1);
  game.setTextColor(enabled ? GOLD : TEXT_DIM, PANEL);
  game.drawCentreString(value, x + w / 2, y + h / 2 + 2, 1);
}

void drawSuppliesPage() {
  drawPageTitle("SUPPLY SHOP", "Tap a card to buy or open");
  char owned[24];
  snprintf(owned, sizeof(owned), "x%u owned - $5", waterPacks);
  drawSupplyCard(5, 72, "WATER +3", owned, credits >= 5);
  snprintf(owned, sizeof(owned), "x%u owned - $7", careKits);
  drawSupplyCard(163, 72, "CARE KITS +3", owned, credits >= 7);
  drawSupplyCard(5, 114, "CHOOSE A SEED", "STRAIN CATALOG", true);
  snprintf(owned, sizeof(owned), "x%u owned - $10", pestDefenseKits);
  drawSupplyCard(163, 114, "PEST DEF +2", owned, credits >= 10);
  snprintf(owned, sizeof(owned), "x%u owned - $12", remediationKits);
  drawSupplyCard(5, 156, "REMEDIATE +2", owned, credits >= 12);
  drawSupplyCard(163, 156, "HASH ROOM",
                 hashRoomUnlocked ? "OPEN THE ROOM" : "$150",
                 hashRoomUnlocked || credits >= HASH_ROOM_COST);
}

// Seed catalog as a 5x2 grid of big strain cards — the whole card is the tap
// target (buySeeds validates unlock/credits/stock), so there are no tiny
// BUY buttons to miss.
// Seed catalog as a picker like the grower select: one giant strain card
// cycled with < and > arrows; BUY grabs a 4-pack of that strain.
uint8_t seedShopCursor = 0;   // position across the whole catalog (0..15)

void drawSeedShopPage() {
  char subtitle[40];
  uint8_t harvestedCount = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    if (harvestedStrainMask & ((uint16_t)1 << i)) ++harvestedCount;
  }
  snprintf(subtitle, sizeof(subtitle), "Harvested %u/%u", harvestedCount, STRAIN_COUNT);
  drawPageTitle("SEED CATALOG", subtitle);
  uint8_t i = strainCatalogOrder[seedShopCursor];
  bool locked = seedShopCursor > strainUnlockedThrough;
  game.fillRoundRect(58, 72, 204, 110, 10, PANEL);
  game.drawRoundRect(58, 72, 204, 110, 10, locked ? 0x3187
                     : seedsByStrain[i] ? ACCENT : PANEL_LIGHT);
  // Locked cards stay fully readable, just grayed out.
  int nameFont = strlen(strainNames[i]) > 14 ? 1 : 2;
  game.setTextColor(locked ? TEXT_DIM : TEXT_MAIN, PANEL);
  game.drawCentreString(strainNames[i], 160, nameFont == 2 ? 80 : 83, nameFont);
  char line[40];
  snprintf(line, sizeof(line), "%s - %s", strainType[i], strainDifficulty[i]);
  game.setTextColor(locked ? TEXT_DIM : GOLD, PANEL);
  game.drawCentreString(line, 160, 102, 1);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString(strainNotes[i], 160, 114, 1);
  snprintf(line, sizeof(line), "$%u per 4-pack", strainPrices[i]);
  game.drawCentreString(line, 160, 130, 1);
  if (locked) {
    // Gray LOCKED tag where the seed bubble sits on unlocked cards.
    game.fillRoundRect(186, 76, 68, 16, 8, 0x3187);
    game.setTextColor(TEXT_DIM, 0x3187);
    game.drawCentreString("LOCKED", 220, 79, 1);
    game.setTextColor(TEXT_DIM, PANEL);
    game.drawCentreString("Harvest your way up to unlock", 160, 150, 1);
  } else {
    // Owned-seed pill: every unlocked card badges how many packs you hold.
    game.fillRoundRect(202, 146, 56, 20, 8, seedsByStrain[i] ? GOLD : PANEL_LIGHT);
    game.setTextColor(seedsByStrain[i] ? TFT_BLACK : TEXT_DIM,
                      seedsByStrain[i] ? GOLD : PANEL_LIGHT);
    snprintf(line, sizeof(line), "x%u", (seedsByStrain[i] + 3) / 4);   // 4-packs
    game.drawCentreString(line, 230, 149, 2);
    game.setTextColor(TEXT_DIM, PANEL);
    game.drawCentreString(seedsByStrain[i] ? "BUY to restock" : "BUY to start",
                          160, 150, 1);
  }
  drawButton(10, 96, 42, 52, "<", ACCENT_DARK);
  drawButton(268, 96, 42, 52, ">", ACCENT_DARK);
  snprintf(line, sizeof(line), "BUY 4-PACK - $%u", strainPrices[i]);
  drawButton(10, 176, 300, 28, line, locked ? PANEL_LIGHT : ACCENT_DARK,
             locked ? TEXT_DIM : TFT_WHITE);
}
void drawMarketCard(int y, const char *title, uint16_t quantity, const char *price,
                    bool enabled) {
  game.fillRoundRect(10, y, 300, 52, 6, PANEL);
  game.drawRoundRect(10, y, 300, 52, 6, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawString(title, 19, y + 6, 2);
  char amount[18];
  snprintf(amount, sizeof(amount), "x%u", quantity);
  game.setTextColor(GOLD, PANEL);
  game.drawString(amount, 19, y + 28, 1);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawString(price, 54, y + 28, 1);
  drawButton(164, y + 12, 62, 29, "SELL 1", enabled ? ACCENT_DARK : PANEL_LIGHT,
             enabled ? TFT_WHITE : TEXT_DIM);
  drawButton(234, y + 12, 68, 29, "SELL ALL", enabled ? ACCENT_DARK : PANEL_LIGHT,
             enabled ? TFT_WHITE : TEXT_DIM);
}

// Big-card grid like the seed catalog: the whole card is the tap target.
void drawMarketCard(int x, int y, const char *label, const char *value, bool enabled) {
  uint16_t fill = PANEL;
  game.fillRoundRect(x, y, 152, 38, 5, fill);
  game.drawRoundRect(x, y, 152, 38, 5, enabled ? ACCENT : PANEL_LIGHT);
  game.setTextColor(enabled ? TEXT_MAIN : TEXT_DIM, fill);
  game.drawCentreString(label, x + 76, y + 4, 2);
  game.setTextColor(enabled ? GOLD : TEXT_DIM, fill);
  game.drawCentreString(value, x + 76, y + 23, 1);
}

void drawMarketPage() {
  drawPageTitle("VIRTUAL MARKET",
                marketGearMode ? "Tap a card to buy gear"
                               : "Tap a sell card, pick a strain, then confirm");
  drawButton(12, 70, 144, 28, "SELL", marketGearMode ? PANEL_LIGHT : ACCENT_DARK);
  drawButton(164, 70, 144, 28, "GEAR", marketGearMode ? ACCENT_DARK : PANEL_LIGHT);
  char units[24];
  if (!marketGearMode) {
    // Sell cards open a strain submenu (big tappable rows); the submenu asks
    // ARE YOU SURE? before anything is actually sold.
    char flowerPrice[24];
    stashPriceText(flowerPrice, sizeof(flowerPrice));
    snprintf(units, sizeof(units), "x%u - %s", stashTotal(), flowerPrice);
    drawMarketCard(5, 102, "SELL 1 FLOWER", units, stashTotal() > 0);
    snprintf(units, sizeof(units), "x%u - %s", stashTotal(), flowerPrice);
    drawMarketCard(163, 102, "SELL ALL FLOWER", units, stashTotal() > 0);
    char hashPrice[24];
    hashPriceText(hashPrice, sizeof(hashPrice));
    snprintf(units, sizeof(units), "x%u - %s", hashTotal(), hashPrice);
    drawMarketCard(5, 144, "SELL 1 HASH", units, hashRoomUnlocked && hashTotal() > 0);
    snprintf(units, sizeof(units), "x%u - %s", hashTotal(), hashPrice);
    drawMarketCard(163, 144, "SELL ALL HASH", units, hashRoomUnlocked && hashTotal() > 0);
    if (marketSellMode) drawSellStrainModal();
    return;
  }

  drawMarketCard(5, 102, "BONG", bongs ? "x1 OWNED" : "$24", !bongs && credits >= 24);
  snprintf(units, sizeof(units), "x%u owned - $6", lighters);
  drawMarketCard(163, 102, "LIGHTERS +3", units, credits >= 6);
  snprintf(units, sizeof(units), "x%u owned - $5", rollingPapers);
  drawMarketCard(5, 144, "PAPERS +5", units, credits >= 5);
  snprintf(units, sizeof(units), "%u%%", stonedLevel);
  drawMarketCard(163, 144, "HIGH METER", units, false);
}

void drawStashPage() {
  drawPageTitle("STASH", "Your flower, hash and jars");
  // Product stats only - supplies and gear counts live on their buy cards.
  const char *labels[] = {"Untrimmed flowers", "Trimmed flowers", "Hash units", "Active plants"};
  uint16_t values[] = {untrimmedUnits, stashTotal(), hashTotal(), plantActive ? PLANTS_PER_ROOM : 0};
  for (int i = 0; i < 4; ++i) {
    int y = 64 + i * 11;
    game.setTextColor(TEXT_DIM, BG);
    game.drawString(labels[i], 18, y, 1);
    char value[12];
    snprintf(value, sizeof(value), "%u", values[i]);
    game.setTextColor(i == 1 || i == 2 ? GOLD : TEXT_MAIN, BG);
    game.drawRightString(value, 302, y, 1);
    game.drawFastHLine(16, y + 10, 288, 0x3187);
  }
  // Jars and hash by strain: flower jars + hash units share each row.
  game.setTextColor(TEXT_DIM, BG);
  game.drawString("STASH BY STRAIN - JARS / HASH", 18, 112, 1);
  uint8_t stocked = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (stashByStrain[i] || hashByStrain[i]) ++stocked;
  if (stocked > 4) {
    char more[16];
    snprintf(more, sizeof(more), "+%u more", stocked - 4);
    game.drawRightString(more, 302, 112, 1);
  }
  int shown = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    if (!stashByStrain[i] && !hashByStrain[i]) continue;
    if (shown >= 4) continue;
    int y = 124 + shown * 12;
    game.setTextColor(TEXT_MAIN, BG);
    game.drawString(strainNames[i], 18, y, 1);
    char row[44];
    if (stashByStrain[i] && hashByStrain[i])
      snprintf(row, sizeof(row), "x%u %s | x%u h%s", stashByStrain[i],
               gradeName(sellGradeForQuality(stashQualityByStrain[i])),
               hashByStrain[i], gradeName(sellGradeForQuality(hashQualityByStrain[i])));
    else if (stashByStrain[i])
      snprintf(row, sizeof(row), "x%u %s", stashByStrain[i],
               gradeName(sellGradeForQuality(stashQualityByStrain[i])));
    else
      snprintf(row, sizeof(row), "x%u h%s", hashByStrain[i],
               gradeName(sellGradeForQuality(hashQualityByStrain[i])));
    game.setTextColor(GOLD, BG);
    game.drawRightString(row, 302, y, 1);
    game.drawFastHLine(16, y + 10, 288, 0x3187);
    ++shown;
  }
  if (shown == 0) {
    game.setTextColor(TEXT_DIM, BG);
    game.drawCentreString("Nothing stashed - trim flowers or press hash", 160, 136, 1);
  }
  if (untrimmedUnits > 0) {
    drawButton(5, 174, 152, 28, "TRIM FLOWERS", ACCENT_DARK);
  } else {
    drawButton(5, 174, 152, 28, "TRIM FLOWERS", PANEL_LIGHT, TEXT_DIM);
  }
  drawButton(163, 174, 152, 28, "SMOKE BREAK", ACCENT_DARK);
  if (smokeMenuOpen) drawSmokeBreakMenu();   // chooser covers the jar list
}
void drawSmokeBreakMenu() {
  game.fillRoundRect(5, 56, 310, 149, 8, PANEL);
  game.drawRoundRect(5, 56, 310, 149, 8, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("SMOKE BREAK", 160, 61, 2);
  const char *rows[3] = {"BONG + FLOWER + LIGHTER", "BONG + HASH", "PAPERS + FLOWER + LIGHTER"};
  bool ready[3] = {(bool)(bongs && stashTotal()),
                   (bool)(bongs && hashRoomUnlocked && hashTotal()),
                   (bool)(stashTotal() && lighters && rollingPapers)};
  if (smokeMenuConfirm) {
    game.drawCentreString("ARE YOU SURE?", 160, 88, 2);
    char detail[44];
    if (smokeMenuPick == 1)
      snprintf(detail, sizeof(detail), "Bong bowl of flower + 1 lighter");
    else if (smokeMenuPick == 2)
      snprintf(detail, sizeof(detail), "Bong bowl of hash");
    else
      snprintf(detail, sizeof(detail), "Rolled joint + lighter");
    game.drawCentreString(detail, 160, 116, 1);
    drawButton(5, 176, 150, 27, "BACK", PANEL_LIGHT);
    drawButton(163, 176, 152, 27, "YES, SMOKE IT", RED, TFT_WHITE);
    return;
  }
  for (uint8_t r = 0; r < 3; ++r) {
    int y = 78 + r * 32;
    game.fillRoundRect(11, y, 298, 28, 5, PANEL);
    game.drawRoundRect(11, y, 298, 28, 5, ready[r] ? ACCENT : PANEL_LIGHT);
    game.setTextColor(ready[r] ? TEXT_MAIN : TEXT_DIM, PANEL);
    game.drawCentreString(rows[r], 160, y + 8, 1);
  }
  drawButton(5, 176, 310, 27, "BACK", PANEL_LIGHT);   // full-width row
}


// Big tappable settings rows for the full-screen pages.

// Row with an inline value button (Music ON/OFF, inversion, ...).


// Row with an inline value button (Music ON/OFF, inversion, ...).

// Fixed title strip above the scrollable settings content.
void drawSettingsTitle() {
  const char *title = "OPTIONS";
  const char *subtitle = "Gear icon exits";
  game.setTextColor(TEXT_MAIN, BG);
  game.drawString(title, 12, 40, 2);
  game.setTextColor(TEXT_DIM, BG);
  game.drawString(subtitle, 12, 60, 1);
}

void drawGrowerIdCard() {
  // Grower ID tag: mini character card with name and all-time stats, sized
  // so the whole options hub fits on one screen without scrolling.
  game.fillRoundRect(10, 72, 300, 56, 8, PANEL);
  game.drawRoundRect(10, 72, 300, 56, 8, ACCENT_DARK);
  drawFarmerHeadLarge(34, 100, farmerCharacter);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawString(growerName, 54, 76, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawString(farmerCharacterName(farmerCharacter), 55, 94, 1);
  // All-time stats, right-aligned inside the card.
  uint8_t strainsFound = 0;
  for (uint16_t m = harvestedStrainMask; m; m >>= 1) strainsFound += m & 1;
  char statLine[28];
  game.setTextColor(GOLD, PANEL);
  snprintf(statLine, sizeof(statLine), "%u harvests", harvestCount);
  game.drawRightString(statLine, 302, 76, 1);
  game.setTextColor(TEXT_MAIN, PANEL);
  snprintf(statLine, sizeof(statLine), "L%u %s", currentMasteryLevel(),
           masteryNicknameForLevel(currentMasteryLevel()));
  game.drawRightString(statLine, 302, 88, 1);
  game.setTextColor(TEXT_DIM, PANEL);
  snprintf(statLine, sizeof(statLine), "%u/%u strains", strainsFound, STRAIN_COUNT);
  game.drawRightString(statLine, 302, 100, 1);
}

// ---- Long-stall audio guard ----------------------------------------------
// Modes that block the loop for seconds (OTA's WiFi connect/download, the SD
// installer's serial waits) starve the audio pump; an emptied DMA ring
// replays its last chunk, which the speaker renders as a glitchy stutter
// that repeats. Kill the voices and mute before entering such a mode, then
// let the garden groove come back on the way out.
static void audioPauseForStall() {
  pcmStream = PCM_NONE;
  chipToneOff();
  drumToneOff();
  activeSong = nullptr;
  drumPlaying = false;
  if (audioTx && musicVolume) { es8311Mute(true); ampEnable(false); }
}
static void audioResumeAfterStall() {
  audioLastVol = 255;  // make the next pump re-apply codec volume + amp
  if (musicVolume && !pcmStream) pcmStart(PCM_THEME);  // theme restarts clean
}

// ============ WiFi login + OTA updates from GitHub (v15) ============
// WIFI LOGIN opens the on-screen keyboard to type the network name and
// password (saved to NVS with the game save). CHECK UPDATES connects,
// compares this build tag with version.txt from the repo's latest release,
// and INSTALL flashes the matching .bin for this board, then reboots.
#define OTA_GFX game
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

static const char *OTA_RELEASE_BASE =
    "https://github.com/VYBN3D/WinFloridaGardenSim/releases/latest/download/";
static const char *OTA_BIN_NAME = "WinFloridaGardenSim_S3.ino.bin";

// ---- Brand lanes: CHECK UPDATES can install any sibling rebrand ----------
struct OtaBrand {
  const char *label;   // shown on the brand picker
  const char *base;    // releases/latest/download/ base URL
  const char *s3Bin;   // 4-inch S3 asset
  const char *s28Bin;  // 2.8-inch S3 asset
};
static const OtaBrand otaBrands[] = {
  {"WIN FLORIDA", "https://github.com/VYBN3D/WinFloridaGardenSim/releases/latest/download/",
   "WinFloridaGardenSim_S3.ino.bin", "WinFloridaGardenSim_S28.ino.bin"},
  {"VYBN", "https://github.com/VYBN3D/VYBNGardenSim/releases/latest/download/",
   "VYBNGardenSim_S3.ino.bin", "VYBNGardenSim_S28.ino.bin"},
};
static const uint8_t OTA_BRAND_COUNT =
    sizeof(otaBrands) / sizeof(otaBrands[0]);
static const uint8_t otaBrandHome = 0;   // this fork's own brand lane
uint8_t otaBrandIndex = 0;              // brand selected on the OTA page

static const char *otaSelectedBase() { return otaBrands[otaBrandIndex].base; }
static const char *otaSelectedBin() {
  const OtaBrand &b = otaBrands[otaBrandIndex];
  return strcmp(FW_BOARD, "S28") == 0 ? b.s28Bin : b.s3Bin;
}
// Installing a different brand is always offered; the home brand only offers
// INSTALL once its version.txt differs from this build.
static bool otaInstallable() {
  return otaLatest[0] && (otaBrandIndex != otaBrandHome ||
                          strcmp(otaLatest, FW_VERSION) != 0);
}

static bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

static void otaRestoreRadio() {
  // Hand the radio back the way the garden likes it: fully log off WiFi
  // (updates connect on demand, they never stay associated).
  WiFi.disconnect();
  if (nearbyReady) {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_now_init();                    // re-arm Nearby after the WiFi session
  } else {
    WiFi.mode(WIFI_OFF);
  }
}
static void wifiRememberNetwork(const char *ssid, const char *pass) {
  for (uint8_t i = 0; i < wifiSavedCount; ++i)
    if (!strcmp(wifiSavedSsid[i], ssid)) {
      strlcpy(wifiSavedPass[i], pass, sizeof(wifiSavedPass[0]));
      return;
    }
  if (wifiSavedCount < 4) {
    strlcpy(wifiSavedSsid[wifiSavedCount], ssid, sizeof(wifiSavedSsid[0]));
    strlcpy(wifiSavedPass[wifiSavedCount], pass, sizeof(wifiSavedPass[0]));
    ++wifiSavedCount;
    return;
  }
  // List full: drop the oldest entry and append this one.
  for (uint8_t i = 0; i < 3; ++i) {
    strlcpy(wifiSavedSsid[i], wifiSavedSsid[i + 1], sizeof(wifiSavedSsid[0]));
    strlcpy(wifiSavedPass[i], wifiSavedPass[i + 1], sizeof(wifiSavedPass[0]));
  }
  strlcpy(wifiSavedSsid[3], ssid, sizeof(wifiSavedSsid[0]));
  strlcpy(wifiSavedPass[3], pass, sizeof(wifiSavedPass[0]));
}
static int wifiSavedIndex(const char *ssid) {
  for (uint8_t i = 0; i < wifiSavedCount; ++i)
    if (!strcmp(wifiSavedSsid[i], ssid)) return i;
  return -1;
}

static void otaConnectToSaved(uint16_t waitMs) {
  if (wifiConnected() || !wifiSsid[0]) return;
  audioPauseForStall();   // the connect wait blocks the pump for seconds
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  unsigned long start = millis();
  while (!wifiConnected() && millis() - start < waitMs) delay(100);
  audioResumeAfterStall();
}

static void otaCheckUpdate() {
  snprintf(otaStatus, sizeof(otaStatus), "Connecting WiFi ...");
  render();
  otaConnectToSaved(9000);
  if (!wifiConnected()) {
    snprintf(otaStatus, sizeof(otaStatus), "No WiFi - check name/password");
    otaRestoreRadio();
    return;
  }
  snprintf(otaStatus, sizeof(otaStatus), "Checking for updates ...");
  render();
  audioPauseForStall();    // the version GET can block the pump for seconds
  HTTPClient http;
  WiFiClientSecure tls;
  tls.setInsecure();                   // repo is public; no pinning needed
  tls.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(12000);
  if (!http.begin(tls, String(otaSelectedBase()) + "version.txt")) {
    snprintf(otaStatus, sizeof(otaStatus), "Update check failed");
  } else {
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      body.trim();
      strlcpy(otaLatest, body.c_str(), sizeof(otaLatest));
      if (otaBrandIndex == otaBrandHome && !strcmp(otaLatest, FW_VERSION))
        snprintf(otaStatus, sizeof(otaStatus), "Up to date (%s)", FW_VERSION);
      else
        snprintf(otaStatus, sizeof(otaStatus), "UPDATE READY: %s %s",
                 otaBrands[otaBrandIndex].label, otaLatest);
    } else {
      snprintf(otaStatus, sizeof(otaStatus), "Update check failed (HTTP %d)", code);
    }
    http.end();
  }
  audioResumeAfterStall();
  otaRestoreRadio();                   // release the radio back to Nearby
}

static void otaInstallUpdate() {
  otaBusy = true;                      // arm the disabled DOWNLOADING button
  snprintf(otaStatus, sizeof(otaStatus), "Connecting WiFi ...");
  render();
  otaConnectToSaved(9000);
  if (!wifiConnected()) {
    snprintf(otaStatus, sizeof(otaStatus), "No WiFi - check name/password");
    otaRestoreRadio();
    otaBusy = false;
    return;
  }
  snprintf(otaStatus, sizeof(otaStatus), "Downloading update ... KEEP STILL");
  render();
  audioPauseForStall();    // the bin download can block the pump for minutes
  HTTPClient http;
  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  if (!http.begin(tls, String(otaSelectedBase()) + otaSelectedBin())) {
    snprintf(otaStatus, sizeof(otaStatus), "Download failed");
    audioResumeAfterStall();
    otaBusy = false;
    return;
  }
  int code = http.GET();
  if (code != 200) {
    snprintf(otaStatus, sizeof(otaStatus), "Download failed (HTTP %d)", code);
    http.end();
    audioResumeAfterStall();
    otaBusy = false;
    return;
  }
  int size = http.getSize();
  if (size <= 0) {
    snprintf(otaStatus, sizeof(otaStatus), "Download failed (no size)");
    http.end();
    audioResumeAfterStall();
    otaBusy = false;
    return;
  }
  if (!Update.begin(size)) {
    snprintf(otaStatus, sizeof(otaStatus), "Not enough OTA space");
    http.end();
    audioResumeAfterStall();
    otaBusy = false;
    return;
  }
  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();
  if (written != (size_t)size) {
    snprintf(otaStatus, sizeof(otaStatus), "Transfer interrupted");
    Update.abort();
    audioResumeAfterStall();
    otaBusy = false;
    return;
  }
  if (!Update.end(true)) {
    snprintf(otaStatus, sizeof(otaStatus), "Flash verify failed");
    audioResumeAfterStall();
    otaBusy = false;
    return;
  }
  OTA_GFX.fillScreen(0x0861);
  OTA_GFX.setTextColor(TFT_WHITE, 0x0861);
  OTA_GFX.drawCentreString("UPDATE INSTALLED", 160, 100, 2);
  OTA_GFX.drawCentreString("REBOOTING", 160, 130, 2);
  presentFrame();
  delay(1200);
  ESP.restart();
}

static void drawOtaPage() {
  drawPageTitle("WIFI / UPDATES", "Gear icon exits");
  char line[48];
  // Small build line: no card, just tiny text to leave room for big buttons.
  OTA_GFX.setTextColor(TEXT_DIM, BG);
  snprintf(line, sizeof(line), "YOUR BUILD: %s - %s", FW_VERSION_FULL, FW_DATE);
  OTA_GFX.drawCentreString(line, 160, 72, 1);
  OTA_GFX.fillRoundRect(10, 82, 300, 22, 6, PANEL);
  OTA_GFX.drawRoundRect(10, 82, 300, 22, 6, PANEL_LIGHT);
  snprintf(line, sizeof(line), "WIFI: %s", wifiConnected() ? WiFi.SSID().c_str()
           : wifiSsid[0] ? wifiSsid : "NOT SET UP");
  OTA_GFX.setTextColor(TEXT_MAIN, PANEL);
  OTA_GFX.drawCentreString(line, 160, 88, 1);
  drawButton(5, 108, 152, 34, "WIFI LOGIN", PANEL_LIGHT);
  drawButton(163, 108, 152, 34, "CHECK UPDATES", PANEL_LIGHT);
  // Brand lane: pick which rebrand's releases CHECK UPDATES / INSTALL use.
  char brandLine[44];
  snprintf(brandLine, sizeof(brandLine), "GAME BRAND: %s - TAP TO SWITCH",
           otaBrands[otaBrandIndex].label);
  drawButton(5, 146, 310, 22, brandLine, ACCENT_DARK, TFT_WHITE);
  OTA_GFX.setTextColor(otaInstallable() ? GOLD : TEXT_DIM, BG);
  OTA_GFX.drawCentreString(otaStatus, 160, 172, 1);
  if (otaInstallable()) {
    if (otaBusy) {
      // Install running: the button dims and reads DOWNLOADING so a second
      // tap can't restart the flash.
      drawButton(5, 178, 310, 28, "DOWNLOADING ...", PANEL_LIGHT, TEXT_DIM);
    } else {
      snprintf(line, sizeof(line), "INSTALL %s %s AND REBOOT",
               otaBrands[otaBrandIndex].label, otaLatest);
      drawButton(5, 178, 310, 28, line, ACCENT_DARK, TFT_WHITE);
    }
  }
}


// Scan helpers: async scan, results deduped by SSID and sorted by signal.
static void wifiScanFill() {
  int found = WiFi.scanComplete();
  if (found < 0) return;
  int rssi[24], index[24];
  uint8_t kept = 0;
  for (int i = 0; i < found && kept < 24; ++i) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (uint8_t k = 0; k < kept; ++k)
      if (ssid == WiFi.SSID(index[k])) { dup = true; break; }
    if (dup) continue;
    rssi[kept] = WiFi.RSSI(i);
    index[kept] = i;
    ++kept;
  }
  for (uint8_t a = 1; a < kept; ++a)                      // strongest first
    for (uint8_t b = a; b > 0 && rssi[b] > rssi[b - 1]; --b) {
      int t = rssi[b]; rssi[b] = rssi[b - 1]; rssi[b - 1] = t;
      int t2 = index[b]; index[b] = index[b - 1]; index[b - 1] = t2;
    }
  for (uint8_t k = 0; k < kept && wifiScanCount < 12; ++k) {
    strlcpy(wifiScanSsid[wifiScanCount], WiFi.SSID(index[k]).c_str(), 33);
    wifiScanRssi[wifiScanCount++] = (int8_t)rssi[k];
  }
  WiFi.scanDelete();
  wifiScanPage = 0;
}

static void wifiScanStart() {
  wifiScanCount = 0;
  wifiScanPage = 0;
  wifiScanPending = true;
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, false, false, 120U);   // async, keeps the UI alive
}

// Repaint only the keyboard's input field so a key press doesn't redraw
// (and flicker) the whole page. Every keyboard shares this field geometry.
static void refreshKeyboardField() {
  game.fillRoundRect(14, 60, 292, 18, 4, PANEL);
  game.setTextColor(TEXT_MAIN, PANEL);
  if (page == WIFI_LOGIN) {
    char dots[65];
    size_t length = strlen(wifiPassEdit);
    memset(dots, '*', length);
    dots[length] = '\0';
    game.drawString(dots, 21, 65, 1);
  } else if (page == NAME_EDIT) {
    game.drawString(growerNameEdit, 21, 65, 1);
  } else if (page == CHEAT_CODE) {
    game.drawString(cheatEdit, 21, 65, 1);
  } else if (page == NEARBY && nearbyMode == 1) {
    game.drawString(outgoingMessage, 21, 65, 1);
  }
  presentFrame();
}

// The send-money keypad's input box.
static void refreshMoneyField() {
  game.fillRoundRect(14, 82, 292, 22, 4, PANEL);
  game.setTextColor(TEXT_MAIN, PANEL);
  char amount[10];
  snprintf(amount, sizeof(amount), "$%s", outgoingCash[0] ? outgoingCash : "0");
  game.drawString(amount, 21, 88, 1);
  presentFrame();
}

static void drawWifiLoginPage() {
  drawPageTitleSmall("WIFI LOGIN",
                wifiLoginStage == 0 ? "Pick a network (* = saved)" : "Type the WiFi password");
  if (wifiLoginStage == 0) {
    // Stage 0: live network list instead of typing the SSID.
    if (wifiScanPending) {
      OTA_GFX.setTextColor(TEXT_DIM, BG);
      OTA_GFX.drawCentreString("Scanning for networks...", 160, 100, 1);
    } else if (!wifiScanCount) {
      OTA_GFX.setTextColor(TEXT_DIM, BG);
      OTA_GFX.drawCentreString("No networks found", 160, 100, 1);
    }
    // Big tappable rows (button-scale): 4 per page, 24 px tall, font-2 text.
    for (uint8_t i = 0; i < 4; ++i) {
      uint8_t row = wifiScanPage + i;
      if (row >= wifiScanCount) break;
      int y = 60 + i * 24;
      OTA_GFX.setTextColor(TEXT_MAIN, BG);
      char name[25];
      snprintf(name, sizeof(name), "%.21s%s", wifiScanSsid[row],
               wifiSavedIndex(wifiScanSsid[row]) >= 0 ? " *" : "");
      OTA_GFX.drawString(name, 16, y + 2, 2);
      char meta[10];
      snprintf(meta, sizeof(meta), "%d", wifiScanRssi[row]);
      OTA_GFX.setTextColor(TEXT_DIM, BG);
      OTA_GFX.drawRightString(meta, 304, y + 6, 1);
      OTA_GFX.drawFastHLine(14, y + 22, 292, 0x3187);
    }
    if (wifiScanCount > 4) {
      OTA_GFX.setTextColor(TEXT_DIM, BG);
      uint8_t last = wifiScanPage + 4 > wifiScanCount ? wifiScanCount : wifiScanPage + 4;
      char more[20];
      snprintf(more, sizeof(more), "%u-%u of %u", wifiScanPage + 1, last, wifiScanCount);
      OTA_GFX.drawRightString(more, 304, 160, 1);
    }
    drawButton(7, 168, 72, 28, "RESCAN", PANEL_LIGHT);
    drawButton(85, 168, 72, 28, "<", PANEL_LIGHT);
    drawButton(163, 168, 72, 28, ">", PANEL_LIGHT);
    drawButton(241, 168, 72, 28, "CANCEL", PANEL_LIGHT);
    return;
  }
  // Stage 1: password keyboard (unchanged).
  OTA_GFX.fillRoundRect(14, 60, 292, 18, 4, PANEL);
  OTA_GFX.setTextColor(TEXT_MAIN, PANEL);
  char dots[65];
  size_t length = strlen(wifiPassEdit);
  memset(dots, '*', length);
  dots[length] = '\0';
  OTA_GFX.drawString(dots, 21, 65, 1);
  drawFullKeyboard(nameKbMode);
  drawButton(7, 168, 92, 28, "SPACE", PANEL_LIGHT);
  drawButton(111, 168, 98, 28, "CONNECT", ACCENT_DARK);
  drawButton(221, 168, 92, 28, "CANCEL", PANEL_LIGHT);
}

static void touchWifiLogin(int x, int y) {
  if (wifiLoginStage == 0) {
    // Stage 0: tap a network row to pick it, or the buttons below.
    if (y >= 60 && y < 156) {
      uint8_t row = wifiScanPage + (y - 60) / 24;
      if (row < wifiScanCount) {
        int saved = wifiSavedIndex(wifiScanSsid[row]);
        if (saved >= 0) {
          // Saved network: connect right away, no password needed.
          strlcpy(wifiSsid, wifiScanSsid[row], sizeof(wifiSsid));
          strlcpy(wifiPass, wifiSavedPass[saved], sizeof(wifiPass));
          snprintf(otaStatus, sizeof(otaStatus), "Connecting to %s ...", wifiSsid);
          page = OTA_PAGE;
          render();
          otaConnectToSaved(10000);
          if (!wifiConnected()) {
            // Password stopped working: fall back to typing it again.
            strlcpy(wifiSsidEdit, wifiSsid, sizeof(wifiSsidEdit));
            wifiPassEdit[0] = '\0';
            wifiLoginStage = 1;
            snprintf(otaStatus, sizeof(otaStatus),
                     "Password rejected - type it again");
            page = WIFI_LOGIN;
            otaRestoreRadio();
            render();
            return;
          }
          strlcpy(wifiSsidEdit, wifiSsid, sizeof(wifiSsidEdit));
          wifiPassEdit[0] = '\0';
          saveGame();                  // keep the last-used network on file
          snprintf(otaStatus, sizeof(otaStatus), "Connected to %s - logged off", wifiSsid);
          otaRestoreRadio();
          render();
          return;
        }
        strlcpy(wifiSsidEdit, wifiScanSsid[row], sizeof(wifiSsidEdit));
        wifiPassEdit[0] = '\0';
        wifiLoginStage = 1;            // password next
        render();
      }
      return;
    }
    if (y >= 168 && y < 196) {
      if (x < 80) {
        wifiScanStart();
        render();
      } else if (x < 160) {
        if (wifiScanPage) wifiScanPage -= 4;
        render();
      } else if (x < 238) {
        if (wifiScanPage + 4 < wifiScanCount) wifiScanPage += 4;
        render();
      } else {
        wifiScanPending = false;
        WiFi.scanDelete();
        otaRestoreRadio();
        page = OTA_PAGE;
        render();
      }
      return;
    }
    return;
  }
  // Stage 1: keyboard for the password.
  char key = fullKeyboardKey(x, y, nameKbMode);
  if (key) {
    size_t length = strlen(wifiPassEdit);
    if (length < sizeof(wifiPassEdit) - 1) { wifiPassEdit[length] = key; wifiPassEdit[length + 1] = '\0'; }
    refreshKeyboardField();
    fieldOnlyRedraw = true;
    return;
  }
  if (y >= 138 && y < 164 && x >= 263) {
    size_t length = strlen(wifiPassEdit);
    if (length) wifiPassEdit[length - 1] = '\0';
    refreshKeyboardField();
    fieldOnlyRedraw = true;
  } else if (y >= 168 && y < 196) {
    if (x < 104) {
      size_t length = strlen(wifiPassEdit);
      if (length > 0 && length < sizeof(wifiPassEdit) - 1) { wifiPassEdit[length] = ' '; wifiPassEdit[length + 1] = '\0'; }
      refreshKeyboardField();
      fieldOnlyRedraw = true;
    } else if (x < 216) {
      if (wifiSsidEdit[0]) {
        strlcpy(wifiSsid, wifiSsidEdit, sizeof(wifiSsid));
        strlcpy(wifiPass, wifiPassEdit, sizeof(wifiPass));
        snprintf(otaStatus, sizeof(otaStatus), "Connecting to %s ...", wifiSsid);
        page = OTA_PAGE;
        render();
        otaConnectToSaved(10000);
        if (wifiConnected()) {
          wifiRememberNetwork(wifiSsid, wifiPass);
          saveGame();                  // creds + remembered network on file
        }
        snprintf(otaStatus, sizeof(otaStatus),
                 wifiConnected() ? "Connected to %s - logged off"
                                 : "Could not connect to %s", wifiSsid);
        otaRestoreRadio();
        render();
      } else {
        page = OTA_PAGE;
      }
      render();
    }
  }
}

static void touchOtaPage(int x, int y) {
  if (y >= 108 && y < 142 && x < 160) {
    wifiLoginStage = 0;
    wifiSsidEdit[0] = '\0';
    wifiPassEdit[0] = '\0';
    page = WIFI_LOGIN;
    wifiScanStart();
    render();
  } else if (y >= 108 && y < 142) {
    otaCheckUpdate();
    render();
  } else if (y >= 146 && y < 168) {
    otaBrandIndex = (uint8_t)((otaBrandIndex + 1) % OTA_BRAND_COUNT);
    otaLatest[0] = '\0';             // re-check against the new brand
    snprintf(otaStatus, sizeof(otaStatus), "Brand: %s - tap CHECK UPDATES",
             otaBrands[otaBrandIndex].label);
    render();
  } else if (y >= 178 && y < 206 && !otaBusy && otaInstallable()) {
    otaInstallUpdate();
    render();
  }
}

// ============ Cheat codes (v15) ============
static void applyCheatCode() {
  char code[16];
  strlcpy(code, cheatEdit, sizeof(code));
  for (char *c = code; *c; ++c) *c = (char)toupper((unsigned char)*c);
  if (!strcmp(code, "CASH") || !strcmp(code, "MONEY")) {
    credits = (uint16_t)min(65535, (int)credits + 500);
    strlcpy(cheatFeedback, "+$500 added", sizeof(cheatFeedback));
  } else if (!strcmp(code, "SEEDS")) {
    seedsByStrain[plantStrain] = min(255, (int)seedsByStrain[plantStrain] + 8);
    snprintf(cheatFeedback, sizeof(cheatFeedback), "+8 %s seeds", strainNames[plantStrain]);
  } else if (!strcmp(code, "STASH")) {
    stashQualityByStrain[plantStrain] = blendQuality(stashQualityByStrain[plantStrain],
                                                     stashByStrain[plantStrain], 90, 10);
    stashByStrain[plantStrain] = min(255, (int)stashByStrain[plantStrain] + 10);
    strlcpy(cheatFeedback, "+10 flower jars (A grade)", sizeof(cheatFeedback));
  } else if (!strcmp(code, "UNLOCK")) {
    strainUnlockedThrough = STRAIN_COUNT - 1;
    strlcpy(cheatFeedback, "all strains unlocked", sizeof(cheatFeedback));
  } else if (!strcmp(code, "HARVEST")) {
    if (plantActive) {
      growth = 100;
      strlcpy(cheatFeedback, "plant ready to harvest", sizeof(cheatFeedback));
    } else {
      strlcpy(cheatFeedback, "no plant in the garden", sizeof(cheatFeedback));
      return;
    }
  } else if (!strcmp(code, "TURBO")) {
    gameSpeed = 2;
    growthBoostTicks = 60;
    strlcpy(cheatFeedback, "TURBO: extreme speed + glow", sizeof(cheatFeedback));
  } else if (!strcmp(code, "HYDRO")) {
    hydration = 100;
    dryStreakTicks = 0;
    strlcpy(cheatFeedback, "soil soaked to 100%", sizeof(cheatFeedback));
  } else if (!strcmp(code, "PEST")) {
    pestType = 0;
    pestSeverity = 0;
    strlcpy(cheatFeedback, "garden pest-free", sizeof(cheatFeedback));
  } else if (!strcmp(code, "CARE")) {
    waterPacks = min(99, waterPacks + 5);
    careKits = min(255, (int)careKits + 5);
    pestDefenseKits = min(255, (int)pestDefenseKits + 5);
    strlcpy(cheatFeedback, "supplies restocked", sizeof(cheatFeedback));
  } else if (!strcmp(code, "SKIP")) {
    if (!plantActive) {
      strlcpy(cheatFeedback, "no plant in the garden", sizeof(cheatFeedback));
      return;
    }
    if (defoliateWindow != 255 && defoliateWindow == currentDefoliateWindow() &&
        defoliateClicks == 0) {
      // Satisfies the open window without the kill risk, so growth resumes.
      defoliateClicks = 1;
      defoliatePromptPending = 255;
      strlcpy(cheatFeedback, "defoliation skipped", sizeof(cheatFeedback));
    } else {
      strlcpy(cheatFeedback, "no open defoliation", sizeof(cheatFeedback));
      return;
    }
  } else {
    strlcpy(cheatFeedback, "unknown code", sizeof(cheatFeedback));
    return;
  }
  saveGame();
}

static void drawCheatPage() {
  drawPageTitleSmall("CHEATS", cheatFeedback[0] ? cheatFeedback : "Type a code, then tap ACTIVATE");
  game.fillRoundRect(14, 60, 292, 18, 4, PANEL);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawString(cheatEdit, 21, 65, 1);
  drawFullKeyboard(nameKbMode);
  drawButton(7, 168, 92, 28, "SPACE", PANEL_LIGHT);
  drawButton(111, 168, 98, 28, "ACTIVATE", ACCENT_DARK);
  drawButton(221, 168, 92, 28, "BACK", PANEL_LIGHT);
}

static void touchCheatPage(int x, int y) {
  char key = fullKeyboardKey(x, y, nameKbMode);
  if (key) {
    size_t length = strlen(cheatEdit);
    if (length < sizeof(cheatEdit) - 1) { cheatEdit[length] = key; cheatEdit[length + 1] = '\0'; }
    refreshKeyboardField();
    fieldOnlyRedraw = true;
    return;
  }
  if (y >= 138 && y < 164 && x >= 263) {
    size_t length = strlen(cheatEdit);
    if (length) cheatEdit[length - 1] = '\0';
    refreshKeyboardField();
    fieldOnlyRedraw = true;
  } else if (y >= 168 && y < 196) {
    if (x < 104) {
      size_t length = strlen(cheatEdit);
      if (length > 0 && length < sizeof(cheatEdit) - 1 &&
          cheatEdit[length - 1] != ' ') {
        cheatEdit[length] = ' ';
        cheatEdit[length + 1] = '\0';
      }
      refreshKeyboardField();
      fieldOnlyRedraw = true;
      return;
    } else if (x < 216) {
      if (cheatEdit[0]) {
        applyCheatCode();
        cheatEdit[0] = '\0';
      }
    } else {
      page = OPTIONS;
    }
    render();
  }
}

// Options as a big-card grid: the whole card is the tap target, so no more
// missed taps on tiny inline value buttons.
void drawOptionsCard(int x, int y, int w, int h, const char *label,
                     const char *value, uint16_t fill) {
  game.fillRoundRect(x, y, w, h, 8, fill);
  game.drawRoundRect(x, y, w, h, 8, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, fill);
  game.drawCentreString(label, x + w / 2, y + 4, 2);
  game.setTextColor(GOLD, fill);
  game.drawCentreString(value, x + w / 2, y + h - 15, 1);
}

void drawOptionsPage() {
  char volLabel[8];
  if (musicVolume) snprintf(volLabel, sizeof(volLabel), "%u%%", musicVolume);
  else strlcpy(volLabel, "OFF", sizeof(volLabel));
  // Grid sits below the settings title band (title + subtitle cover y 28..68).
  // 4-inch board: stretch the option cards and push UPDATE toward the bottom.
  drawOptionsCard(5, 70, 152, 40, "GAME SPEED",
                  gameSpeed == 0 ? "RELAXED" : gameSpeed == 1 ? "QUICK" : "EXTREME", PANEL);
  drawOptionsCard(163, 70, 152, 40, "MUSIC", volLabel, PANEL);
  drawOptionsCard(5, 114, 152, 40, "FAST HARVEST", "TAP", ACCENT_DARK);
  drawOptionsCard(163, 114, 152, 40, "CHEATS", "TAP", PANEL_LIGHT);
  // Bottom row: two big card buttons side by side (the whole card is the
  // tap target). Must stay above the nav bar (NAV_Y = 207) or the nav touch
  // handler steals the taps (that was the STASH-jump bug).
  drawOptionsCard(5, 158, 152, 46, "RESET GAME",
                  resetArmed ? "TAP AGAIN!" : "ERASES SAVE",
                  resetArmed ? RED : PANEL_LIGHT);
  drawOptionsCard(163, 158, 152, 46, "UPDATE", "WIFI / OTA", PANEL_LIGHT);
}

// Grower menu, opened from the leaf: your grower as an ID card.
void drawGrowerMenuPage() {
  drawGrowerIdCard();
  // Full-width, big-font nav rows — the whole row is the tap target.
  // No BACK button: the leaf in the top-left closes this menu.
  const char *labels[2] = {"CHANGE GROWER", "GROWER NAME"};
  const int rowY[2] = {132, 172};
  for (uint8_t i = 0; i < 2; ++i) {
    game.fillRoundRect(10, rowY[i], 300, 34, 8, PANEL);
    game.drawRoundRect(10, rowY[i], 300, 34, 8, PANEL_LIGHT);
    game.setTextColor(TEXT_MAIN, PANEL);
    game.drawCentreString(labels[i], 160, rowY[i] + 4, 4);
    game.setTextColor(TEXT_DIM, PANEL);
    game.drawRightString(">", 296, rowY[i] + 8, 2);
  }
}

void drawGrowerNamePage() {
  drawPageTitleSmall("SET GROWER NAME", "Tap ABC for caps, #+_ for symbols");
  game.fillRoundRect(14, 60, 292, 18, 4, PANEL);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawString(growerNameEdit, 21, 65, 1);
  drawFullKeyboard(nameKbMode);
  drawButton(7, 168, 92, 28, "SPACE", PANEL_LIGHT);
  drawButton(111, 168, 98, 28, "SAVE", ACCENT_DARK);
  drawButton(221, 168, 92, 28, "CANCEL", PANEL_LIGHT);
}

void drawCharacterSelectPage() {
  // Giant character card cycled with < and > arrows; PICK confirms.
  game.fillScreen(BG);
  game.setTextColor(TEXT_MAIN, BG);
  game.drawCentreString("CHOOSE YOUR GROWER", 160, 8, 2);
  // Speaker toggle so the music can be muted before picking a grower.
  game.fillRoundRect(266, 2, 48, 22, 5, PANEL_LIGHT);
  game.drawRoundRect(266, 2, 48, 22, 5, PANEL);
  game.fillRect(272, 10, 4, 7, TEXT_MAIN);
  game.fillTriangle(276, 13, 281, 6, 281, 21, TEXT_MAIN);
  if (musicVolume) {
    game.drawLine(286, 8, 289, 5, TEXT_MAIN);
    game.drawLine(286, 13, 291, 13, TEXT_MAIN);
    game.drawLine(286, 18, 289, 21, TEXT_MAIN);
  } else {
    game.drawLine(286, 6, 294, 14, RED);
    game.drawLine(294, 6, 286, 14, RED);
  }
  uint8_t c = characterSelectIndex % 10;
  game.fillRoundRect(58, 30, 204, 160, 10, PANEL);
  game.drawRoundRect(58, 30, 204, 160, 10, ACCENT_DARK);
  drawFarmerHeadLarge(160, 90, c);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(farmerCharacterName(c), 160, 134, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString("Grower", 160, 154, 1);
  drawButton(10, 88, 44, 50, "<", ACCENT_DARK);
  drawButton(266, 88, 44, 50, ">", ACCENT_DARK);
  drawButton(58, 172, 204, 30, "PICK", ACCENT_DARK);  // above the nav strip
}

void drawHashRoomPage() {
  drawPageTitle("SECRET HASH ROOM", "Unlocked as a game upgrade");
  game.fillRoundRect(14, 82, 292, 70, 8, PANEL);
  game.drawRoundRect(14, 82, 292, 70, 8, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("Hash mode is ready", 160, 97, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString("Choose HASH after a room is mature.", 160, 123, 1);
  drawButton(22, 165, 132, 30, "GARDEN", ACCENT_DARK);
  drawButton(166, 165, 132, 30, "RUN A WASH", ACCENT_DARK);
}

void drawHarvestChoicePage() {
  drawPageTitle("HARVEST ROOM", "Choose what this batch becomes");
  game.fillRoundRect(12, 78, 296, 52, 7, PANEL);
  game.drawRoundRect(12, 78, 296, 52, 7, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawString("FLOWER", 23, 86, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawString("4 untrimmed units; trim before selling", 23, 108, 1);
  drawButton(246, 89, 50, 28, "PICK", ACCENT_DARK);

  game.fillRoundRect(12, 139, 296, 52, 7, PANEL);
  game.drawRoundRect(12, 139, 296, 52, 7, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawString("HASH", 23, 147, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawString(hashRoomUnlocked ? "2 units; secret room conversion" : "Locked: buy the secret room", 23, 169, 1);
  drawButton(246, 150, 50, 28, hashRoomUnlocked ? "PICK" : "LOCK", hashRoomUnlocked ? ACCENT_DARK : PANEL_LIGHT,
             hashRoomUnlocked ? TFT_WHITE : TEXT_DIM);
}

void drawTrimPage() {
  drawPageTitle("TRIMMING ROOM", "Make the harvested flower sale-ready");
  game.fillRoundRect(14, 82, 292, 57, 8, PANEL);
  game.drawRoundRect(14, 82, 292, 57, 8, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("Untrimmed flower units", 160, 91, 2);
  char amount[16];
  snprintf(amount, sizeof(amount), "%u", untrimmedUnits);
  game.setTextColor(GOLD, PANEL);
  game.drawCentreString(amount, 160, 116, 2);
  drawButton(18, 158, 136, 36, "TRIM FLOWERS", ACCENT_DARK);
  drawButton(166, 158, 136, 36, "LATER", PANEL_LIGHT);
}

bool selectedNearbyPeerValid() {
  return selectedNearbyPeer < MAX_NEARBY_PEERS &&
         nearbyPeers[selectedNearbyPeer].active &&
         millis() - nearbyPeers[selectedNearbyPeer].lastSeen < 20000UL;
}

// Compact title band for keyboard pages: frees room for bigger keys.
void drawPageTitleSmall(const char *title, const char *subtitle) {
  game.setTextColor(TEXT_MAIN, BG);
  game.drawString(title, 12, 34, 2);
  game.setTextColor(TEXT_DIM, BG);
  game.drawString(subtitle, 12, 52, 1);
}
void drawNearbyPeerTarget() {
  if (!selectedNearbyPeerValid()) {
    game.setTextColor(RED, BG);
    game.drawCentreString("Tap an online player first", 160, 69, 1);
    return;
  }
  const NearbyPeer &peer = nearbyPeers[selectedNearbyPeer];
  char target[38];
  snprintf(target, sizeof(target), "To %s %02X%02X", peer.grower,
           peer.mac[4], peer.mac[5]);
  game.setTextColor(TEXT_DIM, BG);
  game.drawCentreString(target, 160, 68, 1);
}

void drawNearbyKeyboardRow(const char *keys, int count, int startX, int y) {
  for (int i = 0; i < count; ++i) {
    char key[2] = {keys[i], '\0'};
    drawButton(startX + i * 31, y, 29, 26, key, PANEL);
  }
}

// Full keyboard: three modes share the same geometry. Lowercase is the
// qwerty layout; uppercase is the same keys shifted; symbols mode swaps
// digits and punctuation in. Toggle keys sit in the gaps left of the
// shorter rows: "#+_" (left of home row) and the shift key (left of
// bottom row) — both show "abc" while symbols mode is open.
void keyboardRowStrings(uint8_t mode, const char *&r1, const char *&r2,
                        const char *&r3) {
  if (mode == 2) {
    r1 = "1234567890";
    r2 = "-_.,!?&+";
    r3 = "'():;/";
  } else if (mode == 1) {
    r1 = "QWERTYUIOP";
    r2 = "ASDFGHJKL";
    r3 = "ZXCVBNM";
  } else {
    r1 = "qwertyuiop";
    r2 = "asdfghjkl";
    r3 = "zxcvbnm";
  }
}

void drawFullKeyboard(uint8_t mode) {
  const char *r1, *r2, *r3;
  keyboardRowStrings(mode, r1, r2, r3);
  drawNearbyKeyboardRow(r1, 10, 5, 82);
  drawButton(5, 110, 29, 26, mode == 2 ? "abc" : "#+_", PANEL_LIGHT);
  drawNearbyKeyboardRow(r2, 9, 40, 110);
  drawButton(5, 138, 29, 26, mode == 0 ? "ABC" : "abc", PANEL_LIGHT);
  drawNearbyKeyboardRow(r3, 7, 40, 138);
  drawButton(263, 138, 52, 26, "DEL", PANEL_LIGHT);
}

// Shared touch resolution for the full keyboard. Returns the key to append
// (0 = none), and updates `mode` when a toggle key is pressed. DEL and the
// bottom action row stay with each page's own handler.
char fullKeyboardKey(int x, int y, uint8_t &mode) {
  const char *r1, *r2, *r3;
  keyboardRowStrings(mode, r1, r2, r3);
  if (y >= 82 && y < 108) {
    int index = (x - 5) / 31;
    return index < 10 ? r1[index] : 0;
  }
  if (y >= 110 && y < 136) {
    if (x >= 5 && x < 34) {
      mode = mode == 2 ? 0 : 2;   // #+_ <-> letters
      return 0;
    }
    int index = (x - 40) / 31;
    return x >= 40 && index < 9 ? r2[index] : 0;
  }
  if (y >= 138 && y < 164) {
    if (x >= 5 && x < 34) {
      mode = mode == 1 ? 0 : 1;   // ABC <-> abc (exits symbols to lowercase)
      return 0;
    }
    int index = (x - 40) / 31;
    return x >= 40 && x < 259 && index < 7 ? r3[index] : 0;
  }
  return 0;
}

void drawNearbyPage() {
  if (nearbyMode == 1) {
    drawPageTitleSmall("SEND A MESSAGE", "Tap ABC for caps, #+_ for symbols");
    drawNearbyPeerTarget();
    game.fillRoundRect(14, 60, 292, 18, 4, PANEL);
    game.setTextColor(TEXT_MAIN, PANEL);
    game.drawString(outgoingMessage, 21, 65, 1);
    drawFullKeyboard(msgKbMode);
    drawButton(7, 168, 92, 28, "SPACE", PANEL_LIGHT);
    drawButton(111, 168, 98, 28, "SEND", selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
    drawButton(221, 168, 92, 28, "BACK", PANEL_LIGHT);
    return;
  }

  if (nearbyMode == 2) {
    // Supply-shop-style card grid for gifting.
    char target[44];
    if (selectedNearbyPeerValid()) {
      const NearbyPeer &peer = nearbyPeers[selectedNearbyPeer];
      snprintf(target, sizeof(target), "To %s %02X%02X",
               peer.grower, peer.mac[4], peer.mac[5]);
    } else {
      strlcpy(target, "Pick an online player first", sizeof(target));
    }
    drawPageTitle("SEND SUPPLIES", target);
    // Picker rows: big arrows on the card edges pick the strain; the big
    // SEND button in the middle is a separate, deliberate tap.
    bool seedsReady = seedsByStrain[sendSeedStrain] >= 4 && selectedNearbyPeerValid();
    char seedLine[34];
    game.fillRoundRect(5, 70, 310, 34, 5, PANEL);
    game.drawRoundRect(5, 70, 310, 34, 5, seedsReady ? ACCENT : PANEL_LIGHT);
    drawButton(9, 74, 28, 26, "<", PANEL_LIGHT);
    drawButton(283, 74, 28, 26, ">", PANEL_LIGHT);
    snprintf(seedLine, sizeof(seedLine), "SEND %s x%u", strainNames[sendSeedStrain],
             seedsByStrain[sendSeedStrain]);
    drawButton(41, 74, 238, 26, seedLine,
               seedsReady ? ACCENT_DARK : PANEL_LIGHT,
               seedsReady ? TFT_WHITE : TEXT_DIM);
    // Flower jar + hash jar rows: arrows pick, SEND arms the confirm panel.
    bool stashReady = stashByStrain[sendStashStrain] > 0 && selectedNearbyPeerValid();
    bool hashReady = hashByStrain[sendHashStrain] > 0 && selectedNearbyPeerValid();
    game.fillRoundRect(5, 108, 152, 34, 5, PANEL);
    game.drawRoundRect(5, 108, 152, 34, 5, stashReady ? ACCENT : PANEL_LIGHT);
    drawButton(9, 112, 22, 26, "<", PANEL_LIGHT);
    drawButton(131, 112, 22, 26, ">", PANEL_LIGHT);
    snprintf(seedLine, sizeof(seedLine), "SEND %s", strainNames[sendStashStrain]);
    drawButton(33, 112, 96, 26, seedLine,
               stashReady ? ACCENT_DARK : PANEL_LIGHT,
               stashReady ? TFT_WHITE : TEXT_DIM);
    game.fillRoundRect(163, 108, 152, 34, 5, PANEL);
    game.drawRoundRect(163, 108, 152, 34, 5, hashReady ? ACCENT : PANEL_LIGHT);
    drawButton(167, 112, 22, 26, "<", PANEL_LIGHT);
    drawButton(289, 112, 22, 26, ">", PANEL_LIGHT);
    snprintf(seedLine, sizeof(seedLine), "SEND %s", strainNames[sendHashStrain]);
    drawButton(191, 112, 96, 26, seedLine,
               hashReady ? ACCENT_DARK : PANEL_LIGHT,
               hashReady ? TFT_WHITE : TEXT_DIM);
    char ownedVal[12];
    snprintf(ownedVal, sizeof(ownedVal), "x%u", pestDefenseKits);
    drawSendCard(5, 146, 100, 34, "PEST +1", ownedVal,
                 pestDefenseKits > 0 && selectedNearbyPeerValid());
    snprintf(ownedVal, sizeof(ownedVal), "x%u", waterPacks);
    drawSendCard(110, 146, 100, 34, "WATER +2", ownedVal,
                 waterPacks >= 2 && selectedNearbyPeerValid());
    snprintf(ownedVal, sizeof(ownedVal), "x%u", careKits);
    drawSendCard(215, 146, 100, 34, "CARE +2", ownedVal,
                 careKits >= 2 && selectedNearbyPeerValid());
    drawButton(7, 184, 152, 19, "SEND MONEY",
               selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
    drawButton(165, 184, 148, 19, "BACK", PANEL_LIGHT);
    if (sendConfirmItem) {
      // Send confirmation: BACK cancels, SEND IT gifts the product.
      game.fillRoundRect(5, 146, 310, 58, 8, PANEL);
      game.drawRoundRect(5, 146, 310, 58, 8, GOLD);
      game.setTextColor(TEXT_MAIN, PANEL);
      char msg[44];
      if (sendConfirmItem == 1)
        snprintf(msg, sizeof(msg), "SEND 4 %s SEEDS?", strainNames[sendSeedStrain]);
      else if (sendConfirmItem == 5)
        snprintf(msg, sizeof(msg), "SEND 1 %s FLOWER JAR?", strainNames[sendStashStrain]);
      else
        snprintf(msg, sizeof(msg), "SEND 1 %s HASH JAR?", strainNames[sendHashStrain]);
      game.drawCentreString(msg, 160, 153, 1);
      drawButton(5, 168, 150, 26, "BACK", PANEL_LIGHT);
      drawButton(163, 168, 152, 26, "SEND IT", ACCENT_DARK, TFT_WHITE);
    }
    return;
  }

  if (nearbyMode == 4) {
    char balance[40];
    snprintf(balance, sizeof(balance), "You have $%u", credits);
    drawPageTitle("SEND MONEY", balance);
    drawNearbyPeerTarget();
    game.fillRoundRect(14, 82, 292, 22, 4, PANEL);
    game.setTextColor(TEXT_MAIN, PANEL);
    char amount[10];
    snprintf(amount, sizeof(amount), "$%s", outgoingCash[0] ? outgoingCash : "0");
    game.drawString(amount, 21, 88, 1);
    // Two roomy rows of five digits so every key fits on screen.
    for (int i = 0; i < 10; ++i) {
      char key[2] = {'1' + i, '\0'};
      drawButton(5 + (i % 5) * 62, i < 5 ? 106 : 132, 54, 23, key, PANEL);
    }
    drawButton(5, 158, 98, 21, "CLR", PANEL_LIGHT);
    drawButton(111, 158, 98, 21, "000", PANEL_LIGHT);
    drawButton(217, 158, 98, 21, "DEL", PANEL_LIGHT);
    drawButton(5, 182, 98, 21, "ALL", PANEL_LIGHT);
    drawButton(111, 182, 98, 21, "SEND",
               selectedNearbyPeerValid() && outgoingCash[0] ? ACCENT_DARK : PANEL_LIGHT);
    drawButton(217, 182, 98, 21, "BACK", PANEL_LIGHT);
    return;
  }

  if (nearbyMode == 3) {
    drawPageTitle("MESSAGE INBOX", "Latest message, supply or cash delivery");
    game.fillRoundRect(14, 82, 292, 78, 8, PANEL);
    game.drawRoundRect(14, 82, 292, 78, 8, PANEL_LIGHT);
    game.setTextColor(TEXT_MAIN, PANEL);
    game.drawCentreString(lastNearbyMessage, 160, 105, 2);
    drawButton(108, 174, 104, 27, "BACK", PANEL_LIGHT);
    return;
  }

  char localRank[48];
  snprintf(localRank, sizeof(localRank), "%s - L%u %s", growerName,
           currentMasteryLevel(), masteryNicknameForLevel(currentMasteryLevel()));
  drawPageTitle("NEARBY GROWERS", localRank);
  drawNearbyPeerListOnly();
  drawButton(5, 179, 98, 24, "MESSAGE", selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
  drawButton(111, 179, 98, 24, "SUPPLIES", selectedNearbyPeerValid() ? ACCENT_DARK : PANEL_LIGHT);
  drawButton(217, 179, 98, 24, "INBOX", PANEL_LIGHT);
}

void sendNearbyGift(uint8_t item) {
  if (!selectedNearbyPeerValid()) return;
  const char *giftName = "supply";
  char strainTag[2] = "";
  switch (item) {
    case 1:
      if (seedsByStrain[sendSeedStrain] < 4) return;
      giftName = "4 seeds";
      strainTag[0] = '0' + sendSeedStrain;
      break;
    case 2: if (!pestDefenseKits) return; giftName = "a pest kit"; break;
    case 3: if (waterPacks < 2) return; giftName = "2 water packs"; break;
    case 4: if (careKits < 2) return; giftName = "2 care kits"; break;
    case 5:
      if (!stashByStrain[sendStashStrain]) return;
      giftName = "a stash jar";
      strainTag[0] = '0' + sendStashStrain;
      break;
    case 6:
      if (!hashByStrain[sendHashStrain]) return;
      giftName = "a hash jar";
      strainTag[0] = '0' + sendHashStrain;
      break;
    default: return;
  }
  if (sendNearbyPacket(nearbyPeers[selectedNearbyPeer].mac, PACKET_GIFT, item, strainTag)) {
    if (item == 1) {
      seedsByStrain[sendSeedStrain] -= 4;
      snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent 4 %s seeds", strainNames[sendSeedStrain]);
    }
    else if (item == 2) { --pestDefenseKits; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent %s", giftName); }
    else if (item == 3) { waterPacks -= 2; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent %s", giftName); }
    else if (item == 4) { careKits -= 2; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent %s", giftName); }
    else if (item == 5) { --stashByStrain[sendStashStrain]; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent a %s jar", strainNames[sendStashStrain]); }
    else if (item == 6) { --hashByStrain[sendHashStrain]; snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent a %s hash jar", strainNames[sendHashStrain]); }
    saveGame();
  } else {
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Send failed; supplies kept");
  }
  nearbyMode = 0;
}

void sendNearbyCash() {
  if (!selectedNearbyPeerValid()) return;
  uint32_t amount = (uint32_t)strtoul(outgoingCash, nullptr, 10);
  if (amount > credits) amount = credits;   // never send more than you have
  if (!amount) return;
  char text[8];
  snprintf(text, sizeof(text), "%lu", (unsigned long)amount);
  if (sendNearbyPacket(nearbyPeers[selectedNearbyPeer].mac, PACKET_CASH, 0, text)) {
    credits -= (uint16_t)amount;
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Sent $%lu", (unsigned long)amount);
    saveGame();
  } else {
    snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Send failed; money kept");
  }
  outgoingCash[0] = '\0';
  nearbyMode = 0;
}

void presentFrame() {
  // One atomic panel write: pages draw into the PSRAM frame buffer and are
  // pushed in a single pass, so nothing on screen ever flickers mid-draw.
  if (frameBufferActive) frameBuf.pushSprite(0, 0);
}

// Push the frame with every transient overlay stamped on top: farmer visit,
// nearby popup card and the death modal. Partial redraws that would erase
// stamped content call this instead of pushing the bare buffer.
void presentWithOverlays() {
  if (farmerOverlayActive && !waterAnimActive &&
      (page == GARDEN || page == PLANT_VIEW)) {
    drawFarmerInspector(2);
    drawFarmerSpeechBubble(farmerOverlaySaying);
  }
  if (nearbyPopupActive && page != NEARBY) drawNearbyPopup();
  if (deathWarnModal) {
    drawDeathWarnPopup();
    deathWarnStamped = true;
  }
  presentFrame();
}

void render(bool presentNow) {
  static Page lastSeenPage = GARDEN;
  if (page != lastSeenPage) {
    if (page == CHARACTER_SELECT) characterSelectIndex = farmerCharacter;
    lastSeenPage = page;
  }
  if (page == PLANTING) {
    // Land on a strain you actually hold before drawing the picker/tray.
    if (plantedPotCount() == 0 && !seedsByStrain[plantStrain]) {
      plantStrain = nextSeededStrain(plantStrain, 1);
    }
    game.fillScreen(BG);
    drawPlantingPage();
  } else if (page == PLANT_VIEW) {
    game.fillScreen(BG);
    drawPlantFullScreenPage();
  } else if (page == CHARACTER_SELECT) {
    game.fillScreen(BG);
    drawCharacterSelectPage();
  } else {
    // Settings pages run full screen (no nav bar); the header gear icon
    // toggles back to the garden from any of them.
    bool settingsPage = page == OPTIONS || page == GROWER_MENU;
    if (settingsPage) {
      // Full screen, scrollable: draw the content first (it may bleed under
      // the title strip), then repaint header and fixed title on top.
      static Page lastSettingsPage = GARDEN;
      if (lastSettingsPage != page) { settingsScroll = 0; lastSettingsPage = page; }
      int16_t scrollMax = 0;   // hub fits on one screen now; keep scroll at 0
      if (settingsScroll < 0) settingsScroll = 0;
      if (settingsScroll > scrollMax) settingsScroll = scrollMax;
      game.fillScreen(BG);   // full wipe: no stale content behind any page
      switch (page) {
        case OPTIONS: drawOptionsPage(); break;
        case GROWER_MENU: drawGrowerMenuPage(); break;
        default: break;
      }
      drawHeader();
      game.fillRect(0, HEADER_H, SCREEN_W, 40, BG);
      drawSettingsTitle();
    } else {
      // Keep the persistent header and navigation from being cleared on every
      // button press. Page changes redraw only the central content area.
      game.fillScreen(BG);   // full wipe: no stale content behind any page
      drawHeader();
      switch (page) {
        case GARDEN: drawGardenPage(); break;
        case SUPPLIES: drawSuppliesPage(); break;
        case MARKET: drawMarketPage(); break;
        case STASH: drawStashPage(); break;
        case HASH_ROOM: drawHashRoomPage(); if (washOpen) drawWashModal(); break;
        case HARVEST_SELECT: drawHarvestChoicePage(); break;
        case TRIM_ROOM: drawTrimPage(); break;
        case NEARBY: drawNearbyPage(); break;
        case NAME_EDIT: drawGrowerNamePage(); break;
        case OTA_PAGE: drawOtaPage(); break;
        case WIFI_LOGIN: drawWifiLoginPage(); break;
        case CHEAT_CODE: drawCheatPage(); break;
        case SEED_SHOP: drawSeedShopPage(); break;
        case CHARACTER_SELECT: break;
        case PLANT_VIEW: break;
        case PLANTING: break;
        default: break;
      }
      drawNav();
    }
  }
  if (presentNow) presentWithOverlays();
  overlayDirty = false;  // overlays ride inside the buffer on every push
}

uint8_t waterToleranceNow() {
  uint8_t strain = min((int)activeStrain, (int)STRAIN_COUNT - 1);
  return strainWaterTolerance[strain] - stageWaterPenalty[stageForGrowth()];
}

bool wateringWouldDrown() {
  return plantActive && hydration + WATER_AMOUNT > waterToleranceNow();
}

// Current defoliation window from growth, or 255 before the first one / at harvest.
uint8_t currentDefoliateWindow() {
  if (!plantActive || growth >= 100) return 255;
  uint8_t w = 255;
  for (uint8_t i = 0; i < DEFOLIATE_WINDOWS; ++i) if (growth >= defoliateWindowStart[i]) w = i;
  return w;
}

void resetDefoliationForNewPlant() {
  defoliateWindow = 255;
  defoliateClicks = 0;
  defoliateLimit = 2;
  defoliatePromptPending = 255;
  defoliateReadyAt = 0;
  dryStreakTicks = 0;  // fresh start: soil is watered on planting
}

// Opens a new defoliation window when growth crosses its threshold.
void updateDefoliateWindow() {
  uint8_t w = currentDefoliateWindow();
  if (w == 255 || w == defoliateWindow) return;
  uint8_t strain = min((int)activeStrain, (int)STRAIN_COUNT - 1);
  defoliateWindow = w;
  defoliateClicks = 0;
  defoliateLimit = random(strainDefoliateMin[strain], strainDefoliateMax[strain] + 1);
  defoliatePromptPending = w;
  // The new window stays gray (and its prompt waits) until the cooldown passes.
  defoliateReadyAt = gameMinutes + DEFOLIATE_COOLDOWN_MINUTES;
  saveGame();
}

// 0 = gray (unavailable), 1 = green (safe), 2 = red (next click may kill)
uint8_t defoliateButtonState() {
  uint8_t w = currentDefoliateWindow();
  if (w == 255 || w != defoliateWindow) return 0;
  if (gameMinutes < defoliateReadyAt) return 0;  // cooling down between windows
  uint8_t strain = min((int)activeStrain, (int)STRAIN_COUNT - 1);
  return defoliateClicks >= strainDefoliateMin[strain] ? 2 : 1;
}

void killPlant(uint8_t reason) {
  deathReason = reason;
  deathWarnModal = false;
  plantDeathAnimationRequested = true;
  deathLedUntil = millis() + 5000;  // red back-LED flag for the kill event
  startSong(&deathJingle);  // sad descending chiptune over the cutscene
  // Light red immediately: the death cutscene blocks the loop (animation
  // frames plus a tap-to-continue wait), so updateHarvestLed() cannot
  // refresh the LED until long after this 5 s window would have expired.
  harvestLedRed();
}

void waterPlant() {
  if (!plantActive || waterPacks == 0) return;
  bool drown = wateringWouldDrown();
  --waterPacks;
  hydration = min(100, hydration + WATER_AMOUNT);
  vigor = min(100, vigor + 4);
  if (drown) {
    killPlant(1);
    saveGame();
    return;
  }
  waterAnimationRequested = true;
  saveGame();
}

void defoliateRoom() {
  if (!plantActive || growth >= 100) return;
  if (defoliateButtonState() == 0) return;  // grayed out: not time to defoliate
  ++defoliateClicks;
  if (defoliateClicks > defoliateLimit) {
    killPlant(2);
    saveGame();
    return;
  }
  // Thinning the canopy is maintenance: it lifts the growth pause and helps
  // plant health and pests, but grants no growth percentage.
  vigor = min(100, vigor + 10);
  if (pestType > 0) {
    if (pestSeverity > 20) pestSeverity -= 20;
    else { pestSeverity = 0; pestType = 0; }
  }
  saveGame();
}

void treatPests() {
  if (!plantActive || pestType == 0 || pestDefenseKits == 0) return;
  --pestDefenseKits;
  pestType = 0;
  pestSeverity = 0;
  vigor = min(100, vigor + 12);
  saveGame();
}

void finishHarvest(bool makeHash) {
  if (!plantActive || growth < 100) return;
  if (makeHash && !hashRoomUnlocked) return;

  // A mold event appears on about one harvest in five. The affected crop is
  // discarded immediately; the rest of the batch remains usable.
  moldLossUnits = random(100) < 20 ? (makeHash ? 1 : random(1, 3)) : 0;
  lastHarvestUnits = (makeHash ? 2 : PLANTS_PER_ROOM) - moldLossUnits;
  // Remediation kits buy the toxic portion back: one kit per unit returns
  // it to the batch so it can still be trimmed and sold.
  moldRescuedUnits = 0;
  if (moldLossUnits && remediationKits) {
    moldRescuedUnits = min(moldLossUnits, remediationKits);
    remediationKits -= moldRescuedUnits;
    moldLossUnits -= moldRescuedUnits;
    lastHarvestUnits += moldRescuedUnits;
  }
  moldAnimationRequested = moldLossUnits > 0;
  if (makeHash) {
    // Vigor at harvest grades the batch: it sets what the jar sells for.
    hashQualityByStrain[activeStrain] = blendQuality(hashQualityByStrain[activeStrain],
                                                     hashByStrain[activeStrain], vigor, lastHarvestUnits);
    hashByStrain[activeStrain] += lastHarvestUnits;
  } else {
    untrimmedQuality = blendQuality(untrimmedQuality, untrimmedUnits, vigor, lastHarvestUnits);
    untrimmedUnits += lastHarvestUnits;
    untrimmedStrain = activeStrain;
  }
  moldAnimationRequested = moldLossUnits > 0;
  startSong(&harvestFanfare);  // rising chiptune fanfare
  if (harvestCount < 65535) ++harvestCount;
  harvestedStrainMask |= (uint16_t)1 << activeStrain;
  while (strainUnlockedThrough < STRAIN_COUNT - 1) {
    // Unlock the next strain in easy -> hard catalog order once it is grown.
    if (harvestedStrainMask & ((uint16_t)1 << strainCatalogOrder[strainUnlockedThrough])) {
      ++strainUnlockedThrough;
    } else {
      break;
    }
  }

  pestType = 0;
  pestSeverity = 0;
  growth = 0;
  hydration = 72;
  vigor = 82;
  plantActive = false;
  plantedPotMask = 0;
  if (totalSeeds() >= PLANTS_PER_ROOM) {
    firstPlantingDone = false;
    pageAfterPlanting = makeHash ? HASH_ROOM : TRIM_ROOM;
    page = PLANTING;
  } else {
    page = makeHash ? HASH_ROOM : TRIM_ROOM;
  }
  saveGame();
  if (makeHash) {
    hashAnimationRequested = true;
    lastFarmerVisitAt = millis();
  } else {
    flowerHarvestAnimationRequested = true;
    lastFarmerVisitAt = millis();
  }
}

void trimAllFlowers() {
  if (untrimmedUnits == 0) return;
  // The trimmed batch carries its harvest vigor into the stash jar (blended
  // with whatever is already in that jar).
  stashQualityByStrain[untrimmedStrain] = blendQuality(stashQualityByStrain[untrimmedStrain],
                                                       stashByStrain[untrimmedStrain], untrimmedQuality, untrimmedUnits);
  stashByStrain[untrimmedStrain] += untrimmedUnits;
  untrimmedUnits = 0;
  saveGame();
  page = MARKET;
  flowerTrimAnimationRequested = true;
  lastFarmerVisitAt = millis();
}

void buyWater() {
  if (credits < 5) return;
  credits -= 5;
  waterPacks = min(99, waterPacks + 3);
  playCoin();
  saveGame();
}

void buyCareKits() {
  if (credits < 7) return;
  credits -= 7;
  careKits = min(99, careKits + 3);
  playCoin();
  saveGame();
}

void openSeedShop(Page returnTo) {
  pageAfterSeedShop = returnTo;
  page = SEED_SHOP;
}

void buySeeds(uint8_t strain) {
  if (strain >= STRAIN_COUNT || strainCatalogPos[strain] > strainUnlockedThrough ||
      credits < strainPrices[strain] || totalSeeds() > 95) return;
  credits -= strainPrices[strain];
  seedsByStrain[strain] += PLANTS_PER_ROOM;
  seedStrain = strain;
  plantStrain = strain;   // the pack you just bought is what you'll plant
  if (!plantActive && totalSeeds() >= PLANTS_PER_ROOM) {
    pageAfterPlanting = pageAfterSeedShop;
    firstPlantingDone = false;
    plantedPotMask = 0;
    page = PLANTING;
  } else {
    page = pageAfterSeedShop;
  }
  playCoin();
  saveGame();
}

void buyPestDefense() {
  if (credits < 10) return;
  credits -= 10;
  pestDefenseKits = min(99, pestDefenseKits + 2);
  playCoin();
  saveGame();
}

void buyRemediation() {
  if (credits < 12) return;
  credits -= 12;
  remediationKits = min(99, remediationKits + 2);
  playCoin();
  saveGame();
}

void buyBong() {
  if (credits < 24 || bongs > 0) return;
  credits -= 24;
  bongs = 1;
  playCoin();
  saveGame();
}

void buyLighters() {
  if (credits < 6) return;
  credits -= 6;
  lighters = min(99, lighters + 3);
  playCoin();
  saveGame();
}

void buyRollingPapers() {
  if (credits < 5) return;
  credits -= 5;
  rollingPapers = min(99, rollingPapers + 5);
  playCoin();
  saveGame();
}

void takeSmokeBreak(uint8_t product, bool makeJoint) {
  // Smoking only needs the gear and product; it no longer depends on the plant.
  if (makeJoint) {
    int16_t strain = cheapestStashStrain();
    if (product != 1 || strain < 0 || lighters == 0 || rollingPapers == 0) return;
    --stashByStrain[strain];
    --lighters;
    --rollingPapers;
    smokeAccessoryUsed = 4; // Hand-rolled joint; flower, paper, and lighter used.
    smokeProductUsed = 1;
  } else {
    if (bongs == 0) return;
    int16_t strain = -1;
    if (product == 1) strain = cheapestStashStrain();
    else if (product == 2) strain = cheapestHashStrain();
    if (strain < 0) return;
    if (product == 1) --stashByStrain[strain];
    else --hashByStrain[strain];
    smokeAccessoryUsed = 1; // Reusable bong.
    smokeProductUsed = product;
  }
  growthBoostTicks = min(99, growthBoostTicks + 10);
  stonedLevel = min(100, stonedLevel + 25);
  rainbowUntil = millis() + 5000UL;  // 5s LED flourish, then fade
  smokeAnimationRequested = true;
  lastFarmerVisitAt = millis();
  saveGame();
}

void buyHashRoom() {
  if (hashRoomUnlocked) {
    page = HASH_ROOM;
    return;
  }
  if (credits < HASH_ROOM_COST) return;
  credits -= HASH_ROOM_COST;
  hashRoomUnlocked = true;
  playCoin();
  saveGame();
  page = HASH_ROOM;
}

const char *gradeName(uint8_t grade) {
  switch (grade) {
    case 120: return "A+";
    case 100: return "A";
    case 80:  return "B";
    case 60:  return "C";
    default:  return "D";
  }
}

// Vigor at harvest -> percent of the strain's base price paid at the market:
// 90+ premium (A+), 70-89 full price (A), 50-69 80% (B), 30-49 60% (C), else 40% (D).
uint8_t sellGradeForQuality(uint8_t q) {
  if (q >= 90) return 120;
  if (q >= 70) return 100;
  if (q >= 50) return 80;
  if (q >= 30) return 60;
  return 40;
}

uint16_t flowerUnitPrice(uint8_t strain) {
  return (uint16_t)((uint32_t)strainFlowerValue[strain] *
                    sellGradeForQuality(stashQualityByStrain[strain]) / 100);
}

uint16_t hashUnitPrice(uint8_t strain) {
  return (uint16_t)((uint32_t)strainHashValue[strain] *
                    sellGradeForQuality(hashQualityByStrain[strain]) / 100);
}

// Weighted-blend a jar's existing quality with new units of another quality.
uint8_t blendQuality(uint8_t oldQ, uint16_t oldUnits, uint8_t newQ, uint16_t newUnits) {
  uint32_t total = (uint32_t)oldUnits + newUnits;
  if (!total) return newQ;
  return (uint8_t)(((uint32_t)oldQ * oldUnits + (uint32_t)newQ * newUnits) / total);
}

void sellOneFlower(uint8_t strain) {
  if (strain >= STRAIN_COUNT || !stashByStrain[strain]) return;
  --stashByStrain[strain];
  credits += flowerUnitPrice(strain);
  playCoin();
  saveGame();
}

void sellAllFlowers(uint8_t strain) {
  if (strain >= STRAIN_COUNT || !stashByStrain[strain]) return;
  uint32_t total = (uint32_t)stashByStrain[strain] * flowerUnitPrice(strain);
  stashByStrain[strain] = 0;
  uint32_t newCredits = (uint32_t)credits + total;
  credits = newCredits > 65535 ? 65535 : (uint16_t)newCredits;
  playCoin();
  saveGame();
}

void sellOneHash(uint8_t strain) {
  if (!hashRoomUnlocked || strain >= STRAIN_COUNT || !hashByStrain[strain]) return;
  --hashByStrain[strain];
  credits += hashUnitPrice(strain);
  playCoin();
  saveGame();
}

void sellAllHash(uint8_t strain) {
  if (!hashRoomUnlocked || strain >= STRAIN_COUNT || !hashByStrain[strain]) return;
  uint32_t total = (uint32_t)hashByStrain[strain] * hashUnitPrice(strain);
  hashByStrain[strain] = 0;
  uint32_t newCredits = (uint32_t)credits + total;
  credits = newCredits > 65535 ? 65535 : (uint16_t)newCredits;
  playCoin();
  saveGame();
}

// ============ Market sell submenu ============
// Rebuild the stocked-strain list for the active sell mode, keeping the
// cursor when the strain is still stocked.
static void marketSellRefresh() {
  marketSellCount = 0;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i) {
    uint16_t n = marketSellMode <= 2 ? stashByStrain[i] : hashByStrain[i];
    if (n) marketSellList[marketSellCount++] = i;
  }
  for (uint8_t k = 0; k < marketSellCount; ++k)
    if (marketSellList[k] == marketSellStrain) return;
  if (marketSellCount) marketSellStrain = marketSellList[0];
}

static void openMarketSell(uint8_t mode) {
  marketSellMode = mode;
  marketSellStrain = 0;
  marketSellConfirm = false;
  marketSellRefresh();
  render();
}

// Builds the big SELL bar label for the selected strain.
static void marketSellPriceText(char *out, size_t len) {
  if (marketSellMode == 1)
    snprintf(out, len, "SELL 1 JAR - $%u", flowerUnitPrice(marketSellStrain));
  else if (marketSellMode == 2)
    snprintf(out, len, "SELL ALL x%u - $%u", stashByStrain[marketSellStrain],
             (uint32_t)stashByStrain[marketSellStrain] * flowerUnitPrice(marketSellStrain));
  else if (marketSellMode == 3)
    snprintf(out, len, "SELL 1 HASH - $%u", hashUnitPrice(marketSellStrain));
  else
    snprintf(out, len, "SELL ALL x%u - $%u", hashByStrain[marketSellStrain],
             (uint32_t)hashByStrain[marketSellStrain] * hashUnitPrice(marketSellStrain));
}

static void drawSellStrainModal() {
  bool flower = marketSellMode <= 2;
  game.fillRoundRect(3, 58, 314, 148, 8, PANEL);
  game.drawRoundRect(3, 58, 314, 148, 8, PANEL_LIGHT);
  // BACK sits top-right of the box, above the strain card.
  drawButton(242, 61, 72, 26, "BACK", PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(flower ? "SELL FLOWER" : "SELL HASH", 120, 64, 2);
  if (!marketSellCount) {
    game.setTextColor(TEXT_DIM, PANEL);
    game.drawCentreString("Nothing to sell here", 160, 124, 1);
    return;
  }
  uint8_t s = marketSellStrain;
  if (marketSellConfirm) {
    // One-tap confirmation: nothing sells until YES, SELL IT.
    game.setTextColor(TEXT_MAIN, PANEL);
    game.drawCentreString("ARE YOU SURE?", 160, 102, 2);
    char detail[44];
    if (marketSellMode == 1)
      snprintf(detail, sizeof(detail), "Sell 1 %s jar - $%u", strainNames[marketSellStrain],
               flowerUnitPrice(marketSellStrain));
    else if (marketSellMode == 2)
      snprintf(detail, sizeof(detail), "Sell all %s jars (x%u) - $%u", strainNames[marketSellStrain],
               stashByStrain[marketSellStrain],
               (uint32_t)stashByStrain[marketSellStrain] * flowerUnitPrice(marketSellStrain));
    else if (marketSellMode == 3)
      snprintf(detail, sizeof(detail), "Sell 1 %s hash - $%u", strainNames[marketSellStrain],
               hashUnitPrice(marketSellStrain));
    else
      snprintf(detail, sizeof(detail), "Sell all %s hash (x%u) - $%u", strainNames[marketSellStrain],
               hashByStrain[marketSellStrain],
               (uint32_t)hashByStrain[marketSellStrain] * hashUnitPrice(marketSellStrain));
    game.drawCentreString(detail, 160, 128, 1);
    drawButton(5, 172, 150, 28, "BACK", PANEL_LIGHT);
    drawButton(163, 172, 152, 28, "YES, SELL IT", RED, TFT_WHITE);
    return;
  }
  // One giant card per stashed strain, flanked by big < > arrows.
  drawButton(5, 96, 48, 72, "<", ACCENT_DARK);
  drawButton(257, 96, 48, 72, ">", ACCENT_DARK);
  game.fillRoundRect(57, 96, 196, 72, 8, PANEL);
  game.drawRoundRect(57, 96, 196, 72, 8, ACCENT);
  int nameFont = strlen(strainNames[s]) > 14 ? 1 : 2;
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(strainNames[s], 155, nameFont == 2 ? 104 : 108, nameFont);
  game.setTextColor(GOLD, PANEL);
  char info[40];
  if (flower)
    snprintf(info, sizeof(info), "x%u jars - $%u each", stashByStrain[s], flowerUnitPrice(s));
  else
    snprintf(info, sizeof(info), "x%u hash - $%u each", hashByStrain[s], hashUnitPrice(s));
  game.drawCentreString(info, 155, 130, 1);
  // Full-width SELL bar under the card.
  char price[40];
  marketSellPriceText(price, sizeof(price));
  drawButton(5, 172, 310, 28, price, ACCENT_DARK, TFT_WHITE);
}

// Arrow feedback without a full-page render: repaint only the strain card
// and the SELL bar so tapping < > doesn't flash the whole screen.
static void redrawSellStrainCard() {
  uint8_t s = marketSellStrain;
  bool flower = marketSellMode <= 2;
  game.fillRoundRect(57, 96, 196, 72, 8, PANEL);
  game.drawRoundRect(57, 96, 196, 72, 8, ACCENT);
  int nameFont = strlen(strainNames[s]) > 14 ? 1 : 2;
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(strainNames[s], 155, nameFont == 2 ? 104 : 108, nameFont);
  game.setTextColor(GOLD, PANEL);
  char info[40];
  if (flower)
    snprintf(info, sizeof(info), "x%u jars - $%u each", stashByStrain[s], flowerUnitPrice(s));
  else
    snprintf(info, sizeof(info), "x%u hash - $%u each", hashByStrain[s], hashUnitPrice(s));
  game.drawCentreString(info, 155, 130, 1);
  char price[40];
  marketSellPriceText(price, sizeof(price));
  game.fillRoundRect(5, 172, 310, 28, 6, ACCENT_DARK);
  game.drawRoundRect(5, 172, 310, 28, 6, PANEL_LIGHT);
  game.setTextColor(TFT_WHITE, ACCENT_DARK);
  game.drawCentreString(price, 160, 179, 1);
}


static void touchMarketSellModal(int x, int y) {
  if (marketSellConfirm) {
    if (y >= 172 && y < 200) {
      if (x < 160) {
        marketSellConfirm = false;         // BACK — nothing sold
      } else {
        if (marketSellMode == 1) sellOneFlower(marketSellStrain);
        else if (marketSellMode == 2) sellAllFlowers(marketSellStrain);
        else if (marketSellMode == 3) sellOneHash(marketSellStrain);
        else sellAllHash(marketSellStrain);
        marketSellConfirm = false;
        marketSellRefresh();
        if (!marketSellCount) marketSellMode = 0;
      }
    } else {
      marketSellConfirm = false;           // tap anywhere else cancels
    }
    render();
    return;
  }
  if (y >= 61 && y < 87 && x >= 242) {
    marketSellMode = 0;                    // BACK, top-right of the box
    marketSellConfirm = false;
    render();
  } else if (y >= 96 && y < 168) {
    if (x < 57) {   // giant < arrow: previous stashed strain
      marketSellStrain = marketSellMode <= 2 ? nextStashStrain(marketSellStrain, -1)
                                             : nextHashStrain(marketSellStrain, -1);
      redrawSellStrainCard();
    } else if (x >= 253) {   // giant > arrow: next stashed strain
      marketSellStrain = marketSellMode <= 2 ? nextStashStrain(marketSellStrain, 1)
                                             : nextHashStrain(marketSellStrain, 1);
      redrawSellStrainCard();
    }
  } else if (y >= 172 && y < 200) {
    marketSellConfirm = true;              // arm the ARE YOU SURE? panel
    render();
  }
}


// ---- Hash room wash picker -------------------------------------------------
// One run washes 2 trimmed flower jars from a stash strain into 1 hash unit
// of the same strain, blended with that strain's existing hash quality.

static void runWash() {
  uint8_t s = washStrain;
  if (s >= STRAIN_COUNT || stashByStrain[s] < 2) return;
  stashByStrain[s] -= 2;
  hashQualityByStrain[s] = blendQuality(hashQualityByStrain[s], hashByStrain[s],
                                        stashQualityByStrain[s], 2);
  hashByStrain[s] += 1;
  washOpen = false;
  washConfirm = false;
  saveGame();
  hashAnimationRequested = true;   // play the hash-wash cutscene
  lastFarmerVisitAt = millis();
}

// Strain card body shared by the full modal draw and the arrow-only refresh.
static void washCardBody() {
  uint8_t s = washStrain;
  game.fillRoundRect(57, 96, 196, 72, 8, PANEL);
  game.drawRoundRect(57, 96, 196, 72, 8, ACCENT);
  int nameFont = strlen(strainNames[s]) > 14 ? 1 : 2;
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(strainNames[s], 155, nameFont == 2 ? 102 : 106, nameFont);
  game.setTextColor(GOLD, PANEL);
  char info[44];
  snprintf(info, sizeof(info), "x%u jars - $%u each", stashByStrain[s], flowerUnitPrice(s));
  game.drawCentreString(info, 155, 122, 1);
  game.setTextColor(TEXT_DIM, PANEL);
  snprintf(info, sizeof(info), "SELL 2 JARS: $%u", (uint32_t)2 * flowerUnitPrice(s));
  game.drawCentreString(info, 155, 136, 1);
  snprintf(info, sizeof(info), "WASHED AS HASH: $%u", hashUnitPrice(s));
  game.drawCentreString(info, 155, 148, 1);
}

static void drawWashModal() {
  game.fillRoundRect(3, 58, 314, 148, 8, PANEL);
  game.drawRoundRect(3, 58, 314, 148, 8, PANEL_LIGHT);
  drawButton(242, 61, 72, 26, "BACK", PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("WASH TRIMMED FLOWER", 120, 64, 1);
  bool anyWashable = false;
  for (uint8_t i = 0; i < STRAIN_COUNT; ++i)
    if (stashByStrain[i] >= 2) { anyWashable = true; break; }
  if (!anyWashable) {
    game.setTextColor(TEXT_DIM, PANEL);
    game.drawCentreString("Need 2+ jars of a strain", 160, 120, 1);
    game.drawCentreString("Harvest FLOWER and trim first", 160, 134, 1);
    return;
  }
  if (washConfirm) {
    game.setTextColor(TEXT_MAIN, PANEL);
    game.drawCentreString("ARE YOU SURE?", 160, 102, 2);
    char detail[48];
    snprintf(detail, sizeof(detail), "Wash 2 %s jars into 1 hash?", strainNames[washStrain]);
    game.drawCentreString(detail, 160, 128, 1);
    drawButton(5, 172, 150, 28, "BACK", PANEL_LIGHT);
    drawButton(163, 172, 152, 28, "YES, WASH IT", RED, TFT_WHITE);
    return;
  }
  // One giant strain card flanked by big < > arrows, sell-modal style.
  drawButton(5, 96, 48, 72, "<", ACCENT_DARK);
  drawButton(257, 96, 48, 72, ">", ACCENT_DARK);
  washCardBody();
  // Full-width WASH bar under the card.
  drawButton(5, 172, 310, 28, "WASH 2 JARS -> 1 HASH", ACCENT_DARK, TFT_WHITE);
}

// Arrow feedback without a full-page render.
static void redrawWashCard() { washCardBody(); }

static void touchWashModal(int x, int y) {
  if (washConfirm) {
    if (y >= 172 && y < 200) {
      if (x < 160) {
        washConfirm = false;             // BACK — nothing washed
        render();
      } else {
        runWash();
        render();
      }
    }
    return;
  }
  if (y >= 61 && y < 87 && x >= 242) {
    washOpen = false;                    // BACK, top-right of the box
    washConfirm = false;
    render();
  } else if (y >= 96 && y < 168) {
    if (x < 57) {                        // giant < arrow
      washStrain = nextWashStrain(washStrain, -1);
      redrawWashCard();
    } else if (x >= 253) {               // giant > arrow
      washStrain = nextWashStrain(washStrain, 1);
      redrawWashCard();
    }
  } else if (y >= 172 && y < 200) {
    washConfirm = true;                  // arm the ARE YOU SURE? panel
    render();
  }
}

void prepareDemoHarvest() {
  plantActive = true;
  plantVariant = (uint16_t)random(65536);  // fresh demo pheno
  growth = 100;
  hydration = 100;
  vigor = 100;
  pestType = 0;
  pestSeverity = 0;
  page = GARDEN;
  saveGame();
}

void resetGame() {
  preferences.clear();
  newGame();
  resetArmed = false;
}

void drawWateringFrame(int frame) {
  // The selected farmer crosses the room in the shared branded outfit.
  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);
  uint16_t can = rgb565(202, 169, 73);
  uint16_t water = rgb565(76, 184, 230);
  int x = 136;

  game.fillRect(x + 11, 145, 5, 17, pants);
  game.fillRect(x + 22, 145, 5, 17, pants);
  game.fillRoundRect(x + 6, 124, 27, 25, 6, shirt);
  game.fillRoundRect(x + 2, 126, 8, 20, 3, shirt);
  game.fillRoundRect(x + 29, 126, 8, 20, 3, shirt);
  game.drawFastHLine(x + 2, 144, 8, rgb565(28, 87, 48));
  game.drawFastHLine(x + 29, 144, 8, rgb565(28, 87, 48));
  game.drawLine(x + 8, 130, x - 5, 126, shirt);
  game.drawLine(x - 5, 126, x - 11, 124, skin);
  game.drawLine(x + 33, 135, x + 37, 136, shirt);
  game.drawLine(x + 37, 136, x + 40, 137, skin);
  drawLeafLogo(x + 8, 125, 1, TFT_WHITE);
  drawFarmerHeadLarge(x + 20, 112, farmerCharacter);

  int canX = x - 20;
  game.fillRoundRect(canX, 120, 18, 12, 3, can);
  game.drawRoundRect(canX, 120, 18, 12, 3, rgb565(111, 92, 49));
  game.drawLine(canX + 3, 120, canX + 7, 116, can);
  game.drawLine(canX + 7, 116, canX + 12, 120, can);
  game.drawLine(canX, 124, canX - 8, 121, can);

  int targetX = 22 + frame * 18;
  game.drawLine(canX - 8, 121, targetX, 146, water);
  for (int i = 0; i < 4; ++i) {
    int dx = ((frame + i) % 4) * 3 - 4;
    int dy = ((frame * 2 + i) % 5) * 3;
    game.fillCircle(targetX + dx, 143 + dy, 1, water);
  }
}

void startWateringAnimation() {
  // Non-blocking: draw the base scene and frame 0 now; the loop steps the
  // remaining frames so taps and nav stay live throughout.
  farmerOverlayActive = false;  // the base redraw wipes the farmer anyway
  waterAnimActive = true;
  waterAnimFrame = 1;
  game.fillScreen(BG);
  drawHeader();
  drawPlant();
  drawGardenSidePanel();
  drawGardenActions();
  drawNav();
  game.setTextColor(GOLD, BG);
  game.drawString("WATERING THE ROOM", 12, 37, 1);
  drawWateringFrame(0);
  presentFrame();
  waterAnimNextAt = millis() + 420;
}

void stepWateringAnimation() {
  if (page != GARDEN) { waterAnimActive = false; return; }  // navigated away
  if ((long)(millis() - waterAnimNextAt) < 0) return;
  if (waterAnimFrame < 8) {
    drawWateringFrame(waterAnimFrame++);
    presentFrame();
    waterAnimNextAt = millis() + 420;
  } else {
    waterAnimActive = false;
    render();   // clean full garden redraw; frame trails gone
  }
}

void drawFarmerInspector(int pose) {
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);
  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  // Fullscreen: standing on the right. Garden view: standing just left of the
  // stats panel, as if he walked out from behind it.
  const int cx = page == PLANT_VIEW ? 274 : 152;
  const int footY = page == PLANT_VIEW ? 207 : 168;
  int torsoW = farmerCharacter == 4 ? 34 : 28;
  int torsoH = farmerCharacter == 4 ? 36 : 41;
  game.fillRect(cx - 9, footY - 18, 6, 18, pants);
  game.fillRect(cx + 3, footY - 18, 6, 18, pants);
  game.fillRoundRect(cx - torsoW / 2, footY - 18 - torsoH, torsoW, torsoH, 6, shirt);
  game.fillRoundRect(cx - torsoW / 2 - 4, footY - 18 - torsoH + 5, 6, torsoH - 8, 3, shirt);
  game.fillRoundRect(cx + torsoW / 2 - 2, footY - 18 - torsoH + 5, 6, torsoH - 8, 3, shirt);
  drawLeafLogo(cx - 12, footY - 18 - torsoH + 3, 1, TFT_WHITE);
  game.drawLine(cx - torsoW / 2 - 3, footY - 31, cx - torsoW / 2, footY - 24, skin);
  game.drawLine(cx + torsoW / 2 + 1, footY - 31, cx + torsoW / 2 - 2, footY - 24, skin);
  drawFarmerHeadLarge(cx, footY - 59, farmerCharacter);
}

void drawFarmerSpeechBubble(const FarmerSaying &saying) {
  bool zoomed = page == PLANT_VIEW;
  int bubbleX = zoomed ? 181 : 59;
  int bubbleY = zoomed ? 80 : 37;
  int tailY = bubbleY + 43;
  int apexX = bubbleX + 79;   // tail points at the farmer's head
  game.fillTriangle(apexX - 10, tailY - 2, apexX + 6, tailY - 2, apexX, tailY + 13, TFT_WHITE);
  game.drawTriangle(apexX - 10, tailY - 2, apexX + 6, tailY - 2, apexX, tailY + 13, PANEL_LIGHT);
  game.fillRoundRect(bubbleX, bubbleY, 132, 43, 7, TFT_WHITE);
  game.drawRoundRect(bubbleX, bubbleY, 132, 43, 7, PANEL_LIGHT);
  game.setTextColor(0x0000, TFT_WHITE);
  game.drawCentreString(saying.line1, bubbleX + 66, bubbleY + (saying.line2[0] ? 6 : 15), 1);
  if (saying.line2[0]) game.drawCentreString(saying.line2, bubbleX + 66, bubbleY + 22, 1);
}

void startFarmerOverlay() {
  // Non-blocking: draw the farmer and bubble once; refreshNearbyPopup()
  // re-stamps them only after a repaint and restores the art when he leaves.
  farmerOverlaySaying = farmerSayings[random(20)];
  farmerOverlayActive = true;
  farmerOverlayUntil = millis() + 4200UL;
  overlayDirty = false;
  drawFarmerInspector(2);
  drawFarmerSpeechBubble(farmerOverlaySaying);
  presentFrame();
}

// The covered regions are repainted into the frame buffer and pushed in one
// atomic pass, so the buffer and the panel always agree again afterwards.
void restoreFarmerVisitArt() {
  if (page == GARDEN) {
    drawPlant();             // repaint the covered art region into the buffer
    drawGardenZoomChip();    // the bubble covered the chip's right edge
    drawGardenSidePanel();   // ...and reached into the stats panel
  } else if (page == PLANT_VIEW) {
    restoreFullScreenFallbackRegion();
  } else {
    render();   // page changed mid-overlay: a clean repaint clears him
    return;
  }
  presentFrame();
}

void beginCutsceneScene() {
  // Cutscenes own the display until they finish; no tabs or prior menu are
  // drawn underneath. Frame updates are confined to the scene body.
  game.fillScreen(BG);
  drawHeader();
  presentFrame();
}

void waitForCutsceneContinue() {
  game.fillRect(78, 216, 164, 18, BG);
  game.setTextColor(GOLD, BG);
  game.drawCentreString("TAP TO CONTINUE", 160, 220, 1);
  presentFrame();
  while (touchscreen.tirqTouched() && touchscreen.touched()) pumpDelay(12);
  while (!(touchscreen.tirqTouched() && touchscreen.touched())) pumpDelay(12);
  while (touchscreen.tirqTouched() && touchscreen.touched()) pumpDelay(12);
}

void drawSmokeCutsceneFrame(int frame) {
  game.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  game.fillRoundRect(13, 38, 294, 158, 10, PANEL);
  game.drawRoundRect(13, 38, 294, 158, 10, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("SMOKE BREAK", 160, 47, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString(smokeAccessoryUsed == 4 ? "JOINT + FLOWER" :
                          (smokeProductUsed == 2 ? "BONG + HASH" : "BONG + FLOWER"), 160, 70, 1);

  uint16_t glass = rgb565(82, 191, 173);
  uint16_t glassDark = rgb565(34, 112, 105);
  uint16_t item = rgb565(222, 213, 174);
  if (smokeAccessoryUsed == 1) {
    game.fillRoundRect(65, 122, 34, 39, 10, glassDark);
    game.fillRoundRect(69, 127, 26, 30, 8, glass);
    game.fillRect(78, 91, 8, 35, glassDark);
    game.fillRect(80, 87, 4, 14, glass);
    game.drawLine(83, 112, 96, 104, glassDark);
    game.fillRoundRect(91, 100, 12, 7, 2, rgb565(105, 76, 47));
    game.drawFastHLine(72, 145, 21, rgb565(140, 231, 218));
  } else if (smokeAccessoryUsed == 4) {
    // Hand-rolled joint: a slim paper cone with a glowing tip.
    game.drawLine(72, 149, 125, 131, item);
    game.drawLine(73, 152, 126, 134, rgb565(169, 144, 104));
    game.drawLine(76, 151, 77, 146, rgb565(105, 76, 47));
    game.fillCircle(126, 132, 3, GOLD);
    game.fillCircle(127, 131, 1, TFT_YELLOW);
  } else {
    game.fillRoundRect(74, 128, 53, 34, 4, item);
    game.drawRoundRect(74, 128, 53, 34, 4, GOLD);
    game.drawFastHLine(80, 138, 40, rgb565(169, 144, 104));
    game.drawFastHLine(80, 145, 34, rgb565(169, 144, 104));
    game.drawFastHLine(80, 152, 38, rgb565(169, 144, 104));
  }

  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);
  int cx = 226;
  int footY = 164;
  game.fillRect(cx - 12, footY - 22, 8, 22, pants);
  game.fillRect(cx + 4, footY - 22, 8, 22, pants);
  game.fillRoundRect(cx - 23, footY - 62, 46, 42, 8, shirt);
  game.fillRoundRect(cx - 29, footY - 55, 11, 32, 4, shirt);
  game.fillRoundRect(cx + 18, footY - 55, 11, 32, 4, shirt);
  drawLeafLogo(cx - 12, footY - 58, 1, TFT_WHITE);
  game.drawLine(cx - 19, footY - 53, 160, 117, shirt);
  game.drawLine(160, 117, 156, 119, skin);
  game.drawLine(cx + 23, footY - 48, cx + 25, footY - 24, shirt);
  game.drawLine(cx + 25, footY - 24, cx + 27, footY - 22, skin);
  int headY = footY - 78;
  drawFarmerHeadLarge(cx, headY, farmerCharacter);

  for (int puff = 0; puff < frame; ++puff) {
    int px = 108 + puff * 12;
    int py = 105 - (puff % 2) * 8;
    game.drawCircle(px, py, 3 + (puff % 3), TEXT_DIM);
    game.drawCircle(px + 4, py - 3, 2 + (puff % 2), 0xBDF7);
  }
  game.setTextColor(GOLD, PANEL);
  game.drawCentreString(frame >= 3 ? "GARDEN GLOW ACTIVATED" : "A LITTLE ROOM BOOST", 160, 176, 1);
}



// Scaled copy of drawFarmerHeadLarge for the intro close-up (scale 1 = normal).
void drawFarmerHeadScaled(int cx, int cy, uint8_t character, int s) {
#define SP(v) ((v) * (s))
  uint16_t hair = character == 3 ? rgb565(210, 176, 104) : rgb565(35, 27, 26);
  uint16_t skin = character == 2 ? rgb565(133, 84, 61)
                : character == 6 ? rgb565(172, 112, 78)
                : character == 7 ? rgb565(216, 156, 116)
                : rgb565(230, 184, 143);
  uint16_t beard = rgb565(52, 37, 33);
  uint16_t black = rgb565(16, 19, 22);

  if (character == 5) {
    uint16_t jHair = rgb565(124, 110, 97), jLight = rgb565(168, 156, 142), jDark = rgb565(88, 76, 66);
    game.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(34), min(SP(10), SP(16)), jHair);
    game.fillCircle(cx, cy, SP(10), skin);
    game.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), jHair);
    game.fillRect(cx + SP(-14), cy + SP(-6), SP(5), SP(20), jHair);
    game.fillRect(cx + SP(9), cy + SP(-6), SP(5), SP(20), jHair);
    game.drawLine(cx + SP(-9), cy + SP(-11), cx + SP(9), cy + SP(-13), jLight);
    game.drawLine(cx + SP(-10), cy + SP(-3), cx + SP(-10), cy + SP(9), jLight);
    game.drawLine(cx + SP(10), cy + SP(-3), cx + SP(10), cy + SP(9), jDark);
    game.drawFastHLine(cx + SP(-6), cy + SP(-4), SP(4), jDark);
    game.drawFastHLine(cx + 2, cy + SP(-4), SP(4), jDark);
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    game.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    game.drawFastHLine(cx + SP(-4), cy + SP(5), SP(8), rgb565(150, 90, 80));
    uint16_t jGoatee = rgb565(152, 146, 140);
    game.drawFastHLine(cx + SP(-4), cy + SP(3), SP(8), jGoatee);
    game.drawFastHLine(cx + SP(-3), cy + SP(2), SP(6), jGoatee);
    game.drawFastVLine(cx + SP(-5), cy + SP(3), SP(4), jGoatee);
    game.drawFastVLine(cx + SP(4), cy + SP(3), SP(4), jGoatee);
    game.fillRoundRect(cx + SP(-3), cy + SP(6), SP(7), SP(6), min(SP(2), SP(3)), jGoatee);
    return;
  }

  if (character == 1) {
    uint16_t mHair = rgb565(96, 64, 40), mHairLight = rgb565(134, 95, 60);
    game.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(31), min(SP(10), SP(15)), mHair);
    game.fillCircle(cx, cy, SP(10), skin);
    game.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), mHair);
    game.fillRect(cx + SP(-14), cy + SP(-6), SP(5), SP(23), mHair);
    game.fillRect(cx + SP(9), cy + SP(-6), SP(5), SP(23), mHair);
    game.drawFastVLine(cx + SP(-12), cy + SP(-2), SP(18), mHairLight);
    game.drawFastVLine(cx + SP(-11), cy + SP(2), SP(15), mHairLight);
    game.drawFastVLine(cx + SP(12), cy + SP(-1), SP(16), mHairLight);
    game.drawFastVLine(cx + SP(11), cy + SP(4), SP(13), mHairLight);
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    game.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    game.fillRoundRect(cx + SP(-7), cy + SP(4), SP(14), SP(5), min(SP(2), SP(2)), rgb565(110, 74, 48));
    game.fillRect(cx + SP(-1), cy + SP(4), SP(2), SP(2), skin);
    return;
  }

  if (character == 6) {
    uint16_t buzz = rgb565(45, 32, 26);
    game.fillCircle(cx, cy + SP(-2), SP(11), buzz);             // buzz crescent
    game.fillCircle(cx, cy, SP(10), skin);                      // face carves it back
    for (int i = -8; i <= 8; i += 4)
      game.drawFastVLine(cx + SP(i), cy + SP(-12), SP(3), rgb565(74, 55, 42));
    game.fillRect(cx + SP(-10), cy + SP(-5), SP(2), SP(2), buzz);
    game.fillRect(cx + SP(8), cy + SP(-5), SP(2), SP(2), buzz);
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), rgb565(206, 226, 232));
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), rgb565(206, 226, 232));
    game.drawRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(-4), cy + SP(4), SP(8), beard);
    game.fillRect(cx + SP(-2), cy + SP(7), SP(4), SP(3), beard);
    return;
  }

  if (character == 7) {
    uint16_t vHair = rgb565(58, 38, 26), vCurl = rgb565(94, 64, 42);
    game.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(30), min(SP(10), SP(14)), vHair);
    game.fillCircle(cx, cy, SP(10), skin);
    game.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), vHair);
    game.fillRect(cx + SP(-14), cy + SP(-6), SP(5), SP(21), vHair);
    game.fillRect(cx + SP(9), cy + SP(-6), SP(5), SP(21), vHair);
    for (int i = -12; i <= 12; i += 6)
      game.fillCircle(cx + SP(i), cy + SP(-15) + abs(i) / 3 * s / 3, s + s / 2, vCurl);
    game.fillCircle(cx + SP(-12), cy + SP(-2), s + s / 2, vCurl);
    game.fillCircle(cx + SP(12), cy + SP(1), s + s / 2, vCurl);
    game.fillCircle(cx + SP(-12), cy + SP(5), s + s / 2, vCurl);
    game.fillCircle(cx + SP(12), cy + SP(7), s + s / 2, vCurl);
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    game.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    return;
  }

  game.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(36), min(SP(10), SP(17)), hair);
  if (character == 2) {
    for (int i = -2; i <= 2; ++i) {
      game.drawFastVLine(cx + SP(i * 5), cy + SP(-12), SP(36), rgb565(65, 44, 35));
      game.drawFastVLine(cx + SP(i * 5 + 2), cy + SP(-10), SP(33), hair);
    }
  }
  game.fillCircle(cx, cy, SP(10), skin);

  if (character == 0) {
    game.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(8), min(SP(6), SP(11)), hair);
    game.fillRect(cx + SP(-13), cy + SP(-6), SP(5), SP(28), hair);
    game.fillRect(cx + SP(8), cy + SP(-6), SP(5), SP(28), hair);
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    game.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    game.drawFastHLine(cx + SP(-7), cy + SP(4), SP(14), beard);
    game.drawFastHLine(cx + SP(-5), cy + SP(7), SP(10), TFT_WHITE);
    game.fillTriangle(cx + SP(-6), cy + SP(9), cx + SP(6), cy + SP(9), cx, cy + SP(16), beard);
  } else if (character == 2) {
    game.fillRoundRect(cx + SP(-10), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), TFT_BLACK);
    game.fillRoundRect(cx + SP(1), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), TFT_BLACK);
    game.drawLine(cx + SP(-1), cy + SP(-2), cx + SP(1), cy + SP(-2), black);
    game.drawLine(cx + SP(-10), cy + SP(-5), cx + SP(-1), cy + SP(-5), black);
    game.drawLine(cx + SP(1), cy + SP(-5), cx + SP(10), cy + SP(-5), black);
    game.fillTriangle(cx + SP(-8), cy + SP(4), cx + SP(8), cy + SP(4), cx, cy + SP(17), beard);
    game.drawFastHLine(cx + SP(-4), cy + SP(6), SP(8), TFT_WHITE);
  } else if (character == 8) {
    // Pedro: same head as drawFarmerHeadLarge's Pedro branch, scaled.
    uint16_t pHair = rgb565(30, 26, 24), pSkin = rgb565(212, 168, 138);
    uint16_t pBeard = rgb565(62, 52, 47);
    game.fillRoundRect(cx + SP(-12), cy + SP(-12), SP(24), SP(24), min(SP(8), SP(12)), pHair);
    game.fillCircle(cx, cy, SP(10), pSkin);
    game.fillRoundRect(cx + SP(-11), cy + SP(-11), SP(22), SP(5), min(SP(4), SP(7)), pHair);
    game.fillRect(cx + SP(-11), cy + SP(-6), SP(2), SP(6), pHair);
    game.fillRect(cx + SP(9), cy + SP(-6), SP(2), SP(6), pHair);
    game.fillRoundRect(cx + SP(-9), cy + SP(2), SP(18), SP(13), min(SP(6), SP(10)), pBeard);
    game.drawFastHLine(cx + SP(-8), cy + SP(4), SP(5), rgb565(122, 114, 106));
    game.drawFastHLine(cx + SP(3), cy + SP(5), SP(5), rgb565(122, 114, 106));
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(2), SP(5)), black);
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(2), SP(5)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawLine(cx + SP(-8), cy + SP(-4), cx + SP(-5), cy + SP(-2), rgb565(95, 95, 102));
  } else if (character == 4) {
    game.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), hair);
    game.fillRoundRect(cx + SP(-15), cy + SP(-4), SP(5), SP(22), min(SP(3), 2 * s), hair);
    game.fillRoundRect(cx + SP(10), cy + SP(-4), SP(5), SP(22), min(SP(3), 2 * s), hair);
    game.fillRoundRect(cx + SP(-10), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), rgb565(10, 13, 16));
    game.fillRoundRect(cx + SP(1), cy + SP(-5), SP(9), SP(7), min(SP(2), SP(3)), rgb565(10, 13, 16));
    game.drawFastHLine(cx + SP(-1), cy + SP(-2), SP(2), rgb565(10, 13, 16));
    game.drawFastHLine(cx + SP(-9), cy + SP(-4), SP(7), rgb565(96, 107, 111));
    game.drawFastHLine(cx + SP(2), cy + SP(-4), SP(7), rgb565(96, 107, 111));
    game.drawFastHLine(cx + SP(-4), cy + SP(6), SP(8), rgb565(112, 65, 52));
  } else if (character == 9) {
    uint16_t bHair = rgb565(146, 114, 70), bLight = rgb565(184, 152, 100),
             bDark = rgb565(112, 84, 52), bBeard = rgb565(66, 46, 32);
    game.fillRoundRect(cx + SP(-14), cy + SP(-14), SP(28), SP(32), min(SP(10), SP(16)), bHair);
    game.fillCircle(cx, cy, SP(10), skin);
    game.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(9), min(SP(6), SP(11)), bHair);
    for (int i = 0; i < 3; ++i) {
      int y = cy + SP(-8 + i * 9);
      game.fillRoundRect(cx + SP(-16), y, SP(4), SP(9), min(SP(2), SP(3)),
                         i % 2 ? bLight : bDark);
      game.fillRoundRect(cx + SP(12), y + SP(3), SP(4), SP(9), min(SP(2), SP(3)),
                         i % 2 ? bDark : bLight);
    }
    game.fillCircle(cx + SP(-12), cy + SP(19), SP(2), bLight);
    game.fillCircle(cx + SP(12), cy + SP(20), SP(2), bDark);
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    game.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    game.fillRoundRect(cx + SP(-7), cy + SP(4), SP(14), SP(4), min(SP(2), SP(3)), bBeard);
    game.fillRect(cx + SP(-1), cy + SP(4), SP(2), SP(2), skin);
    game.fillRect(cx + SP(-3), cy + SP(8), SP(7), SP(3), bBeard);
    for (int i = -2; i <= 2; i += 2)
      game.drawPixel(cx + SP(i), cy + SP(7), bBeard);
  } else {
    game.fillRoundRect(cx + SP(-12), cy + SP(-14), SP(24), SP(8), min(SP(6), SP(11)), hair);
    game.fillRect(cx + SP(-13), cy + SP(-6), SP(5), SP(29), hair);
    game.fillRect(cx + SP(8), cy + SP(-6), SP(5), SP(29), hair);
    game.fillRoundRect(cx + SP(-10), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.fillRoundRect(cx + SP(1), cy + SP(-6), SP(9), SP(8), min(SP(1), SP(4)), black);
    game.drawFastHLine(cx + SP(-1), cy + SP(-4), SP(2), black);
    game.drawFastHLine(cx + SP(-14), cy + SP(-5), SP(4), black);
    game.drawFastHLine(cx + SP(10), cy + SP(-5), SP(4), black);
    game.drawLine(cx + SP(-8), cy + SP(-3), cx + SP(-5), cy + SP(-1), rgb565(70, 90, 100));
    game.drawFastVLine(cx + SP(-11), cy + SP(2), SP(15), rgb565(239, 216, 151));
    game.drawFastHLine(cx + SP(-4), cy + SP(6), SP(8), rgb565(167, 83, 78));
    game.drawFastHLine(cx + SP(-3), cy + SP(8), SP(6), TFT_WHITE);
  }
#undef SP
}

void drawGownUpCutsceneFrame(int frame) {
  bool bearded = farmerCharacter == 0 || farmerCharacter == 2 || farmerCharacter == 5 ||
                  farmerCharacter == 6 || farmerCharacter == 9;
  // Only the opening frame gets text; the rest of the tour speaks for itself.
  const char *captions[] = {
    "New hire!", "", "", "", "", "", "", ""
  };
  game.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  game.fillRect(0, NAV_Y, SCREEN_W, SCREEN_H - NAV_Y, BG);  // clear caption strip
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);

  int cx, footY, cy;
  if (frame <= 3) {
    // ---- Extreme close-up: slow push-in on the grower's face ----
    static const int zoomS[] = {3, 4, 5, 5};
    static const int zoomCy[] = {142, 136, 128, 126};
    for (int y = HEADER_H; y < NAV_Y; ++y) {
      float blend = (float)(y - HEADER_H) / (NAV_Y - HEADER_H);
      uint8_t v = 34 + (uint8_t)(blend * 22);
      game.drawBandHLine(0, y, SCREEN_W, rgb565(v - 4, v + 8, v + 4));
    }
    cx = 160;
    cy = zoomCy[frame];
    drawFarmerHeadScaled(cx, cy, farmerCharacter, zoomS[frame]);
    game.setTextColor(TEXT_DIM, BG);
    game.drawCentreString(captions[min(frame, 9)], 160, 212, 1);
    return;
  } else if (frame <= 5) {
    // ---- Locker room: hairnet, then beard net ----
    for (int y = HEADER_H; y < 172; ++y) {
      float blend = (float)(y - HEADER_H) / 144.0f;
      uint8_t v = 30 + (uint8_t)(blend * 26);
      game.drawBandHLine(0, y, SCREEN_W, rgb565(v - 4, v + 8, v + 4));
    }
    game.fillRect(0, 172, SCREEN_W, NAV_Y - 172, rgb565(38, 46, 42));
    game.drawFastHLine(0, 172, SCREEN_W, rgb565(58, 70, 60));
    for (int i = 0; i < 6; ++i) {  // lockers along the back wall
      int lx = 14 + i * 49;
      game.fillRoundRect(lx, 46, 42, 110, 4, rgb565(52, 74, 78));
      game.drawRoundRect(lx, 46, 42, 110, 4, rgb565(70, 96, 100));
      for (int v = 0; v < 3; ++v)
        game.drawFastHLine(lx + 10, 62 + v * 5, 22, rgb565(40, 58, 62));
      game.fillRect(lx + 30, 96, 4, 12, rgb565(140, 150, 140));
    }
    cx = 160;
    footY = 174;
    game.fillRoundRect(cx - 11, footY - 2, 22, 3, 1, rgb565(30, 38, 34));
    game.fillRect(cx - 10, footY - 22, 6, 22, pants);
    game.fillRect(cx + 4, footY - 22, 6, 22, pants);
    cy = footY - 78;
  } else if (frame == 6) {
    // ---- Walking down the hallway toward the grow-room door ----
    for (int y = HEADER_H; y < 172; ++y) {
      float blend = (float)(y - HEADER_H) / 144.0f;
      uint8_t v = 30 + (uint8_t)(blend * 26);
      game.drawBandHLine(0, y, SCREEN_W, rgb565(v - 4, v + 8, v + 4));
    }
    game.fillRect(0, 172, SCREEN_W, NAV_Y - 172, rgb565(38, 46, 42));
    game.drawFastHLine(0, 172, SCREEN_W, rgb565(58, 70, 60));
    game.fillRect(252, 44, 52, 128, rgb565(12, 40, 22));  // open doorway
    game.fillRect(256, 48, 44, 120, rgb565(24, 88, 44));
    for (int i = 0; i < 3; ++i) {
      int px = 262 + i * 14;
      game.fillRect(px, 128, 2, 30, rgb565(14, 50, 26));
      game.fillCircle(px + 1, 124, 5, rgb565(20, 70, 36));
    }
    game.drawRoundRect(250, 42, 56, 132, 4, rgb565(90, 110, 96));
    game.fillTriangle(256, 172, 300, 172, 236, NAV_Y - 1, rgb565(26, 74, 40));

    cx = 196;
    footY = 173;  // mid-stride bob
    game.fillRoundRect(cx - 11, footY - 2, 22, 3, 1, rgb565(30, 38, 34));
    game.fillRect(cx - 11, footY - 20, 6, 18, pants);  // stride legs
    game.fillRect(cx + 5, footY - 22, 6, 22, pants);
    for (int i = 0; i < 3; ++i)  // motion dashes behind the farmer
      game.drawFastHLine(cx - 26 - i * 6, footY - 14 - i * 6, 6,
                           rgb565(120, 140, 130));
    cy = footY - 78;
  } else {
    // ---- Grow room interior: rows of plants under hanging lights ----
    for (int y = HEADER_H; y < 204; ++y) {
      float blend = (float)(y - HEADER_H) / 176.0f;
      uint8_t r = 5 + (uint8_t)(blend * 10);
      uint8_t g = 19 + (uint8_t)(blend * 30);
      uint8_t b = 21 + (uint8_t)(blend * 20);
      game.drawBandHLine(0, y, SCREEN_W, rgb565(r, g, b));
    }
    game.drawFastHLine(0, 70, SCREEN_W, rgb565(23, 44, 42));
    game.drawFastHLine(0, 118, SCREEN_W, rgb565(23, 44, 42));
    game.fillRect(0, 204, SCREEN_W, NAV_Y - 204, rgb565(32, 49, 39));
    game.drawFastHLine(0, 204, SCREEN_W, rgb565(76, 91, 61));
    for (int i = 0; i < 3; ++i) {  // hanging grow lights
      int lx = 34 + i * 104;
      game.fillRect(lx, 34, 44, 5, rgb565(70, 82, 88));
      game.drawFastHLine(lx + 2, 40, 40, GOLD);
      game.drawFastHLine(lx + 8, 42, 28, rgb565(180, 170, 90));
    }
    bool savedActive = plantActive;  // borrowed by the live plant drawing
    uint8_t savedGrowth = growth;
    plantActive = true;
    growth = 55;  drawMiniRoomPlant(50, 150, 0.55f);
    growth = 75;  drawMiniRoomPlant(105, 150, 0.60f);
    growth = 40;  drawMiniRoomPlant(215, 150, 0.55f);
    growth = 90;  drawMiniRoomPlant(270, 150, 0.60f);
    growth = 100; drawMiniRoomPlant(45, 196, 0.95f);
    growth = 100; drawMiniRoomPlant(120, 198, 1.05f);
    growth = 100; drawMiniRoomPlant(200, 198, 1.00f);
    growth = 100; drawMiniRoomPlant(275, 196, 0.95f);
    plantActive = savedActive;
    growth = savedGrowth;

    cx = 160;
    footY = 202;
    game.fillRoundRect(cx - 11, footY - 2, 22, 3, 1, rgb565(26, 40, 32));
    game.fillRect(cx - 10, footY - 22, 6, 22, pants);
    game.fillRect(cx + 4, footY - 22, 6, 22, pants);
    cy = footY - 78;
  }

  game.fillRoundRect(cx - 14, footY - 62, 28, 42, 7, shirt);
  game.fillRoundRect(cx - 19, footY - 55, 6, 32, 3, shirt);
  game.fillRoundRect(cx + 13, footY - 55, 6, 32, 3, shirt);
  drawLeafLogo(cx - 12, footY - 58, 1, TFT_WHITE);
  drawFarmerHeadLarge(cx, cy, farmerCharacter);

  if (frame >= 4) {  // hairnet goes on in the locker room and stays on
    uint16_t net = rgb565(238, 238, 238), mesh = rgb565(185, 190, 195);
    game.fillRoundRect(cx - 13, cy - 17, 26, 14, 7, net);
    for (int i = -9; i <= 9; i += 6) game.drawFastVLine(cx + i, cy - 16, 11, mesh);
    game.drawFastHLine(cx - 12, cy - 13, 24, mesh);
    game.drawFastHLine(cx - 13, cy - 9, 26, mesh);
    game.drawFastHLine(cx - 13, cy - 4, 26, mesh);
  }
  if (frame >= 5 && bearded) {  // beard net after the hairnet
    uint16_t net = rgb565(238, 238, 238), mesh = rgb565(185, 190, 195);
    game.fillRoundRect(cx - 8, cy + 2, 16, 15, 5, net);
    game.drawFastVLine(cx - 3, cy + 3, 13, mesh);
    game.drawFastVLine(cx + 3, cy + 3, 13, mesh);
    game.drawLine(cx - 8, cy + 4, cx - 13, cy - 1, mesh);
    game.drawLine(cx + 8, cy + 4, cx + 13, cy - 1, mesh);
  }
  if (captions[min(frame, 7)][0]) {
    game.setTextColor(TEXT_DIM, BG);
    game.drawCentreString(captions[min(frame, 7)], 160, 212, 1);
  }
}

// Bare cutscene chrome matching the SD intro art: an empty header bar with
// no wordmark, grower name, money or status text.
void beginBareHeaderScene() {
  game.fillScreen(BG);
  game.fillRect(0, 0, SCREEN_W, HEADER_H, 0x0861);
  game.drawFastHLine(0, HEADER_H - 1, SCREEN_W, 0x4D89);
  presentFrame();
}

void animateGownUpCutscene() {
  beginBareHeaderScene();
  for (int frame = 0; frame < 8; ++frame) {
    drawGownUpCutsceneFrame(frame);
    presentFrame();
    // Close-up push-in moves faster; the tour frames linger a little.
    pumpDelay(frame <= 3 ? 600 : 1400);  // whole intro lands near 8 seconds
  }
  waitForCutsceneContinue();
}

void animateSmokeBreak() {
  // The bong hit carries the scene: the groove sits out, the sample plays,
  // and the garden theme drops back in when the hit ends.
  chipToneOff();
  drumToneOff();
  activeSong = nullptr;
  drumPlaying = false;
  if (audioTx && musicVolume) pcmStart(PCM_BONG);
  beginCutsceneScene();
  for (int frame = 0; frame < 5; ++frame) {
    drawSmokeCutsceneFrame(frame);
    presentFrame();
    pumpDelay(420);
  }
  waitForCutsceneContinue();
}

void drawHashWashFrame(int frame) {
  // Cinematic wash room in the intro's visual language: gradient wall, a
  // hanging work lamp, and the grower working right beside the barrel on a
  // step stool — arms bent, gripping partway up the shaft, no straight-line
  // reach across the whole room.
  for (int y = HEADER_H; y < 190; ++y) {
    float blend = (float)(y - HEADER_H) / 176.0f;
    uint8_t r = 8 + (uint8_t)(blend * 12);
    uint8_t g = 13 + (uint8_t)(blend * 17);
    uint8_t b = 12 + (uint8_t)(blend * 13);
    game.drawBandHLine(0, y, SCREEN_W, rgb565(r, g, b));
  }
  game.drawFastHLine(0, 190, SCREEN_W, rgb565(76, 91, 61));
  game.fillRect(0, 191, SCREEN_W, NAV_Y - 191, rgb565(34, 47, 40));

  // Work lamp hanging over the barrel: cord, shade, warm bulb.
  game.fillRect(194, 30, 2, 14, rgb565(57, 66, 60));
  game.fillTriangle(178, 44, 212, 44, 195, 60, rgb565(57, 66, 60));
  game.drawFastHLine(178, 43, 35, GOLD);
  game.fillCircle(195, 46, 3, rgb565(224, 220, 183));

  uint16_t woodDark = rgb565(74, 42, 30);
  uint16_t wood = rgb565(143, 83, 49);
  uint16_t woodLight = rgb565(190, 126, 77);
  uint16_t water = rgb565(56, 137, 170);

  // Wash barrel, framed on the right half and sitting on the floor.
  game.fillRoundRect(120, 102, 150, 88, 14, woodDark);
  game.fillRoundRect(126, 106, 138, 78, 11, wood);
  for (int x = 142; x <= 248; x += 21) {
    game.drawFastVLine(x, 112, 66, woodDark);
    game.drawFastVLine(x + 2, 114, 60, woodLight);
  }
  game.fillRect(120, 126, 150, 4, rgb565(58, 60, 62));
  game.fillRect(120, 158, 150, 4, rgb565(58, 60, 62));
  game.drawFastHLine(126, 126, 138, rgb565(96, 99, 102));
  game.drawFastHLine(126, 158, 138, rgb565(96, 99, 102));
  // Water trough along the barrel's top rim.
  game.fillRoundRect(128, 94, 134, 18, 8, woodDark);
  game.fillRoundRect(132, 95, 126, 13, 6, rgb565(55, 45, 35));
  game.fillRoundRect(136, 98, 118, 7, 3, water);
  game.drawFastHLine(140, 99, 110, rgb565(115, 204, 216));

  // Leafy material drops into the barrel, then the paddle sweeps through it.
  if (frame == 0) {
    drawFanLeaf(176, 82, 0.28f, -12);
    drawFanLeaf(190, 80, 0.24f, 18);
  } else if (frame == 1) {
    drawFanLeaf(172, 90, 0.26f, -12);
    drawFanLeaf(190, 91, 0.23f, 18);
  } else {
    drawFanLeaf(166, 97, 0.20f, -20);
    drawFanLeaf(189, 96, 0.19f, 15);
  }

  // Paddle sweeps the wash; the grower grips partway up the shaft.
  const int paddleX[] = {228, 212, 192, 176, 192, 212, 228};
  int sweep = paddleX[frame];
  game.drawLine(114, 78, sweep, 150, woodLight);
  game.drawLine(113, 78, sweep - 2, 150, woodDark);
  game.fillTriangle(sweep - 9, 146, sweep + 9, 146, sweep, 164, woodDark);
  game.fillTriangle(sweep - 6, 147, sweep + 6, 147, sweep, 160, woodLight);
  if (frame == 5) {  // lifting out: drips fall from the paddle head
    for (int d = 0; d < 4; ++d)
      game.drawPixel(sweep - 6 + d * 4, 165 + (d % 2) * 3, water);
  }

  // The grower on a step stool beside the barrel; rear hand on the rim,
  // front arm bent up to the shaft with a real gripping hand.
  const uint16_t shirt = rgb565(39, 132, 71);
  int cx = 80, footY = 190;
  game.fillRoundRect(cx - 17, footY - 12, 36, 12, 3, wood);
  game.fillRoundRect(cx - 14, footY - 9, 30, 6, 2, woodLight);
  int gripX = 114 + (sweep - 114) * 2 / 5;   // partway up the shaft
  int gripY = 78 + 72 * 2 / 5;
  game.drawLine(cx - 14, footY - 55, 118, 96, shirt);   // rim hand (behind)
  game.drawLine(cx - 15, footY - 54, 117, 96, shirt);
  drawGrippingHand(118, 96, -1);
  drawGrowerWorker(cx, footY - 12, gripX, gripY);

  // Bubbles and ripples while the paddle works the wash.
  if (frame >= 2 && frame <= 5) {
    for (int ripple = 0; ripple < 3; ++ripple) {
      int rippleX = 142 + ((ripple * 37 + frame * 11) % 100);
      int rippleY = 99 + (ripple % 2) * 3;
      game.drawFastHLine(rippleX, rippleY, 12 + (frame % 3) * 4, rgb565(137, 218, 222));
    }
    for (int bubble = 0; bubble < frame - 1; ++bubble) {
      int bx = 146 + ((bubble * 27 + frame * 9) % 95);
      game.drawCircle(bx, 93 - (bubble % 2) * 5, 2, rgb565(164, 225, 224));
    }
  }
  if (frame == 6) {
    // Pressed hash on the collection plate, pulled out to the side.
    game.fillRoundRect(24, 136, 44, 42, 5, rgb565(135, 166, 132));
    game.drawRoundRect(24, 136, 44, 42, 5, rgb565(198, 219, 171));
    game.fillRoundRect(32, 129, 17, 9, 2, woodLight);
    game.fillCircle(46, 156, 7, rgb565(177, 190, 90));
    game.fillCircle(44, 154, 2, rgb565(221, 226, 141));
    game.fillCircle(50, 158, 2, rgb565(221, 226, 141));
  }
  game.setTextColor(GOLD, BG);
  if (frame == 6) {
    char batchLabel[30];
    snprintf(batchLabel, sizeof(batchLabel), "%u HASH UNITS READY", lastHarvestUnits);
    game.drawCentreString(batchLabel, 160, 212, 1);
  } else {
    game.drawCentreString("LEAVES IN - PADDLE WASH", 160, 212, 1);
  }
}

void animateHashWash() {
  beginCutsceneScene();
  for (int frame = 0; frame < 7; ++frame) {
    drawHashWashFrame(frame);
    presentFrame();
    pumpDelay(700);
  }
  waitForCutsceneContinue();
}

// An actual gripping hand wrapped around a tool: fist block, knuckle
// creases and a thumb, so the arm ends in a hand instead of a ball.
void drawGrippingHand(int hx, int hy, int dir) {
  uint16_t skin = farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143);
  uint16_t crease = farmerCharacter == 2 ? rgb565(103, 62, 43) : rgb565(206, 156, 116);
  game.fillRoundRect(hx - 4, hy - 4, 8, 8, 2, skin);            // fist
  for (int f = 0; f < 4; ++f)                                   // fingers
    game.drawFastHLine(hx - 3 + f * 2, hy - 3 + (f % 2), 2, crease);
  game.fillCircle(hx + dir * 4, hy - 1, 2, skin);               // thumb wrap
  game.drawFastHLine(hx - 3, hy + 2, 6, crease);                // fold line
}

// A cannabis bud: tapered stack of calyxes with sugar leaves and orange
// pistils, drawn tip-up with the base centre at (bx, by).
void drawBud(int bx, int by, float s) {
  uint16_t dark = rgb565(74, 105, 52);
  uint16_t mid = rgb565(105, 143, 75);
  uint16_t light = rgb565(133, 163, 88);
  uint16_t hair = rgb565(206, 124, 58);
  game.fillCircle(bx, by + int(2 * s), int(5 * s), dark);       // base calyx
  game.fillCircle(bx - int(2 * s), by - int(1 * s), int(4 * s), mid);
  game.fillCircle(bx + int(2 * s), by - int(1 * s), int(4 * s), dark);
  game.fillCircle(bx, by - int(3 * s), int(4 * s), mid);
  game.fillCircle(bx - int(1 * s), by - int(5 * s), int(2 * s), light);
  game.fillCircle(bx + int(2 * s), by - int(5 * s), int(2 * s), mid);
  drawFanLeaf(bx + int(3 * s), by - int(7 * s), 0.06f, 24);     // sugar leaf
  for (int i = 0; i < 5; ++i) {                                 // pistils
    int px = bx + ((i * 5) % 7) - 3;
    int py = by - int(2 * s) + (i % 3) - 1;
    game.drawPixel(px, py, hair);
    game.drawPixel(px + 1, py - 1, hair);
  }
}

// A slim grower figure with one arm reaching to (handX, handY) — the grower
// does the cutting / trimming / washing in the cutscenes instead of a
// disembodied hand. The arm bends at an elbow and ends in a gripping hand.
void drawGrowerWorker(int cx, int footY, int handX, int handY) {
  uint16_t shirt = rgb565(39, 132, 71);
  uint16_t pants = rgb565(22, 23, 26);
  const int torsoW = 28, torsoH = 34;
  game.fillRect(cx - 9, footY - 16, 6, 16, pants);
  game.fillRect(cx + 3, footY - 16, 6, 16, pants);
  game.fillRoundRect(cx - torsoW / 2, footY - 16 - torsoH, torsoW, torsoH, 6, shirt);
  int sx = handX >= cx ? cx + torsoW / 2 - 3 : cx - torsoW / 2 + 3;
  int sy = footY - 16 - torsoH + 7;
  int mx = (sx + handX) / 2, my = (sy + handY) / 2 + 5;   // slight elbow bend
  game.drawLine(sx, sy, mx, my, shirt);
  game.drawLine(sx, sy + 1, mx, my + 1, shirt);
  game.drawLine(mx, my, handX, handY, shirt);
  game.drawLine(mx, my + 1, handX, handY + 1, shirt);
  drawGrippingHand(handX, handY, handX >= cx ? 1 : -1);
  drawFarmerHeadLarge(cx, footY - 16 - torsoH, farmerCharacter);
}

void drawFlowerHarvestFrame(int frame) {
  game.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  game.fillRoundRect(13, 38, 294, 158, 10, PANEL);
  game.drawRoundRect(13, 38, 294, 158, 10, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("FLOWER HARVEST", 160, 46, 2);

  static const int plantX[] = {53, 99, 145, 191};
  static const int potCx[] = {24, 68, 112, 156};   // garden pots: same seeds
  for (int i = frame; i < 4; ++i) {
    int x = plantX[i];
    // Exactly the roommate from the small garden (same seed), sized to fit.
    drawArtPlantT(potCx[i], stageForGrowth(), x, 129 + HEADER_H, 0.75f, false);
    game.fillRoundRect(x - 12, 158, 24, 9, 3, rgb565(91, 49, 33));
  }
  for (int i = 0; i < min(frame, 4); ++i) {   // fresh-cut stumps remain
    game.drawFastVLine(plantX[i], 138, 20, rgb565(35, 104, 48));
    game.drawFastHLine(plantX[i] - 3, 137, 7, rgb565(96, 136, 72));
  }

  // The grower works his way down the row, shears in hand.
  int cutX = plantX[min(frame, 3)];
  game.drawCircle(cutX - 5, 145, 5, rgb565(185, 195, 196));
  game.drawCircle(cutX + 5, 145, 5, rgb565(185, 195, 196));
  game.drawLine(cutX - 2, 142, cutX + 14, 132, rgb565(208, 218, 218));
  game.drawLine(cutX + 2, 142, cutX - 13, 132, rgb565(208, 218, 218));
  drawGrowerWorker(max(44, cutX - 30), 178, cutX + 5, 144);

  game.fillRoundRect(232, 130, 47, 39, 6, rgb565(117, 70, 43));
  game.drawRoundRect(232, 130, 47, 39, 6, rgb565(193, 139, 83));
  game.drawFastHLine(238, 137, 35, rgb565(219, 164, 103));
  for (int i = 0; i < min(frame, 6); ++i) {
    int bx = 243 + (i % 3) * 12;
    int by = 150 + (i / 3) * 11;
    drawBud(bx, by, 0.9f);   // real buds piling into the basket
  }
  game.setTextColor(GOLD, PANEL);
  if (frame >= 5) {
    char batchLabel[32];
    snprintf(batchLabel, sizeof(batchLabel), "%u FLOWER UNITS GATHERED", lastHarvestUnits);
    game.drawCentreString(batchLabel, 160, 180, 1);
  } else {
    game.drawCentreString("GATHERING THE ROOM", 160, 180, 1);
  }
}

void animateFlowerHarvest() {
  beginCutsceneScene();
  for (int frame = 0; frame < 6; ++frame) {
    drawFlowerHarvestFrame(frame);
    presentFrame();
    pumpDelay(520);
  }
  waitForCutsceneContinue();
}

void drawToxicMoldLogo(int cx, int cy, int scale) {
  const uint16_t toxic = rgb565(216, 244, 68);
  const uint16_t dark = rgb565(39, 55, 35);
  const uint16_t ink = rgb565(28, 34, 29);

  // Bright hazard badge with a large skull-and-crossbones toxic mark.
  game.fillCircle(cx, cy, 18 * scale, toxic);
  game.fillCircle(cx, cy, 15 * scale, dark);
  for (int offset = -1; offset <= 1; ++offset) {
    game.drawLine(cx - 10 * scale + offset, cy - 8 * scale,
                    cx + 10 * scale + offset, cy + 9 * scale, toxic);
    game.drawLine(cx - 10 * scale + offset, cy + 9 * scale,
                    cx + 10 * scale + offset, cy - 8 * scale, toxic);
  }
  for (int side = -1; side <= 1; side += 2) {
    game.fillCircle(cx + side * 10 * scale, cy - 8 * scale, 2 * scale, toxic);
    game.fillCircle(cx + side * 10 * scale, cy + 9 * scale, 2 * scale, toxic);
    game.fillCircle(cx + side * 10 * scale, cy - 8 * scale, scale, dark);
    game.fillCircle(cx + side * 10 * scale, cy + 9 * scale, scale, dark);
  }

  game.fillCircle(cx, cy - 2 * scale, 7 * scale, toxic);
  game.fillRoundRect(cx - 6 * scale, cy + 2 * scale, 12 * scale,
                       6 * scale, 2 * scale, toxic);
  game.fillCircle(cx - 3 * scale, cy - 2 * scale, 2 * scale, ink);
  game.fillCircle(cx + 3 * scale, cy - 2 * scale, 2 * scale, ink);
  game.fillTriangle(cx - scale, cy + scale, cx + scale, cy + scale,
                      cx, cy + 3 * scale, ink);
  for (int tooth = -2; tooth <= 2; ++tooth) {
    game.drawFastVLine(cx + tooth * 2 * scale, cy + 4 * scale,
                         3 * scale, ink);
  }
}

void drawMoldWarningFrame(int scale) {
  game.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  game.fillRoundRect(8, 36, 304, 160, 10, PANEL);
  game.drawRoundRect(8, 36, 304, 160, 10, RED);
  drawToxicMoldLogo(83, 119, scale);

  game.setTextColor(RED, PANEL);
  game.drawCentreString("MOLD FOUND", 222, 59, 2);
  game.setTextColor(TEXT_MAIN, PANEL);
  char discarded[28];
  char saved[28];
  snprintf(discarded, sizeof(discarded), "%u UNIT%s DISCARDED",
           moldLossUnits, moldLossUnits == 1 ? "" : "S");
  snprintf(saved, sizeof(saved), "%u UNIT%s SAVED",
           lastHarvestUnits, lastHarvestUnits == 1 ? "" : "S");
  game.drawCentreString("TOXIC CROP", 222, 89, 2);
  game.setTextColor(GOLD, PANEL);
  game.drawCentreString(discarded, 222, 120, 1);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString(saved, 222, 140, 1);
  game.drawCentreString("Contaminated portion removed", 222, 164, 1);
  if (moldRescuedUnits) {
    char rescued[28];
    snprintf(rescued, sizeof(rescued), "REMEDIATION SAVED %u UNIT%s",
             moldRescuedUnits, moldRescuedUnits == 1 ? "" : "S");
    game.setTextColor(0x2C64, PANEL);
    game.drawCentreString(rescued, 222, 152, 1);
  }
}

void animateMoldWarning() {
  for (int scale = 1; scale <= 4; ++scale) {
    if (scale == 1) beginCutsceneScene();
    drawMoldWarningFrame(scale);
    presentFrame();
    pumpDelay(300);
  }
  waitForCutsceneContinue();
}

void drawFlowerTrimFrame(int frame) {
  // Full-bleed cinematic trim room in the intro's visual language: gradient
  // wall, warm hanging lamp, deep work bench — the grower works his way down
  // the bench with the shears held in a properly bent arm.
  for (int y = 0; y < 196; ++y) {
    float blend = (float)y / 196.0f;
    uint8_t v = 30 + (uint8_t)(blend * 26);
    game.drawBandHLine(0, y, SCREEN_W, rgb565(v - 4, v + 8, v + 4));
  }
  game.drawFastHLine(0, 196, SCREEN_W, rgb565(76, 91, 61));
  game.fillRect(0, 197, SCREEN_W, NAV_Y - 197, rgb565(34, 47, 40));

  // Hanging work lamp over the bench: cord, shade, warm bulb.
  game.fillRect(248, 0, 2, 14, rgb565(57, 66, 60));
  game.fillTriangle(232, 14, 266, 14, 249, 30, rgb565(57, 66, 60));
  game.drawFastHLine(232, 13, 35, GOLD);
  game.fillCircle(249, 16, 3, rgb565(224, 220, 183));

  const int cutX = 60 + frame * 36;
  const uint16_t woodDark = rgb565(74, 42, 30);
  const uint16_t wood = rgb565(143, 83, 49);
  const uint16_t woodLight = rgb565(190, 126, 77);
  const uint16_t steel = rgb565(208, 218, 218);
  const uint16_t steelDim = rgb565(185, 195, 196);

  // Shears at the cut point; the bench edge hides the handle grips.
  game.drawCircle(cutX - 3, 144, 3, steelDim);
  game.drawCircle(cutX + 3, 144, 3, steelDim);
  game.drawLine(cutX - 4, 142, cutX - 14, 131, steel);
  game.drawLine(cutX + 4, 142, cutX + 14, 133, steel);

  // The grower, properly proportioned, tracks the shears down the bench;
  // rear arm resting on the bench edge, front arm bent to the cut.
  const int bodyCx = min(286, cutX + 26);
  drawGrowerWorker(bodyCx, 170, cutX, 146);
  const uint16_t shirt = rgb565(39, 132, 71);
  game.drawLine(bodyCx + 8, 130, bodyCx + 20, 148, shirt);
  game.drawLine(bodyCx + 9, 131, bodyCx + 21, 148, shirt);
  game.fillCircle(bodyCx + 21, 149, 2,
                  farmerCharacter == 2 ? rgb565(133, 84, 61) : rgb565(230, 184, 143));

  // Deep work bench drawn over the grower's lower half.
  game.fillRect(20, 150, 280, 9, woodLight);
  game.drawFastHLine(20, 150, 280, woodDark);
  game.fillRect(24, 159, 272, 15, wood);
  game.fillRect(24, 159, 272, 2, woodDark);
  for (int x = 44; x < 296; x += 44) game.drawFastVLine(x, 161, 12, woodDark);
  game.fillRect(36, 174, 8, 22, woodDark);
  game.fillRect(276, 174, 8, 22, woodDark);

  // Untrimmed buds still standing on the bench, ahead of the shears.
  for (int i = 0; i < 5; ++i) {
    int x = 74 + i * 35;
    if (x > cutX + 12) drawBud(x, 146, 1.1f);
  }

  // Trimmed sugar leaves drift down as the shears move across.
  for (int i = 0; i < frame; ++i) {
    int leafX = 40 + ((i * 47 + frame * 13) % 240);
    int leafY = 112 + (i % 4) * 7;
    drawFanLeaf(leafX, leafY, 0.10f, i * 14);
  }

  game.fillRect(0, NAV_Y, SCREEN_W, SCREEN_H - NAV_Y, BG);
  game.setTextColor(GOLD, BG);
  game.drawCentreString(frame >= 5 ? "FLOWER READY FOR MARKET" : "TRIMMING THE HARVEST",
                        160, 212, 1);
}
void animateFlowerTrim() {
  beginBareHeaderScene();   // full-bleed: the scene owns the whole display
  for (int frame = 0; frame < 6; ++frame) {
    drawFlowerTrimFrame(frame);
    presentFrame();
    pumpDelay(520);
  }
  waitForCutsceneContinue();
}

void touchGarden(int x, int y) {
  if (x < ART_W && y >= HEADER_H && y < ACTION_Y) {
    page = PLANT_VIEW;
    return;
  }
  if (y < ACTION_Y) return;
  if (!plantActive) {
    if (x >= 80 && x <= 240) {
      if (totalSeeds() >= PLANTS_PER_ROOM) {
        pageAfterPlanting = GARDEN;
        firstPlantingDone = false;
        plantedPotMask = 0;
        page = PLANTING;
        saveGame();
      } else {
        openSeedShop(GARDEN);
      }
    }
    return;
  }
  if (growth >= 100) {
    if (x < 80) waterPlant();
    else if (x < 160) defoliateRoom();
    else if (x < 240) treatPests();
    else page = HARVEST_SELECT;
  } else {
    // Wide three-button layout while the plant is growing.
    if (x < 106) waterPlant();
    else if (x < 211) defoliateRoom();
    else treatPests();
  }
}

void handleNearbyTouch(int x, int y) {
  if (nearbyMode == 0) {
    if (y >= 80 && y < 172) {
      int index = (y - 80) / 23;
      if (index < MAX_NEARBY_PEERS && nearbyPeers[index].active &&
          millis() - nearbyPeers[index].lastSeen < 20000UL) {
        selectedNearbyPeer = index;
      }
    } else if (y >= 179 && y < 205) {
      if (x < 108 && selectedNearbyPeerValid()) {
        nearbyMode = 1;
        outgoingMessage[0] = '\0';
        msgKbMode = 0;
      } else if (x < 214 && selectedNearbyPeerValid()) {
        nearbyMode = 2;
        sendSeedStrain = seedStrain;
        if (!seedsByStrain[sendSeedStrain]) sendSeedStrain = nextSeededStrain(sendSeedStrain, 1);
        sendStashStrain = seedStrain;
        if (!stashByStrain[sendStashStrain]) sendStashStrain = nextStashStrain(sendStashStrain, 1);
        sendHashStrain = seedStrain;
        if (!hashByStrain[sendHashStrain]) sendHashStrain = nextHashStrain(sendHashStrain, 1);
      } else if (x >= 214) {
        nearbyMode = 3;
      }
    }
    return;
  }

  if (nearbyMode == 1) {
    char key = fullKeyboardKey(x, y, msgKbMode);
    if (key) {
      int length = strlen(outgoingMessage);
      if (length < (int)sizeof(outgoingMessage) - 1) {
        outgoingMessage[length] = key;
        outgoingMessage[length + 1] = '\0';
      }
      refreshKeyboardField();
      fieldOnlyRedraw = true;
      return;
    }
    int length = strlen(outgoingMessage);
    if (y >= 138 && y < 164 && x >= 263) {
      if (length) outgoingMessage[length - 1] = '\0';
      refreshKeyboardField();
      fieldOnlyRedraw = true;
    } else if (y >= 168 && y < 196) {
      if (x < 104) {
        if (length > 0 && length < (int)sizeof(outgoingMessage) - 1 &&
            outgoingMessage[length - 1] != ' ') {
          outgoingMessage[length] = ' ';
          outgoingMessage[length + 1] = '\0';
        }
        refreshKeyboardField();
        fieldOnlyRedraw = true;
      } else if (x < 216) {
        if (selectedNearbyPeerValid() && length > 0 &&
            sendNearbyPacket(nearbyPeers[selectedNearbyPeer].mac,
                             PACKET_MESSAGE, 0, outgoingMessage)) {
          snprintf(lastNearbyMessage, sizeof(lastNearbyMessage), "Message sent");
        }
        nearbyMode = 0;
      } else {
        nearbyMode = 0;
      }
    }
    return;
  }

  if (nearbyMode == 2) {
    // Products (seeds, jars, hash) ask once before sending: the first tap
    // arms the SEND IT? / BACK panel, the second tap confirms.
    if (sendConfirmItem) {
      if (y >= 168 && y < 194) {
        if (x < 160) {
          sendConfirmItem = 0;             // BACK — nothing sent
        } else {
          uint8_t item = sendConfirmItem;
          sendConfirmItem = 0;
          sendNearbyGift(item);
        }
      } else {
        sendConfirmItem = 0;               // tap anywhere else cancels
      }
      render();
      return;
    }
    // Big arrows pick the strain; the SEND button is separate from them.
    if (y >= 70 && y < 104) {
      if (x < 39) {
        sendSeedStrain = nextCatalogStrain(sendSeedStrain, -1);
      } else if (x >= 281) {
        sendSeedStrain = nextCatalogStrain(sendSeedStrain, 1);
      } else if (selectedNearbyPeerValid() &&
                 seedsByStrain[sendSeedStrain] >= 4) {
        sendConfirmItem = 1;
      }
      render();
      return;
    }
    if (y >= 108 && y < 142) {
      // Left half: flower jar; right half: hash jar.
      if (x < 160) {
        if (x < 33) {
          sendStashStrain = nextCatalogStrain(sendStashStrain, -1);
        } else if (x >= 129) {
          sendStashStrain = nextCatalogStrain(sendStashStrain, 1);
        } else if (selectedNearbyPeerValid() && stashByStrain[sendStashStrain] > 0) {
          sendConfirmItem = 5;
        }
      } else {
        if (x < 191) {
          sendHashStrain = nextCatalogStrain(sendHashStrain, -1);
        } else if (x >= 287) {
          sendHashStrain = nextCatalogStrain(sendHashStrain, 1);
        } else if (selectedNearbyPeerValid() && hashByStrain[sendHashStrain] > 0) {
          sendConfirmItem = 6;
        }
      }
      render();
      return;
    }
    if (y >= 146 && y < 180) {
      if (x < 110) {
        if (selectedNearbyPeerValid() && pestDefenseKits) sendNearbyGift(2);
      } else if (x < 215) {
        if (selectedNearbyPeerValid() && waterPacks >= 2) sendNearbyGift(3);
      } else if (selectedNearbyPeerValid() && careKits >= 2) {
        sendNearbyGift(4);
      }
      return;
    }
    if (y >= 184 && y < 203) {
      if (x < 162) {
        if (selectedNearbyPeerValid()) {
          nearbyMode = 4;
          outgoingCash[0] = '\0';
        }
      } else {
        nearbyMode = 0;
      }
    }
    render();
    return;
  }

  if (nearbyMode == 4) {
    int length = strlen(outgoingCash);
    if ((y >= 106 && y < 129) || (y >= 132 && y < 155)) {
      const char *keys = "1234567890";
      int index = (x - 5) / 62;   // five keys per row, pitch 62
      if (x >= 5 && index >= 0 && index < 5 && length < 4) {
        outgoingCash[length] = keys[(y >= 132 ? 5 : 0) + index];
        outgoingCash[length + 1] = '\0';
      }
      refreshMoneyField();
      fieldOnlyRedraw = true;
    } else if (y >= 158 && y < 179) {
      if (x < 108) {
        outgoingCash[0] = '\0';
      } else if (x < 216) {
        if (length && length < 4) strncat(outgoingCash, "000", 4 - length);
      } else {
        if (length) outgoingCash[length - 1] = '\0';
      }
      refreshMoneyField();
      fieldOnlyRedraw = true;
    } else if (y >= 182 && y < 203) {
      if (x < 108) {
        snprintf(outgoingCash, sizeof(outgoingCash), "%u", credits);
      } else if (x < 216) {
        sendNearbyCash();
      } else {
        nearbyMode = 0;
      }
    }
    return;
  }

  if (nearbyMode == 3 && y >= 174 && y < 205) nearbyMode = 0;
}

void appendGrowerNameLetter(char letter) {
  size_t length = strlen(growerNameEdit);
  if (length >= sizeof(growerNameEdit) - 1) return;
  if (length == 0 && isalpha((unsigned char)letter)) letter = (char)toupper((unsigned char)letter);
  growerNameEdit[length] = letter;
  growerNameEdit[length + 1] = '\0';
}

void handleGrowerNameTouch(int x, int y) {
  char key = fullKeyboardKey(x, y, nameKbMode);
  if (key) {
    appendGrowerNameLetter(key);
    refreshKeyboardField();
    fieldOnlyRedraw = true;
    return;
  }
  if (y >= 138 && y < 164 && x >= 263) {
    int length = strlen(growerNameEdit);
    if (length) growerNameEdit[length - 1] = '\0';
    refreshKeyboardField();
    fieldOnlyRedraw = true;
  } else if (y >= 168 && y < 196) {
    int length = strlen(growerNameEdit);
    if (x < 104) {
      if (length > 0 && length < sizeof(growerNameEdit) - 1 &&
          growerNameEdit[length - 1] != ' ') appendGrowerNameLetter(' ');
      refreshKeyboardField();
      fieldOnlyRedraw = true;
    } else if (x < 216) {
      if (growerNameEdit[0]) {
        strlcpy(growerName, growerNameEdit, sizeof(growerName));
        saveGame();
      }
      page = growerNameReturn;
    } else {
      page = growerNameReturn;
    }
  }
}

void chooseFarmerCharacter(uint8_t character) {
  bool firstTimeSetup = !farmerCharacterChosen && !selectingFarmerFromOptions;
  const char *oldCharacterName = farmerCharacterName(farmerCharacter);
  bool growerNameIsDefault = strcmp(growerName, oldCharacterName) == 0;
  farmerCharacter = min((int)character, 9);
  farmerCharacterChosen = true;
  if (growerNameIsDefault || !growerName[0]) {
    strlcpy(growerName, farmerCharacterName(farmerCharacter), sizeof(growerName));
  }
  if (firstTimeSetup) {
    // First launch only: the new grower sanitizes up, then picks a strain
    // from the catalog before any seeds can be planted. After planting the
    // first room they land in the garden like everyone else.
    animateGownUpCutscene();
    pageAfterSeedShop = GARDEN;
    page = SEED_SHOP;
  } else {
    page = selectingFarmerFromOptions ? (growerFromMenu ? GROWER_MENU : OPTIONS) : PLANTING;
  }
  growerFromMenu = false;
  selectingFarmerFromOptions = false;
  saveGame();
}

void handleTouch(int x, int y) {
  fieldOnlyRedraw = false;   // cleared unless a tap only refreshed its field
  if (deathWarnModal) {
    // Any tap dismisses the crisis popup and jumps to the garden.
    deathWarnModal = false;
    deathWarnNextAllowed = millis() + 90000UL;
    if (plantActive) page = GARDEN;
    render();
    return;
  }
  if (page == CHARACTER_SELECT) {
    if (y >= 2 && y < 24 && x >= 266) {
      // Speaker toggle: mute now, tap again to restore the volume.
      if (musicVolume) {
        musicVolumeBeforeMute = musicVolume;
        musicVolume = 0;
        chipToneOff();
        drumToneOff();
        activeSong = nullptr;
        drumPlaying = false;
      } else {
        musicVolume = musicVolumeBeforeMute ? musicVolumeBeforeMute : 40;
        startSong(backgroundSong);
      }
      render();
      return;
    }
    if (y >= 88 && y < 138 && x < 54) {
      characterSelectIndex = (characterSelectIndex + 9) % 10;  // wrap: 10 growers
      render();
      return;
    }
    if (y >= 88 && y < 138 && x >= 266) {
      characterSelectIndex = (characterSelectIndex + 1) % 10;
      render();
      return;
    }
    if (y >= 172 && y < 202) {
      chooseFarmerCharacter(characterSelectIndex);
      render();
      return;
    }
    render();
    return;
  }

  if (page == PLANT_VIEW) {
    page = GARDEN;
    render();
    return;
  }
  if (y < HEADER_H && x > 282) {
    // Gear icon: enter the options hub, or tap again to go back to the garden.
    if (page == OTA_PAGE || page == WIFI_LOGIN) {
      otaRestoreRadio();                 // release the radio back to Nearby
      page = GARDEN;
    } else {
      page = page == OPTIONS ? GARDEN : OPTIONS;
    }
    resetArmed = false;
    render();
    return;
  }
  if (y < HEADER_H && x < 105) {
    // Leaf: open the grower ID menu; tap again to go back.
    if (page == GROWER_MENU) {
      page = growerMenuReturn;
    } else {
      growerMenuReturn = page;
      page = GROWER_MENU;
    }
    render();
    return;
  }

  if (y >= NAV_Y) {
    const Page pages[] = {GARDEN, SUPPLIES, MARKET, STASH, NEARBY};
    int tab = constrain(x / 64, 0, 4);
    if (page == OTA_PAGE || page == WIFI_LOGIN) otaRestoreRadio();
    page = pages[tab];
    render();
    return;
  }

  bool redrawGardenCare = false;
  switch (page) {
    case GARDEN:
      touchGarden(x, y);
      redrawGardenCare = page == GARDEN && plantActive &&
                         y >= ACTION_Y && y < NAV_Y &&
                         (growth >= 100 ? (x >= 80 && x < 240) : x >= 106) &&
                         !waterAnimationRequested;
      break;
    case SUPPLIES:
      // Big-card grid like the seed catalog: each card is its own tap target.
      if (y >= 72 && y < 112) {
        if (x < 160) buyWater();
        else buyCareKits();
      } else if (y >= 114 && y < 154) {
        if (x < 160) openSeedShop(SUPPLIES);
        else buyPestDefense();
      } else if (y >= 156 && y < 196) {
        if (x < 160) buyRemediation();
        else buyHashRoom();
      }
      break;
    case MARKET:
      if (y >= 70 && y < 98) {
        marketGearMode = x >= 160;
        marketSellMode = 0;
        marketSellConfirm = false;
      } else if (marketSellMode) {
        touchMarketSellModal(x, y);
        return;   // the modal renders itself; skip the full-page tail render
      } else if (marketGearMode) {
        // Gear grid: bong / lighters / papers cards; the stoned card is info.
        if (y >= 102 && y < 140) {
          if (x < 160) buyBong();
          else buyLighters();
        } else if (y >= 144 && y < 182) {
          if (x < 160) buyRollingPapers();
        }
      } else {
        // Sell grid: each card opens the strain submenu.
        if (y >= 102 && y < 140) {
          if (x < 160) openMarketSell(1);
          else openMarketSell(2);
        } else if (y >= 144 && y < 182) {
          if (x < 160) openMarketSell(3);
          else openMarketSell(4);
        }
      }
      break;
    case STASH:
      if (smokeMenuOpen) {
        if (smokeMenuConfirm) {
          if (y >= 176 && y < 203) {
            if (x < 160) {
              smokeMenuConfirm = false;          // BACK - nothing smoked
            } else {
              smokeMenuConfirm = false;
              smokeMenuOpen = false;
              if (smokeMenuPick == 1) takeSmokeBreak(1, false);
              else if (smokeMenuPick == 2) takeSmokeBreak(2, false);
              else takeSmokeBreak(1, true);      // tail runs the smoke scene
            }
          } else {
            smokeMenuConfirm = false;            // tap anywhere else cancels
          }
        } else if (y >= 176 && y < 203) {
          smokeMenuOpen = false;                 // BACK, full-width row
        } else if (y >= 78 && y < 172) {
          uint8_t r = (y - 78) / 32;
          bool ready[3] = {(bool)(bongs && stashTotal()),
                           (bool)(bongs && hashRoomUnlocked && hashTotal()),
                           (bool)(stashTotal() && lighters && rollingPapers)};
          if (r < 3 && ready[r]) {
            smokeMenuPick = r + 1;
            smokeMenuConfirm = true;             // ask once before smoking
          }
        }
        break;
      }
      if (y >= 174 && y < 203) {
        if (x < 160) {
          if (untrimmedUnits > 0) page = TRIM_ROOM;
        } else {
          smokeMenuOpen = true;                  // open the smoke-break chooser
        }
      }
      break;
    case OPTIONS:
      // Big-card grid: each card is its own tap target, anywhere on the card.
      if (y >= 70 && y < 110 && x < 160) {
        gameSpeed = (gameSpeed + 1) % 3;   // RELAXED -> QUICK -> EXTREME
        lastGameTick = millis();
        saveGame();
      } else if (y >= 70 && y < 110) {
        // Volume steps: OFF -> 10 -> 20 -> ... -> 100 -> OFF
        musicVolume = musicVolume == 0 ? 10 : musicVolume >= 100 ? 0 : musicVolume + 10;
        if (musicVolume) startSong(backgroundSong);
        else { chipToneOff(); drumToneOff(); activeSong = nullptr; drumPlaying = false; }
        saveGame();
      } else if (y >= 114 && y < 154 && x < 160) {
        prepareDemoHarvest();
      } else if (y >= 114 && y < 154) {
        page = CHEAT_CODE;               // cheat console
        cheatEdit[0] = '\0';
        cheatFeedback[0] = '\0';
        render();
        return;
      } else if (y >= 158 && y < 204) {
        if (x < 160) {
          if (resetArmed) resetGame();
          else {
            resetArmed = true;
            resetArmedAt = millis();
          }
        } else {
          page = OTA_PAGE;               // WiFi login + OTA updates screen
          render();
          return;
        }
      }
      break;
    case OTA_PAGE:
      touchOtaPage(x, y);
      break;
    case WIFI_LOGIN:
      touchWifiLogin(x, y);
      break;
    case CHEAT_CODE:
      touchCheatPage(x, y);
      break;
    case GROWER_MENU:
      if (y >= 72 && y < 128) {
        growerFromMenu = true;             // the card itself: change grower
        selectingFarmerFromOptions = true;
        page = CHARACTER_SELECT;
      } else if (y >= 132 && y < 166) {
        growerFromMenu = true;
        selectingFarmerFromOptions = true;
        page = CHARACTER_SELECT;
      } else if (y >= 170 && y < 204) {
        strlcpy(growerNameEdit, growerName, sizeof(growerNameEdit));
        growerNameReturn = GROWER_MENU;
        nameKbMode = 0;
        page = NAME_EDIT;
      }
      break;
    case HASH_ROOM:
      if (washOpen) { touchWashModal(x, y); return; }
      if (y >= 165 && y < 199) {
        if (x < 160) {
          page = GARDEN;
        } else {
          washOpen = true;               // RUN A WASH picker
          washConfirm = false;
          washStrain = nextWashStrain((uint8_t)(washStrain + STRAIN_COUNT - 1), 1);
          render();
        }
      }
      break;
    case HARVEST_SELECT:
      if (y >= 78 && y < 130 && x >= 240) finishHarvest(false);
      else if (y >= 139 && y < 192 && x >= 240 && hashRoomUnlocked) finishHarvest(true);
      break;
    case TRIM_ROOM:
      if (y >= 158 && y < 198) {
        if (x < 160) trimAllFlowers();
        else page = GARDEN;
      }
      break;
    case NEARBY:
      handleNearbyTouch(x, y);
      break;
    case NAME_EDIT:
      handleGrowerNameTouch(x, y);
      break;
    case SEED_SHOP:
      // Picker: arrows cycle ALL strains (locked ones gray out); BUY only
      // works on cards you have unlocked.
      if (y >= 96 && y < 148 && x < 54) {
        seedShopCursor = seedShopCursor ? seedShopCursor - 1 : STRAIN_COUNT - 1;
        render();
        return;
      } else if (y >= 96 && y < 148 && x >= 266) {
        seedShopCursor = seedShopCursor >= STRAIN_COUNT - 1 ? 0 : seedShopCursor + 1;
        render();
        return;
      } else if (y >= 176 && y < 204 && seedShopCursor <= strainUnlockedThrough) {
        buySeeds(strainCatalogOrder[seedShopCursor]);
      }
      break;
    case PLANT_VIEW:
      page = GARDEN;
      break;
    case PLANTING:
      break;
  }
  if (fieldOnlyRedraw) return;   // keyboard tap already refreshed its field
  bool cutscenePending = waterAnimationRequested || smokeAnimationRequested ||
                         hashAnimationRequested || flowerHarvestAnimationRequested ||
                         moldAnimationRequested || flowerTrimAnimationRequested ||
                         plantDeathAnimationRequested;
  if (plantDeathAnimationRequested) animatePlantDeath();
  if (waterAnimationRequested) {
    waterAnimationRequested = false;
    startWateringAnimation();   // frames step in the loop; touch stays live
    return;
  }
  if (smokeAnimationRequested) {
    smokeAnimationRequested = false;
    animateSmokeBreak();
  }
  if (hashAnimationRequested) {
    hashAnimationRequested = false;
    animateHashWash();
  }
  if (flowerHarvestAnimationRequested) {
    flowerHarvestAnimationRequested = false;
    animateFlowerHarvest();
  }
  if (moldAnimationRequested) {
    moldAnimationRequested = false;
    animateMoldWarning();
  }
  if (flowerTrimAnimationRequested) {
    flowerTrimAnimationRequested = false;
    animateFlowerTrim();
  }
  if (cutscenePending) render();
  else if (redrawGardenCare) {
    drawGardenSidePanel();
    drawGardenActions();
    presentFrame();
  } else render();
}

void drawPlantDeathFrame(int frame) {
  game.fillRect(0, HEADER_H, SCREEN_W, NAV_Y - HEADER_H, BG);
  game.fillRoundRect(8, 36, 304, 160, 10, PANEL);
  game.drawRoundRect(8, 36, 304, 160, 10, RED);

  // Wilting plant: the stem bends and the leaves brown frame by frame.
  int baseX = 80, baseY = 168;
  uint8_t g = 150 - frame * 18;
  uint16_t leaf = rgb565(90 + frame * 14, g, 50);
  uint16_t stem = rgb565(80 + frame * 10, 120 - frame * 12, 50);
  int droop = frame * 6;
  game.drawLine(baseX, baseY, baseX + droop / 2, baseY - 60 + droop, stem);
  game.drawLine(baseX + 1, baseY, baseX + 1 + droop / 2, baseY - 60 + droop, stem);
  for (int i = 0; i < 4; ++i) {
    int ly = baseY - 18 - i * 12 + droop * (i + 1) / 5;
    int lx = baseX + droop * i / 8;
    game.fillTriangle(lx, ly, lx - 18, ly + 4 + frame * 2, lx - 8, ly + 8, leaf);
    game.fillTriangle(lx, ly, lx + 18, ly + 4 + frame * 2, lx + 8, ly + 8, leaf);
  }
  if (deathReason == 1) {
    for (int i = 0; i < frame * 3; ++i)
      game.fillCircle(40 + (i * 23) % 80, 176 - (i % 3) * 3, 2, 0x3DDF);
  } else {
    for (int i = 0; i < frame * 2; ++i)
      game.fillTriangle(40 + (i * 29) % 80, 178, 46 + (i * 29) % 80, 174, 50 + (i * 29) % 80, 180, leaf);
  }
  game.fillRoundRect(baseX - 22, baseY, 44, 18, 4, rgb565(150, 86, 52));

  game.setTextColor(RED, PANEL);
  game.drawCentreString("PLANT LOST", 222, 59, 2);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(deathReason == 1 ? "OVERWATERED"
                        : deathReason == 3 ? "DEHYDRATED" : "OVER-DEFOLIATED", 222, 89, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString(deathReason == 1 ? "The roots drowned"
                        : deathReason == 3 ? "The soil went bone dry"
                        : "Too many leaves removed", 222, 120, 1);
  game.drawCentreString(currentStrainName(), 222, 140, 1);
  game.setTextColor(GOLD, PANEL);
  game.drawCentreString("The room has been cleared", 222, 164, 1);
}

void animatePlantDeath() {
  plantDeathAnimationRequested = false;
  for (int frame = 0; frame <= 5; ++frame) {
    if (frame == 0) beginCutsceneScene();
    drawPlantDeathFrame(frame);
    presentFrame();
    pumpDelay(260);
  }
  waitForCutsceneContinue();

  // The cutscene (frames + tap-to-continue) can outlast the window started in
  // killPlant(); restart the 5 s red-strobe window now that the loop is free
  // to run updateHarvestLed() again.
  deathLedUntil = millis() + 5000;

  plantActive = false;
  plantedPotMask = 0;
  growth = 0;
  hydration = 72;
  vigor = 82;
  pestType = 0;
  pestSeverity = 0;
  growthBoostTicks = 0;
  resetDefoliationForNewPlant();
  page = GARDEN;
  lastFarmerVisitAt = millis();
  saveGame();
}

void showDefoliatePrompt() {
  uint8_t w = defoliatePromptPending;
  defoliatePromptPending = 255;
  if (w >= DEFOLIATE_WINDOWS || !plantActive) return;
  beginCutsceneScene();
  game.fillRoundRect(8, 36, 304, 160, 10, PANEL);
  game.drawRoundRect(8, 36, 304, 160, 10, ACCENT);
  game.setTextColor(ACCENT, PANEL);
  game.drawCentreString(defoliateWindowNames[w], 160, 62, 2);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("Your plants need care", 160, 96, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString("Tap DEFOLIATE while it is green.", 160, 126, 1);
  game.drawCentreString("Red means one more cut could kill it.", 160, 142, 1);
  game.setTextColor(GOLD, PANEL);
  game.drawCentreString("Each strain tolerates a different amount", 160, 166, 1);
  presentFrame();
  waitForCutsceneContinue();
  render();
}

// ---------------------------------------------------------------------------
// Pest invasion cutscene: bugs crawl all over the whole screen.
// ---------------------------------------------------------------------------
const int PEST_BUG_MAX = 46;
const int PEST_PANEL_X = 34, PEST_PANEL_Y = 78, PEST_PANEL_W = 252, PEST_PANEL_H = 84;

struct PestBug {
  float x, y;
  float heading;
  float speed;
};
PestBug pestBugs[PEST_BUG_MAX];

void drawPestBug(int x, int y, float heading, int frame, uint8_t type) {
  float dx = cosf(heading), dy = sinf(heading);
  float px = -dy, py = dx;  // perpendicular, for legs
  int wiggle = (frame & 1) ? 1 : -1;
  if (type == 1) {
    // Spider mite: small red body, 8 legs.
    uint16_t body = rgb565(205, 52, 38), leg = rgb565(120, 30, 24);
    for (int i = -1; i <= 2; ++i) {
      float ox = dx * (i - 0.5f) * 1.3f, oy = dy * (i - 0.5f) * 1.3f;
      int w = (i & 1) ? wiggle : -wiggle;
      game.drawLine(x + ox, y + oy, x + ox + px * 5 + dx * w, y + oy + py * 5 + dy * w, leg);
      game.drawLine(x + ox, y + oy, x + ox - px * 5 - dx * w, y + oy - py * 5 - dy * w, leg);
    }
    game.fillCircle(x, y, 3, body);
    game.drawPixel(x + dx * 2, y + dy * 2, TFT_BLACK);
  } else if (type == 2) {
    // Aphid: green pear-shaped body, 6 legs, antennae.
    uint16_t body = rgb565(126, 200, 70), dark = rgb565(60, 110, 40);
    for (int i = -1; i <= 1; ++i) {
      float ox = dx * i * 2.0f, oy = dy * i * 2.0f;
      int w = (i & 1) ? wiggle : -wiggle;
      game.drawLine(x + ox, y + oy, x + ox + px * 5 + dx * w, y + oy + py * 5 + dy * w, dark);
      game.drawLine(x + ox, y + oy, x + ox - px * 5 - dx * w, y + oy - py * 5 - dy * w, dark);
    }
    game.fillCircle(x - dx * 1.5f, y - dy * 1.5f, 3, body);
    game.fillCircle(x + dx * 2.5f, y + dy * 2.5f, 2, body);
    game.drawLine(x + dx * 4, y + dy * 4, x + dx * 7 + px * 2, y + dy * 7 + py * 2, dark);
    game.drawLine(x + dx * 4, y + dy * 4, x + dx * 7 - px * 2, y + dy * 7 - py * 2, dark);
  } else {
    // Fungus gnat: dark body with flapping gray wings.
    uint16_t body = rgb565(30, 30, 34), wing = rgb565(170, 180, 190);
    int span = (frame & 1) ? 4 : 2;
    game.fillCircle(x + px * span, y + py * span, 2, wing);
    game.fillCircle(x - px * span, y - py * span, 2, wing);
    game.fillCircle(x, y, 2, body);
    game.drawLine(x, y, x - dx * 4, y - dy * 4, body);
  }
}

void erasePestBug(int x, int y) {
  const int R = 8;
  int ex = x - R, ey = y - R, ew = R * 2 + 1, eh = R * 2 + 1;
  game.fillRect(ex, ey, ew, eh, BG);
  // Restore the part of the message panel the bug was covering.
  int ix0 = max(ex, PEST_PANEL_X + 4), iy0 = max(ey, PEST_PANEL_Y + 4);
  int ix1 = min(ex + ew, PEST_PANEL_X + PEST_PANEL_W - 4);
  int iy1 = min(ey + eh, PEST_PANEL_Y + PEST_PANEL_H - 4);
  if (ix1 > ix0 && iy1 > iy0) game.fillRect(ix0, iy0, ix1 - ix0, iy1 - iy0, PANEL);
}

void drawPestPanelText(uint8_t type, bool showContinue) {
  const char *title = type == 1 ? "YOU HAVE MITES!" : (type == 2 ? "YOU HAVE APHIDS!" : "YOU HAVE GNATS!");
  const char *line = type == 1 ? "Spider mites are draining the leaves"
                   : (type == 2 ? "Aphids are sucking the sap" : "Fungus gnats are in the soil");
  game.setTextColor(RED, PANEL);
  game.drawCentreString(title, 160, PEST_PANEL_Y + 12, 4);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString(line, 160, PEST_PANEL_Y + 44, 1);
  game.setTextColor(showContinue ? GOLD : TEXT_DIM, PANEL);
  game.drawCentreString(showContinue ? "TAP TO CONTINUE - then TREAT" : "Use TREAT with a pest kit",
                          160, PEST_PANEL_Y + 62, 1);
}

void animatePestInvasion() {
  pestAnimationRequested = false;
  uint8_t type = pestType ? pestType : 1;
  startSong(&pestAlarm);  // two-note siren while the bugs crawl in

  game.fillScreen(BG);
  game.fillRoundRect(PEST_PANEL_X, PEST_PANEL_Y, PEST_PANEL_W, PEST_PANEL_H, 10, PANEL);
  game.drawRoundRect(PEST_PANEL_X, PEST_PANEL_Y, PEST_PANEL_W, PEST_PANEL_H, 10, RED);
  drawPestPanelText(type, false);

  // Bugs swarm in from every edge of the screen.
  for (int i = 0; i < PEST_BUG_MAX; ++i) {
    int edge = random(4);
    PestBug &b = pestBugs[i];
    if (edge == 0)      { b.x = random(SCREEN_W); b.y = -6; }
    else if (edge == 1) { b.x = SCREEN_W + 6; b.y = random(SCREEN_H); }
    else if (edge == 2) { b.x = random(SCREEN_W); b.y = SCREEN_H + 6; }
    else                { b.x = -6; b.y = random(SCREEN_H); }
    b.heading = atan2f(SCREEN_H / 2 - b.y + random(-80, 80), SCREEN_W / 2 - b.x + random(-120, 120));
    b.speed = (type == 3 ? 3.0f : 1.4f) + random(0, 10) / 10.0f;
  }

  unsigned long start = millis();
  bool continueShown = false;
  int active = 6;
  for (int frame = 0; ; ++frame) {
    if (active < PEST_BUG_MAX && (frame % 3) == 0) ++active;

    for (int i = 0; i < active; ++i) erasePestBug((int)pestBugs[i].x, (int)pestBugs[i].y);
    game.drawRoundRect(PEST_PANEL_X, PEST_PANEL_Y, PEST_PANEL_W, PEST_PANEL_H, 10, RED);

    bool canContinue = millis() - start > 2500;
    drawPestPanelText(type, canContinue);
    continueShown = canContinue;

    for (int i = 0; i < active; ++i) {
      PestBug &b = pestBugs[i];
      b.heading += (random(-30, 31) / 100.0f) * (type == 3 ? 2.0f : 1.0f);
      b.x += cosf(b.heading) * b.speed;
      b.y += sinf(b.heading) * b.speed;
      // Turn back inward instead of leaving the screen.
      if (b.x < 4 || b.x > SCREEN_W - 4 || b.y < 4 || b.y > SCREEN_H - 4) {
        if (frame > 40) b.heading = atan2f(SCREEN_H / 2 - b.y, SCREEN_W / 2 - b.x) + random(-60, 61) / 100.0f;
      }
      drawPestBug((int)b.x, (int)b.y, b.heading, frame + i, type);
    }
    presentFrame();
    pumpDelay(type == 3 ? 25 : 40);

    if (continueShown && touchscreen.tirqTouched() && touchscreen.touched()) {
      while (touchscreen.tirqTouched() && touchscreen.touched()) delay(12);
      break;
    }
  }
  previousTouch = false;
  page = GARDEN;
  game.fillScreen(BG);
  render();
}

void gameTick() {
  bool boostWasActive = growthBoostTicks > 0;

  if (plantActive) {
    if (!ticksDuringSleep) {
      // Water used per tick. Raise these to make plants need water more often.
      uint8_t waterUse = WATER_DRAIN_PER_TICK;
      if (activeStrain == 0 || activeStrain == 2 || activeStrain == 7 ||
          activeStrain == 8 || activeStrain == 9) waterUse += THIRSTY_STRAIN_EXTRA_DRAIN;
      hydration = hydration > waterUse ? hydration - waterUse : 0;
      if (vigor > 0) --vigor;

      // Wilt timer: count how long the plant sits below 20% hydration and kill
      // it if the drought outlasts the limit (reason 3 = DEHYDRATED).
      if (hydration < 20) {
        if (dryStreakTicks < DRY_TICKS_LIMIT) ++dryStreakTicks;
        if (dryStreakTicks >= DRY_TICKS_LIMIT) {
          killPlant(3);
          saveGame();
          return;
        }
      } else {
        dryStreakTicks = 0;
      }

      if (pestType == 0 && growth > 12 && random(100) < 3) {
        pestType = random(1, 4);
        pestAnimationRequested = true;
        pestSeverity = 5;
      }
      if (pestType > 0) {
        pestSeverity = min(100, pestSeverity + 7);
        // Infestations stress the plant: vigor and hydration both drain
        // faster the worse it gets, until the pest is treated.
        if (pestSeverity >= 35 && vigor > 3)
          vigor -= min(3, 1 + pestSeverity / 34);   // ramps 2 -> 3 per tick
        uint8_t pestThirst = pestSeverity / 25;     // up to +4 water/tick
        if (pestThirst) hydration = hydration > pestThirst ? hydration - pestThirst : 0;
      }

      // Dying-plant modal: fires once per crisis on any page; re-arms when
      // the plant recovers or the dismissal cooldown lapses.
      bool plantCrisis = hydration < 20 || pestSeverity >= 80;
      if (plantCrisis && !deathWarnModal &&
          (int32_t)(millis() - deathWarnNextAllowed) >= 0) {
        deathWarnModal = true;
        render();                        // stamp the popup over this page
      } else if (!plantCrisis && !deathWarnModal) {
        deathWarnNextAllowed = 0;        // healthy again: re-arm
      }
    }

    uint8_t strainIndex = min((int)activeStrain, (int)STRAIN_COUNT - 1);
    if (!ticksDuringSleep && hydration == 0) {
      // Bone-dry soil: the plant withers fast and will not reach harvest —
      // vigor drains hard even through pest pressure, and at zero it dies.
      vigor = vigor > 6 ? vigor - 6 : 0;
      if (vigor == 0) {
        killPlant(3);
        saveGame();
        return;
      }
    } else if (hydration >= strainHydrationNeed[strainIndex] && vigor >= 18 &&
               growth < 100 && pestSeverity < 85 &&
               // Defoliation gate: once a window is ready (cooldown passed)
               // and still untouched, growth pauses until it is done —
               // defoliating is required care, not a bonus.
               !(defoliateWindow != 255 && gameMinutes >= defoliateReadyAt &&
                 defoliateWindow == currentDefoliateWindow() && defoliateClicks == 0)) {
      // With the sleep-proof wall clock, normal speed targets a ~30-minute
      // grow: base 1 + bonuses lands around 2 per 30 s tick (~50 ticks).
      int pace = gameSpeed == 0 ? 1 : (gameSpeed == 1 ? 3 : 6);
      pace += strainPaceModifier[strainIndex];
      pace = max(1, pace);
      if (boostWasActive) pace += 3;
      if (pestType > 0 && pestSeverity >= 35) pace = max(1, pace / 2);
      // Well-watered soil grows a touch faster: keep 20+ points above the
      // strain's need and the plant gains a bit of pace.
      if (hydration >= strainHydrationNeed[strainIndex] + 20) pace += 1;
      growth = min(100, growth + pace);
    } else if (!ticksDuringSleep && vigor > 2 && pestSeverity < 35) {
      vigor -= 2;
    }
  }
  if (boostWasActive) {
    --growthBoostTicks;
    saveGame();
  }
  updateDefoliateWindow();
  if (stonedLevel > 0) --stonedLevel;
  ++gameMinutes;
  // Save timed growth every five minutes; touchscreen actions save immediately.
  if (gameMinutes % 10 == 0) saveGame();
}

// --- Harvest-ready LED ---------------------------------------------------
// On this CYD the back RGB LED is common-anode (active-LOW): a GPIO pin
// sinks current, so LOW lights a color and HIGH turns it off.

void harvestLedOff() {
#if defined(S3_CYD)
  harvestPixel.setPixelColor(0, 0, 0, 0);
  harvestPixel.show();
#else
  if (LED_SINGLE_PIN != 255) {
    digitalWrite(LED_SINGLE_PIN, LOW);
    return;
  }
  digitalWrite(LED_R_PIN, HIGH);
  digitalWrite(LED_G_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);
#endif
}

void harvestLedOn() {
#if defined(S3_CYD)
  harvestPixel.setPixelColor(0, harvestPixel.Color(0, 255, 0));
  harvestPixel.show();
#else
  if (LED_SINGLE_PIN != 255) {
    digitalWrite(LED_SINGLE_PIN, HIGH);
    return;
  }
  digitalWrite(LED_R_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);
  digitalWrite(LED_G_PIN, LOW);
#endif
}

void harvestLedRed() {
#if defined(S3_CYD)
  harvestPixel.setPixelColor(0, harvestPixel.Color(255, 0, 0));
  harvestPixel.show();
#else
  if (LED_SINGLE_PIN != 255) {
    digitalWrite(LED_SINGLE_PIN, HIGH);
    return;
  }
  digitalWrite(LED_G_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);
  digitalWrite(LED_R_PIN, LOW);
#endif
}

// Applies the user's Display inversion setting. On the S3 CYD the panel is
// initialized with the ILI9341 inversion command ON (TFT_INVERSION_ON) to
// show correct colors, so the meaning of invertDisplay() flips there.
void applyDisplayInversion() {
#if defined(S3_CYD)
  canvas.invertDisplay(!displayInverted);
#else
  canvas.invertDisplay(displayInverted);
#endif
}

// Back LED states, in priority order: flashing green when the plant is ready
// to harvest; flashing red while a plant death animation is playing (the
// plant was killed); a very fast red strobe while a pest infestation is
// active; dark the rest of the time.
void updateHarvestLed() {
  static unsigned long lastToggle = 0;
  static bool lit = false;

  // Charge status on USB power: pulsing red while charging, solid green
  // when the pack is full. Overrides the game LED states until unplugged.
  if (Serial.isConnected()) {
    float v = analogReadMilliVolts(9) / 1000.0f * BATT_DIV;
    if (v > 2.5f) {                          // battery is connected
      if (v >= 4.15f) {
        harvestLedOn();                      // full: solid green
      } else {
        // pulse red on a slow 4s breathe cycle
        uint32_t t = millis() % 4000;
        uint8_t duty = t < 2000 ? (uint8_t)(t * 255 / 2000)
                               : (uint8_t)((4000 - t) * 255 / 2000);
        harvestPixel.setPixelColor(0, harvestPixel.Color(duty, 0, 0));
        harvestPixel.show();
      }
      lit = true;                            // so leaving this state turns it off
      return;
    }
  }

  bool ready = plantActive && growth >= 100;
  bool dying = plantDeathAnimationRequested || (long)(deathLedUntil - millis()) > 0;
  bool pest = plantActive && growth < 100 && pestType > 0;
  if (!ready && !dying && !pest) {
    if (millis() < rainbowUntil) {
      // Smoke-break rainbow flourish: ~5s of full-spectrum drift after a
      // smoke break, then a ~1.5s fade before the LED goes dark.
      unsigned long left = rainbowUntil - millis();
      uint8_t v = left < 1500 ? (uint8_t)(left * 150 / 1500) : 150;
      harvestPixel.setPixelColor(0, harvestPixel.ColorHSV((uint16_t)(millis() * 32), 255, v));
      harvestPixel.show();
      lit = true;
      return;
    }
    if (lit) {
      harvestLedOff();
      lit = false;
    }
    return;
  }
  unsigned long interval = (pest && !ready && !dying) ? 60 : 250;
  if (millis() - lastToggle >= interval) {
    lastToggle = millis();
    lit = !lit;
    if (lit) {
      if (ready) harvestLedOn();
      else harvestLedRed();
    } else {
      harvestLedOff();
    }
  }
}

void initSdAndTouch() {
  // Reset the FT6336U, then capacitive touch over I2C (SDA=16, SCL=15).
  pinMode(FT_RST_PIN, OUTPUT);
  digitalWrite(FT_RST_PIN, LOW);
  delay(50);
  digitalWrite(FT_RST_PIN, HIGH);
  delay(50);
  pinMode(FT_INT_PIN, INPUT_PULLUP);
  Wire.begin(16, 15);
  Serial.println("Plants are drawn in code — no SD card needed");
}

// Deep sleep: near-zero drain (radios off, RAM lost). Wakes as a full
// reboot on the next BOOT press; the game state was saved to flash.
bool sleepPromptActive = false;

void drawSleepPrompt(bool deepHighlight) {
  game.fillScreen(BG);
  game.fillRoundRect(30, 40, 260, 140, 8, PANEL);
  game.drawRoundRect(30, 40, 260, 140, 8, PANEL_LIGHT);
  game.setTextColor(TEXT_MAIN, PANEL);
  game.drawCentreString("POWER", 160, 52, 2);
  game.setTextColor(TEXT_DIM, PANEL);
  game.drawCentreString("QUICK PRESS = SLEEP", 160, 88, 1);
  if (deepHighlight) {
    game.setTextColor(RED, PANEL);
    game.drawCentreString("HOLD = DEEP SLEEP...", 160, 104, 1);
  } else {
    game.drawCentreString("HOLD 1.5s = DEEP SLEEP", 160, 104, 1);
  }
  drawButton(70, 130, 180, 26, "TAP SCREEN TO CANCEL", ACCENT_DARK);
  presentFrame();
}

void enterDeepSleep() {
  saveGame();
  chipToneOff();
  drumToneOff();
  ampEnable(false);   // FM8002E off through deep sleep
  activeSong = nullptr;
  drumPlaying = false;
  otaRestoreRadio();                 // close any WiFi session before sleep
  harvestLedOff();                   // LED rainbow pauses through sleep
  if (nearbyReady) {
    esp_now_deinit();
    nearbyReady = false;
    WiFi.mode(WIFI_OFF);
  }
  game.fillScreen(0x0861);
  game.setTextColor(TFT_WHITE, 0x0861);
  game.drawCentreString("DEEP SLEEP", 160, 108, 2);
  game.drawCentreString("PRESS BOOT TO POWER ON", 160, 139, 1);
  presentFrame();
  delay(400);
  canvas.writecommand(0x28);  // display off
  digitalWrite(TFT_BL, LOW);

  // Release the button before arming wake, or GPIO0 LOW would wake at once.
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();  // does not return; wake = full reboot
}

// Charge light for the charging-idle loop: pulsing red while charging,
// solid green when the pack is full, dark with no battery present.
void chargeLedSleepTick() {
  if (Serial.isConnected()) {
    float v = analogReadMilliVolts(9) / 1000.0f * BATT_DIV;
    if (v > 2.5f) {
      if (v >= 4.15f) {
        harvestLedOn();
      } else {
        uint32_t t = millis() % 4000;
        uint8_t duty = t < 2000 ? (uint8_t)(t * 255 / 2000)
                               : (uint8_t)((4000 - t) * 255 / 2000);
        harvestPixel.setPixelColor(0, harvestPixel.Color(duty, 0, 0));
        harvestPixel.show();
      }
      return;
    }
  }
  harvestLedOff();
}

void sleepUntilBootWake() {
  saveGame();
  chipToneOff();      // silence both voices through light sleep
  drumToneOff();
  ampEnable(false);   // amp off through light sleep
  activeSong = nullptr;
  drumPlaying = false;
  otaRestoreRadio();                 // close any WiFi session before sleep
  harvestLedOff();                   // LED rainbow pauses through sleep
  if (nearbyReady) {
    esp_now_deinit();
    nearbyReady = false;
    WiFi.mode(WIFI_OFF);
  }
  game.fillScreen(0x0861);
  game.setTextColor(TFT_WHITE, 0x0861);
  game.drawCentreString("SLEEP MODE", 160, 108, 2);
  game.drawCentreString("PRESS BOOT TO WAKE", 160, 139, 1);
  presentFrame();
  delay(250);
  canvas.writecommand(0x28);  // ILI9341 display off
  digitalWrite(TFT_BL, LOW);

  // Release the button before arming low-level wake, or GPIO0 would wake at once.
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);

  // The charger chip charges in any mode (it is hardware), but the charge
  // light needs the CPU. On USB power: stay awake with the screen off and
  // run the charge light; on battery: true light sleep, with a 10 s timer
  // wake that notices a USB plug-in and starts the charge light.
  for (;;) {
    while (Serial.isConnected() && analogReadMilliVolts(9) / 1000.0f * BATT_DIV > 2.5f &&
           digitalRead(BOOT_BUTTON_PIN) == HIGH) {
      chargeLedSleepTick();
      delay(20);
    }
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) break;   // BOOT = wake up for real
    harvestLedOff();  // unplugged mid-charge: LED dark while light-sleeping
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_sleep_enable_timer_wakeup(10000000ULL);       // re-check USB every 10 s
    esp_light_sleep_start();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) break;  // BOOT
    // Timer wake: charge if USB appeared, otherwise sleep again.
  }
  justWokeFromSleep = true;  // loop runs the missed ticks dormant (no drying)
  while (digitalRead(BOOT_BUTTON_PIN) == LOW) delay(10);

  canvas.init();
  canvas.setRotation(1);
  applyDisplayInversion();
  initSdAndTouch();
  startNearbyRadio();
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  audioLastVol = 255;  // make the next pump re-apply codec volume + amp
  previousTouch = false;
  lastGameTick = millis();
  lastFarmerVisitAt = millis();
  render();
}

void showBootSplash() {
  // Solid black backdrop, then the leaf breathes via a small 72x72 sprite
  // blitted atomically each step - no erase-then-redraw flicker.
  game.fillScreen(TFT_BLACK);
  game.setTextColor(TFT_WHITE, TFT_BLACK);
  game.drawCentreString("WIN FLORIDA", 160, 134, 4);
  game.setTextColor(0x9EFB, TFT_BLACK);
  game.drawCentreString("GARDEN SIM", 160, 176, 2);
  // Firmware tagline: tiny dim line under the title, handy after OTA swaps.
  game.setTextColor(0x630C, TFT_BLACK);
  game.drawCentreString(FW_VERSION_FULL " - " FW_DATE, 160, 196, 1);
  presentFrame();

  TFT_eSprite spr(&canvas);  // panel-bound sprite; blitted into the scaled view
  // Logo block art scaled onto the 480x320 panel: 24 mask cells drawn with
  // alternating 4/5 px blocks = 108x108, at the scaled (124,50) anchor.
  int cellX[24], cellY[24];
  int acc = 0;
  for (int i = 0; i < 24; ++i) { cellX[i] = acc; acc += (i % 2) ? 5 : 4; }
  acc = 0;
  for (int i = 0; i < 24; ++i) { cellY[i] = acc; acc += (i % 2) ? 5 : 4; }
  spr.createSprite(108, 108);
  const int frames = 20;
  for (int i = 0; i < frames; ++i) {
    float t = (float)i / (float)frames;
    float pulse = 0.5f - 0.5f * cosf(t * 2.0f * PI);  // 0 -> 1 -> 0, slow breath
    int v = 140 + (int)(pulse * 115.0f);  // logo brightness 140..255
    spr.fillSprite(TFT_BLACK);
    for (int row = 0; row < 24; ++row) {
      for (int col = 0; col < 24; ++col) {
        int bit = row * 24 + col;
        if (winLeafLogoMask[bit >> 3] & (0x80 >> (bit & 7)))
          spr.fillRect(cellX[col], cellY[row], (col % 2) ? 5 : 4,
                       (row % 2) ? 5 : 4, rgb565(v - 18, v, v - 10));
      }
    }
    spr.pushSprite(SX(124), SY(50));
    delay(80);
    audioPump();  // keep the boot chant's DMA fed while the logo breathes
  }
  spr.deleteSprite();
}

// ---------------------------------------------------------------------------
// Touch calibration
// Raw XPT2046 readings that line up with screen edges x=0/319 and y=0/239.
// Defaults are the common CYD values. Recalibrate from the options hub
// (TOUCH CAL button), then tap the 4 crosshairs.
// The result is saved to flash and survives RESET GAME.
// ---------------------------------------------------------------------------
int touchRawXMin = 200;
int touchRawXMax = 3700;
int touchRawYMin = 240;
int touchRawYMax = 3800;

void loadTouchCalibration() {
  touchRawXMin = preferences.getShort("tcXMin", 200);
  touchRawXMax = preferences.getShort("tcXMax", 3700);
  touchRawYMin = preferences.getShort("tcYMin", 240);
  touchRawYMax = preferences.getShort("tcYMax", 3800);
  if (abs(touchRawXMax - touchRawXMin) < 1000 || abs(touchRawYMax - touchRawYMin) < 1000) {
    touchRawXMin = 200; touchRawXMax = 3700;
    touchRawYMin = 240; touchRawYMax = 3800;
  }
}


// Converts the current touch to screen coordinates using the calibration.
void readTouch(int &x, int &y) {
#if defined(S3_CYD)
  // FT6336U reports 480x320 display pixels; scale to game space.
  TS_Point raw = touchscreen.getPoint();
  x = constrain(raw.x * 2 / 3, 0, SCREEN_W - 1);
  y = constrain(raw.y * 3 / 4, 0, SCREEN_H - 1);
#else
  TS_Point raw = touchscreen.getPoint();
  x = constrain(map(raw.x, touchRawXMin, touchRawXMax, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
  y = constrain(map(raw.y, touchRawYMin, touchRawYMax, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
#endif
}


// Waits for one tap and returns the averaged raw reading.



void setup() {
  pinMode(TFT_BL, OUTPUT);
  // Backlight stays off until a clean frame is on the panel, so the power-on
  // white / rainbow garbage on the display is never visible.

  Serial.begin(115200);
#if defined(S3_CYD)
  delay(3000);  // S3 native USB-CDC needs time to enumerate before logging
#endif
  Serial.println();
#if defined(S3_CYD)
  Serial.printf("[BUILD] %s: brand picker OTA\n", FW_VERSION_FULL);
#else
  Serial.println("[BUILD] v9: pedro + giant card select + sd installer + easy-first catalog");
#endif
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // Chiptune audio comes up after the I2C bus is ready (audioInit in setup).
  // Harvest LED pins
#if defined(S3_CYD)
  harvestPixel.begin();
  harvestPixel.show();  // dark until the garden needs the LED
#else
  if (LED_SINGLE_PIN != 255) {
    pinMode(LED_SINGLE_PIN, OUTPUT);
  } else {
    pinMode(LED_R_PIN, OUTPUT);
    pinMode(LED_G_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
  }
  harvestLedOff();
#endif
  canvas.init();
  canvas.setRotation(1);  // Landscape 480x320 on this panel

  // Full-frame render buffer: every page draws into PSRAM and presentFrame()
  // pushes it in one atomic pass — no more mid-draw flicker anywhere.
  frameBuf.setColorDepth(16);
  if (frameBuf.createSprite(PANEL_W, PANEL_H)) {
    game.tft = (TFT_eSPI *)&frameBuf;
    frameBufferActive = true;
  } else {
    Serial.println("[GFX] frame buffer alloc failed - direct drawing");
  }

  loadGame();
  catchUpGrowTime();   // grow continues while the screen slept / board reset
  Serial.printf("[CLOCK] t=%lus, grow epoch=%lu (sleep catch-up done)\n",
                (unsigned long)time(nullptr), (unsigned long)growEpoch);
  applyDisplayInversion();
  game.fillScreen(BG);
  presentFrame();
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);  // panel is clean, light it up

  initSdAndTouch();
  audioInit();         // ES8311 codec + I2S (Wire is up now)
  playBootChant();     // crowd chant plays over the boot splash
  startNearbyRadio();
  randomSeed((uint32_t)micros() ^ (uint32_t)analogRead(34));
  loadTouchCalibration();
  showBootSplash();
  lastGameTick = millis();
  lastFarmerVisitAt = millis();
  render();
}



void loop() {
  if (sleepPromptActive) {
    // Power popup is up: the next BOOT press's duration chooses the mode,
    // any screen tap cancels back to the game.
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      delay(35);
      if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        unsigned long pressStart = millis();
        bool deepArmed = false;
        while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
          if (!deepArmed && millis() - pressStart >= 1500) {
            deepArmed = true;
            drawSleepPrompt(true);
          }
          delay(10);
        }
        sleepPromptActive = false;
        if (deepArmed) enterDeepSleep();
        else sleepUntilBootWake();
        return;
      }
    }
    if (touchscreen.tirqTouched() && touchscreen.touched()) {
      while (touchscreen.tirqTouched() && touchscreen.touched()) delay(10);
      sleepPromptActive = false;
      audioResumeAfterStall();  // back to the game: groove returns clean
      render();
    }
    delay(8);
    return;
  }
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    delay(35);
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      sleepPromptActive = true;
      audioPauseForStall();  // the popup parks the loop; keep DMA fed with silence
      drawSleepPrompt(false);
      return;
    }
  }

  // New-message popup: tap it to jump to the inbox, tap elsewhere to dismiss,
  // or let it time out (the underlying page is redrawn once, on expiry).
  if (nearbyPopupActive) {
    if (page == NEARBY) {
      nearbyPopupActive = false;   // inbox is already on screen; drop the card
    } else if ((long)(nearbyPopupUntil - millis()) <= 0) {
      nearbyPopupActive = false;
      render();
    } else if (touchscreen.tirqTouched() && touchscreen.touched()) {
      int x, y;
      readTouch(x, y);
      bool onPopup = x >= 160 && y >= 190;
      while (touchscreen.tirqTouched() && touchscreen.touched()) delay(10);
      if (onPopup) {
        nearbyPopupActive = false;
        page = NEARBY;
        nearbyMode = 3;
        render();
      }
      refreshNearbyPopup();  // keep overlays stamped on the consume path
      return;  // consume the tap either way so the page under doesn't react
    }
  }

  serviceNearby();
  updateHarvestLed();
  // Async WiFi scan for the login screen: pick up the results when ready.
  if (wifiScanPending && page == WIFI_LOGIN && wifiLoginStage == 0 &&
      WiFi.scanComplete() >= 0) {
    wifiScanPending = false;
    wifiScanFill();
    render();
  }
  updateMusic();  // advance the chiptune sequencer

  // Grow clock ticks by WALL time (the RTC keeps counting through sleep), so
  // a wake from light sleep or deep sleep catches up on the missed ticks.
  uint32_t nowE = (uint32_t)time(nullptr);
  unsigned long now = millis();
  const uint8_t secPerTick = gameSpeed == 2 ? 4 : 30;
  if (nowE > growEpoch) {
    uint32_t ticks = (nowE - growEpoch) / secPerTick;
    if (ticks > 0) {
      if (ticks > 2880) { ticks = 2880; growEpoch = nowE; }  // bound a huge jump
      else growEpoch += ticks * secPerTick;
      // A burst right after wake is sleep time: run it dormant (no drying).
      ticksDuringSleep = justWokeFromSleep;
      while (ticks--) gameTick();
      ticksDuringSleep = false;
      // Light refresh: while the growth stage (plant art) is unchanged, the
      // garden page only needs its stat numbers updated, and the fullscreen
      // view (no stats) needs no redraw at all. No more periodic flicker.
      if (page == GARDEN && plantActive && growth < 100 &&
          stageForGrowth() == lastDrawnStage) {
        drawGardenSidePanel();
        presentWithOverlays();   // side panel only: keep overlays stamped
      } else if (page == PLANT_VIEW && plantActive &&
                 stageForGrowth() == lastDrawnStage) {
        // Fullscreen art unchanged; nothing on screen needs updating.
      } else {
        render();
      }
    }
  }

  if (plantDeathAnimationRequested) {
    animatePlantDeath();
    render();
  }
  if (pestAnimationRequested && plantActive && page != PLANTING && page != CHARACTER_SELECT) {
    animatePestInvasion();
  }
  if (defoliatePromptPending != 255 && gameMinutes >= defoliateReadyAt &&
      (page == GARDEN || page == PLANT_VIEW)) {
    showDefoliatePrompt();
  }

  if (resetArmed && millis() - resetArmedAt > 10000UL) {
    resetArmed = false;
    render();
  }

  if (waterAnimActive) stepWateringAnimation();

  if ((page == GARDEN || page == PLANT_VIEW) && plantActive && growth < 100 &&
      !waterAnimActive && !deathWarnModal &&
      now - lastFarmerVisitAt >= 10000UL) {
    lastFarmerVisitAt = now;
    startFarmerOverlay();   // non-blocking: re-stamps only after repaints
    now = millis();
  }

  if (page == PLANTING) {
    bool seedTouched = touchscreen.tirqTouched() && touchscreen.touched();
    if (!seedTouched) {
      previousTouch = false;
      refreshNearbyPopup();
      delay(8);
      return;
    }
    int x, y;
    readTouch(x, y);
    if (!previousTouch) {
      previousTouch = true;
      if (deathWarnModal) {
        deathWarnModal = false;
        deathWarnNextAllowed = millis() + 90000UL;
        if (plantActive) page = GARDEN;
        render();
        refreshNearbyPopup();
        delay(8);
        return;
      }
      // Strain picker arrows (batch must still be untouched).
      if (plantedPotCount() == 0 && countSeededStrains() > 1 && y >= 56 && y < 78) {
        if (x < 54) plantStrain = nextSeededStrain(plantStrain, -1);
        else if (x > 266) plantStrain = nextSeededStrain(plantStrain, 1);
        render();
        refreshNearbyPopup();
        delay(8);
        return;
      }
      plantSeedFromTap(x, y);
    }
    refreshNearbyPopup();
    delay(8);
    return;
  }

  // (Settings pages use the normal tap handling again; the drag-scroll
  // gesture system was removed when the hub stopped scrolling.)
  bool isTouched = touchscreen.tirqTouched() && touchscreen.touched();
  if (!isTouched) {
    previousTouch = false;
    refreshNearbyPopup();
    delay(8);
    return;
  }

  if (!previousTouch) {
    previousTouch = true;
    int x, y;
    readTouch(x, y);
    handleTouch(x, y);
  }
  refreshNearbyPopup();
  delay(4);
}
