#pragma once

// Compile-time switch for verbose serial diagnostics. Set to 1 by the
// esp32dev-debug PlatformIO environment (build_flags += -DNAT_DEBUG=1); defaults
// to 0 for the normal deployed build. Guards the uplink self-test, the periodic
// AP/DHCP/DNS/RSSI status line, and the DHCP-DNS-offer readback helpers. The
// always-on state-transition logging (NAPT on/off, DHCP leases, connect/reconnect)
// is intentionally NOT behind this flag — it is the deployed debugging surface.
#ifndef NAT_DEBUG
#define NAT_DEBUG 0
#endif
