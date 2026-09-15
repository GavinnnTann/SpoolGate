#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "logbuf.h"
#include "config.h"
#include "nat_debug.h"
#include "net/health.h"
#include "net/napt.h"
#include "net/softap.h"
#include "net/uplink.h"
#include "statusled.h"
#include "watchdog.h"
#include "web/portal.h"

namespace {

constexpr uint32_t kBackoffInitialMs = 2000;

// How long a single connection attempt is allowed to run before it is abandoned
// and retried. An enterprise SSID is carried by many APs, and esp_wifi works
// through them in turn, raising a disconnect event for each BSSID it fails on
// while remaining in the connecting state throughout. Those per-BSSID events are
// not attempt failures, so the attempt is bounded by this deadline instead.
// Successful associations here complete in 1.5-3 s, so 20 s is generous.
constexpr uint32_t kAttemptTimeoutMs = 20000;

// How long the driver may stay silent after a disconnect, with no association, before
// the attempt is written off. esp_wifi moves to the next BSSID behind the SSID within
// a few hundred milliseconds, so silence this long means it has run out of candidates
// rather than that it is still working. Without this an attempt always costs the full
// kAttemptTimeoutMs even when it failed in the first 30 ms, which is precisely why
// pressing reset beat waiting: reset restarts at the initial backoff immediately.
constexpr uint32_t kAttemptQuietMs = 2000;

// Grace period after a client associates before NAPT may be turned on. Enabling
// NAPT starves the SoftAP's DHCP server, so a client that still needs to lease
// must be given room to finish first. A client that re-associates on a lease it
// already holds never does DHCP at all, which is why this is a timer and not
// purely a wait for AP_STAIPASSIGNED.
constexpr uint32_t kNaptLeaseGraceMs = 8000;

// Consecutive failed probe rounds before recovery acts. At one round a minute
// this is roughly three minutes of a path that carries no packets.
constexpr uint8_t kUplinkFailStreak = 3;
constexpr uint8_t kClientFailStreak = 3;
// Capped low on purpose. Exponential backoff to a minute assumes repeated failure
// means a persistent outage; here it usually means one campus AP refused us, and the
// next attempt a few seconds later succeeds. Backing off to 60 s turned a transient
// refusal into a minute of downtime for no benefit.
constexpr uint32_t kBackoffMaxMs = 15000;

uint32_t backoffMs = kBackoffInitialMs;
uint32_t reconnectAtMs = 0;
bool reconnectPending = false;
bool uplinkReady = false;

// True from the moment uplink::begin() is called until the attempt either yields
// an IP or hits kAttemptTimeoutMs. While set, disconnect events are informational
// only: acting on them is what produced "sta is connecting, cannot set config"
// (ESP_ERR_WIFI_STATE), where the retry timer fired into an attempt that was
// still running and WiFi.begin() silently did nothing at all.
bool attemptActive = false;
uint32_t attemptStartedMs = 0;

// Set when the current attempt reaches association, cleared when it drops again.
// While associated the driver is genuinely mid-handshake — EAP and DHCP still have
// work to do — so only the hard deadline applies; the quiet rule would abort a
// connection about to succeed.
bool attemptAssociated = false;
uint32_t lastDisconnectMs = 0;

// Downstream client tracking. clientIp is the address handed out by DHCP, kept so
// the downstream probe has something to aim at; it survives the client leaving so
// a re-association on the same lease is still probeable.
// Cached radio state. WiFi.softAPgetStationNum() calls esp_wifi_ap_get_sta_list()
// and WiFi.channel() calls esp_wifi_get_channel(); both take the WiFi API lock and
// copy driver state. Reading them from every loop() iteration is tens of thousands
// of lock acquisitions a second, contending with the very WiFi task that forwards
// NAT traffic — measurably slower throughput for information that changes at human
// speed. Sampled at 2 Hz instead, and read from cache everywhere in the hot path.
// Heartbeat cadence when nothing is changing. See the heartbeat block in loop().
constexpr uint32_t kIdleBeatMs = 600000;

constexpr uint32_t kRadioPollMs = 500;
uint8_t radioStations = 0;
int32_t radioChannel = 0;
uint32_t lastRadioPollMs = 0;

IPAddress clientIp;
uint32_t lastApAssocMs = 0;
bool leaseSeen = false;
bool softApFailed = false;

const char *healthName(health::Result r) {
  switch (r) {
    case health::Result::Ok:     return "ok";
    case health::Result::Failed: return "FAIL";
    default:                     return "?";
  }
}

void logLine(const char *msg) {
  logbuf::printf("[%10lu] %s\n", millis(), msg);
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
  logbuf::printf("[%10lu] selftest DNS neverssl.com -> %s (ok=%d)\n", millis(), resolved.toString().c_str(), dnsOk);

  NetworkClient client;
  bool tcpOk = client.connect(IPAddress(1, 1, 1, 1), 80, 5000);
  logbuf::printf("[%10lu] selftest TCP 1.1.1.1:80 ok=%d\n", millis(), tcpOk);
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
    logbuf::printf("[%10lu] scan: no networks visible (%d)\n", millis(), n);
  } else {
    logbuf::printf("[%10lu] scan: %d networks visible (2.4 GHz band only)\n", millis(), n);
    for (int i = 0; i < n; i++) {
      logbuf::printf(
        "   ch%-3d %4d dBm  %-16s \"%s\"\n", WiFi.channel(i), WiFi.RSSI(i), authName(WiFi.encryptionType(i)), WiFi.SSID(i).c_str()
      );
    }
  }
  WiFi.scanDelete();
}

#endif  // NAT_DEBUG

// The single path that starts a connection attempt, so the bookkeeping cannot
// drift out of step with the radio.
void startAttempt() {
  attemptActive = true;
  attemptStartedMs = millis();
  attemptAssociated = false;
  lastDisconnectMs = 0;
  uplink::begin();
}

// Drives NAPT from the state the system is actually in, rather than from the one
// event that happened to be convenient. NAPT used to be switched on only by
// AP_STAIPASSIGNED, so a client re-associating on a lease it still held never
// turned it back on: uplink up, client associated, LED blue, nothing forwarded and
// no indication anywhere that anything was wrong. Reconciling every loop() makes
// the state self-correcting no matter which events did or did not arrive.
// Samples the radio counters that the hot path needs, at a rate the hot path can
// afford. Everything downstream reads the cache rather than the driver.
void pollRadio() {
  if (lastRadioPollMs != 0 && millis() - lastRadioPollMs < kRadioPollMs) {
    return;
  }
  lastRadioPollMs = millis();
  radioStations = WiFi.softAPgetStationNum();
  radioChannel = WiFi.channel();
}

void reconcileNapt() {
  bool haveClient = radioStations > 0;

  // The grace period is what protects DHCP: a freshly associated client that still
  // needs an address gets a window with NAPT off, and one that never asks (because
  // its lease is still valid) is picked up when that window expires.
  bool leaseSettled = leaseSeen || (lastApAssocMs != 0 && millis() - lastApAssocMs >= kNaptLeaseGraceMs);
  bool want = uplinkReady && haveClient && leaseSettled;

  if (want == napt::isEnabled()) {
    return;
  }
  if (want) {
    napt::enable();
    logLine("NAPT ON");
  } else {
    napt::disable();
    logLine("NAPT OFF");
  }
}

void scheduleReconnect() {
  reconnectAtMs = millis() + backoffMs;
  reconnectPending = true;
  logbuf::printf("[%10lu] STA reconnect scheduled in %lu ms\n", millis(), (unsigned long)backoffMs);
  backoffMs = min(backoffMs * 2, kBackoffMaxMs);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logLine("STA associated with uplink AP");
      attemptAssociated = true;
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      uplinkReady = true;
      logLine("STA got IP");
      logbuf::printf(
        "  IP: %s  Gateway: %s  DNS: %s  channel: %d\n", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
        WiFi.dnsIP(0).toString().c_str(), WiFi.channel()
      );
      // NAPT is deliberately NOT enabled here, even though the uplink is now
      // usable. Enabling NAPT stops the SoftAP's DHCP server from completing
      // leases: clients associate normally but never receive an address and fall
      // back to 169.254.x.x. Confirmed by A/B test on this hardware — identical
      // firmware, NAPT the only variable. So NAPT is deferred until a client
      // actually holds a lease (see ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED).
      backoffMs = kBackoffInitialMs;
      reconnectPending = false;
      attemptActive = false;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      uplinkReady = false;
      if (napt::isEnabled()) {
        napt::disable();
        logLine("NAPT OFF (uplink lost)");
      }
      // Only a drop from an established link schedules a retry. During an attempt
      // these events are the driver working through the BSSIDs behind the SSID —
      // treating each as a failure is what made the retry timer fire into a
      // connection that was still being set up, where WiFi.begin() is refused
      // outright and the backoff doubles for an attempt never actually made.
      // An attempt that genuinely gets nowhere is caught by kAttemptTimeoutMs.
      attemptAssociated = false;
      lastDisconnectMs = millis();
      if (!attemptActive) {
        scheduleReconnect();
      }
      break;

    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      logLine("Printer associated with SoftAP");
      lastApAssocMs = millis();
      leaseSeen = false;  // it may re-DHCP; the grace timer covers it if it does not
      break;

    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      clientIp = IPAddress(info.wifi_ap_staipassigned.ip.addr);
      logbuf::printf("[%10lu] DHCP lease issued: %s\n", millis(), clientIp.toString().c_str());
      leaseSeen = true;
      break;

    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      logLine("Printer left SoftAP");
      if (WiFi.softAPgetStationNum() == 0) {
        leaseSeen = false;
        lastApAssocMs = 0;
        // Forget the address too. Probing an address nobody holds fails forever,
        // and the downstream recovery below reads that as a wedged client and
        // deauths — which, if a client is mid-DHCP at the time, kicks it off during
        // the exchange it needs to complete to become reachable at all.
        clientIp = IPAddress((uint32_t)0);
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
  logbuf::begin();
  logbuf::printf("\nSpoolGate — ESP32 NAT router — arduino-esp32 core %s\n", ESP_ARDUINO_VERSION_STR);

  watchdog::begin();

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
    logbuf::printf("[%10lu] Admin portal at http://%s/\n", millis(), config::settings.apIp.toString().c_str());
  }

  health::begin();

  startAttempt();
  logLine("STA connecting");
}

void loop() {
  // First statement in loop(): reaching it is the definition of "still alive",
  // and it is also what clears the uplink stall clock once the STA holds an IP.
  watchdog::update(uplinkReady);

  portal::loop();
  pollRadio();

  // NAPT follows the actual state of the uplink and the client, checked every
  // iteration rather than inferred from whichever events happened to fire.
  reconcileNapt();

  // Probe both directions. Non-blocking: lwIP's ping task does the work and this
  // only starts rounds and collects results.
  // Downstream is probed only while a client is associated and holding a lease we
  // have seen. Outside that, there is nothing meaningful to ask and a failed answer
  // would mean nothing.
  bool clientProbeable = radioStations > 0 && leaseSeen;
  health::update(uplinkReady, WiFi.gatewayIP(), clientProbeable ? clientIp : IPAddress((uint32_t)0));

  // An uplink that holds an IP but answers nothing is worse than one that is
  // plainly down: every status signal reads healthy while no traffic moves. Tear
  // it down and reconnect. Gated on the gateway having answered at least once, so
  // a network that drops ICMP by policy can never drive a reconnect loop.
  if (uplinkReady && health::upstreamEverOk() && health::upstreamFailStreak() >= kUplinkFailStreak) {
    logLine("RECOVERY: uplink holds an IP but is unreachable — reconnecting");
    health::resetStreaks();
    uplinkReady = false;
    attemptActive = false;
    WiFi.disconnect(false, false, 0);
    scheduleReconnect();
  }

  // Same reasoning downstream: associated and leased but not answering means the
  // client's link is wedged, and the only lever we have is to push it off so it
  // re-associates. Also gated on it having answered before, so a client that never
  // replies to ICMP is never deauthed on suspicion.
  if (clientProbeable && health::downstreamEverOk() && health::downstreamFailStreak() >= kClientFailStreak) {
    logLine("RECOVERY: client unreachable — deauthing to force re-association");
    health::resetStreaks();
    esp_wifi_deauth_sta(0);  // 0 = every associated station
  }

  // The radio serves one channel for both interfaces, so associating upstream
  // drags the SoftAP onto the uplink AP's channel and every downstream client is
  // dropped and has to find the AP again. That is the single most likely reason
  // for a client that will not rejoin, and it was previously invisible.
  static int32_t lastChannel = -1;
  int32_t channel = radioChannel;
  if (channel != lastChannel) {
    if (lastChannel != -1) {
      logbuf::printf("[%10lu] SoftAP channel moved %d -> %d (clients must re-associate)\n", millis(), lastChannel, channel);
    }
    lastChannel = channel;
  }

#if !NAT_DEBUG
  // A deployed router that prints nothing between state changes is
  // indistinguishable from a hung one. "Amber and silent" reads as a crash even
  // when the only thing happening is that no client has joined yet, so the
  // production build emits one line every 30 s saying so.
  // Printed the moment anything changes, and otherwise only every ten minutes.
  // At one line per 30 s a 96-line ring held barely 45 minutes, so an overnight
  // fault had scrolled out of the portal log long before anyone read it. RSSI is
  // excluded from the comparison because it moves constantly and would defeat the
  // suppression entirely; it is still printed.
  static uint32_t lastBeatMs = 0;
  static char lastBeat[96] = "";
  char beat[96];
  snprintf(
    beat, sizeof(beat), "uplink=%s ch=%d clients=%u napt=%d health up=%s down=%s", uplinkReady ? "up" : "down", channel, radioStations,
    napt::isEnabled(), healthName(health::upstream()), healthName(health::downstream())
  );
  if (lastBeatMs == 0 || strcmp(beat, lastBeat) != 0 || millis() - lastBeatMs >= kIdleBeatMs) {
    lastBeatMs = millis();
    strncpy(lastBeat, beat, sizeof(lastBeat) - 1);
    logbuf::printf("[%10lu] %s rssi=%d\n", millis(), beat, WiFi.RSSI());
  }
#endif

  // Status LED reflects link state at a glance (see statusled.h for the colour map).
  statusled::State ledState;
  if (softApFailed) {
    ledState = statusled::State::Fault;
  } else if (!uplinkReady) {
    ledState = statusled::State::UpstreamDown;
  } else if (radioStations == 0) {
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
    logbuf::printf(
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
    logbuf::printf("[%10lu] STA status: %s\n", millis(), wlStatusName(WiFi.status()));
  }
#endif  // NAT_DEBUG

  // Abandon an attempt that has run past its deadline without producing an IP,
  // and fall into the normal backoff. This is what bounds a connect that the
  // driver never resolves either way.
  if (attemptActive) {
    bool hardTimeout = millis() - attemptStartedMs >= kAttemptTimeoutMs;
    // The driver has disconnected and then gone quiet without associating, so it has
    // exhausted the BSSIDs behind this SSID. Retry now rather than sitting out the
    // rest of the deadline.
    bool gaveUp = !attemptAssociated && lastDisconnectMs != 0 && millis() - lastDisconnectMs >= kAttemptQuietMs;
    if (hardTimeout || gaveUp) {
      attemptActive = false;
      logLine(hardTimeout ? "STA attempt timed out" : "STA attempt failed (driver out of candidates)");
      scheduleReconnect();
    }
  }

  if (reconnectPending && millis() >= reconnectAtMs) {
    reconnectPending = false;
    logLine("STA reconnecting");
    startAttempt();
  }
}
