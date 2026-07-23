#include "config.h"

#include <Preferences.h>

#include "secrets.h"

namespace config {

Settings settings;

namespace {

constexpr const char *kNamespace = "natcfg";

Preferences prefs;

// The admin password factory-defaults to the SoftAP (downstream) password — a
// value the operator already knows, and one that grants no extra access since the
// portal is only reachable from the SoftAP anyway.
IPAddress parseIp(const String &s, const IPAddress &fallback) {
  IPAddress ip;
  return ip.fromString(s) ? ip : fallback;
}

void loadFromDefaults() {
  settings.uplinkSsid = UPLINK_SSID;
  settings.uplinkEnterprise = (UPLINK_AUTH_ENTERPRISE != 0);
  settings.uplinkPass = UPLINK_PASS;
  settings.eapIdentity = UPLINK_EAP_IDENTITY;
  settings.eapUsername = UPLINK_EAP_USERNAME;
  settings.eapPassword = UPLINK_EAP_PASSWORD;
  settings.apSsid = PRINTERNET_SSID;
  settings.apPass = PRINTERNET_PASS;
  settings.apIp = IPAddress(192, 168, 5, 1);
  settings.dns = IPAddress(1, 1, 1, 1);
  settings.ledBrightness = 64;  // ~25% — bright enough to read, not glaring
  settings.adminUser = "admin";
  settings.adminPass = PRINTERNET_PASS;
}

}  // namespace

void begin() {
  // Start from the compiled-in defaults; NVS (if provisioned) overlays them.
  loadFromDefaults();

  // Open read-write so the namespace is created if absent — a read-only open of a
  // never-written namespace logs "nvs_open failed: NOT_FOUND". isKey() uses the raw
  // nvs_get_* calls, which are silent on a missing key (unlike the getString/getBool
  // wrappers), so this existence check is quiet on a blank device.
  // "Provisioned" is keyed off uSsid, which save() always writes — so a config
  // saved by any earlier build is detected and never clobbered by a reseed.
  prefs.begin(kNamespace, /*readOnly=*/false);
  bool provisioned = prefs.isKey("uSsid");
  prefs.end();

  if (!provisioned) {
    // First boot: persist the defaults so every subsequent read finds its key (no
    // per-key NOT_FOUND error spam). `settings` already holds the defaults.
    save();
    return;
  }

  prefs.begin(kNamespace, /*readOnly=*/true);
  settings.uplinkSsid = prefs.getString("uSsid", settings.uplinkSsid);
  settings.uplinkEnterprise = prefs.getBool("uEnt", settings.uplinkEnterprise);
  settings.uplinkPass = prefs.getString("uPass", settings.uplinkPass);
  settings.eapIdentity = prefs.getString("eapId", settings.eapIdentity);
  settings.eapUsername = prefs.getString("eapUser", settings.eapUsername);
  settings.eapPassword = prefs.getString("eapPass", settings.eapPassword);
  settings.apSsid = prefs.getString("apSsid", settings.apSsid);
  settings.apPass = prefs.getString("apPass", settings.apPass);
  settings.apIp = parseIp(prefs.getString("apIp", settings.apIp.toString()), settings.apIp);
  settings.dns = parseIp(prefs.getString("dns", settings.dns.toString()), settings.dns);
  settings.ledBrightness = prefs.getUChar("ledBri", settings.ledBrightness);
  settings.adminUser = prefs.getString("admUser", settings.adminUser);
  settings.adminPass = prefs.getString("admPass", settings.adminPass);
  prefs.end();
}

void save() {
  prefs.begin(kNamespace, /*readOnly=*/false);
  prefs.putString("uSsid", settings.uplinkSsid);
  prefs.putBool("uEnt", settings.uplinkEnterprise);
  prefs.putString("uPass", settings.uplinkPass);
  prefs.putString("eapId", settings.eapIdentity);
  prefs.putString("eapUser", settings.eapUsername);
  prefs.putString("eapPass", settings.eapPassword);
  prefs.putString("apSsid", settings.apSsid);
  prefs.putString("apPass", settings.apPass);
  prefs.putString("apIp", settings.apIp.toString());
  prefs.putString("dns", settings.dns.toString());
  prefs.putUChar("ledBri", settings.ledBrightness);
  prefs.putString("admUser", settings.adminUser);
  prefs.putString("admPass", settings.adminPass);
  prefs.end();
}

void factoryReset() {
  prefs.begin(kNamespace, /*readOnly=*/false);
  prefs.clear();
  prefs.end();
  loadFromDefaults();
  save();
}

}  // namespace config
