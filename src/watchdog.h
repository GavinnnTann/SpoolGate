#pragma once

// Recovery supervisor: reboots the board when it stops being useful.
//
// Two distinct failure modes, both ending in esp_restart():
//
//   Loop stall    loop() has not completed an iteration for kLoopStallMs. Catches
//                 a blocking call that never returns or a wait on something that
//                 never arrives — cases where the firmware is stuck but the CPU
//                 is not starved, so the IDF task watchdog never fires.
//   Uplink stall  loop() is running perfectly but the STA has had no IP for
//                 kUplinkStallMs. Nothing is hung at all here: the LED still
//                 flashes and the portal still serves. The router is simply not
//                 routing, which no conventional watchdog can see.
//
// The second is the one that matters in practice — a NAT router with a dead
// uplink looks completely healthy from the inside.
//
// This deliberately does NOT touch the IDF task watchdog. That is already
// running (CONFIG_ESP_TASK_WDT_INIT=y) at a 5 s timeout with CPU0's idle task
// subscribed, where it catches CPU starvation. Re-tuning it to minutes to cover
// the cases above would trade a fast, working guard for a slow one.
namespace watchdog {

// Logs why the board last reset, then starts the supervisor task. Call early in
// setup(), after Serial is up so the reset reason is visible.
void begin();

// Call once per loop() iteration, as early as possible — reaching this call is
// what counts as "loop() is alive". `uplinkReady` is the STA-has-an-IP flag;
// true clears the uplink stall timer.
void update(bool uplinkReady);

// Why the board last reset, as a human-readable string. Captured once in begin(),
// because esp_reset_reason() describes the boot that is running now. Surfacing it
// in the admin portal is the only way to see a brownout or a watchdog reboot on a
// board deployed with no serial cable attached.
const char *lastResetReason();

}  // namespace watchdog
