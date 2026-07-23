#pragma once

#include <stdint.h>

// Onboard WS2812 RGB LED (GPIO48 on the ESP32-S3-DevKitC-1, via RGB_BUILTIN) used
// as an at-a-glance link-status indicator, so the router's state is visible once
// deployed without a serial cable.
//
//   Boot          solid dim white  — powered, network not up yet
//   UpstreamDown  red flashing      — STA has no IP (not connected to upstream WiFi)
//   NoClients     amber flashing    — upstream up, but no downstream client joined
//   Connected     blue flashing     — upstream up AND >=1 downstream client
//   Fault         solid magenta     — SoftAP failed to start (fatal, should not happen)
namespace statusled {

enum class State {
  Boot,
  UpstreamDown,
  NoClients,
  Connected,
  Fault,
};

// Sets the master brightness (0 = off, 255 = full). Call before begin() and again
// whenever it changes. Colours below are defined at full scale and scaled by this.
void setBrightness(uint8_t brightness);

// Initialises the LED (solid white). Call once from setup().
void begin();

// Drives the LED for the given state. Call every loop() iteration — it handles the
// flash timing internally and only touches the LED hardware when the visible colour
// actually changes, so it is cheap to call at full loop speed.
void update(State s);

}  // namespace statusled
