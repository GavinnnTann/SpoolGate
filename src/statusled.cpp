#include "statusled.h"

#include <Arduino.h>

// Which GPIO drives the onboard WS2812. Defaults to the variant's RGB_BUILTIN
// (GPIO48 on the generic esp32s3 variant). Some ESP32-S3-DevKitC-1 board
// revisions/clones wire the LED to GPIO38 instead; override with
// -DSTATUS_LED_PIN=38 if the LED stays dark on 48.
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN RGB_BUILTIN
#endif

namespace statusled {

namespace {

// ~1.25 Hz flash (400 ms on, 400 ms off) for the operational states.
constexpr uint32_t kFlashHalfPeriodMs = 400;

struct Rgb {
  uint8_t r, g, b;
};

// Full-scale hues; actual output is scaled by the configurable master brightness
// (config::settings.ledBrightness) so the whole indicator dims together.
constexpr Rgb kOff{0, 0, 0};
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kAmber{255, 90, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kMagenta{255, 0, 255};

uint8_t g_brightness = 64;

// Cache so the RMT bit-bang (which briefly disables interrupts) only runs on an
// actual visible change, not every loop iteration.
int32_t lastWrittenPacked = -1;

void write(Rgb c) {
  Rgb scaled{
    static_cast<uint8_t>(c.r * g_brightness / 255),
    static_cast<uint8_t>(c.g * g_brightness / 255),
    static_cast<uint8_t>(c.b * g_brightness / 255),
  };
  int32_t packed = (int32_t(scaled.r) << 16) | (int32_t(scaled.g) << 8) | scaled.b;
  if (packed == lastWrittenPacked) {
    return;
  }
  lastWrittenPacked = packed;
  rgbLedWrite(STATUS_LED_PIN, scaled.r, scaled.g, scaled.b);
}

}  // namespace

void setBrightness(uint8_t brightness) {
  g_brightness = brightness;
  lastWrittenPacked = -1;  // force the next update() to rewrite at the new level
}

void begin() {
  lastWrittenPacked = -1;
  write(kWhite);
}

void update(State s) {
  Rgb color;
  bool flashing;
  switch (s) {
    case State::UpstreamDown:
      color = kRed;
      flashing = true;
      break;
    case State::NoClients:
      color = kAmber;
      flashing = true;
      break;
    case State::Connected:
      color = kBlue;
      flashing = true;
      break;
    case State::Fault:
      color = kMagenta;
      flashing = false;
      break;
    case State::Boot:
    default:
      color = kWhite;
      flashing = false;
      break;
  }

  bool phaseOn = !flashing || ((millis() / kFlashHalfPeriodMs) % 2 == 0);
  write(phaseOn ? color : kOff);
}

}  // namespace statusled
