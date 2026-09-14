#include "health.h"

#include <Arduino.h>
#include <atomic>
#include <lwip/inet.h>
#include <ping/ping_sock.h>

namespace health {

namespace {

// One round every 60 s. Frequent enough to notice a stall within a couple of
// minutes, rare enough that the ICMP traffic is negligible.
constexpr uint32_t kRoundIntervalMs = 60000;

// Three echo requests per round, 500 ms apart, 1 s timeout each — a round is over
// in about 2 s. A single reply is enough to call the direction healthy: the
// question is whether the path carries packets at all, not what its loss rate is.
constexpr uint32_t kPingCount = 3;
constexpr uint32_t kPingIntervalMs = 500;
constexpr uint32_t kPingTimeoutMs = 1000;
constexpr uint32_t kPingStackBytes = 2560;
constexpr uint32_t kPingPriority = 2;

struct Probe {
  const char *name = "";
  esp_ping_handle_t handle = nullptr;
  // Written by lwIP's ping task, read from loop(). Atomics rather than volatile:
  // volatile orders nothing between the two fields, and harvest() depends on
  // seeing a settled reply count once it observes the finish flag.
  std::atomic<uint32_t> replies{0};
  std::atomic<bool> finished{false};
  bool running = false;
  Result result = Result::Unknown;
  uint8_t failStreak = 0;
  bool everOk = false;
};

Probe g_up;
Probe g_down;
uint32_t g_lastRoundMs = 0;

// Both callbacks run on lwIP's ping task, so they do the minimum possible: bump a
// counter and set a flag. The session is torn down from loop() instead, because
// deleting a session from inside its own callback would free the task's context
// out from under it.
void onPingSuccess(esp_ping_handle_t, void *args) {
  static_cast<Probe *>(args)->replies.fetch_add(1, std::memory_order_relaxed);
}

void onPingEnd(esp_ping_handle_t, void *args) {
  static_cast<Probe *>(args)->finished.store(true, std::memory_order_release);
}

void startProbe(Probe &p, const IPAddress &target) {
  if (p.running) {
    return;
  }

  ip_addr_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.type = IPADDR_TYPE_V4;
  addr.u_addr.ip4.addr = static_cast<uint32_t>(target);

  esp_ping_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.count = kPingCount;
  cfg.interval_ms = kPingIntervalMs;
  cfg.timeout_ms = kPingTimeoutMs;
  cfg.data_size = 32;
  cfg.tos = 0;
  cfg.ttl = 64;
  cfg.target_addr = addr;
  cfg.task_stack_size = kPingStackBytes;
  cfg.task_prio = kPingPriority;
  cfg.interface = 0;

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_ping_success = onPingSuccess;
  cbs.on_ping_end = onPingEnd;
  cbs.cb_args = &p;

  p.replies.store(0, std::memory_order_relaxed);
  p.finished.store(false, std::memory_order_release);

  if (esp_ping_new_session(&cfg, &cbs, &p.handle) != ESP_OK) {
    p.handle = nullptr;
    return;
  }
  if (esp_ping_start(p.handle) != ESP_OK) {
    esp_ping_delete_session(p.handle);
    p.handle = nullptr;
    return;
  }
  p.running = true;
}

void harvest(Probe &p) {
  if (!p.running || !p.finished.load(std::memory_order_acquire)) {
    return;
  }

  bool ok = p.replies.load(std::memory_order_relaxed) > 0;
  p.result = ok ? Result::Ok : Result::Failed;
  if (ok) {
    p.everOk = true;
    if (p.failStreak != 0) {
      Serial.printf("[%10lu] health: %s recovered\n", millis(), p.name);
    }
    p.failStreak = 0;
  } else {
    if (p.failStreak < 255) {
      p.failStreak++;
    }
    Serial.printf("[%10lu] health: %s unreachable (%u in a row)\n", millis(), p.name, p.failStreak);
  }

  esp_ping_stop(p.handle);
  esp_ping_delete_session(p.handle);
  p.handle = nullptr;
  p.running = false;
}

}  // namespace

void begin() {
  g_up.name = "upstream gateway";
  g_down.name = "downstream client";
  g_lastRoundMs = 0;
}

void update(bool uplinkReady, const IPAddress &gateway, const IPAddress &client) {
  harvest(g_up);
  harvest(g_down);

  // A round is only meaningful once the uplink holds an address; before that the
  // answer is known and probing it would just add noise to the log.
  if (!uplinkReady) {
    g_up.result = Result::Unknown;
    g_up.failStreak = 0;
    return;
  }

  if (g_lastRoundMs != 0 && millis() - g_lastRoundMs < kRoundIntervalMs) {
    return;
  }
  if (g_up.running || g_down.running) {
    return;  // previous round still in flight
  }
  g_lastRoundMs = millis();

  if (static_cast<uint32_t>(gateway) != 0) {
    startProbe(g_up, gateway);
  }
  if (static_cast<uint32_t>(client) != 0) {
    startProbe(g_down, client);
  }
}

Result upstream() {
  return g_up.result;
}
Result downstream() {
  return g_down.result;
}
uint8_t upstreamFailStreak() {
  return g_up.failStreak;
}
uint8_t downstreamFailStreak() {
  return g_down.failStreak;
}
bool upstreamEverOk() {
  return g_up.everOk;
}
bool downstreamEverOk() {
  return g_down.everOk;
}

void resetStreaks() {
  g_up.failStreak = 0;
  g_down.failStreak = 0;
}

}  // namespace health
