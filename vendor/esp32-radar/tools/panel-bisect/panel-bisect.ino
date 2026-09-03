// Panel config bisect. Cycles LovyanGFX configurations, 4 s each, drawing a
// large variant number on a distinctly coloured screen. Tell me which number
// looks correct (or which show anything at all) and that pins the config.
//
// Pins are known good — FlightRadar24 drove this panel via TFT_eSPI on exactly
// these. What is unproven is my LovyanGFX panel configuration.
//
// Serial at 115200 announces each variant as it starts.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static constexpr int PIN_SCK = 18, PIN_MOSI = 23, PIN_CS = 15;
static constexpr int PIN_DC = 2, PIN_RST = 4, PIN_BLK = 32;
static constexpr int W = 170, H = 320;

struct Variant {
  const char* note;
  uint8_t  spi_mode;
  bool     invert;
  int      offset_x;
  bool     rgb_order;
  bool     three_wire;
  uint32_t freq;
};

static const Variant V[] = {
  { "baseline: mode0 inv=1 ox=35 3wire=1 40MHz", 0, true,  35, false, true,  40000000 },
  { "spi_mode 3",                                3, true,  35, false, true,  40000000 },
  { "invert OFF",                                0, false, 35, false, true,  40000000 },
  { "offset_x 0",                                0, true,   0, false, true,  40000000 },
  { "rgb_order BGR",                             0, true,  35, true,  true,  40000000 },
  { "spi_3wire OFF",                             0, true,  35, false, false, 40000000 },
  { "20 MHz",                                    0, true,  35, false, true,  20000000 },
  { "mode3 + 3wire OFF + 20MHz",                 3, true,  35, false, false, 20000000 },
};
static constexpr int NV = sizeof(V) / sizeof(V[0]);

class LGFX : public lgfx::LGFX_Device {
 public:
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_SPI      bus;
  void apply(const Variant& v) {
    { auto c = bus.config();
      c.spi_host = VSPI_HOST;
      c.spi_mode = v.spi_mode;
      c.freq_write = v.freq;
      c.freq_read = 8000000;
      c.spi_3wire = v.three_wire;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_SCK; c.pin_mosi = PIN_MOSI; c.pin_miso = -1; c.pin_dc = PIN_DC;
      bus.config(c); panel.setBus(&bus); }
    { auto c = panel.config();
      c.pin_cs = PIN_CS; c.pin_rst = PIN_RST; c.pin_busy = -1;
      c.panel_width = W; c.panel_height = H;
      c.memory_width = 240; c.memory_height = 320;   // ST7789 controller RAM
      c.offset_x = v.offset_x; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = v.invert; c.rgb_order = v.rgb_order;
      c.dlen_16bit = false; c.bus_shared = false;
      panel.config(c); }
    setPanel(&panel);
  }
};
static LGFX tft;

static const uint16_t TINT[NV] = {
  0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFC00, 0xFFFF
};

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n=== PANEL CONFIG BISECT ===");
  Serial.printf("SCK=%d MOSI=%d CS=%d DC=%d RST=%d BLK=%d\n",
                PIN_SCK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST, PIN_BLK);
  pinMode(PIN_BLK, OUTPUT);
  digitalWrite(PIN_BLK, HIGH);
  Serial.println("backlight HIGH. If the panel never glows at all, stop here - "
                 "that is power/BLK, not configuration.\n");
}

void loop() {
  for (int i = 0; i < NV; i++) {
    Serial.printf("[%d] %s\n", i + 1, V[i].note);
    tft.apply(V[i]);
    tft.init();
    tft.setRotation(0);

    tft.fillScreen(TINT[i]);
    tft.fillRect(6, 6, W - 12, H - 12, 0x0000);     // black inner panel
    tft.drawRect(6, 6, W - 12, H - 12, 0xFFFF);

    tft.setTextDatum(lgfx::middle_center);
    tft.setTextColor(0xFFFF, 0x0000);
    tft.setFont(&fonts::Font7);                      // 7-segment, very legible
    tft.drawString(String(i + 1), W / 2, H / 2 - 30);

    tft.setFont(&fonts::Font2);
    tft.setTextColor(TINT[i], 0x0000);
    tft.drawString("VARIANT", W / 2, H / 2 + 30);

    // corner markers prove the offset is right: all four must be fully visible
    tft.fillRect(8, 8, 14, 14, 0xF800);
    tft.fillRect(W - 22, 8, 14, 14, 0x07E0);
    tft.fillRect(8, H - 22, 14, 14, 0x001F);
    tft.fillRect(W - 22, H - 22, 14, 14, 0xFFE0);
    tft.setTextDatum(lgfx::top_left);

    delay(4000);
  }
  Serial.println("--- cycle complete, repeating ---\n");
}
