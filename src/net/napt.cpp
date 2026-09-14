#include "napt.h"

#include "../logbuf.h"

#include <WiFi.h>

namespace napt {

static bool s_enabled = false;

void enable() {
  if (s_enabled) {
    return;
  }
  bool ok = WiFi.AP.enableNAPT(true);
  logbuf::printf("[%10lu] enableNAPT(true) returned %d\n", millis(), ok);
  s_enabled = ok;
}

void disable() {
  if (!s_enabled) {
    return;
  }
  WiFi.AP.enableNAPT(false);
  s_enabled = false;
}

bool isEnabled() {
  return s_enabled;
}

}  // namespace napt
