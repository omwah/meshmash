// When the CPU may light-sleep between loop passes: only with the screen off and locked, nothing
// of Bluetooth running, no USB host, no sound, no GPS wanted, nothing for MeshCore to send or
// retry, and the unlock button released. Plain logic so it can be tested on the host; the first
// condition that fails is the reason shown on the Power page.
#pragma once
#include <stddef.h>

namespace app {

struct SleepInputs {
  bool screenOff, locked;
  bool bleOn;        // the phone link is on (Bluetooth enabled), or remote mode, which needs it
  bool usbHost;      // a computer is attached to the USB port
  bool audio;        // a sound is playing
  bool gps;          // the receiver is wanted (boot clock sync, position sharing)
  bool pending;      // MeshCore has packets to send or retry
  bool unlockHeld;   // the unlock button (trackball click, G0) is down
};

// nullptr: may sleep.
inline const char* sleepBlock(const SleepInputs& s) {
  if (!s.screenOff) return "screen";
  if (!s.locked) return "unlocked";
  if (s.bleOn) return "BLE on";
  if (s.usbHost) return "USB";
  if (s.audio) return "audio";
  if (s.gps) return "GPS";
  if (s.pending) return "work";
  if (s.unlockHeld) return "button";
  return nullptr;
}

}  // namespace app
