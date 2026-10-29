// Diagnostics pages, the received-packet log, GPS status and heard adverts.
#include <time.h>
#include "ui/screens/screens.h"
#include "gen/mc_version.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace ui { namespace screens {

namespace {

// ------------------------------------------------------------- diagnostics
//
// Built as a list of items (meters and label/value pairs) that is laid out
// into rows for the current text size and split into pages, so every size
// shows everything: small fits it on two pages, larger sizes use more.

struct DiagItem {
  enum Kind : uint8_t { Meter, Pair, Line, Break } kind;
  const char* label;
#ifdef MC_BOARD_TDECK
  char value[44];
#else
  char value[28];
#endif
  int num, lo, hi;
  uint8_t col;
};

class DiagScreen : public Screen {
public:
  const char* title(App& a) override {
    // The header is drawn before the page: lay out once to know the count.
    _pages = layout(a, a.content(), nullptr);
    if (_page >= _pages) _page = _pages - 1;
    snprintf(_title, sizeof _title, "Diag: %s %d/%d", _names[_page], _page + 1, _pages);
    return _title;
  }
  bool tick(App&, uint32_t now) override {
    if (now - _last >= 1000) { _last = now; return true; }
    return false;
  }
  void draw(App& a, Canvas& c, const Rect& r) override { _pages = layout(a, r, &c); }

  // Lay the items out for this text size; draws the current page when `cp`
  // is given. Returns the number of pages.
  int layout(App& a, const Rect& r, Canvas* cp) {
    int n = build(a);
    _powerPage = 1 << 20;
    const mc_font_t* f = a.fSmall();
    const int cw = f->width, lh = f->height + 2;
    const int x0 = r.x + 4, w = r.w - 8;
    const int rows = (r.h - 6) / lh - 1;             // the first row of every page is the firmware version
    const int pairW = 9 * cw + 9 * cw;               // label + value
    const int cols = w >= 2 * pairW + 8 ? 2 : 1;

    // Lay items into rows, then rows into pages.
    struct Cell { int item, col; };
    static Cell cells[80];
    static int rowOf[80];
    int row = 0, colAt = 0, nc = 0;
    int pageStart[16] = {0}, pages = 1;
    // Each page is named for its section; one that runs over says so ("Radio (2)").
    const char* sec = "Radio";
    int sub = 1;
    auto name = [&](int p) {
      if (sub > 1) snprintf(_names[p], sizeof _names[p], "%s (%d)", sec, sub);
      else snprintf(_names[p], sizeof _names[p], "%s", sec);
    };
    name(0);
    for (int i = 0; i < n && nc < 80; i++) {
      const DiagItem& it = _items[i];
      if (it.kind == DiagItem::Break) {
        if (colAt) { row++; colAt = 0; }
        sec = it.label;
        sub = 1;
        if (row - pageStart[pages - 1] > 0 && pages < 16) pageStart[pages++] = row;
        name(pages - 1);
        continue;
      }
      bool full = it.kind != DiagItem::Pair;
      if (full && colAt) { row++; colAt = 0; }
      if (row - pageStart[pages - 1] >= rows && pages < 16) {
        pageStart[pages++] = row;
        sub++;
        name(pages - 1);
      }
      if (i == _powerItem) _powerPage = pages - 1;
      cells[nc] = {i, colAt};
      rowOf[nc++] = row;
      if (full || ++colAt >= cols) { row++; colAt = 0; }
    }
    if (!cp) return pages;
    Canvas& c = *cp;
    int page = _page < pages ? _page : pages - 1;
    int first = pageStart[page], last = page + 1 < pages ? pageStart[page + 1] : 1 << 20;

    // On every page, so a photo of any of them names the build. The label can be long
    // (1.2.0+19.9bf8937.dirty), hence the whole width.
    char fw[48];
    snprintf(fw, sizeof fw, "fw %s", MC_UI_VERSION);
    c.text(x0, r.y + 3, fw, f, C_DIM, -1, w);
    for (int k = 0; k < nc; k++) {
      if (rowOf[k] < first || rowOf[k] >= last) continue;
      const DiagItem& it = _items[cells[k].item];
      int y = r.y + 3 + (rowOf[k] - first + 1) * lh;
      if (it.kind == DiagItem::Meter) {
        int lw = 6 * cw, vw = 9 * cw;
        c.text(x0, y, it.label, f, C_DIM);
        int bx = x0 + lw, bw = w - lw - vw - 4;
        int bh = f->height / 2, by = y + (f->height - bh) / 2;
        c.fillRect(bx, by, bw, bh, C_ZEBRA);
        int v = it.num < it.lo ? it.lo : it.num > it.hi ? it.hi : it.num;
        c.fillRect(bx, by, it.hi > it.lo ? (v - it.lo) * bw / (it.hi - it.lo) : 0, bh, it.col);
        c.text(x0 + w - Canvas::textWidth(it.value, f), y, it.value, f, C_TEXT);
      } else if (it.kind == DiagItem::Pair) {
        int cx = x0 + cells[k].col * (w / cols);
        c.text(cx, y, it.label, f, C_DIM);
        c.text(cx + 9 * cw, y, it.value, f, it.col, -1, w / cols - 9 * cw - 2);
      } else {
        c.text(x0, y, it.value, f, it.col, -1, w);
      }
    }
    // Page dots.
    for (int p = 0; p < pages; p++)
      c.fillRect(r.x + r.w - 6 * pages - 2 + p * 6, r.y + r.h - 4, 4, 3, p == page ? C_ACCENT : C_ZEBRA);
    return pages;
  }
  void onKey(App& a, const Key& k) override {
    if (k.code == K_ESC) a.pop();
    else if (k.repeat) return;
    else if (k.code == K_ENTER && _page >= _powerPage)
      a.confirm("Reset the power counters?", [](App& app, int) { app.m().resetPower(); app.invalidate(); });
    else if ((k.code == K_RIGHT || k.code == K_TAB) && _page + 1 < _pages) _page++;
    else if (k.code == K_LEFT && _page > 0) _page--;
  }
  const Hint* hints(App&) override {
    static const Hint h[] = {{"<>", "page"}, {"esc", "back"}, {nullptr, nullptr}};
    static const Hint hp[] = {{"<>", "page"}, {"ent", "reset"}, {"esc", "back"}, {nullptr, nullptr}};
    return _page >= _powerPage ? hp : h;
  }

private:
  void meter(int& n, const char* label, int num, int lo, int hi, uint8_t col, const char* fmt, ...)
      __attribute__((format(printf, 8, 9))) {
    DiagItem& it = _items[n++];
    it = DiagItem{DiagItem::Meter, label, {}, num, lo, hi, col};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it.value, sizeof it.value, fmt, ap);
    va_end(ap);
  }
  void pair(int& n, const char* label, uint8_t col, const char* fmt, ...) __attribute__((format(printf, 5, 6))) {
    DiagItem& it = _items[n++];
    it = DiagItem{DiagItem::Pair, label, {}, 0, 0, 0, col};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it.value, sizeof it.value, fmt, ap);
    va_end(ap);
  }

  void line(int& n, uint8_t col, const char* fmt, ...) __attribute__((format(printf, 4, 5))) {
    DiagItem& it = _items[n++];
    it = DiagItem{DiagItem::Line, "", {}, 0, 0, 0, col};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it.value, sizeof it.value, fmt, ap);
    va_end(ap);
  }

  int build(App& a) {
    model::Diag d = a.m().diag();
    int n = 0;
    char snr[8];
    fmtSnr(snr, sizeof snr, d.lastSnr_x4);
    // Radio.
    meter(n, "noise", d.noiseFloor, -130, -80, d.noiseFloor > -95 ? C_ERR : d.noiseFloor > -110 ? C_WARN : C_OK,
          "%d dBm", d.noiseFloor);
    meter(n, "rssi", d.lastRssi, -130, -40, d.lastRssi > -100 ? C_OK : d.lastRssi > -115 ? C_WARN : C_ERR, "%d dBm",
          d.lastRssi);
    meter(n, "snr", d.lastSnr_x4, -20 * 4, 12 * 4, snrColor(d.lastSnr_x4), "%s dB", snr);
    meter(n, "duty", d.dutyPct, 0, 10, d.dutyPct >= 10 ? C_ERR : d.dutyPct >= 5 ? C_WARN : C_OK, "%u%%", d.dutyPct);
    pair(n, "rx", C_TEXT, "%lu", (unsigned long)d.rxPackets);
    pair(n, "tx", C_TEXT, "%lu", (unsigned long)d.txPackets);
    pair(n, "rx f/d", C_TEXT, "%lu/%lu", (unsigned long)d.rxFlood, (unsigned long)d.rxDirect);
    pair(n, "tx f/d", C_TEXT, "%lu/%lu", (unsigned long)d.txFlood, (unsigned long)d.txDirect);
    pair(n, "dups", d.dups ? C_WARN : C_TEXT, "%lu", (unsigned long)d.dups);
    pair(n, "errors", d.rxErrors ? C_ERR : C_TEXT, "%lu", (unsigned long)d.rxErrors);
    char t1[12], t2[12];
    fmtDur(t1, sizeof t1, d.airTxMs / 1000);
    fmtDur(t2, sizeof t2, d.airRxMs / 1000);
    pair(n, "air tx", C_TEXT, "%s", t1);
    pair(n, "air rx", C_TEXT, "%s", t2);
    pair(n, "queue", d.queueLen > 4 ? C_WARN : C_TEXT, "%u", d.queueLen);
    pair(n, "app q", C_TEXT, "%u", d.offlineQueue);
    model::Radio rd = a.m().radio();
    DiagItem& line = _items[n++];
    line = DiagItem{DiagItem::Line, "", {}, 0, 0, 0, C_ACCENT};
    snprintf(line.value, sizeof line.value, "%.3f/%g/SF%u/4:%u %ddBm", rd.freqMHz, rd.bwKHz, rd.sf, rd.cr, rd.txDbm);
    _items[n++] = DiagItem{DiagItem::Break, "System", {}, 0, 0, 0, 0};
    // System.
    meter(n, "heap", (int)(d.heapFree / 1024), 0, 160, d.heapFree < 20000 ? C_ERR : d.heapFree < 40000 ? C_WARN : C_OK,
          "%luK", (unsigned long)(d.heapFree / 1024));
    meter(n, "min", (int)(d.heapMin / 1024), 0, 160, d.heapMin < 15000 ? C_ERR : d.heapMin < 30000 ? C_WARN : C_OK,
          "%luK", (unsigned long)(d.heapMin / 1024));
    meter(n, "block", (int)(d.heapMaxBlock / 1024), 0, 128, d.heapMaxBlock < 8000 ? C_ERR : C_OK, "%luK",
          (unsigned long)(d.heapMaxBlock / 1024));
    meter(n, "batt", d.battPct, 0, 100, d.charging ? C_OK : d.battPct <= 15 ? C_ERR : d.battPct <= 35 ? C_WARN : C_OK,
          "%u.%02uV", d.battMv / 1000, d.battMv % 1000 / 10);
    fmtDur(t1, sizeof t1, d.uptimeS);
    pair(n, "uptime", C_TEXT, "%s", t1);
    pair(n, "battery", C_TEXT, "%u%%%s", d.battPct, d.charging ? " chg" : "");
    pair(n, "contacts", d.contacts * 10 >= d.contactsMax * 9 ? C_WARN : C_TEXT, "%u/%u", d.contacts, d.contactsMax);
    pair(n, "channels", C_TEXT, "%u/%u", d.channels, d.channelsMax);
    if (a.m().mode() == model::Mode::Remote) {
      // The radio, counters and contacts above are the node's; heap and
      // battery are this device's. The node's own battery:
      model::Link l = a.m().link();
      bool up = l.state == model::LinkState::Ready;
      if (up) pair(n, "node", l.synced ? C_OK : C_ACCENT, "%u.%02uV %ddBm", l.nodeBattMv / 1000, l.nodeBattMv % 1000 / 10, l.rssi);
      else pair(n, "node", C_WARN, "%s", l.saved ? "no link" : "none chosen");
    } else {
      pair(n, "ble", a.m().bleConnected() ? C_OK : C_TEXT, "%s",
           a.m().bleAvailable() ? (a.m().bleConnected() ? "connected" : a.m().prefs().bleEnabled ? "on" : "off") : "none");
    }
    uint8_t k[6];
    a.m().selfKey(k);
    pair(n, "node id", C_ACCENT, "%02X%02X%02X%02X", k[0], k[1], k[2], k[3]);
#ifdef MC_BOARD_TDECK
    storage(n, d);
#endif
#ifdef MC_BOARD_TDECK
    bluetooth(n, a, d.power);
#endif
    power(n, a, d.power);
    return n;
  }

#ifdef MC_BOARD_TDECK
  // The phone link: whether Bluetooth is on, and how the phones' connections have gone since boot,
  // for finding why a paired phone is sometimes not let back in.
  void bluetooth(int& n, App& a, const model::Power& p) {
    _items[n++] = DiagItem{DiagItem::Break, "Bluetooth", {}, 0, 0, 0, 0};
    bool remote = a.m().mode() == model::Mode::Remote;
    if (!a.m().bleAvailable()) line(n, C_DIM, "Power       n/a");
    else if (remote) line(n, C_ACCENT, "Power       node link");
    else if (!a.m().prefs().bleEnabled) line(n, C_DIM, "Power       off");
    else line(n, a.m().bleConnected() ? C_OK : C_ACCENT, "Power       %s", a.m().bleConnected() ? "on, connected" : "on");
    if (p.bleBonds < 0) return;
    line(n, C_TEXT, "Bonds       %d", p.bleBonds);
    line(n, C_TEXT, "Phone       %u conn  %u ok  %u fail", p.bleConnects, p.bleAuthOk, p.bleAuthFail);
    if (p.bleHist[0] || p.bleHist[2] || p.bleHist[4] || p.bleHist[6]) {
      char h[16] = {};
      for (int i = 0; i < 4; i++) {
        h[i * 3] = p.bleHist[i * 2] ? p.bleHist[i * 2] : '-';
        h[i * 3 + 1] = p.bleHist[i * 2 + 1] ? p.bleHist[i * 2 + 1] : '-';
        h[i * 3 + 2] = i < 3 ? ' ' : 0;
      }
      line(n, C_TEXT, "Recent      %s", h);
      line(n, C_DIM, "B/n known/new, then + paired - failed ? dropped");
    }
    if (p.bleAuthFail)
      line(n, C_WARN, "Last fail   enc %d  auth %d  bond %d", p.bleFailEnc, p.bleFailAuth, p.bleFailBond);
    if (p.bleConnects)
      line(n, p.blePeerBonded ? C_OK : C_WARN, "Last        addr %u/%u  %s", p.blePeerOta, p.blePeerId,
           p.blePeerBonded ? "known" : "not bonded");
  }
#endif

  // What the handheld's power management has done since the counters were reset.
  void power(int& n, App& a, const model::Power& p) {
    _items[n++] = DiagItem{DiagItem::Break, "Power", {}, 0, 0, 0, 0};
    _powerItem = n;
    char t[12], u[12];
    fmtDur(t, sizeof t, p.sinceS);
    line(n, C_TEXT, "Since reset %s", t);
    {   // the two radios the Phase 5 sleep work depends on
      const model::Prefs& pr = a.m().prefs();
      bool remote = a.m().mode() == model::Mode::Remote;
      if (!a.m().bleAvailable()) line(n, C_DIM, "Bluetooth   n/a");
      else if (remote) line(n, C_ACCENT, "Bluetooth   node link");
      else if (!pr.bleEnabled) line(n, C_DIM, "Bluetooth   off");
      else line(n, a.m().bleConnected() ? C_OK : C_ACCENT, "Bluetooth   %s", a.m().bleConnected() ? "connected" : "on");
      model::Gps g = a.m().gps();
      line(n, !g.enabled ? C_DIM : g.fix ? C_OK : C_WARN, "GPS         %s", !g.enabled ? "off" : g.fix ? "fix" : "searching");
    }
    if (!p.cpuMhz) line(n, C_DIM, "CPU         n/a");
    else line(n, C_TEXT, "CPU         %lu MHz", (unsigned long)p.cpuMhz);
    if (p.cpuMhz) {   // the idle clock: it should add up while the screen is off
      fmtDur(u, sizeof u, p.lowClockS);
      line(n, p.lowClockS ? C_OK : C_TEXT, "Idle clock  %s", u);
    }
    fmtDur(t, sizeof t, p.screenOffS);
    line(n, C_TEXT, "Screen off  %s", t);
    if (!p.sleepKnown) {
      line(n, C_DIM, "Sleep calls n/a");
      line(n, C_DIM, "Wakes n/a");
    } else {
      fmtDur(u, sizeof u, p.sleepS);
      if (p.screenOffS) line(n, C_TEXT, "Sleep calls %s  %u%%", u, (unsigned)(p.sleepS * 100ULL / p.screenOffS));
      else line(n, C_TEXT, "Sleep calls %s  -", u);
      line(n, C_TEXT, "Wakes %lu%s%s", (unsigned long)p.wakes, p.block[0] ? "   Block: " : "", p.block);
    }
    if (p.charging) line(n, C_OK, "Battery     charging");
    else if (!p.battMv) line(n, C_DIM, "Battery     no rested reading");
    else if (p.rateKnown) line(n, C_TEXT, "Battery %u mV  %+d mV/h", p.battMv, p.mvPerHour);
    else line(n, C_TEXT, "Battery %u mV  -", p.battMv);
  }

#ifdef MC_BOARD_TDECK
  // The message store: where the history lives and what has gone wrong with it.
  static void clock(uint32_t ts, char* out, int n) {
    if (!ts) { snprintf(out, n, "-"); return; }
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, n, "%H:%M", &tmv);
  }

  void storage(int& n, const model::Diag& d) {
    _items[n++] = DiagItem{DiagItem::Break, "Storage", {}, 0, 0, 0, 0};
    if (d.flashTotalKb)
      meter(n, "flash", (int)d.flashUsedKb, 0, (int)d.flashTotalKb,
            d.flashUsedKb * 10 >= d.flashTotalKb * 9 ? C_ERR : d.flashUsedKb * 4 >= d.flashTotalKb * 3 ? C_WARN : C_OK,
            "%lu/%luK", (unsigned long)d.flashUsedKb, (unsigned long)d.flashTotalKb);
    // The settings store the Bluetooth pairings are written to: full, they are not kept.
    if (d.power.nvsTotal)
      meter(n, "nvs", d.power.nvsUsed, 0, d.power.nvsTotal,
            d.power.nvsUsed * 10 >= d.power.nvsTotal * 9 ? C_ERR : d.power.nvsUsed * 4 >= d.power.nvsTotal * 3 ? C_WARN : C_OK,
            "%u/%u", d.power.nvsUsed, d.power.nvsTotal);
    if (d.sdTotalKb)
      meter(n, "sd", (int)(d.sdUsedKb / 1024), 0, (int)(d.sdTotalKb / 1024), C_OK, "%lu/%luM",
            (unsigned long)(d.sdUsedKb / 1024), (unsigned long)(d.sdTotalKb / 1024));
    else
      line(n, d.archiveOn ? C_WARN : C_DIM, "sd      %s", d.archiveOn ? "gone since boot" : "no card at boot: no archive");
    static const char* const kErr[] = {"none", "open", "grow", "write", "lost", "rebuild"};
    char t1[12], t2[12];
    clock(d.msgOkTs, t1, sizeof t1);
    clock(d.msgErrTs, t2, sizeof t2);
    line(n, C_TEXT, "ring %s %luK  saved #%lu  next #%lu", d.msgOnCard ? "sd" : "fl", (unsigned long)(d.msgRingBytes / 1024),
         (unsigned long)d.msgSeqFlash, (unsigned long)d.msgSeqNext);
    line(n, d.msgWriteFails ? C_ERR : C_TEXT, "writes  %lu ok  %lu failed", (unsigned long)d.msgWrites,
         (unsigned long)d.msgWriteFails);
    line(n, d.msgErr ? C_ERR : C_DIM, "last ok %s  last err %s (%s)", t1, t2, kErr[d.msgErr < 6 ? d.msgErr : 0]);
    line(n, d.archiveFails ? C_ERR : C_TEXT, "archive %lu ok  %lu failed", (unsigned long)d.archived,
         (unsigned long)d.archiveFails);
    if (d.winN)
      line(n, d.winNewest ? C_TEXT : C_WARN, "window  #%lu-#%lu (%u)%s", (unsigned long)d.winFirst,
           (unsigned long)d.winLast, d.winN, d.winNewest ? "" : " not newest");
    else
      line(n, C_DIM, "window  no chat open");
    if (d.winN) line(n, C_DIM, "chat's newest #%lu", (unsigned long)d.winThreadLast);
  }

#endif

#ifdef MC_BOARD_TDECK
  DiagItem _items[72];   // 56 used
#else
  DiagItem _items[40];   // 37 used
#endif
  int _page = 0, _pages = 1;
  int _powerItem = 0;               // the first Power item, and the page it lands on
  int _powerPage = 1 << 20;
  uint32_t _last = 0;
  char _title[40];
  char _names[16][14];   // each page's section
};

DiagScreen s_diag;


// ------------------------------------------------------------- packet log

const char* payloadName(uint8_t t) {
  static const char* const kNames[] = {"REQ", "RESP", "TXT", "ACK", "ADV", "GRP", "GDAT", "ANON",
                                       "PATH", "TRACE", "MULTI", "CTRL", "?", "?", "?", "RAW"};
  return kNames[t & 15];
}

uint8_t payloadColor(uint8_t t) {
  switch (t) {
    case 2: return C_CHAT;    // text
    case 3: return C_OK;      // ack
    case 4: return C_RPT;     // advert
    case 5: case 6: return C_CHAN;
    case 8: case 9: return C_ACCENT;
    default: return C_DIM;
  }
}

class LogScreen : public Screen {
public:
  const char* title(App& a) override {
    snprintf(_title, sizeof _title, "Packet log (%d)", a.m().rxLogCount());
    return _title;
  }
  bool tick(App& a, uint32_t) override {
    if (a.m().revision() != _rev) { _rev = a.m().revision(); return true; }
    return false;
  }
  void draw(App& a, Canvas& c, const Rect& r) override {
    const mc_font_t* f = a.fSmall();
    const int cw = f->width, lh = f->height + 1;
    // Columns on a character grid; the length column goes when it won't fit.
    bool withLen = 33 * cw <= r.w - 6;
    const int cAge = 0, cType = 4, cRt = 10, cHop = 12, cSnr = 15, cRssi = 21, cLen = 26, cSrc = withLen ? 30 : 26;
    auto at = [&](int col) { return r.x + 3 + col * cw; };
    c.fillRect(r.x, r.y, r.w, lh, C_ZEBRA);
    c.text(at(cAge), r.y, "age", f, C_ACCENT);
    c.text(at(cType), r.y, "type", f, C_ACCENT);
    c.text(at(cRt), r.y, "r", f, C_ACCENT);
    c.text(at(cHop), r.y, "hp", f, C_ACCENT);
    c.text(at(cSnr), r.y, "snr", f, C_ACCENT);
    c.text(at(cRssi), r.y, "rssi", f, C_ACCENT);
    if (withLen) c.text(at(cLen), r.y, "len", f, C_ACCENT);
    c.text(at(cSrc), r.y, "src", f, C_ACCENT);
    int n = a.m().rxLogCount();
    int vis = (r.h - lh) / lh;
    _vis = vis;
    _nav.clamp(n, vis);
    if (!n) c.text(r.x + 8, r.y + r.h / 2 - 6, "Nothing received yet", a.fMed(), C_DIM);
    for (int row = 0; row < vis && _nav.top + row < n; row++) {
      int i = _nav.top + row;
      model::RxLogEntry e;
      if (!a.m().rxLog(i, e)) continue;
      int y = r.y + lh + row * lh;
      a.hit({r.x, y, r.w, lh}, HitTarget::ROW, i);
      if (i == _nav.sel) c.fillRect(r.x, y, r.w - 3, lh, C_SEL);
      char ago[8], snr[8], l[16];
      fmtAge(ago, sizeof ago, (a.m().uptimeMs() - e.ms) / 1000);
      c.text(at(cAge), y, ago, f, C_DIM);
      c.text(at(cType), y, payloadName(e.payloadType), f, payloadColor(e.payloadType));
      c.text(at(cRt), y, e.routeType == 1 || e.routeType == 0 ? "F" : "D", f, C_TEXT);
      snprintf(l, sizeof l, "%u", e.pathLen);
      c.text(at(cHop), y, l, f, C_TEXT);
      fmtSnr(snr, sizeof snr, e.snr_x4);
      c.text(at(cSnr), y, snr, f, snrColor(e.snr_x4));
      snprintf(l, sizeof l, "%d", e.rssi);
      c.text(at(cRssi), y, l, f, C_TEXT);
      if (withLen) {
        snprintf(l, sizeof l, "%u", e.len);
        c.text(at(cLen), y, l, f, C_TEXT);
      }
      snprintf(l, sizeof l, "%02X%s", e.srcHash, e.dup ? "*" : "");
      c.text(at(cSrc), y, l, f, e.dup ? C_WARN : C_DIM);
    }
    Rect sr{r.x, r.y + lh, r.w, r.h - lh};
    drawScrollbar(c, sr, n, _nav.top, vis);
  }
  void onKey(App& a, const Key& k) override {
    if (k.code == K_ESC) a.pop();
    else _nav.key(k, a.m().rxLogCount(), _vis);
  }
  bool selectRow(App& a, int i) override {
    if (i >= 0 && i < a.m().rxLogCount()) _nav.sel = i;
    return false;   // nothing to open
  }
  const Hint* hints(App&) override {
    static const Hint h[] = {{"^v", "scroll"}, {"esc", "back"}, {nullptr, nullptr}};
    return h;
  }

private:
  ListNav _nav;
  int _vis = 8;
  uint32_t _rev = 0;
  char _title[24];
};

LogScreen s_log;

// ------------------------------------------------------------- GPS

class GpsScreen : public Screen {
public:
  const char* title(App&) override { return "GPS"; }
  bool tick(App&, uint32_t now) override {
    if (now - _last >= 1000) { _last = now; return true; }
    return false;
  }
  void draw(App& a, Canvas& c, const Rect& r) override {
    model::Gps g = a.m().gps();
    const mc_font_t* f = a.fMed();
    const int lh = f->height + 1;
    int x = r.x + 6, y = r.y + 3;
    c.bitmap(x, y + (f->height - 8) / 2, icon::gps, 8, 8, !g.enabled ? C_DIM : g.fix ? C_OK : C_WARN);
    c.text(x + 12, y, !g.enabled ? "Receiver off" : g.fix ? "3D fix" : "Searching...", f,
           !g.enabled ? C_DIM : g.fix ? C_OK : C_WARN);
    y += lh + 2;
    char l[48];
    snprintf(l, sizeof l, "Sats %u", g.sats);
    int bx = c.text(x, y, l, f, C_TEXT) + 6;
    int bw = (r.x + r.w - 6 - bx) / 12;
    for (int i = 0; i < 12 && bw >= 3; i++)
      c.fillRect(bx + i * bw, y + 2, bw - 2, f->height - 4, i < g.sats ? (g.sats >= 6 ? C_OK : C_WARN) : C_ZEBRA);
    y += lh;
    if (g.fix) {
      snprintf(l, sizeof l, "Lat %.5f", g.lat_e6 / 1e6);  c.text(x, y, l, f, C_TEXT); y += lh;
      snprintf(l, sizeof l, "Lon %.5f", g.lon_e6 / 1e6);  c.text(x, y, l, f, C_TEXT); y += lh;
      snprintf(l, sizeof l, "Alt %ld m  HDOP %u.%u", (long)(g.altCm / 100), g.hdop_x10 / 10, g.hdop_x10 % 10);
      c.text(x, y, l, f, C_TEXT, -1, r.w - 12);
      y += lh;
    } else {
      c.text(x, y, "Position unknown", f, C_DIM);
      y += lh;
    }
    const model::Prefs& p = a.m().prefs();
    snprintf(l, sizeof l, "adverts: %s  clock sync: %s", p.gpsShareInAdvert ? "on" : "off", p.gpsTimeSyncAtBoot ? "on" : "off");
    if (y + a.fSmall()->height <= r.y + r.h) c.text(x, y + 1, l, a.fSmall(), C_DIM, -1, r.w - 12);
  }
  void onKey(App& a, const Key& k) override {
    if (k.code == K_ESC) a.pop();
    else if (k.code == K_ENTER) {
      if (a.m().gpsSyncTime()) a.toast("Clock set from GPS", C_OK);
      else a.toast("No GPS fix yet", C_WARN);
    } else if (k.code == K_LEFT || k.code == K_RIGHT) {
      if (k.repeat) return;
      model::Prefs p = a.m().prefs();   // the Settings row's old path
      p.gpsEnabled = !p.gpsEnabled;
      a.m().setPrefs(p);
      a.applyPrefs();
    } else if (k.code == K_TAB) openSettings(a);
  }
  const Hint* hints(App&) override {
    static const Hint h[] = {{"<>", "on/off"}, {"ent", "set clock"}, {"tab", "settings"}, {"esc", "back"}, {nullptr, nullptr}};
    return h;
  }

private:
  uint32_t _last = 0;
};

GpsScreen s_gps;

// ------------------------------------------------------------- heard

class HeardScreen : public Screen {
public:
  bool repeatersOnly = false;
  const char* title(App& a) override {
    if (repeatersOnly) return a.m().discoverState() == model::Req::Busy ? "Discovering..." : "Repeaters nearby";
    return "Heard adverts";
  }
  bool tick(App& a, uint32_t) override {
    if (a.m().revision() != _rev) { _rev = a.m().revision(); rebuild(a); return true; }
    return false;
  }
  void onEnter(App& a) override { rebuild(a); }
  void draw(App& a, Canvas& c, const Rect& r) override {
    const int RH = a.mx().row;
    const mc_font_t* sm = a.fSmall();
    int vis = r.h / RH;
    _vis = vis;
    _nav.clamp(_n, vis);
    if (!_n) {
      const char* msg = repeatersOnly ? "Listening for repeaters..." : "No adverts heard yet";
      c.text(r.x + 8, r.y + (r.h - a.fMed()->height) / 2, msg, a.fMed(), C_DIM, -1, r.w - 16);
      return;
    }
    for (int row = 0; row < vis && _nav.top + row < _n; row++) {
      int i = _idx[_nav.top + row];
      model::Contact ct;
      if (!a.m().heard(i, ct)) continue;
      int y = r.y + row * RH;
      const int iy = y + (RH - 8) / 2, sy = y + (RH - sm->height) / 2;
      a.hit({r.x, y, r.w, RH}, HitTarget::ROW, _nav.top + row);
      if (_nav.top + row == _nav.sel) c.fillRect(r.x, y, r.w - 3, RH, C_SEL);
      else if (row & 1) c.fillRect(r.x, y, r.w - 3, RH, C_ZEBRA);
      uint8_t col = typeColor(ct.type);
      c.bitmap(r.x + 4, iy, typeIcon(ct.type), 8, 8, col);
      char snr[8], ago[8], l[24];
      fmtAgo(ago, sizeof ago, a.m().now(), ct.lastHeard);
      int x = r.x + r.w - 6 - Canvas::textWidth(ago, sm);
      c.text(x, sy, ago, sm, C_DIM);
      fmtSnr(snr, sizeof snr, ct.snr_x4);
      snprintf(l, sizeof l, "%s", snr);
      x -= Canvas::textWidth(l, sm) + 6;
      c.text(x, sy, l, sm, snrColor(ct.snr_x4));
      x -= 13;
      drawSignal(c, x, iy, ct.snr_x4);
      if (ct.hops >= 0) {
        snprintf(l, sizeof l, "%d", ct.hops);
        x -= Canvas::textWidth(l, sm) + 4;
        c.text(x, sy, l, sm, ct.hops == 0 ? C_OK : C_DIM);
      }
      a.fitText(c, r.x + 16, y + (RH - a.fList()->height) / 2, ct.name, a.fList(), C_TEXT, x - r.x - 22,
                _nav.top + row == _nav.sel);
    }
    drawScrollbar(c, r, _n, _nav.top, vis);
  }
  // A tap selects; a tap on the selected advert adds it.
  bool selectRow(App&, int i) override {
    if (i < 0 || i >= _n) return false;
    bool again = i == _nav.sel;
    _nav.sel = i;
    return again;
  }
  void onKey(App& a, const Key& k) override {
    if (k.code == K_ESC) { a.pop(); return; }
    if (_nav.key(k, _n, _vis)) return;
    if (k.code == K_ENTER && _n) {
      if (a.m().addHeard(_idx[_nav.sel])) a.toast("Added to contacts", C_OK);
      else a.toast("Already a contact, or contacts full", C_WARN);
    }
  }
  const Hint* hints(App&) override {
    static const Hint h[] = {{"ent", "add contact"}, {"^v", "move"}, {"esc", "back"}, {nullptr, nullptr}};
    return h;
  }

private:
  void rebuild(App& a) {
    _n = 0;
    for (int i = 0, n = a.m().heardCount(); i < n && _n < 64; i++) {
      model::Contact ct;
      if (!a.m().heard(i, ct)) continue;
      if (repeatersOnly && (ct.type != model::NodeType::Repeater || ct.hops != 0)) continue;
      _idx[_n++] = (uint8_t)i;
    }
  }
  ListNav _nav;
  int _vis = 6;
  uint8_t _idx[64];
  int _n = 0;
  uint32_t _rev = 0;
};

HeardScreen s_heard;

}  // namespace

void openDiag(App& a) { a.push(&s_diag); }
void openPacketLog(App& a) { a.push(&s_log); }
void openGps(App& a) { a.push(&s_gps); }
void openHeard(App& a, bool rptOnly) {
  s_heard.repeatersOnly = rptOnly;
  a.push(&s_heard);
}

}}  // namespace ui::screens
