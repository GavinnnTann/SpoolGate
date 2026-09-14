#include "logbuf.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <stdarg.h>

namespace logbuf {

namespace {

char g_lines[kLines][kLineLen];
size_t g_head = 0;   // next slot to write
size_t g_count = 0;  // lines held, saturating at kLines

// A spinlock rather than a mutex: the framework log hook below can be reached from
// contexts where blocking is not allowed, and every critical section here is a
// bounded memcpy of a single line. The _SAFE variants work from both task and ISR
// context, which a mutex would not.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

vprintf_like_t g_prevVprintf = nullptr;

// Copies one line into the ring, dropping trailing newlines and any ANSI colour
// escapes the IDF logger adds — neither survives usefully in HTML.
void store(const char *text) {
  char clean[kLineLen];
  size_t w = 0;
  for (size_t r = 0; text[r] != '\0' && w < kLineLen - 1; ++r) {
    char c = text[r];
    if (c == '\x1b') {
      while (text[r] != '\0' && text[r] != 'm') {  // skip to the end of the escape
        ++r;
      }
      if (text[r] == '\0') {
        break;
      }
      continue;
    }
    if (c == '\r' || c == '\n') {
      continue;
    }
    clean[w++] = c;
  }
  clean[w] = '\0';

  if (w == 0) {
    return;
  }

  portENTER_CRITICAL_SAFE(&g_mux);
  memcpy(g_lines[g_head], clean, w + 1);
  g_head = (g_head + 1) % kLines;
  if (g_count < kLines) {
    g_count++;
  }
  portEXIT_CRITICAL_SAFE(&g_mux);
}

// Installed as the ESP-IDF log sink. Captures the framework's own output — the
// "Reason: 15 - 4WAY_HANDSHAKE_TIMEOUT" and "sta is connecting, cannot set config"
// lines — then hands the call to the original sink so serial output is unchanged.
int hookVprintf(const char *fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  char buf[kLineLen];
  int n = vsnprintf(buf, sizeof(buf), fmt, copy);
  va_end(copy);

  if (n > 0) {
    store(buf);
  }
  return g_prevVprintf != nullptr ? g_prevVprintf(fmt, args) : ::vprintf(fmt, args);
}

}  // namespace

void begin() {
  g_prevVprintf = esp_log_set_vprintf(hookVprintf);
}

void printf(const char *fmt, ...) {
  char buf[kLineLen];

  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  Serial.print(buf);
  store(buf);
}

void render(String &out) {
  // One line is copied out under the lock at a time. Holding it across the String
  // appends would mean allocating inside a critical section, which is exactly what
  // a spinlock must never do.
  char line[kLineLen];
  for (size_t i = 0; i < kLines; ++i) {
    portENTER_CRITICAL_SAFE(&g_mux);
    if (i >= g_count) {
      portEXIT_CRITICAL_SAFE(&g_mux);
      break;
    }
    size_t idx = (g_head + kLines - g_count + i) % kLines;
    memcpy(line, g_lines[idx], kLineLen);
    portEXIT_CRITICAL_SAFE(&g_mux);

    line[kLineLen - 1] = '\0';
    out += line;
    out += '\n';
  }
}

size_t count() {
  portENTER_CRITICAL_SAFE(&g_mux);
  size_t n = g_count;
  portEXIT_CRITICAL_SAFE(&g_mux);
  return n;
}

}  // namespace logbuf
