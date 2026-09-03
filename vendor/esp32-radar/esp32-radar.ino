// ESP32-RADAR — Wi-Fi / BLE radar scope for the ideaspark ESP32 board
// with the integrated 1.9" ST7789 (170x320 portrait).
//
//   Angle   stable per device, hashed from BSSID / BLE address, so a contact
//           holds its bearing between sweeps instead of jumping around.
//   Radius  signal strength. Strong is close to the centre, -95 dBm is the rim.
//   Colour  green = Wi-Fi AP, amber = BLE advertiser.
//
//   BOOT button cycles BOTH -> WI-FI -> BLE.
//
// Both scans run asynchronously so the sweep keeps turning while they work; a
// blocking scan would freeze the display for seconds at a time.
//
// Persistence is per-contact rather than a full-buffer phosphor fade: fading
// 54,400 pixels every frame costs more than it is worth, and brightening each
// blip as the beam passes gives the same read.

#include <WiFi.h>
#include <NimBLEDevice.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static constexpr int PIN_SCK = 18, PIN_MOSI = 23, PIN_CS = 15;
static constexpr int PIN_DC = 2, PIN_RST = 4, PIN_BLK = 32;
static constexpr int PIN_BTN = 0;

static constexpr int W = 170, H = 320;
static constexpr int CX = 85, CY = 112, R = 78;      // scope centre and radius
static constexpr uint32_t BLE_MS = 3000;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel; lgfx::Bus_SPI _bus;
 public:
  LGFX() {
    { auto c = _bus.config();
      c.spi_host = VSPI_HOST; c.spi_mode = 0;
      c.freq_write = 40000000; c.freq_read = 16000000;
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_SCK; c.pin_mosi = PIN_MOSI; c.pin_miso = -1; c.pin_dc = PIN_DC;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = PIN_CS; c.pin_rst = PIN_RST; c.pin_busy = -1;
      c.panel_width = W; c.panel_height = H;
      c.memory_width = 240; c.memory_height = 320;   // ST7789 controller RAM
      c.offset_x = 35; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    setPanel(&_panel);
  }
};
static LGFX tft;
static LGFX_Sprite fb(&tft);
static bool haveFB = false;

// ---------------------------------------------------------------- palette --
// An 8-bit palette sprite, not 16-bit: the full-colour buffer is 108,800 bytes
// and left only ~13 KB of heap, so NimBLE called abort() the moment it tried to
// start a scan. A radar needs about a dozen colours plus a green ramp, so 8bpp
// costs nothing visually and gives back 54 KB.
enum : uint8_t {
  C_BG = 0, C_GRID, C_RING, C_TEXT, C_HOT, C_BLE, C_WARN, C_HOTDIM, C_BLEDIM,
  RAMP0 = 16, RAMPN = 32                     // 32-step green ramp for the sweep
};
struct RGB8 { uint8_t r, g, b; };
static RGB8     PAL8[RAMP0 + RAMPN];   // authored values, 8 bits per channel
static uint16_t PAL [RAMP0 + RAMPN];   // same colours in RGB565, for direct draw
// Sprite drawing takes palette indices; direct drawing needs real colours.
static inline uint32_t C(uint8_t i) { return haveFB ? (uint32_t)i : (uint32_t)PAL[i]; }

static void buildPalette() {
  PAL8[C_BG]     = {  0,   0,   0};
  PAL8[C_GRID]   = {  0,  46,  18};
  PAL8[C_RING]   = {  0,  92,  36};
  PAL8[C_TEXT]   = { 96, 220, 130};
  PAL8[C_HOT]    = { 40, 255,  90};
  PAL8[C_BLE]    = {255, 168,  32};
  PAL8[C_WARN]   = {255,  64,  48};
  PAL8[C_HOTDIM] = { 16, 110,  44};
  PAL8[C_BLEDIM] = {120,  76,  12};
  for (int i = 0; i < RAMPN; i++) {
    float f = (float)i / (RAMPN - 1);
    PAL8[RAMP0 + i] = { (uint8_t)(30 * f), (uint8_t)(235 * f), (uint8_t)(70 * f) };
  }
  for (int i = 0; i < RAMP0 + RAMPN; i++)
    PAL[i] = tft.color565(PAL8[i].r, PAL8[i].g, PAL8[i].b);

  // Must use the (index, r, g, b) overload. The templated setPaletteColor(index,
  // color) runs convert_to_rgb888() on its argument, which reads a plain integer
  // as RGB888 — feeding it an RGB565 value turns the whole palette near-black and
  // the panel shows nothing at all.
  if (haveFB)
    for (int i = 0; i < RAMP0 + RAMPN; i++)
      fb.setPaletteColor(i, PAL8[i].r, PAL8[i].g, PAL8[i].b);
}

// ---------------------------------------------------------------- contacts -
struct Blip {
  char     id[22];
  char     key[18];
  uint16_t ang;        // degrees, stable
  uint8_t  rad;        // px from centre
  int8_t   rssi;
  uint8_t  type;       // 0 wifi, 1 ble
  bool     open;
  uint32_t seen;
};
// Per-type quotas. BLE privacy addresses rotate, so a busy room delivers dozens
// of fresh keys per sweep; a single shared table lets that churn evict every
// Wi-Fi AP within one pass and the scope shows only ephemeral BLE contacts.
static constexpr int MAXB = 128;      // ~52 B each, so 6.6 KB of .bss
static constexpr int CAP_WIFI = 56;
static constexpr int CAP_BLE  = 72;
static Blip blips[MAXB];
static int nBlip = 0;
static int nWifi = 0, nBle = 0;

static uint16_t hashAngle(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h % 360;
}

static void contact(const char* key, const char* id, int8_t rssi, uint8_t type, bool open) {
  int slot = -1;
  for (int i = 0; i < nBlip; i++) if (!strcmp(blips[i].key, key)) { slot = i; break; }
  if (slot < 0) {
    int same = 0;
    for (int i = 0; i < nBlip; i++) if (blips[i].type == type) same++;
    const int cap = type ? CAP_BLE : CAP_WIFI;
    if (nBlip < MAXB && same < cap) slot = nBlip++;
    else {                                   // evict the stalest of the SAME type
      slot = -1;
      for (int i = 0; i < nBlip; i++) {
        if (blips[i].type != type) continue;
        if (slot < 0 || blips[i].seen < blips[slot].seen) slot = i;
      }
      if (slot < 0) {                        // none of this type yet, table full
        slot = 0;
        for (int i = 1; i < nBlip; i++) if (blips[i].seen < blips[slot].seen) slot = i;
      }
    }
    snprintf(blips[slot].key, sizeof blips[slot].key, "%s", key);
    blips[slot].ang = hashAngle(key);
  }
  snprintf(blips[slot].id, sizeof blips[slot].id, "%s", id);
  blips[slot].rssi = rssi;
  blips[slot].type = type;
  blips[slot].open = open;
  blips[slot].seen = millis();
  int r = map(constrain(rssi, -95, -30), -95, -30, R - 4, 9);
  blips[slot].rad = constrain(r, 8, R - 3);
}

// forward declarations: serviceScans() below triggers the dump, which is
// defined further down alongside the other reporting code
static void dumpRegister();
static bool contDump = false;

// ------------------------------------------------------------------ scans --
enum Phase : uint8_t { PH_WIFI, PH_BLE };
static Phase phase = PH_WIFI;
static uint8_t mode = 0;                     // 0 both, 1 wifi, 2 ble
static const char* MODE_NAME[3] = { "BOTH", "WI-FI", "BLE" };

static void startWifi() { WiFi.scanNetworks(true, true); }
static void startBle()  { NimBLEDevice::getScan()->start(BLE_MS, false, true); }

static void nextPhase() {
  if (mode == 1) { startWifi(); phase = PH_WIFI; return; }
  if (mode == 2) { startBle();  phase = PH_BLE;  return; }
  if (phase == PH_WIFI) { startBle(); phase = PH_BLE; }
  else                  { startWifi(); phase = PH_WIFI; }
}

static void serviceScans() {
  if (phase == PH_WIFI) {
    int r = WiFi.scanComplete();
    if (r >= 0) {
      nWifi = r;
      for (int i = 0; i < r; i++) {
        String ss = WiFi.SSID(i);
        contact(WiFi.BSSIDstr(i).c_str(), ss.length() ? ss.c_str() : "<hidden>",
                WiFi.RSSI(i), 0, WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      }
      WiFi.scanDelete();
      nextPhase();
    } else if (r == WIFI_SCAN_FAILED) startWifi();
  } else {
    NimBLEScan* s = NimBLEDevice::getScan();
    if (!s->isScanning()) {
      NimBLEScanResults res = s->getResults();
      nBle = res.getCount();
      for (int i = 0; i < nBle; i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        std::string a = d->getAddress().toString();
        contact(a.c_str(), d->haveName() ? d->getName().c_str() : a.c_str(),
                d->getRSSI(), 1, false);
      }
      s->clearResults();
      if (contDump) dumpRegister();
      nextPhase();
    }
  }
}


// ------------------------------------------------------------------ dump ---
// Printed on demand, not every sweep: a full listing is ~100 lines and
// Serial.printf blocks, which at 115200 would stall the sweep for ~0.5 s.

static void dumpRegister() {
  uint32_t now = millis();
  int idx[MAXB], n = nBlip;
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 1; i < n; i++) {                 // strongest first
    int k = idx[i], j = i - 1;
    while (j >= 0 && blips[idx[j]].rssi < blips[k].rssi) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = k;
  }
  Serial.printf("\n=== CONTACT REGISTER  %d tracked  (%d AP / %d BLE this sweep)  mode %s ===\n",
                n, nWifi, nBle, MODE_NAME[mode]);
  Serial.println("TYPE  BRG  RSSI  AGE   NAME                   ADDRESS");
  Serial.println("----  ---  ----  ----  ---------------------  -----------------");
  for (int r = 0; r < n; r++) {
    const Blip& b = blips[idx[r]];
    Serial.printf("%-4s  %3u  %4d  %3lus  %-21s  %s\n",
                  b.type ? "BLE" : (b.open ? "OPEN" : "WIFI"),
                  b.ang, b.rssi, (unsigned long)((now - b.seen) / 1000),
                  b.id, b.key);
  }
  Serial.printf("--- BRG is the on-screen bearing in degrees, 0 = right, "
                "clockwise. OPEN = unencrypted AP (red blip).\n\n");
}

static void pollCommands() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'd' || c == 'D') dumpRegister();
    else if (c == 'c' || c == 'C') {
      contDump = !contDump;
      Serial.printf("[cmd] continuous dump %s\n", contDump ? "ON" : "OFF");
    } else if (c == 'h' || c == 'H' || c == '?') {
      Serial.println("[cmd] d = dump register once, c = dump every sweep, h = help");
    }
  }
}

// ---------------------------------------------------------------- drawing --
static float sweep = 0;                       // degrees

static inline uint8_t rampIdx(float f) {
  int i = (int)(f * (RAMPN - 1));
  if (i < 0) i = 0; if (i > RAMPN - 1) i = RAMPN - 1;
  return (uint8_t)(RAMP0 + i);
}

static void drawScope() {
  auto& g = haveFB ? *(LovyanGFX*)&fb : *(LovyanGFX*)&tft;
  g.fillScreen(C(C_BG));

  // range rings, labelled in dBm
  for (int i = 1; i <= 4; i++) {
    int rr = R * i / 4;
    g.drawCircle(CX, CY, rr, C(i == 4 ? C_RING : C_GRID));
  }
  g.drawFastHLine(CX - R, CY, R * 2, C(C_GRID));
  g.drawFastVLine(CX, CY - R, R * 2, C(C_GRID));

  // sweep: a short trailing wedge, brightest at the leading edge
  for (int k = 0; k < 26; k++) {
    float a = (sweep - k * 2.2f) * 0.017453f;
    float f = 1.0f - (float)k / 26.0f;
    g.drawLine(CX, CY, CX + (int)(cosf(a) * R), CY + (int)(sinf(a) * R),
               C(rampIdx(f * f * 0.85f)));
  }

  // contacts
  uint32_t now = millis();
  for (int i = 0; i < nBlip; i++) {
    const Blip& b = blips[i];
    float a = b.ang * 0.017453f;
    int x = CX + (int)(cosf(a) * b.rad);
    int y = CY + (int)(sinf(a) * b.rad);

    // how far behind the beam this bearing sits, 0 = just painted
    float delta = fmodf(sweep - b.ang + 720.0f, 360.0f);
    float lit = delta < 90.0f ? 1.0f - delta / 90.0f : 0.0f;
    bool stale = (now - b.seen) > 20000;

    uint8_t col;
    if (b.type == 1)      col = C_BLE;
    else if (b.open)      col = C_WARN;
    else                  col = C_HOT;
    if (lit < 0.25f)      col = b.type ? C_BLEDIM : (stale ? C_GRID : C_HOTDIM);

    int sz = 2 + (int)(lit * 2);
    g.fillRect(x - sz / 2, y - sz / 2, sz, sz, C(col));
    if (lit > 0.55f) {                        // brief halo as the beam passes
      g.drawCircle(x, y, 3 + (int)(lit * 3), C(col));
    }
  }

  g.drawCircle(CX, CY, R, C(C_RING));
  g.fillRect(CX - 1, CY - 1, 3, 3, C(C_TEXT));
}

static void drawPanel() {
  auto& g = haveFB ? *(LovyanGFX*)&fb : *(LovyanGFX*)&tft;

  g.setTextDatum(lgfx::top_left);
  g.setFont(&fonts::Font0);
  g.setTextColor(C(C_TEXT), C(C_BG));
  g.drawString("RADAR", 6, 6);
  g.setTextDatum(lgfx::top_right);
  g.drawString(MODE_NAME[mode], W - 6, 6);
  g.setTextDatum(lgfx::top_left);

  // rim labels for the range rings
  g.setTextColor(C(C_GRID), C(C_BG));
  g.drawString("-30", CX + 4, CY - 10);
  g.drawString("-95", CX + R - 18, CY - 10);

  int y = CY + R + 12;
  g.drawFastHLine(0, y, W, C(C_GRID));
  y += 6;

  g.setTextColor(C(C_TEXT), C(C_BG));
  char b[40];
  snprintf(b, sizeof b, "AP %d   BLE %d   TRK %d", nWifi, nBle, nBlip);
  g.drawString(b, 6, y);
  y += 14;

  // strongest few contacts, closest first
  int idx[MAXB], n = nBlip;
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 1; i < n; i++) {
    int k = idx[i], j = i - 1;
    while (j >= 0 && blips[idx[j]].rssi < blips[k].rssi) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = k;
  }
  int rows = (H - y - 6) / 15;
  if (rows > 7) rows = 7;
  for (int r = 0; r < rows && r < n; r++) {
    const Blip& bl = blips[idx[r]];
    uint8_t col = bl.type ? C_BLE : (bl.open ? C_WARN : C_HOT);
    g.fillRect(6, y + 4, 4, 4, C(col));
    g.setTextColor(C(C_TEXT), C(C_BG));
    g.setClipRect(14, y, 112, 10);
    g.drawString(bl.id, 14, y);
    g.clearClipRect();
    g.setTextDatum(lgfx::top_right);
    char rb[8]; snprintf(rb, sizeof rb, "%d", bl.rssi);
    g.drawString(rb, W - 6, y);
    g.setTextDatum(lgfx::top_left);
    y += 15;
  }
}

// ------------------------------------------------------------------ setup --
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== ESP32 WI-FI / BLE RADAR ===");
  Serial.println("BOOT cycles BOTH / WI-FI / BLE");
  Serial.println("serial: d = dump full contact list, c = dump every sweep, h = help");

  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_BLK, OUTPUT); digitalWrite(PIN_BLK, HIGH);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BG);

  // Drawn direct with real colours, no sprite involved: if this appears but the
  // radar does not, the fault is in the sprite/palette path, not the panel.
  tft.fillScreen(tft.color565(0, 24, 10));
  tft.setTextDatum(lgfx::middle_center);
  tft.setFont(&fonts::Font4);
  tft.setTextColor(tft.color565(40, 255, 90));
  tft.drawString("RADAR", W / 2, H / 2 - 14);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(tft.color565(96, 220, 130));
  tft.drawString("wifi + ble", W / 2, H / 2 + 14);
  tft.setTextDatum(lgfx::top_left);
  delay(1200);

  // MUST be palette_8bit, not 8. In LovyanGFX `rgb332_1Byte = 8` and
  // `palette_8bit = 8 | has_palette`; createSprite only builds a palette when
  // that bit is set. With plain 8 the sprite is RGB332, setPaletteColor returns
  // silently, and drawing with palette indices 0-47 renders them as RGB332
  // values that are all but black — a completely blank panel.
  fb.setColorDepth(lgfx::palette_8bit);
  haveFB = fb.createSprite(W, H);
  buildPalette();
  Serial.printf("panel %dx%d, framebuffer %s (%u B), heap %u\n",
                tft.width(), tft.height(), haveFB ? "OK 8bpp" : "FAILED (direct)",
                (unsigned)(haveFB ? W * H : 0), (unsigned)ESP.getFreeHeap());

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(100);

  NimBLEDevice::init("");
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setActiveScan(true);
  s->setInterval(100);
  s->setWindow(99);
  s->setMaxResults(100);  // Schiphol saturated 64; each result costs heap, so
                          // watch the heap line if this is raised further

  startWifi();
  phase = PH_WIFI;
  Serial.println("sweeping...");
}

void loop() {
  static uint32_t last = millis(), secT = 0;
  static int frames = 0;
  uint32_t now = millis();
  float dt = (now - last) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  last = now;

  pollCommands();
  serviceScans();

  static bool prev = true; static uint32_t bt = 0;
  bool bs = digitalRead(PIN_BTN);
  if (prev && !bs && now - bt > 250) {
    bt = now;
    mode = (mode + 1) % 3;
    Serial.printf("[button] mode -> %s\n", MODE_NAME[mode]);
    if (mode == 1 && phase == PH_BLE) { NimBLEDevice::getScan()->stop(); startWifi(); phase = PH_WIFI; }
    if (mode == 2 && phase == PH_WIFI) { WiFi.scanDelete(); startBle(); phase = PH_BLE; }
  }
  prev = bs;

  sweep += 62.0f * dt;                       // ~5.8 s per revolution
  if (sweep >= 360.0f) sweep -= 360.0f;

  drawScope();
  drawPanel();
  if (haveFB) fb.pushSprite(0, 0);

  frames++;
  if (now - secT >= 1000) {
    Serial.printf("fps %d  ap %d  ble %d  tracked %d  mode %s  heap %u\n",
                  frames, nWifi, nBle, nBlip, MODE_NAME[mode],
                  (unsigned)ESP.getFreeHeap());
    frames = 0; secT = now;
  }
}
