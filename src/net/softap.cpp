#include "softap.h"

#include <WiFi.h>
#include <esp_netif.h>
#include <dhcpserver/dhcpserver.h>

#include "../config.h"

namespace softap {

namespace {

void applyDnsOffer(const IPAddress &dns);

// Lease range start for the /24: AP IP host octet + 1 (e.g. .1 -> .2), clamped to
// avoid landing on .255 / overflowing the octet.
IPAddress leaseStartFor(const IPAddress &apIp) {
  uint8_t last = apIp[3] < 254 ? apIp[3] + 1 : apIp[3] - 1;
  return IPAddress(apIp[0], apIp[1], apIp[2], last);
}

}  // namespace

bool begin() {
  const config::Settings &s = config::settings;
  const IPAddress subnet(255, 255, 255, 0);

  if (!WiFi.AP.begin()) {
    return false;
  }
  // Must happen before create() — see header comment.
  if (!WiFi.AP.config(s.apIp, s.apIp, subnet, leaseStartFor(s.apIp), s.dns)) {
    return false;
  }
  // Channel is left at its default; the radio is shared with the STA link, so the
  // AP's channel is forced to follow whatever channel STA associates on. Setting it
  // explicitly here would just be overridden.
  if (!WiFi.AP.create(s.apSsid.c_str(), s.apPass.c_str(), /*channel=*/1, /*ssid_hidden=*/0, /*max_connection=*/4, /*ftm_responder=*/false, WIFI_AUTH_WPA2_PSK)) {
    return false;
  }
  if (!WiFi.AP.waitStatusBits(ESP_NETIF_STARTED_BIT, 1000)) {
    return false;
  }

  // Re-apply the DHCP DNS offer now that the AP is fully up. config() already set
  // it, but bringing the interface up afterwards clears the offer flag while leaving
  // the IP and lease-range settings intact — the observable result is a client that
  // gets a correct address and gateway (raw IPs route fine) but no resolver, so
  // every hostname lookup fails. The stop/start bracket is required:
  // esp_netif_dhcps_option() returns ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED and does
  // nothing if the server is running. Safe here because no client can be associated
  // yet at bring-up time.
  applyDnsOffer(s.dns);

  return true;
}

namespace {

void applyDnsOffer(const IPAddress &dns) {
  esp_netif_t *netif = WiFi.AP.netif();
  if (!netif) {
    return;
  }

  esp_netif_dhcps_stop(netif);

  esp_netif_dns_info_t dnsInfo = {};
  dnsInfo.ip.type = IPADDR_TYPE_V4;
  dnsInfo.ip.u_addr.ip4.addr = static_cast<uint32_t>(dns);
  esp_err_t dnsErr = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dnsInfo);

  dhcps_offer_t offer = OFFER_DNS;
  esp_err_t optErr = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));

  esp_err_t startErr = esp_netif_dhcps_start(netif);

#if NAT_DEBUG
  Serial.printf(
    "[%10lu] DNS offer %s: set_dns=%s option=%s dhcps_start=%s\n", millis(), dns.toString().c_str(), esp_err_to_name(dnsErr), esp_err_to_name(optErr),
    esp_err_to_name(startErr)
  );
#else
  (void)dnsErr;
  (void)optErr;
  (void)startErr;
#endif
}

}  // namespace

#if NAT_DEBUG
IPAddress dnsOffered() {
  esp_netif_t *netif = WiFi.AP.netif();
  if (!netif) {
    return IPAddress((uint32_t)0);
  }
  esp_netif_dns_info_t dnsInfo = {};
  if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dnsInfo) != ESP_OK) {
    return IPAddress((uint32_t)0);
  }
  return IPAddress(dnsInfo.ip.u_addr.ip4.addr);
}

const char *dhcpsStatusName() {
  esp_netif_t *netif = WiFi.AP.netif();
  if (!netif) {
    return "NO_NETIF";
  }
  esp_netif_dhcp_status_t status;
  if (esp_netif_dhcps_get_status(netif, &status) != ESP_OK) {
    return "QUERY_FAILED";
  }
  switch (status) {
    case ESP_NETIF_DHCP_STARTED:
      return "STARTED";
    case ESP_NETIF_DHCP_STOPPED:
      return "STOPPED";
    case ESP_NETIF_DHCP_INIT:
      return "INIT";
    default:
      return "UNKNOWN";
  }
}
#endif  // NAT_DEBUG

}  // namespace softap
