#include "uplink.h"

#include <WiFi.h>
#include <esp_eap_client.h>

#include "../logbuf.h"
#include "../config.h"

namespace uplink {

void begin() {
  // Reconnect timing is driven explicitly by main's capped exponential backoff,
  // not the framework's built-in auto-reconnect.
  WiFi.setAutoReconnect(false);

  // Abort anything still in flight before touching the config. STAClass::connect()
  // only tears down an STA that is fully connected; if the previous attempt is
  // still associating or authenticating, esp_wifi_set_config() refuses with
  // ESP_ERR_WIFI_STATE ("sta is connecting, cannot set config") and WiFi.begin()
  // does nothing whatsoever — the log records an attempt that was never made.
  // Async (timeout 0) so this never blocks loop(); the resulting disconnect event
  // lands while main has attemptActive set and is correctly ignored.
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false, /*timeoutLength=*/0);

  const config::Settings &s = config::settings;

  // Log the target on every attempt. NVS overrides secrets.h, so a stale NVS entry
  // is otherwise invisible: the firmware dials an SSID you thought you had changed
  // and the only symptom is NO_AP_FOUND. The length is printed because a trailing
  // space or a homoglyph in the SSID looks identical on a serial console.
  logbuf::printf(
    "[%10lu] STA target: SSID=\"%s\" (%u chars) auth=%s\n", millis(), s.uplinkSsid.c_str(), s.uplinkSsid.length(),
    s.uplinkEnterprise ? "WPA2-Enterprise" : "WPA2-Personal"
  );

  if (s.uplinkEnterprise) {
    // Identity and username are logged, the password only as a length. An empty
    // value in any of the three makes the association fail immediately with a bare
    // "Reason: 1 - UNSPECIFIED", which is indistinguishable from the AP simply
    // refusing us — so the credentials have to be visible here to tell the two
    // apart. They are wiped by any code path that writes config without them.
    logbuf::printf(
      "[%10lu] EAP: identity=\"%s\" username=\"%s\" password=%u chars\n", millis(), s.eapIdentity.c_str(), s.eapUsername.c_str(),
      s.eapPassword.length()
    );

    esp_eap_client_set_identity(reinterpret_cast<const unsigned char *>(s.eapIdentity.c_str()), s.eapIdentity.length());
    esp_eap_client_set_username(reinterpret_cast<const unsigned char *>(s.eapUsername.c_str()), s.eapUsername.length());
    esp_eap_client_set_password(reinterpret_cast<const unsigned char *>(s.eapPassword.c_str()), s.eapPassword.length());
    esp_wifi_sta_enterprise_enable();
    WiFi.begin(s.uplinkSsid.c_str());
  } else {
    WiFi.begin(s.uplinkSsid.c_str(), s.uplinkPass.c_str());
  }
}

}  // namespace uplink
