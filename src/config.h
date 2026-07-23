#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// Runtime configuration, persisted in NVS flash. secrets.h supplies the factory
// defaults that seed NVS on first boot (and that a factory reset restores). Once
// provisioned via the web portal, NVS is the source of truth — secrets.h is only
// the fallback. This is why the net modules read config::settings rather than the
// secrets.h macros directly.
namespace config {

struct Settings {
  // Upstream (STA / the network the ESP32 joins as a client).
  String uplinkSsid;
  bool uplinkEnterprise;  // false = WPA2-Personal, true = WPA2-Enterprise / 802.1X
  String uplinkPass;      // WPA2-Personal pre-shared key
  String eapIdentity;     // 802.1X outer identity
  String eapUsername;     // 802.1X username
  String eapPassword;     // 802.1X password

  // Downstream (SoftAP / the private network the printer joins).
  String apSsid;
  String apPass;

  // Network.
  IPAddress apIp;  // SoftAP address + gateway (also determines the /24 subnet)
  IPAddress dns;   // DNS server handed to DHCP clients

  // Status LED.
  uint8_t ledBrightness;  // 0 (off) .. 255 (full) master brightness for the RGB LED

  // Web-portal admin login.
  String adminUser;
  String adminPass;
};

extern Settings settings;

// Loads settings from NVS, falling back to the secrets.h factory defaults for any
// key not yet stored. Call once, early in setup().
void begin();

// Persists the current `settings` to NVS.
void save();

// Restores the secrets.h factory defaults into `settings` and NVS.
void factoryReset();

}  // namespace config
