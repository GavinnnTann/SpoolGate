#include "logbuf.h"
#include "watchdog.h"

#include <Arduino.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace watchdog {

namespace {

// Both thresholds are 5 minutes. For the uplink that is five full attempts at the
// capped 60 s reconnect backoff — long enough that a briefly absent AP, a campus
// RADIUS hiccup or a roam between buildings resolves on its own without a reboot.
constexpr uint32_t kLoopStallMs = 300000;
constexpr uint32_t kUplinkStallMs = 300000;

// Poll rate of the supervisor. Nothing here is time-critical against a 5 minute
// threshold; this only bounds how long past the deadline a reboot happens.
constexpr uint32_t kPollIntervalMs = 1000;

// Core 0. loopTask runs on core 1 (ARDUINO_RUNNING_CORE), so pinning the
// supervisor to the other core means a loop() spinning without yielding cannot
// starve the very task meant to notice it.
constexpr BaseType_t kSupervisorCore = 0;

// Priority 2 — above loopTask (1), far below the WiFi/lwIP tasks (18+), which
// block on their own queues and so never crowd this out.
constexpr UBaseType_t kSupervisorPriority = 2;

constexpr uint32_t kSupervisorStackBytes = 4096;

// Written by loop() via update(), read by the supervisor task. Each is a single
// aligned 32-bit word, so reads and writes are atomic on this architecture and no
// lock is needed — the worst case is the supervisor acting on a value up to one
// poll interval stale, which is immaterial against a five-minute threshold.
volatile uint32_t g_lastLoopMs = 0;
volatile uint32_t g_uplinkDownSinceMs = 0;  // 0 means the uplink is up
volatile bool g_armed = false;

const char *g_resetReason = "(not read)";

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:    return "POWERON (clean power-up)";
    case ESP_RST_EXT:        return "EXT (external reset pin)";
    case ESP_RST_SW:         return "SW (esp_restart — portal save or watchdog)";
    case ESP_RST_PANIC:      return "PANIC (crash)";
    case ESP_RST_INT_WDT:    return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:   return "TASK_WDT (IDF task watchdog — CPU starved)";
    case ESP_RST_WDT:        return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT (supply sagged — undersized USB source?)";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE error";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH (supply glitch)";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP (double exception)";
    default:                 return "UNKNOWN";
  }
}

// Marks the uplink as down, starting the stall clock if it is not already running.
// millis() is 0 for the first millisecond after boot, which would collide with the
// "uplink is up" sentinel, so that single value is nudged to 1.
void markUplinkDown() {
  if (g_uplinkDownSinceMs != 0) {
    return;
  }
  uint32_t now = millis();
  g_uplinkDownSinceMs = (now == 0) ? 1 : now;
}

void reboot(const char *why) {
  logbuf::printf("[%10lu] WATCHDOG: %s — rebooting\n", millis(), why);
  Serial.flush();
  delay(50);  // let the UART drain; the reset reason on the next boot reads SW
  esp_restart();
}

void supervisorTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    if (!g_armed) {
      continue;
    }

    // Unsigned subtraction, so the ~49.7 day millis() rollover is handled without
    // a special case: the difference stays correct across the wrap.
    uint32_t now = millis();

    if (now - g_lastLoopMs >= kLoopStallMs) {
      reboot("loop() has not run for 5 min");
    }

    uint32_t downSince = g_uplinkDownSinceMs;
    if (downSince != 0 && now - downSince >= kUplinkStallMs) {
      reboot("no upstream IP for 5 min");
    }
  }
}

}  // namespace

void begin() {
  // The single most useful line in the boot log once the board is deployed on a
  // wall socket with no serial cable: it distinguishes, after the fact, a brownout
  // from a watchdog reboot from a clean power-up. BROWNOUT here means the USB
  // supply cannot hold the rail under WiFi transmit bursts.
  esp_reset_reason_t reason = esp_reset_reason();
  g_resetReason = resetReasonName(reason);
  logbuf::printf("[%10lu] last reset: %s\n", millis(), resetReasonName(reason));

  // Start both clocks running now. The uplink is down until proven otherwise, so
  // a board that never manages to associate still reboots on schedule rather than
  // waiting for a first success that never comes.
  g_lastLoopMs = millis();
  markUplinkDown();
  g_armed = true;

  BaseType_t ok = xTaskCreatePinnedToCore(
    supervisorTask, "watchdog", kSupervisorStackBytes, nullptr, kSupervisorPriority, nullptr, kSupervisorCore
  );
  if (ok != pdPASS) {
    // Non-fatal: the router works, it just loses unattended recovery. Worth saying
    // out loud rather than silently running without the safety net.
    logbuf::printf("[%10lu] WATCHDOG: supervisor task failed to start — no auto-recovery\n", millis());
    g_armed = false;
  }
}

const char *lastResetReason() {
  return g_resetReason;
}

void update(bool uplinkReady) {
  g_lastLoopMs = millis();

  if (uplinkReady) {
    g_uplinkDownSinceMs = 0;
  } else {
    markUplinkDown();
  }
}

}  // namespace watchdog
