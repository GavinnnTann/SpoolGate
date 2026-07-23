#pragma once

// STA connection to the upstream network. The auth method (WPA2-Personal vs.
// WPA2-Enterprise / 802.1X) is chosen at runtime from config::settings, so it can
// be changed via the web portal without reflashing. Both code paths are always
// compiled in.
namespace uplink {

void begin();

}  // namespace uplink
