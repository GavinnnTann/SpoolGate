#pragma once

#include <IPAddress.h>

#include "../nat_debug.h"

namespace softap {

// Brings up the SoftAP from config::settings (SSID, password, AP IP, DNS).
// Ordering matters: the netif must be configured (IP, mask, DHCP lease range, DNS)
// BEFORE the AP is created, and the DNS offer re-applied after it is fully up.
// Configuring a live AP after creation leaves the DHCP server reporting "STARTED"
// while never actually handing out leases (clients fall back to 169.254.x.x), and
// bringing the interface up clears the DNS-offer flag — hence the explicit
// re-apply. See softap.cpp.
bool begin();

#if NAT_DEBUG
// Diagnostics (debug build only): whether the AP's DHCP server is running, and the
// DNS server currently configured on the AP netif — i.e. what the DHCP server hands
// clients as their resolver.
const char *dhcpsStatusName();
IPAddress dnsOffered();
#endif

}  // namespace softap
