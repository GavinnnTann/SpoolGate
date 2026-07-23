# SpoolGate

**ESP32 Wi-Fi NAT router that gets a Bambu Lab 3D printer onto locked-down
enterprise / campus Wi-Fi.**

Get a Bambu Lab 3D printer working on a restricted **enterprise, university, or
campus Wi-Fi** network — the kind where the printer connects but never appears in
**Bambu Handy** or **Bambu Studio** — using a cheap **ESP32-S3** as a tiny NAT
router. SpoolGate joins the restricted network as a client and re-broadcasts its own
clean private Wi-Fi for the printer, NAT-routing traffic between the two, and is
configured through a built-in router-style **web admin portal**.

**Keywords:** ESP32 NAT router · Bambu Lab A1 / A1 Mini / P1P / P1S / X1C · enterprise
Wi-Fi · WPA2-Enterprise · 802.1X · eduroam · campus / university / office Wi-Fi ·
client isolation · AP isolation · mDNS blocked · Bambu cloud offline · Bambu Handy
can't find printer · LAN mode · SoftAP · NAPT · web admin portal · Arduino ·
PlatformIO · ESP32-S3-DevKitC-1

---

## Does this describe your problem?

You have a Bambu Lab printer on an enterprise / campus / office network and:

- The printer **connects to Wi-Fi and gets an IP**, but the **cloud icon never
  appears** on its screen.
- **Bambu Handy** shows it offline, and **Bambu Studio's LAN discovery** never finds
  it — even though laptop and printer are on the same SSID.
- It works perfectly on a **phone hotspot** or your **home Wi-Fi**, so the printer,
  your Bambu account, and the firmware are all fine.
- IT can't or won't change the network policy, so you're stuck sneakernetting gcode
  on an SD card.

If that's you, the cause is almost certainly **client/AP isolation plus blocked
multicast (mDNS)** — standard policy on managed enterprise WLANs. The printer can
reach the gateway but not any peer, and the multicast discovery both Bambu apps rely
on is dropped. Nothing configurable on the printer fixes it. This project puts a NAT
boundary in the way so the printer lands on a clean, unisolated network instead.

Tested on a **Bambu Lab A1 Mini**, but the same cloud + local-discovery mechanism is
shared across the Bambu lineup (A1, A1 Mini, P1P, P1S, X1/X1C), so the same failure —
and the same fix — applies.

## Why it happens (and how this fixes it)

The printer associates with the campus network fine but is invisible to Bambu because
of **client isolation** (it can't talk to peers) and **blocked mDNS** (multicast
discovery is dropped). Diagnostics that pinned it down before any code was written:

| Test | Result | Rules out |
|------|--------|-----------|
| Printer on a phone hotspot | Works — cloud icon, Handy app, prints all fine | Printer hardware, Bambu account, firmware. The problem is the network. |
| `Test-NetConnection us.mqtt.bambulab.com -Port 8883` from campus Wi-Fi | `TcpTestSucceeded: True` | The campus firewall does **not** block Bambu's MQTT broker. Outbound traffic has somewhere to go, so a NAT boundary can actually fix this. |
| TP-Link Archer C80 operation modes | Router / AP only | No wireless-client uplink — can't be the NAT device. |

That second result is the one that makes the whole approach viable: the printer's
cloud traffic is *allowed out*; it's only peer isolation and discovery that break it.

**The fix:** run the ESP32 as **STA + SoftAP simultaneously** with lwIP **NAPT**
enabled.

- It joins the upstream Wi-Fi as an ordinary client (STA).
- It broadcasts its own private, WPA2-protected Wi-Fi on `192.168.5.0/24` (SoftAP).
- It NATs the printer's traffic outbound.

```
  Bambu printer ──Wi-Fi──> [ESP32-S3  SoftAP | NAPT | STA] ──Wi-Fi──> Upstream AP ──> Internet
    192.168.5.2            192.168.5.1        (one client)          (campus / office / home)
```

From the upstream network's view there is exactly one client (the ESP32). From the
printer's view it's on a clean, unisolated /24 with a working default route and a real
DNS server, so cloud connectivity and discovery work again.

### Why not an off-the-shelf router?

- **A consumer router** (e.g. TP-Link Archer C80) typically only supports Router / AP
  modes — no WISP / wireless-client uplink — so it can't join an existing Wi-Fi as a
  client. Check yours; many can't do this.
- **A GL.iNet travel router** *would* work and is the easy buy-it option. This project
  exists because the ESP32s were already on hand, cost ~nothing, and a thumb-sized 5 V
  board is far less conspicuous in a shared lab than a mains-powered consumer router.

## Hardware & platform

- **Board:** ESP32-S3-DevKitC-1 (Xtensa S3, 8 MB flash). Any dual-core ESP32/ESP32-S3
  with 2.4 GHz Wi-Fi works. Originally prototyped on an ESP32-S3-Zero, which ran too
  hot to touch under continuous AP+STA+NAT load — the DevKitC runs cooler and faster.
  Flash/monitor via the DevKitC's **UART** port (stable COM port + auto-reset). See
  [`docs/NOTES.md`](docs/NOTES.md).
- **Framework:** PlatformIO + arduino-esp32 **core 3.x** (via the pioarduino platform
  fork). Core 3.x is required for `WiFi.AP.enableNAPT()`; the official PlatformIO
  registry platform still ships core 2.0.17, where it doesn't exist. Full rationale in
  the header of [`platformio.ini`](platformio.ini).
- **2.4 GHz only, both sides.** Bambu printers have no 5 GHz radio, and the ESP32
  SoftAP is forced onto the STA uplink's channel (a single-radio hardware constraint).
- **Throughput ceiling ~10–15 Mbps** — one half-duplex radio shared between both links
  plus software NAT. Plenty for gcode upload and MQTT cloud/LAN control; not for the
  cloud camera stream (disable that on the printer).

## Quick start

1. Install [PlatformIO](https://platformio.org/).
2. `cp src/secrets.h.example src/secrets.h` and fill in your Wi-Fi details. These are
   the **factory defaults** — you can also set everything later from the web portal
   (below), which is the easier path for WPA2-Enterprise credentials.
3. Pick the default auth type in `secrets.h`: `UPLINK_AUTH_ENTERPRISE 0` for a normal
   password network (WPA2-Personal / home), `1` for **WPA2-Enterprise / 802.1X**
   (eduroam-style campus login with username + password).
4. Flash and monitor:

```
pio run -e esp32dev -t upload            # production build (clean serial logging)
pio run -e esp32dev-debug -t upload      # + verbose diagnostics (self-test, DHCP/DNS status)
pio device monitor -p COMx -b 115200     # watch the serial log
```

`pio run` with no `-e` builds only the production environment. `src/secrets.h` is
gitignored so your credentials never get committed.

> **Windows note:** build from PowerShell, not Git Bash — the toolchain installer
> refuses to run under MSYS. Details in [`docs/NOTES.md`](docs/NOTES.md).

## Web admin portal

SpoolGate is configured through a router-style web UI — no reflashing needed to change
credentials or settings. Connect a device to the SoftAP and browse to the AP address
(`http://192.168.5.1/` by default; it's also printed on the serial log at boot).

- **Login:** default username `admin`, default password = the SoftAP (downstream)
  Wi-Fi password. Both are changeable in the portal. **Change the admin password on
  first use.**
- **Tabs:** **Network** (upstream SSID + WPA2-Personal password or WPA2-Enterprise
  identity/username/password, downstream SSID + password, AP IP + client DNS),
  **LED** (status-LED brightness, `0` = off), and **Admin** (login credentials).
- **Save & reboot** persists settings to NVS flash and restarts to apply. Once saved,
  **NVS is the source of truth** — `secrets.h` only seeds a factory-fresh device, so
  editing it later has no effect unless you factory-reset.

Security: the portal is served **only to clients on the private SoftAP subnet** —
every request from off-subnet (i.e. the upstream network) is rejected, so the admin
page is never exposed to the campus/enterprise side. Sessions use a random cookie
token. Credentials are stored in NVS in plaintext (standard for this device class);
treat physical access to the board as full access.

## Status LED

The onboard WS2812 RGB LED (GPIO38 on this DevKitC-1 board — some revisions use
GPIO48; set via `STATUS_LED_PIN` in `platformio.ini`) shows link state at a glance, so
the router is diagnosable once deployed without a serial cable:

| LED | Meaning |
|-----|---------|
| Solid white | Powered, network not up yet (boot) |
| **Red** flashing | Upstream not connected — STA has no IP |
| **Amber** flashing | Upstream connected, no downstream device joined |
| **Blue** flashing | Upstream connected **and** a downstream device joined |
| Solid magenta | Fault — SoftAP failed to start (should not happen) |

## Firmware structure

```
src/
  main.cpp            boot, event wiring, and the NAPT/DHCP state machine
  config.*            runtime settings, persisted in NVS (secrets.h = factory defaults)
  net/uplink.*        STA connection; WPA2-Personal and 802.1X, chosen at runtime
  net/softap.*        SoftAP bring-up, DHCP lease range, and DNS-offer setup
  net/napt.*          NAPT enable/disable, gated on uplink + client-lease state
  web/portal.*        router-style web admin UI (login, config, LED), SoftAP-only
  statusled.*         onboard RGB LED link-status indicator
  nat_debug.h         NAT_DEBUG compile switch for the diagnostics
  secrets.h(.example) factory-default credentials
docs/NOTES.md         build-environment, board, and platform gotchas
```

Two non-obvious behaviours are load-bearing, hard-won, and documented in the code —
they may save you hours if you're building something similar:

1. **NAPT is deferred until a client actually holds a DHCP lease.** Enabling NAPT
   while a client is mid-DHCP *starves the SoftAP's DHCP server* — the client
   associates but never gets an address and falls back to `169.254.x.x`. NAPT is
   switched on in the `AP_STAIPASSIGNED` handler and off again when the last client
   leaves.
2. **The SoftAP hands out `1.1.1.1` as DNS, set explicitly.** On ESP-IDF 5.5 the
   SoftAP otherwise advertises its *own* IP as the DNS server, which nothing answers —
   so clients get an address and a route but can't resolve hostnames (raw IPs work,
   domain names don't). Same symptom, different layer.

## Prior art & alternatives

If you want a **full-featured, general-purpose** ESP32 NAT router — firewall/ACLs,
port forwarding, OTA updates, a do-everything config UI — use one of these mature
projects instead; they are more capable at being a router than this is:

- [martin-ger/esp32_nat_router](https://github.com/martin-ger/esp32_nat_router) — the
  original ESP-IDF NAT router.
- [dchristl/esp32_nat_router_extended](https://github.com/dchristl/esp32_nat_router_extended)
  — feature-rich fork with a web UI and WPA2-Enterprise support.
- [gjroots/esp32_nat_router_plus](https://github.com/gjroots/esp32_nat_router_plus).

**What SpoolGate does differently:** it's deliberately minimal and single-purpose —
Arduino/PlatformIO (approachable for the 3D-printing/maker crowd rather than raw
ESP-IDF), a focused web UI with just the settings this job needs, and written up
specifically around the **"get a Bambu Lab printer onto restrictive campus/enterprise
Wi-Fi"** use case, with the printer-vs-network diagnostics and the DHCP/DNS/NAPT
gotchas documented in one place. If you just need *this* problem solved and want to
understand what's happening, start here; if you need a Swiss-army NAT router with
firewall/ACLs and port forwarding, start with martin-ger's.

## Scope

**In:** a NAT bridge for a single client (the printer), WPA2-Personal and
WPA2-Enterprise/802.1X uplinks, a web admin portal, and an RGB status LED.
**Out:** port forwarding / firewall rules / VPN, many-client tuning, and the printer's
cloud camera stream (the ESP32 can't carry it — disable it on the printer).

## Responsible use

An ESP32 doing NAT with stored network credentials is, to an enterprise WLAN
controller, **indistinguishable from a rogue AP** — and controllers actively scan for
these. Only run this on a network you're authorised to use, keep the SSID unremarkable
and the radio power modest, and understand your institution's acceptable-use policy.
The clean long-term fix is to ask IT to place the printer's MAC on an IoT VLAN; treat
this as an interim workaround pursued *alongside* that request, not instead of it.

## Suggested GitHub topics

`esp32` `esp32-s3` `nat-router` `wifi-repeater` `bambu-lab` `3d-printing` `platformio`
`arduino` `napt` `wpa2-enterprise` `802-1x` `eduroam` `softap` `iot`
