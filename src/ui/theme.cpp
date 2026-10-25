#include "ui/theme.h"
#include <math.h>

namespace ui {

//                bg                zebra             head              htxt              sel               text              dim               accent            ok                warn              err               badge             chan              chat              rpt               room
const Theme kThemes[] = {
  {"Midnight",   {rgb(8,10,18),    rgb(20,24,38),    rgb(28,46,92),    rgb(240,244,255),    rgb(40,72,140),   rgb(232,236,244), rgb(128,138,160), rgb(110,190,255), rgb(80,210,120),  rgb(250,200,60),  rgb(250,80,80),   rgb(240,60,90),   rgb(80,220,220),  rgb(150,200,255), rgb(255,170,80),  rgb(200,140,255)}},
  {"Amber",      {rgb(14,8,0),     rgb(30,18,4),     rgb(70,40,0),     rgb(255,230,170),     rgb(95,55,0),    rgb(255,190,90),  rgb(186,137,79),  rgb(255,220,140), rgb(190,230,90),  rgb(255,230,60),  rgb(255,90,50),   rgb(255,120,40),  rgb(255,210,120), rgb(255,180,80),  rgb(255,150,60),  rgb(230,170,110)}},
  {"Phosphor",   {rgb(0,10,4),     rgb(4,24,10),     rgb(0,56,24),     rgb(200,255,210),      rgb(0,83,35),     rgb(120,255,150), rgb(40,150,70),   rgb(190,255,200), rgb(120,255,150), rgb(230,255,90),  rgb(255,110,90),  rgb(250,250,120), rgb(100,240,200), rgb(140,255,170), rgb(200,255,120), rgb(160,220,255)}},
  {"Contrast",   {rgb(0,0,0),      rgb(24,24,24),    rgb(0,0,160),     rgb(255,255,255), rgb(0,90,200),   rgb(255,255,255), rgb(180,180,180), rgb(0,255,255),   rgb(0,255,0),     rgb(255,255,0),   rgb(255,0,0),     rgb(255,0,255),   rgb(0,255,255),   rgb(255,255,255), rgb(255,160,0),   rgb(255,132,255)}},
  {"Daylight",   {rgb(246,246,240),rgb(228,230,236), rgb(40,90,170),   rgb(255,255,255), rgb(170,205,255), rgb(16,20,30),    rgb(95,103,118), rgb(20,80,180),   rgb(20,140,60),   rgb(180,114,0),   rgb(200,30,40),   rgb(220,30,80),   rgb(0,126,135),   rgb(30,70,170),   rgb(180,86,0),    rgb(120,50,170)}},
  {"Ocean",      {rgb(2,18,28),    rgb(6,32,46),     rgb(0,82,110),    rgb(230,250,255),     rgb(0,98,114),   rgb(220,245,250), rgb(110,160,175), rgb(90,230,230),  rgb(90,230,150),  rgb(250,210,90),  rgb(255,100,90),  rgb(255,110,150), rgb(120,240,220), rgb(150,210,255), rgb(255,180,110), rgb(200,170,255)}},
};
static float luminance(uint16_t c) {
  auto lin = [](int v) {   // 0-255 channel
    float s = v / 255.0f;
    return s <= 0.03928f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
  };
  int r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
  r |= r >> 5; g |= g >> 6; b |= b >> 5;
  return 0.2126f * lin(r) + 0.7152f * lin(g) + 0.0722f * lin(b);
}

float contrast(uint16_t a, uint16_t b) {
  float la = luminance(a), lb = luminance(b);
  if (la < lb) { float t = la; la = lb; lb = t; }
  return (la + 0.05f) / (lb + 0.05f);
}

Col onColour(const Theme& t, uint8_t fill) {
  return contrast(t.pal[C_HTXT], t.pal[fill]) >= contrast(t.pal[C_BG], t.pal[fill]) ? C_HTXT : C_BG;
}

const int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

}  // namespace ui
