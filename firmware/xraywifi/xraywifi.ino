// xraywifi — Wi-Fi / BLE radar for ESP32-C3 with LAN backhaul.
// Scan logic follows oisinryan/esp32-radar. No TFT.
//
// Joins home Wi-Fi, finds the Mac via UDP beacon, streams JSON over TCP.
// USB serial still works for debug and first-time provisioning.
//
// If no SSID is stored, starts AP xraywifi-XXXX (pass xraywifi) at 192.168.4.1
// Serial: ssid / pass / join / host / erase / d / c / j / m / h

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <cstdarg>

static constexpr int PIN_BTN = 9;
static constexpr uint32_t BLE_MS = 3000;
static constexpr uint16_t UDP_PORT = 49421;
static constexpr uint16_t TCP_PORT_DEFAULT = 8082;

struct Blip {
  char id[22];
  char key[18];
  uint16_t ang;
  int8_t rssi;
  uint8_t type;
  bool open;
  uint32_t seen;
};

static constexpr int MAXB = 128;
static constexpr int CAP_WIFI = 56;
static constexpr int CAP_BLE = 72;
static Blip blips[MAXB];
static int nBlip = 0;
static int nWifi = 0, nBle = 0;

static char stationId[18];
static bool jsonSweep = true;
static bool contDump = false;

static Preferences prefs;
static char wifiSsid[33];
static char wifiPass[65];
static char hostIpStored[16];
static uint16_t hostPort = TCP_PORT_DEFAULT;
static bool portalOn = false;
static bool haveHost = false;
static IPAddress hostAddr;
static WiFiClient net;
static WiFiUDP udp;
static WiFiServer portal(80);
static uint32_t lastWifiTry = 0;
static uint32_t lastTcpTry = 0;
static bool scanArmed = false;

static uint16_t hashAngle(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619u;
  }
  return h % 360;
}

static void contact(const char* key, const char* id, int8_t rssi, uint8_t type, bool open) {
  int slot = -1;
  for (int i = 0; i < nBlip; i++)
    if (!strcmp(blips[i].key, key)) {
      slot = i;
      break;
    }
  if (slot < 0) {
    int same = 0;
    for (int i = 0; i < nBlip; i++)
      if (blips[i].type == type) same++;
    const int cap = type ? CAP_BLE : CAP_WIFI;
    if (nBlip < MAXB && same < cap)
      slot = nBlip++;
    else {
      slot = -1;
      for (int i = 0; i < nBlip; i++) {
        if (blips[i].type != type) continue;
        if (slot < 0 || blips[i].seen < blips[slot].seen) slot = i;
      }
      if (slot < 0) {
        slot = 0;
        for (int i = 1; i < nBlip; i++)
          if (blips[i].seen < blips[slot].seen) slot = i;
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
}

enum Phase : uint8_t { PH_WIFI, PH_BLE };
static Phase phase = PH_WIFI;
static uint8_t mode = 0;
static const char* MODE_NAME[3] = { "BOTH", "WI-FI", "BLE" };

static void dumpRegister();
static void emitSweepJson();
static void startPortal();
static void stopPortal();
static void joinWifi();

static bool bleReady = false;
static bool udpReady = false;

static void ensureUdp() {
  if (udpReady) return;
  udp.begin(UDP_PORT);
  udpReady = true;
}

static void ensureBle() {
  if (bleReady) return;
  NimBLEDevice::init("");
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setActiveScan(true);
  s->setInterval(100);
  s->setWindow(99);
  s->setMaxResults(64);
  bleReady = true;
}

static void startWifiScan() { WiFi.scanNetworks(true, true, false, 280); }
static void startBle() {
  ensureBle();
  NimBLEDevice::getScan()->start(BLE_MS, false, true);
}

static void nextPhase() {
  if (mode == 1) {
    startWifiScan();
    phase = PH_WIFI;
    return;
  }
  if (mode == 2) {
    startBle();
    phase = PH_BLE;
    return;
  }
  if (phase == PH_WIFI) {
    startBle();
    phase = PH_BLE;
  } else {
    startWifiScan();
    phase = PH_WIFI;
  }
}

static void outWrite(const uint8_t* p, size_t n) {
  if (!n) return;
  Serial.write(p, n);
  if (!net.connected()) return;
  size_t off = 0;
  while (off < n) {
    int w = net.write(p + off, n - off);
    if (w <= 0) {
      net.stop();
      break;
    }
    off += (size_t)w;
  }
}

static void outPrint(const char* s) { outWrite((const uint8_t*)s, strlen(s)); }

static void outPrintf(const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    outWrite((const uint8_t*)buf, (size_t)n);
  }
}

static void jsonStr(const char* s) {
  outPrint("\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      char esc[2] = { '\\', (char)c };
      outWrite((const uint8_t*)esc, 2);
    } else if (c == '\n')
      outPrint("\\n");
    else if (c == '\r')
      outPrint("\\r");
    else if (c >= 32) {
      uint8_t b = c;
      outWrite(&b, 1);
    }
  }
  outPrint("\"");
}

static const char* typeName(const Blip& b) {
  if (b.type) return "BLE";
  return b.open ? "OPEN" : "WIFI";
}

static const char* viaName() { return net.connected() ? "wifi" : "usb"; }

static void emitSweepJson() {
  uint32_t now = millis();
  for (int i = 0; i < nBlip; i++) {
    const Blip& b = blips[i];
    outPrint("{\"t\":\"blip\",\"id\":");
    jsonStr(stationId);
    outPrint(",\"type\":");
    jsonStr(typeName(b));
    outPrintf(",\"open\":%s,\"brg\":%u,\"rssi\":%d,\"age\":%lu,\"name\":",
              b.open ? "true" : "false", b.ang, (int)b.rssi,
              (unsigned long)((now - b.seen) / 1000));
    jsonStr(b.id);
    outPrint(",\"addr\":");
    jsonStr(b.key);
    outPrint("}\n");
  }
  outPrint("{\"t\":\"sweep\",\"id\":");
  jsonStr(stationId);
  outPrint(",\"mode\":");
  jsonStr(MODE_NAME[mode]);
  outPrintf(",\"ap\":%d,\"ble\":%d,\"tracked\":%d,\"heap\":%u,\"ms\":%u,\"via\":",
            nWifi, nBle, nBlip, (unsigned)ESP.getFreeHeap(), (unsigned)now);
  jsonStr(viaName());
  outPrint("}\n");
}

static void dumpRegister() {
  uint32_t now = millis();
  int idx[MAXB], n = nBlip;
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 1; i < n; i++) {
    int k = idx[i], j = i - 1;
    while (j >= 0 && blips[idx[j]].rssi < blips[k].rssi) {
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = k;
  }
  Serial.printf("\n=== CONTACT REGISTER %d tracked (%d AP / %d BLE this sweep) mode %s node %s ===\n",
                n, nWifi, nBle, MODE_NAME[mode], stationId);
  Serial.println("TYPE BRG RSSI AGE NAME                  ADDRESS");
  Serial.println("---- --- ---- ---- --------------------- -----------------");
  for (int r = 0; r < n; r++) {
    const Blip& b = blips[idx[r]];
    Serial.printf("%-4s %3u %4d %3lus %-21s %s\n", typeName(b), b.ang, b.rssi,
                  (unsigned long)((now - b.seen) / 1000), b.id, b.key);
  }
  Serial.printf("--- BRG is hashed bearing in degrees, 0 = right, clockwise. "
                "OPEN = unencrypted AP.\n\n");
}

static void serviceScans() {
  if (phase == PH_WIFI) {
    int r = WiFi.scanComplete();
    if (r >= 0) {
      nWifi = r;
      for (int i = 0; i < r; i++) {
        String ss = WiFi.SSID(i);
        String bssid = WiFi.BSSIDstr(i);
        contact(bssid.c_str(), ss.length() ? ss.c_str() : " ",
                WiFi.RSSI(i), 0, WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      }
      WiFi.scanDelete();
      if (mode == 1) {
        if (jsonSweep) emitSweepJson();
        if (contDump) dumpRegister();
      }
      nextPhase();
    } else if (r == WIFI_SCAN_FAILED)
      startWifiScan();
  } else {
    NimBLEScan* s = NimBLEDevice::getScan();
    if (!s->isScanning()) {
      NimBLEScanResults res = s->getResults();
      nBle = res.getCount();
      for (int i = 0; i < nBle; i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        std::string a = d->getAddress().toString();
        std::string name = d->haveName() ? d->getName() : a;
        contact(a.c_str(), name.c_str(), d->getRSSI(), 1, false);
      }
      s->clearResults();
      if (jsonSweep) emitSweepJson();
      if (contDump) dumpRegister();
      nextPhase();
    }
  }
}

static void applyMode(uint8_t next) {
  mode = next % 3;
  Serial.printf("[button] mode -> %s\n", MODE_NAME[mode]);
  if (!bleReady) {
    phase = PH_WIFI;
    return;
  }
  if (mode == 1 && phase == PH_BLE) {
    NimBLEDevice::getScan()->stop();
    startWifiScan();
    phase = PH_WIFI;
  }
  if (mode == 2 && phase == PH_WIFI) {
    WiFi.scanDelete();
    startBle();
    phase = PH_BLE;
  }
}

static void saveWifiPrefs() {
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("host", hostIpStored);
  prefs.putUShort("port", hostPort);
}

static void loadWifiPrefs() {
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  String h = prefs.getString("host", "");
  hostPort = prefs.getUShort("port", TCP_PORT_DEFAULT);
  snprintf(wifiSsid, sizeof wifiSsid, "%s", s.c_str());
  snprintf(wifiPass, sizeof wifiPass, "%s", p.c_str());
  snprintf(hostIpStored, sizeof hostIpStored, "%s", h.c_str());
  if (hostIpStored[0] && hostAddr.fromString(hostIpStored)) haveHost = true;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c |= 32;
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  return 0;
}

static void urlDecode(char* s) {
  char* o = s;
  for (char* p = s; *p; p++) {
    if (*p == '+')
      *o++ = ' ';
    else if (*p == '%' && p[1] && p[2]) {
      *o++ = (char)((hexVal(p[1]) << 4) | hexVal(p[2]));
      p += 2;
    } else
      *o++ = *p;
  }
  *o = 0;
}

static void formVal(const char* body, const char* key, char* dst, size_t dstN) {
  dst[0] = 0;
  char pat[24];
  snprintf(pat, sizeof pat, "%s=", key);
  const char* p = strstr(body, pat);
  if (!p) return;
  p += strlen(pat);
  size_t i = 0;
  while (p[i] && p[i] != '&' && i + 1 < dstN) {
    dst[i] = p[i];
    i++;
  }
  dst[i] = 0;
  urlDecode(dst);
}

static const char PORTAL_PAGE[] PROGMEM = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>xraywifi</title>
<style>
body{font-family:sans-serif;background:#050806;color:#c8f0d0;padding:24px;max-width:28em}
input,button{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box}
button{background:#1c7a3a;color:#e8ffe8;border:0}
</style>
<h1>xraywifi</h1>
<p>Join the same Wi-Fi as the Mac running the dashboard. Leave host blank to auto-find it.</p>
<form method=POST>
SSID<input name=ssid>
Password<input name=pass type=password>
Host IP (optional)<input name=host placeholder="192.168.1.10">
<button>Save and join</button>
</form>
)HTML";

static void servicePortal() {
  WiFiClient c = portal.available();
  if (!c) return;
  c.setTimeout(800);
  String req = c.readStringUntil('\n');
  bool post = req.startsWith("POST");
  int contentLen = 0;
  while (c.connected()) {
    String line = c.readStringUntil('\n');
    line.trim();
    if (!line.length()) break;
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:"))
      contentLen = line.substring(15).toInt();
  }
  char body[192];
  body[0] = 0;
  if (post && contentLen > 0) {
    int n = c.readBytes(body, min(contentLen, (int)sizeof body - 1));
    body[n] = 0;
  }
  if (post) {
    formVal(body, "ssid", wifiSsid, sizeof wifiSsid);
    formVal(body, "pass", wifiPass, sizeof wifiPass);
    formVal(body, "host", hostIpStored, sizeof hostIpStored);
    saveWifiPrefs();
    c.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
    c.print("<body style='font-family:sans-serif;background:#050806;color:#c8f0d0;padding:24px'>Saved. Joining Wi-Fi…");
    c.stop();
    delay(200);
    joinWifi();
    return;
  }
  c.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
  c.print(PORTAL_PAGE);
  c.stop();
}

static void startPortal() {
  stopPortal();
  char ap[20];
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(ap, sizeof ap, "xraywifi-%02X%02X", m[4], m[5]);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap, "xraywifi");
  portal.begin();
  portalOn = true;
  Serial.printf("[wifi] setup AP %s  pass xraywifi  open http://192.168.4.1/\n", ap);
}

static void stopPortal() {
  if (!portalOn) return;
  portal.stop();
  WiFi.softAPdisconnect(true);
  portalOn = false;
}

static void joinWifi() {
  if (!wifiSsid[0]) {
    startPortal();
    return;
  }
  stopPortal();
  haveHost = hostIpStored[0] && hostAddr.fromString(hostIpStored);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid, wifiPass);
  lastWifiTry = millis();
  scanArmed = true;
  Serial.printf("[wifi] joining %s\n", wifiSsid);
}

static bool sameSubnet(IPAddress a, IPAddress b) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static void parseBeacon(char* buf, IPAddress from) {
  if (strncmp(buf, "XRAYWIFI", 8) != 0) return;
  if (net.connected()) return;
  char ipstr[16] = {0};
  int port = TCP_PORT_DEFAULT;
  char* pip = strstr(buf, "ip=");
  char* pport = strstr(buf, "tcp=");
  if (pip) {
    pip += 3;
    size_t i = 0;
    while (pip[i] && pip[i] != ' ' && pip[i] != '\n' && i < 15) {
      ipstr[i] = pip[i];
      i++;
    }
    ipstr[i] = 0;
  }
  if (pport) port = atoi(pport + 4);
  IPAddress a;
  if (!(ipstr[0] && a.fromString(ipstr))) a = from;
  IPAddress sta = WiFi.localIP();
  if (sta && !sameSubnet(a, sta)) return;
  hostAddr = a;
  hostPort = (uint16_t)(port > 0 ? port : TCP_PORT_DEFAULT);
  haveHost = true;
}

static void serviceNet() {
  uint32_t now = millis();
  if (portalOn) {
    servicePortal();
    return;
  }
  if (wifiSsid[0] && WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry > 8000) {
      WiFi.begin(wifiSsid, wifiPass);
      lastWifiTry = now;
      Serial.println("[wifi] retry");
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;

  if (scanArmed) {
    scanArmed = false;
    ensureUdp();
    ensureBle();
    startWifiScan();
    phase = PH_WIFI;
    Serial.println("sweeping...");
  }

  ensureUdp();
  int pkt = udp.parsePacket();
  if (pkt > 0) {
    char buf[96];
    int n = udp.read(buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = 0;
      parseBeacon(buf, udp.remoteIP());
    }
  }

  if (haveHost && !net.connected() && now - lastTcpTry >= 8000) {
    lastTcpTry = now;
    net.setNoDelay(true);
    if (net.connect(hostAddr, hostPort, 2500)) {
      Serial.printf("[wifi] tcp %s:%u\n", hostAddr.toString().c_str(), hostPort);
      outPrint("{\"t\":\"hello\",\"id\":");
      jsonStr(stationId);
      outPrint(",\"ip\":");
      jsonStr(WiFi.localIP().toString().c_str());
      outPrint(",\"via\":\"wifi\"}\n");
    } else {
      Serial.printf("[wifi] tcp fail %s:%u\n", hostAddr.toString().c_str(), hostPort);
      net.stop();
    }
  }
}

static void printHelp() {
  Serial.println("[cmd] d dump  c human dump  j json  m mode");
  Serial.println("[cmd] ssid NAME   pass SECRET   join   host auto|IP   erase");
}

static void handleLine(char* line) {
  while (*line == ' ') line++;
  if (!*line) return;
  if (!line[1]) {
    char c = line[0];
    if (c == 'd' || c == 'D') dumpRegister();
    else if (c == 'c' || c == 'C') {
      contDump = !contDump;
      Serial.printf("[cmd] continuous human dump %s\n", contDump ? "ON" : "OFF");
    } else if (c == 'j' || c == 'J') {
      jsonSweep = !jsonSweep;
      Serial.printf("[cmd] json sweep %s\n", jsonSweep ? "ON" : "OFF");
    } else if (c == 'm' || c == 'M')
      applyMode(mode + 1);
    else if (c == 'h' || c == 'H' || c == '?')
      printHelp();
    return;
  }
  if (!strncmp(line, "ssid ", 5)) {
    snprintf(wifiSsid, sizeof wifiSsid, "%s", line + 5);
    Serial.printf("[wifi] ssid %s\n", wifiSsid);
  } else if (!strncmp(line, "pass ", 5)) {
    snprintf(wifiPass, sizeof wifiPass, "%s", line + 5);
    Serial.println("[wifi] pass stored");
  } else if (!strcmp(line, "join")) {
    saveWifiPrefs();
    joinWifi();
  } else if (!strncmp(line, "host ", 5)) {
    const char* h = line + 5;
    if (!strcmp(h, "auto")) {
      hostIpStored[0] = 0;
      haveHost = false;
      Serial.println("[wifi] host auto (UDP)");
    } else {
      snprintf(hostIpStored, sizeof hostIpStored, "%s", h);
      haveHost = hostAddr.fromString(hostIpStored);
      Serial.printf("[wifi] host %s\n", hostIpStored);
    }
    saveWifiPrefs();
  } else if (!strcmp(line, "erase")) {
    prefs.clear();
    wifiSsid[0] = wifiPass[0] = hostIpStored[0] = 0;
    haveHost = false;
    net.stop();
    Serial.println("[wifi] erased, reboot");
    delay(200);
    ESP.restart();
  } else if (line[0] == 'h' || line[0] == 'H' || line[0] == '?')
    printHelp();
}

static char cmdBuf[96];
static uint8_t cmdLen = 0;

static void pollCommands() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n' || cmdLen >= sizeof cmdBuf - 1) {
      cmdBuf[cmdLen] = 0;
      handleLine(cmdBuf);
      cmdLen = 0;
      continue;
    }
    cmdBuf[cmdLen++] = (char)c;
  }
}

static void loadStationId() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(stationId, sizeof stationId, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  uint32_t wait = millis();
  while (!Serial && millis() - wait < 2000) delay(10);
  delay(200);
  loadStationId();
  prefs.begin("xray", false);
  loadWifiPrefs();

  Serial.println("\n\n=== ESP32 WI-FI / BLE RADAR ===");
  outPrint("{\"t\":\"hello\",\"id\":");
  jsonStr(stationId);
  outPrintf(",\"heap\":%u,\"via\":\"usb\"}\n", (unsigned)ESP.getFreeHeap());
  printHelp();

  pinMode(PIN_BTN, INPUT_PULLUP);

  if (wifiSsid[0])
    joinWifi();
  else
    startPortal();
}

void loop() {
  static uint32_t secT = 0;
  uint32_t now = millis();

  pollCommands();
  serviceNet();
  if (!portalOn && (WiFi.status() == WL_CONNECTED || !wifiSsid[0]))
    serviceScans();
  else if (!portalOn && wifiSsid[0] && WiFi.status() != WL_CONNECTED)
    ;  // wait for join; do not scan-hop while associating
  else if (portalOn)
    delay(2);

  static bool prev = true;
  static uint32_t bt = 0;
  bool bs = digitalRead(PIN_BTN);
  if (prev && !bs && now - bt > 250) {
    bt = now;
    applyMode(mode + 1);
  }
  prev = bs;

  if (now - secT >= 1000) {
    outPrint("{\"t\":\"status\",\"id\":");
    jsonStr(stationId);
    outPrint(",\"mode\":");
    jsonStr(MODE_NAME[mode]);
    outPrintf(",\"ap\":%d,\"ble\":%d,\"tracked\":%d,\"heap\":%u,\"rssi\":%d,\"via\":",
              nWifi, nBle, nBlip, (unsigned)ESP.getFreeHeap(),
              WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    jsonStr(viaName());
    if (WiFi.status() == WL_CONNECTED) {
      outPrint(",\"ip\":");
      jsonStr(WiFi.localIP().toString().c_str());
    }
    outPrint("}\n");
    secT = now;
  }
  delay(5);
}
