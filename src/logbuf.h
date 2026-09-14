#pragma once

#include <Arduino.h>

// In-memory ring of the most recent log lines, so the serial log can be read from
// the admin portal instead of a USB cable. A router deployed on a wall socket is
// exactly where its log is least reachable and most needed.
//
// Everything written through printf() goes to Serial as before and is additionally
// kept here. begin() also intercepts the ESP-IDF / Arduino logging path, so the
// framework's own warnings — the "Reason: 15 - 4WAY_HANDSHAKE_TIMEOUT" and
// "sta is connecting, cannot set config" lines, which are the most diagnostically
// valuable output the firmware produces — are captured too, despite never passing
// through Serial.printf.
namespace logbuf {

// Lines retained. At roughly one line per state change plus a heartbeat every
// 30 s, this is a comfortable window over the last several minutes.
constexpr size_t kLines = 64;
constexpr size_t kLineLen = 128;

// Installs the framework log hook. Call once, after Serial.begin().
void begin();

// Writes to Serial and appends to the ring. Drop-in for Serial.printf.
void printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Appends the buffer, oldest line first, to `out`. Each line already ends in a
// newline. Safe to call while other tasks are logging.
void render(String &out);

// Number of lines currently held.
size_t count();

}  // namespace logbuf
