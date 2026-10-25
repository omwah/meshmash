// Host simulator: runs the real UI against MockModel with a virtual clock.
//
//   sim [-s script] [-o outdir]          (script from stdin by default)
//
// Commands (see runner.cpp for key/hold/type/wait/shot/theme, and with
// MC_BOARD_TDECK tap/long/swipe):
//   rx dm <contact> <text>, rx chan <slot> <sender>|<text>
//   ble on|off|pairing, gps on|off|fix|nofix
//   fresh, empty                (before anything else)
#ifndef PIO_UNIT_TESTING
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/app.h"
#include "ui/screens/screens.h"
#include "mock_model.h"
#include "runner.h"

namespace {

class MockWorld : public sim::World {
public:
  explicit MockWorld(sim::MockModel& m) : _m(m) {}
  bool setup(const char* cmd, const char*) override {
    if (!strcmp(cmd, "fresh")) { _m.setFresh(true); return true; }
    if (!strcmp(cmd, "empty")) { _m.setEmpty(); return true; }
    return false;
  }
  void step(uint32_t) override { _m.step(); }
  bool command(const char* cmd, const char* arg, char* err, size_t n) override {
    if (!strcmp(cmd, "rx")) {
      char kind[8] = {};
      int idx = 0, u = 0;
      sscanf(arg, "%7s %d %n", kind, &idx, &u);
      const char* rest = arg + u;
      if (!strcmp(kind, "dm")) {
        _m.rxDirect(idx, rest);
      } else {
        const char* bar = strchr(rest, '|');
        if (!bar) { snprintf(err, n, "rx chan needs sender|text"); return true; }
        char sender[32];
        snprintf(sender, sizeof sender, "%.*s", (int)(bar - rest), rest);
        _m.rxChannel(idx, sender, bar + 1);
      }
      return true;
    }
    if (!strcmp(cmd, "corpus")) {   // corpus <file> <source channel> <slot> [count]
      char path[200] = {};
      int src = 0, slot = 0, count = 60;
      if (sscanf(arg, "%199s %d %d %d", path, &src, &slot, &count) < 3) { snprintf(err, n, "corpus <file> <chan> <slot> [count]"); return true; }
      if (!_m.loadCorpus(path, src, slot, count)) snprintf(err, n, "no messages for channel %d in %s", src, path);
      return true;
    }
    if (!strcmp(cmd, "ble")) {
      if (!strcmp(arg, "pairing")) _m.setPairing(true);
      else { _m.setPairing(false); _m.setBle(!strcmp(arg, "on")); }
      return true;
    }
    if (!strcmp(cmd, "pref")) {   // pref notify led|screen|dark|both, pref timeout <s>, pref lock on|off, pref scroll same|opposite
      char what[16] = {}, val[16] = {};
      sscanf(arg, "%15s %15s", what, val);
      model::Prefs pr = _m.prefs();
      if (!strcmp(what, "notify")) {
        static const char* const modes[] = {"led", "screen", "dark", "both"};
        int m = 0;
        while (m < 4 && strcmp(val, modes[m])) m++;
        if (m == 4) { snprintf(err, n, "unknown notify mode '%s'", val); return true; }
        pr.notify = (model::NotifyMode)m;
      } else if (!strcmp(what, "timeout")) {
        pr.screenTimeoutS = (uint16_t)atoi(val);
      } else if (!strcmp(what, "scroll")) {   // pref scroll same|opposite
        pr.touchScrollNatural = !strcmp(val, "opposite");
      } else if (!strcmp(what, "lock")) {
        pr.lockOnTimeout = !strcmp(val, "on");
      } else {
        snprintf(err, n, "unknown pref '%s'", what);
        return true;
      }
      _m.setPrefs(pr);
      return true;
    }
    if (!strcmp(cmd, "gps")) {
      if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
        model::Prefs pr = _m.prefs();
        pr.gpsEnabled = !strcmp(arg, "on");
        _m.setPrefs(pr);
      } else {
        _m.setGpsFix(!strcmp(arg, "fix"));
      }
      return true;
    }
    return false;
  }

private:
  sim::MockModel& _m;
};

uint32_t g_now = 0;

}  // namespace

int main(int argc, char** argv) {
  const char* script = nullptr;
  const char* out = ".";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s") && i + 1 < argc) script = argv[++i];
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
  }
  FILE* in = script ? fopen(script, "r") : stdin;
  if (!in) { perror(script); return 1; }

  static sim::MockModel model;
  model.setClock(&g_now);
  static sim::SimDisplay display;
  static sim::SimKeyboard kb(&g_now);
  static ui::App app(model, display, kb);
  static MockWorld world(model);
#ifdef MC_BOARD_TDECK
  static sim::SimTouch touch(&g_now);
  app.setTouch(&touch);
  return sim::runScript(in, out, app, ui::screens::boot(), kb, display, g_now, world, &touch);
#else
  return sim::runScript(in, out, app, ui::screens::boot(), kb, display, g_now, world);
#endif
}
#endif
