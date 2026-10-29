// Which Diagnostics pages show: a bit each, set in Settings. A new install shows Radio and System;
// an existing one keeps seeing every page, as it did before the choice existed.
#pragma once
#include <stdint.h>

namespace app {

enum DiagPage : uint8_t {
  DIAG_RADIO = 1, DIAG_SYSTEM = 2, DIAG_POWER = 4, DIAG_STORAGE = 8, DIAG_BLUETOOTH = 16,
  DIAG_ALL = 31,
  DIAG_NEW_INSTALL = DIAG_RADIO | DIAG_SYSTEM,
};

// `diagSet` 0 is a file that predates the choice.
inline void settleDiagPages(uint8_t& pages, uint8_t& diagSet, bool freshInstall) {
  if (diagSet) return;
  diagSet = 1;
  pages = freshInstall ? (uint8_t)DIAG_NEW_INSTALL : (uint8_t)DIAG_ALL;
}

// The pages to show: never none, so Diagnostics always has something to open.
inline uint8_t diagPagesShown(uint8_t pages) { return (pages & DIAG_ALL) ? (uint8_t)(pages & DIAG_ALL) : (uint8_t)DIAG_RADIO; }

}  // namespace app
