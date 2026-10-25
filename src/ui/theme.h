// Colour themes. The canvas stores palette indices, so every slot below is a
// *role*, not a colour: switching theme only swaps the 16-entry table.
#pragma once
#include <stdint.h>

namespace ui {

enum Col : uint8_t {
  C_BG = 0,      // content background
  C_ZEBRA,       // alternate list rows, panels, footer
  C_HEAD,        // header bar background
  C_HTXT,        // text on the header bar (and on badges)
  C_SEL,         // selection bar
  C_TEXT,        // primary text
  C_DIM,         // secondary text, separators
  C_ACCENT,      // titles, active tab, focused edges
  C_OK,          // delivered, good signal, connected
  C_WARN,        // pending, fair signal, low battery
  C_ERR,         // failed, poor signal, critical
  C_BADGE,       // unread badges
  C_CHAN,        // channels
  C_CHAT,        // chat contacts
  C_RPT,         // repeaters
  C_ROOM,        // rooms and sensors
};

struct Theme {
  const char* name;
  uint16_t pal[16];
};

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// WCAG contrast ratio of two RGB565 colours, as the panel shows them.
float contrast(uint16_t a, uint16_t b);
// Text on a filled badge or pill: the header text colour or the background,
// whichever reads better on `fill` in this theme.
Col onColour(const Theme& t, uint8_t fill);

extern const Theme kThemes[];
extern const int kThemeCount;

}  // namespace ui
