# Measuring throughput, and reading the result

The upstream project this one is based on,
[martin-ger/esp32_nat_router](https://github.com/martin-ger/esp32_nat_router), says to
"expect something in the range from 5 - 15 mbps under reasonable conditions". 15 Mbps
is the top of that range on a good day, not a number to plan around. This document
covers how to find out what *this* board on *this* network actually does, and what to
conclude from the answer.

There are two tests because there are two different questions, and a single figure
cannot answer both.

---

## Test 1 — the downstream radio link

**In the portal: Settings → Speed test.** No setup, no second machine. Join the
SpoolGate Wi-Fi from a phone or laptop, sign in to the portal, pick a size.

This streams filler from the ESP32 straight to the browser and measures one hop: this
radio to this client. It answers "is the downstream Wi-Fi link itself healthy?" and
nothing else.

It is **not** the speed of NAT-routed traffic, and it will always read higher. A
forwarded packet crosses the same radio twice — received on the STA link, re-sent on
the AP link, same channel, same air-time budget — so routed traffic gets at best
roughly half of what this test reports, before NAT and CPU overhead.

Use it as a floor check. If this reads 3 Mbit/s, stop: the radio link is the problem
and nothing about NAT is worth investigating yet. The same page shows the client's
signal strength as the router hears it, and the heap low-water mark.

## Test 2 — the end-to-end path

```powershell
# joined to the SpoolGate SoftAP
.\tools\speedtest.ps1 -Label through-spoolgate

# then join the upstream Wi-Fi directly and run it again
.\tools\speedtest.ps1 -Label campus-direct

# then diff them
.\tools\speedtest.ps1 -Compare .\results\speedtest-through-spoolgate-*.json,.\results\speedtest-campus-direct-*.json
```

Run both from the same physical spot, a few minutes apart. **The difference between
the two runs is the only number that says what the ESP32 costs you.** A single reading
cannot separate "the ESP32 is slow" from "the campus network is slow", and on a busy
managed WLAN the second is common.

The script measures four things, and reads nothing back to the router — it only GETs
`/stat.json`.

| What | Why it is there |
|---|---|
| Download and upload throughput | bulk rate over a fixed-length window, so fast and slow paths get the same load period |
| Idle latency ladder | the ESP32, the upstream gateway, the open internet — so a bad hop can be named instead of guessed at |
| Latency under load | the same ping sampled *during* each transfer |
| Router-side state | `/stat.json` before and after: per-client signal, heap low-water mark |

**Latency under load is the row to read first.** A path whose RTT goes from 5 ms idle
to 800 ms under load will stutter a stream at any throughput figure, and it is the
most common reason a stream plays for ten seconds and then freezes.

---

## What the numbers should look like

Rough expectations for an ESP32-S3 doing AP+STA+NAPT on one radio:

| Measurement | Healthy | Marginal | Broken |
|---|---|---|---|
| Portal test (one hop, AP link) | 20–40 Mbit/s | 8–20 | under 8 |
| Routed download | 6–15 Mbit/s | 3–6 | under 3 |
| Routed upload | 5–12 Mbit/s | 2–5 | under 2 |
| Idle RTT to the ESP32 | 2–6 ms | 6–20 | over 20, or any loss |
| RTT under load, routed | under 100 ms | 100–400 | over 400 |
| Client signal at the router | better than −65 dBm | −65 to −75 | worse than −75 |

Where you land inside the upstream project's 5–15 Mbps range is set mostly by:

- **Both signal strengths.** Two links share one radio, and the weaker one sets the
  modulation rate for its half of the air time. `/stat.json` reports both.
- **Channel congestion.** The SoftAP cannot pick its own channel — associating
  upstream drags it onto the uplink AP's channel. On a campus network that channel is
  usually crowded, and air time is shared with every other client on it.
- **Free heap.** The Wi-Fi driver's transmit buffers come out of the same heap as
  everything else. If the low-water mark falls sharply during a run, the driver is
  running out of buffers, and that is a ceiling no amount of signal will lift.

## Reading the results

| Symptom | Most likely cause | What to do |
|---|---|---|
| Portal test also slow | downstream radio link | move the board closer to the printer; check the client signal on the speed-test page |
| Portal test fine, routed slow, both runs slow | the upstream network, not the ESP32 | compare against the baseline run before touching anything here |
| Portal test fine, routed slow, baseline fast | the ESP32 is the bottleneck | expected to a degree — see the halving above; check signal and channel |
| Throughput fine, RTT under load huge | buffering along the path | the usual cause of a stuttering stream at adequate bandwidth |
| Throughput fine, packet loss non-zero | radio or driver buffers | check both signals, then the heap low-water mark |
| Heap low-water mark drops a lot per run | Wi-Fi driver buffer exhaustion | a build-level limit; see the NAPT resource note in [`NOTES.md`](NOTES.md) |
| Everything fine, stream still broken | not the network | see below |

---

## The camera specifically

The A1 mini's Live View camera is **MJPEG at roughly 0.5–1 frame per second** at
1080p — Bambu caps the frame rate deliberately to keep load off the printer's
mainboard. Call it **1–3 Mbit/s** in round numbers.

That is well under even a pessimistic estimate of this router's ceiling, which has a
concrete consequence for debugging:

> **Raw bandwidth is probably not why the camera will not play.** If the routed
> throughput comes back anywhere above about 4 Mbit/s, the bottleneck is somewhere
> else, and chasing a bigger number is wasted effort.

What to suspect instead, in order:

1. **Latency spikes and loss, not rate.** MJPEG at 1 fps is not a smooth trickle — it
   is a ~100–300 KB burst once a second. A burst that size needs a path that can
   absorb it without dropping segments; every retransmit pushes the frame past the
   viewer's deadline and shows up as a freeze. The `RTT under load` and `loss` rows,
   and the iperf3 retransmit count, are the measurements that matter here.
2. **The cloud relay, not the local path.** Live View from Handy or Studio in cloud
   mode goes printer → Bambu cloud → your viewer. Both legs are involved, and the
   router only affects the first. The baseline run tells you what the campus link
   contributes.
3. **Direction.** The printer is the *sender*, so a camera stream leaves over the
   **upload** path. Upload is the weaker direction through a SoftAP, which has to
   receive each packet over the air before it can re-send it. Read the upload row, not
   the download row.
4. **LAN-mode viewing cannot work here at all.** If Studio is on the upstream network
   and the printer is behind the NAT, nothing upstream can open a connection *inward*
   to the printer — that is what a NAT boundary does. Local Live View only works from
   a device joined to the SpoolGate Wi-Fi alongside the printer.

The project README already recommends disabling the cloud camera stream for this
reason. These tests are how to confirm whether that recommendation still applies to
your setup or whether something else has gone wrong.

---

## Optional: iperf3

Sharper than the HTTPS test, because it reports TCP retransmits and can hold a fixed
offered rate while measuring loss and jitter — which is much closer to what a stream
actually does.

```powershell
.\tools\speedtest.ps1 -Label through-spoolgate -Iperf vx15
```

It needs a server you can genuinely reach:

- **A box on the campus LAN will not work.** Client isolation is the entire reason
  this project exists; peers on that network are unreachable.
- **A host over Tailscale does work**, since the tunnel is an outbound connection.
  Expect a few percent of overhead from the encapsulation.
- `winget install iperf3` on this machine; `iperf3 -s` on the server.

The UDP ramp is the most directly useful part. It offers 1, 3, 5 and 8 Mbit/s in turn
and reports loss and jitter at each. **The lowest rate that starts losing datagrams is
this path's real ceiling for a live stream.** If that ceiling sits below about
3 Mbit/s, the camera cannot work over this path regardless of what the bulk throughput
test says.

## Limits of these tools

- **The portal test blocks `loop()` while it runs.** NAPT reconciliation, the health
  probes and the status LED all pause for the duration. Capped at 64 MiB and 20 s, and
  the loop watchdog does not act for five minutes, so this is safe — but do not run it
  while something is relying on the router.
- **There is no firmware-side upload test.** The Arduino `WebServer` buffers a whole
  non-form POST body into RAM before the handler ever sees it, and caps the wait at
  5 s, so a large upload cannot be measured this way at all. The upload direction is
  measured end-to-end by the script instead, which is the more useful figure anyway.
- **Windows reports whole milliseconds for ICMP.** Coarse for a LAN hop, but ample for
  spotting the multi-hundred-millisecond stalls that actually break a stream.
- **The download endpoints are third-party.** The script tries a Singapore mirror
  first and falls back; if all of them fail it says so with the HTTP status rather than
  blaming the router. `-DownUrl` points it at your own server instead.
- **One reading is not a measurement.** 2.4 GHz is shared with everything nearby.
  Run each side two or three times before believing a difference.
