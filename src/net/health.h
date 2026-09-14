#pragma once

#include <IPAddress.h>
#include <stdint.h>

// Reachability probes in both directions.
//
// "Associated" and "holds an IP" are not the same as "traffic flows", and the
// gap between them is invisible from every signal the firmware had until now: a
// client that re-associates on a still-valid DHCP lease leaves the uplink up,
// the station count non-zero and the status LED blue while nothing is forwarded
// at all. These probes close that gap by actually putting packets on the wire —
// upstream to the gateway, downstream to the client — and reporting whether
// anything came back.
//
// Probing runs in lwIP's own ping task, so update() never blocks loop().
namespace health {

enum class Result : uint8_t {
  Unknown,  // never probed, or no address to probe yet
  Ok,
  Failed,
};

void begin();

// Call every loop(). Starts a probe round when one is due and harvests finished
// ones. `gateway` and `client` may be unset (0.0.0.0), in which case that
// direction is skipped and stays Unknown.
void update(bool uplinkReady, const IPAddress &gateway, const IPAddress &client);

Result upstream();
Result downstream();

// Consecutive failed rounds. Recovery actions are keyed off these rather than a
// single failure, because one lost round means very little on a busy 2.4 GHz band.
uint8_t upstreamFailStreak();
uint8_t downstreamFailStreak();

// True once that direction has answered at least once since boot. Recovery must
// be gated on this: a host that never replies to ICMP at all (a printer with a
// strict firewall, a gateway that drops pings by policy) would otherwise look
// permanently broken and drive an endless reconnect or deauth loop.
bool upstreamEverOk();
bool downstreamEverOk();

// Clears both streaks. Call after acting on a failure so the next round starts
// from a clean slate rather than immediately re-triggering.
void resetStreaks();

}  // namespace health
