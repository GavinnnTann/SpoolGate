#include "uplink.h"

#include <WiFi.h>
#include <esp_eap_client.h>

#include "../config.h"

namespace uplink {

void begin() {
  // Reconnect timing is driven explicitly by main's capped exponential backoff,
  // not the framework's built-in auto-reconnect.
  WiFi.setAutoReconnect(false);

  const config::Settings &s = config::settings;

  if (s.uplinkEnterprise) {
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
