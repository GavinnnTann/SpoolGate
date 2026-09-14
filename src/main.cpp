#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "nat_debug.h"
#include "net/napt.h"
#include "net/softap.h"
#include "net/uplink.h"
#include "statusled.h"
#include "web/portal.h"

namespace {

constexpr uint32_t kBackoffInitialMs = 2000;
constexpr uint32_t kBackoffMaxMs = 60000;

uint32_t backoffMs = kBackoffInitialMs;
uint32_t reconnectAtMs = 0;
bool reconnectPending = false;
bool uplinkReady = false;
bool softApFailed = false;

void logLine(const char *msg) {
  Serial.printf("[%10lu] %s\n", millis(), msg);
}

#if NAT_DEBUG
const char *wlStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

// Checks that the ESP32's own uplink can actually reach the internet. Runs in
// loop(), not the WiFi event callback, because it blocks on network I/O and the
// event callback must not stall. Separates "NAPT isn't forwarding" from "the
// uplink itself has no route out", which look identical from a client.
void runUplinkSelfTest() {
  IPAddress resolved;
  bool dnsOk = WiFi.hostByName("neverssl.com", resolved);
  Serial.printf("[%10lu] selftest DNS neverssl.com -> %s (ok=%d)\n", millis(), resolved.toString().c_str(), dnsOk);

  NetworkClient client;
  bool tcpOk = client.connect(IPAddress(1, 1, 1, 1), 80, 5000);
  Serial.printf("[%10lu] selftest TCP 1.1.1.1:80 ok=%d\n", millis(), tcpOk);
  client.stop();
}

const char *authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_ENTERPRISE:      return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    default:                        return "OTHER";
  }
}

// Dumps every AP the radio can actually see. NO_AP_FOUND on its own is ambiguous:
// it only says the configured SSID was absent from the scan results, which covers
// both a wrong SSID string and an AP that is out of reach. The ESP32-S3 has no
// 5 GHz radio, so a dual-band campus SSID that is 5 GHz-only in this room is
// invisible here while every laptop nearby sits happily connected to it. Seeing
// the raw scan list separates those two cases in one glance.
void runScan() {
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.printf("[%10lu] scan: no networks visible (%d)\n", millis(), n);
  } else {
    Serial.printf("[%10lu] scan: %d networks visible (2.4 GHz band only)\n", millis(), n);
    for (int i = 0; i < n; i++) {
      Serial.printf(
        "   ch%-3d %4d dBm  %-16s \"%s\"\n", WiFi.channel(i), WiFi.RSSI(i), authName(WiFi.encryptionType(i)), WiFi.SSID(i).c_str()
      );
    }
  }
  WiFi.scanDelete();
}

#endif  // NAT_DEBUG

void scheduleReconnect() {
  reconnectAtMs = millis() + backoffMs;
  reconnectPending = true;
  Serial.printf("[%10lu] STA reconnect scheduled in %lu ms\n", millis(), (unsigned long)backoffMs);
  backoffMs = min(backoffMs * 2, kBackoffMaxMs);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logLine("STA associated with uplink AP");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      uplinkReady = true;
      logLine("STA got IP");
      Serial.printf(
        "  IP: %s  Gateway: %s  DNS: %s\n", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP(0).toString().c_str()
      );
      // NAPT is deliberately NOT enabled here, even though the uplink is now
      // usable. Enabling NAPT stops the SoftAP's DHCP server from completing
      // leases: clients associate normally but never receive an address and fall
      // back to 169.254.x.x. Confirmed by A/B test on this hardware — identical
      // firmware, NAPT the only variable. So NAPT is deferred until a client
      // actually holds a lease (see ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED).
      if (WiFi.softAPgetStationNum() > 0) {
        // A client is already associated and presumably already leased (it must
        // have leased while NAPT was off), so it is safe to turn NAPT on now.
        napt::enable();
        logLine("NAPT ON");
      }
      backoffMs = kBackoffInitialMs;
      reconnectPending = false;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      uplinkReady = false;
      if (napt::isEnabled()) {
        napt::disable();
        logLine("NAPT OFF (uplink lost)");
      }
      scheduleReconnect();
      break;

    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      logLine("Printer associated with SoftAP");
      break;

    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.printf("[%10lu] DHCP lease issued: %s\n", millis(), IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      // The lease is done, so NAPT can now be turned on without starving DHCP.
      if (uplinkReady && !napt::isEnabled()) {
        napt::enable();
        logLine("NAPT ON");
      }
      break;

    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      logLine("Printer left SoftAP");
      // Drop NAPT once nothing is associated, so the next client's DHCP
      // handshake can complete. It gets re-enabled as soon as that client leases.
      if (WiFi.softAPgetStationNum() == 0 && napt::isEnabled()) {
        napt::disable();
        logLine("NAPT OFF (no clients — keeps DHCP able to serve)");
      }
      break;

    default:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nSpoolGate — ESP32 NAT router — arduino-esp32 core %s\n", ESP_ARDUINO_VERSION_STR);

  config::begin();
  statusled::setBrightness(config::settings.ledBrightness);
  statusled::begin();

  WiFi.mode(WIFI_AP_STA);
  WiFi.onEvent(onWiFiEvent);

  if (!softap::begin()) {
    softApFailed = true;
    logLine("FATAL: SoftAP bring-up failed");
  } else {
    logLine("AP UP");
    portal::begin();
    Serial.printf("[%10lu] Admin portal at http://%s/\n", millis(), config::settings.apIp.toString().c_str());
  }

  uplink::begin();
  logLine("STA connecting");
}

void loop() {
  portal::loop();

  // Status LED reflects link state at a glance (see statusled.h for the colour map).
  statusled::State ledState;
  if (softApFailed) {
    ledState = statusled::State::Fault;
  } else if (!uplinkReady) {
    ledState = statusled::State::UpstreamDown;
  } else if (WiFi.softAPgetStationNum() == 0) {
    ledState = statusled::State::NoClients;
  } else {
    ledState = statusled::State::Connected;
  }
  statusled::update(ledState);

#if NAT_DEBUG
  // Scan whenever the uplink is down. Cheap on the 60 s backoff, and it is the only
  // thing that tells a wrong SSID apart from an AP the ESP32's 2.4 GHz radio cannot
  // reach. Blocking for ~3 s is fine here — nothing is being forwarded anyway.
  static uint32_t lastScan = 0;
  if (!uplinkReady && (lastScan == 0 || millis() - lastScan > 30000)) {
    lastScan = millis();
    runScan();
  }

  // Uplink self-test. Repeats rather than running once, so the result can't be
  // missed by a serial monitor that attaches after boot.
  static uint32_t lastSelfTest = 0;
  if (uplinkReady && (lastSelfTest == 0 || millis() - lastSelfTest > 30000)) {
    lastSelfTest = millis();
    runUplinkSelfTest();
  }

  // Periodic AP/DHCP/DNS/RSSI snapshot — confirms the SoftAP is broadcasting, the
  // DHCP server is up, and the DNS server being offered to clients (should read
  // 1.1.1.1, not the AP's own IP).
  static uint32_t lastApLog = 0;
  if (millis() - lastApLog > 5000) {
    lastApLog = millis();
    Serial.printf(
      "[%10lu] AP IP: %s  Stations: %d  DHCPS: %s  DNSoffer: %s  NAPT: %d  ch: %d  STA rssi: %d\n", millis(), WiFi.softAPIP().toString().c_str(),
      WiFi.softAPgetStationNum(), softap::dhcpsStatusName(), softap::dnsOffered().toString().c_str(), napt::isEnabled(), WiFi.channel(), WiFi.RSSI()
    );
  }

  // STA status poll, independent of WiFi events. A "no matching SSID found" or
  // similar early-failure condition doesn't always fire an
  // ARDUINO_EVENT_WIFI_STA_DISCONNECTED event, which otherwise leaves this
  // event-driven state machine with nothing to log — a silent stall that looks
  // identical to a working-but-quiet system. Gated on NAPT being off so it goes
  // quiet once steady state is reached.
  static uint32_t lastStatusLog = 0;
  if (!napt::isEnabled() && millis() - lastStatusLog > 3000) {
    lastStatusLog = millis();
    Serial.printf("[%10lu] STA status: %s\n", millis(), wlStatusName(WiFi.status()));
  }
#endif  // NAT_DEBUG

  if (reconnectPending && millis() >= reconnectAtMs) {
    reconnectPending = false;
    logLine("STA reconnecting");
    uplink::begin();
  }
}
