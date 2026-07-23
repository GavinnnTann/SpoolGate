#pragma once

// NAPT must only be on while the uplink has a valid IP. Leaving it enabled with a
// dead uplink makes clients blackhole silently instead of failing fast.
namespace napt {

void enable();
void disable();
bool isEnabled();

}  // namespace napt
