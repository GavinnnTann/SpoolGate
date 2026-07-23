#pragma once

// Router-style web admin portal, served on the SoftAP. Lets the operator edit the
// upstream/downstream credentials and network settings (config::settings) and the
// admin login itself, then applies them with a reboot. Deliberately reachable ONLY
// from the private SoftAP subnet — every handler rejects requests whose source IP is
// not on the AP's /24, so the admin page is never exposed to the upstream network.
namespace portal {

// Starts the web server. Call after the SoftAP is up.
void begin();

// Services pending HTTP requests and a scheduled post-save reboot. Call from loop().
void loop();

}  // namespace portal
