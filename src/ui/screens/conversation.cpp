// A conversation (DM, room or channel): messages bottom-anchored like a chat
// app, a meta line per message (sender, time, delivery or signal), and a
// compose box. Up/Down scroll; Enter or typing starts composing. A "New" line
// marks where the messages that came in since the last visit begin.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "ui/thread.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace ui { namespace screens {

namespace {

constexpr int GAP = 3;

struct DrawCtx {
  Canvas* c;
  const mc_font_t* f;
  int x, y;
  uint8_t col;
  bool mention;     // the previous line ended inside a @[mention]
};

void drawLine(void* p, const char* s, int len, int line) {
  DrawCtx* d = (DrawCtx*)p;
  char tmp[model::TEXT_LEN];
  if (len >= (int)sizeof tmp) len = sizeof tmp - 1;
  memcpy(tmp, s, len);
  tmp[len] = 0;
  d->c->text(d->x, d->y + line * d->f->height, tmp, d->f, d->col, -1, 0, &d->mention);
}

// A message's height in a list: the meta line, its text, a gap.
int messageH(App& a, const model::Message& m, const mc_font_t* f, int textW) {
  return a.mx().meta + wrapText(m.text, f, textW, nullptr, nullptr) * f->height + GAP;
}

// A message answers touches, within the area it is drawn in (it may be cut at either end).
void hitMessage(App& a, const Rect& area, int top, int h, int index) {
  int y0 = top < area.y ? area.y : top, y1 = top + h > area.y + area.h ? area.y + area.h : top + h;
  if (y1 > y0) a.hit({area.x, y0, area.w, y1 - y0}, HitTarget::ROW, index);
}

void drawMessage(App& a, Canvas& c, const model::Message& m, bool chan, bool sel, const Rect& r, int top,
                const mc_font_t* f, int textW) {
  uint8_t bar = m.outgoing ? C_ACCENT : chan ? C_CHAN : C_CHAT;
  int lines = wrapText(m.text, f, textW, nullptr, nullptr);
  int h = a.mx().meta + lines * f->height;
  if (sel) c.fillRect(r.x + 4, top, r.w - 7, h + 1, C_SEL);
  else if (m.outgoing) c.fillRect(r.x + 4, top, r.w - 7, h + 1, C_ZEBRA);
  c.fillRect(r.x + 4, top, 2, h + 1, bar);

  // Meta line: who, when, then delivery state or signal.
  const mc_font_t* sf = a.fSmall();
  char t[16];
  fmtStamp(t, sizeof t, a.m().now(), m.ts);
  int x = r.x + 9;
  if (m.outgoing) {
    c.text(x + 1, top + 1, "me", sf, C_ACCENT);
    x = c.text(x, top + 1, "me", sf, C_ACCENT) + 5;
  } else if (m.sender[0]) {   // channels and rooms carry the author
    c.text(x + 1, top + 1, m.sender, sf, C_CHAT, -1, 100);   // twice, a pixel apart: bold, so a
    x = c.text(x, top + 1, m.sender, sf, C_CHAT, -1, 100) + 5;   // mention in the text doesn't outshine it
  }
  x = c.text(x, top + 1, t, sf, C_DIM) + 5;
  if (m.outgoing) {
    const uint8_t* ic = icon::clock;
    uint8_t col = C_WARN;
    const char* label = "sending";
    switch (m.status) {
      case model::Delivery::Pending: break;
      case model::Delivery::Sent: ic = icon::check; col = C_DIM; label = chan ? "sent" : "sent, no ack"; break;
      case model::Delivery::Delivered: ic = icon::check; col = C_OK; label = "delivered"; break;
      case model::Delivery::Failed: ic = icon::cross; col = C_ERR; label = "failed"; break;
      default: ic = nullptr; label = ""; break;
    }
    if (ic) c.bitmap(x, top + (sf->height + 1 - 8) / 2, ic, 8, 8, col);
    x = c.text(x + 10, top + 1, label, sf, col) + 4;
    if (chan && m.repeats) {
      char rp[16];
      snprintf(rp, sizeof rp, "heard %u repeat%s", m.repeats, m.repeats == 1 ? "" : "s");
      c.text(x, top + 1, rp, sf, C_OK);
    } else if (m.retries) {
      char rt[12];
      snprintf(rt, sizeof rt, "retry %u", m.retries);
      c.text(x, top + 1, rt, sf, C_WARN);
    }
  } else {
    char s[24];
    if (m.hops >= 0) {
      snprintf(s, sizeof s, m.hops == 0 ? "direct" : "%d hop%s", m.hops, m.hops == 1 ? "" : "s");
      x = c.text(x, top + 1, s, sf, m.hops == 0 ? C_OK : C_DIM) + 5;
    }
    if (m.snr_x4 != INT8_MIN) {
      char snr[8];
      fmtSnr(snr, sizeof snr, m.snr_x4);
      snprintf(s, sizeof s, "%sdB", snr);
      c.text(x, top + 1, s, sf, snrColor(m.snr_x4));
    }
  }

  DrawCtx d{&c, f, r.x + 9, top + a.mx().meta, C_TEXT, false};
  wrapText(m.text, f, textW, drawLine, &d);
}

// The thread screen, below ConversationScreen.
void showThread(App& a, model::ThreadId id, const int* idx, int n);

class ConversationScreen : public Screen {
public:
  model::ThreadId id;

  const char* title(App& a) override {
    if (id.kind == model::ThreadKind::Channel) {
      model::Channel ch;
      for (int i = 0, n = a.m().channelCount(); i < n; i++)
        if (a.m().channel(i, ch) && ch.idx == id.idx) { snprintf(_title, sizeof _title, "%s", ch.name); return _title; }
    } else {
      model::Contact ct;
      if (a.m().contact(id.idx, ct)) { snprintf(_title, sizeof _title, "%s", ct.name); return _title; }
    }
    return "Chat";
  }

  void onEnter(App& a) override {
    a.m().markRead(id);
    _readRev = a.m().revision();
    // A room shows its posts only after a login: do it once per opening,
    // with the saved password if one worked before, else ask.
    if (_loginTried || id.kind != model::ThreadKind::Direct) return;
    model::Contact ct;
    if (!a.m().contact(id.idx, ct) || ct.type != model::NodeType::Room || a.m().loggedIn(id.idx)) return;
    _loginTried = true;
    if (a.m().hasSavedPassword(id.idx)) {
      if (a.m().loginSaved(id.idx)) { _loggingIn = true; a.toast("Logging in to the room...", C_ACCENT); }
    } else {
      a.prompt("Room password (blank: guest)", "", 15, [](App& app, int ci) {
        if (app.m().login(ci, app.promptText())) {
          s_roomLoginStarted();
          app.toast("Logging in to the room...", C_ACCENT);
        }
      }, id.idx, true);
    }
  }

  bool textMode(App&) override { return _composing; }

  bool tick(App& a, uint32_t now) override {
    if (_loggingIn && a.m().requestState() != model::Req::Busy) {
      _loggingIn = false;
      bool ok = a.m().requestState() == model::Req::Ok;
      a.toast(ok ? "Logged in: posts will arrive" : a.m().requestText(), ok ? C_OK : C_ERR);
      return true;
    }
    // New messages arriving while open are read; keep the view at the bottom
    // unless the user scrolled up.
    if (a.m().revision() != _readRev) {
      a.m().markRead(id);
      _readRev = a.m().revision();
      return true;
    }
    // Blink the caret.
    if (_composing && (now / 500) != _blink) { _blink = now / 500; return true; }
    return false;
  }

  void draw(App& a, Canvas& c, const Rect& r) override {
    Rect mr = r;
    if (_composing) mr.h -= composeH(a);
    int n = a.m().messageCount(id);
    const mc_font_t* f = a.fBody();
    int textW = mr.w - 14;

    if (n == 0) {
      const char* msg = "No messages yet. Press Enter to write.";
      DrawCtx d{&c, a.fMed(), r.x + 10, mr.y + 30, C_DIM, false};
      wrapText(msg, a.fMed(), r.w - 20, drawLine, &d);
    }

    // After sliding the window, scroll so reading continues where it was:
    // loaded newer messages wait below the one that was last; loaded older
    // ones end at the bottom of the view, right above the one that was first.
    if (_keepBottom > 0 || _showOlder > 0) {
      int hidden = 0, from = _keepBottom > 0 ? n - _keepBottom : _showOlder;
      for (int i = n - 1; i >= from && i >= 0; i--) {
        model::Message m;
        if (a.m().message(id, i, m)) hidden += rowH(a, m, f, textW);
      }
      _scroll = hidden;
      _keepBottom = _showOlder = 0;
    }
    // Just opened with new messages: start with the "New" line at the top
    // (or as high as it goes).
    if (_toDivider) {
      _toDivider = false;
      int below = 0;
      for (int i = n - 1; i >= 0; i--) {
        model::Message m;
        if (!a.m().message(id, i, m)) continue;
        below += rowH(a, m, f, textW);
        if (_divSeq && m.seq == _divSeq) { _scroll = below > mr.h ? below - mr.h : 0; break; }
      }
    }
    // Walk from the newest message up. _scroll counts pixels hidden below.
    int bottom = mr.y + mr.h + _scroll;
    int total = 0;
    int selTop = 0, selBottom = 0;
    _lowSeq = 0;
    for (int i = n - 1; i >= 0; i--) {
      model::Message m;
      if (!a.m().message(id, i, m)) continue;
      int h = rowH(a, m, f, textW);
      total += h;
      int top = bottom - h;
      if (_sel && m.seq == _selSeq) { selTop = top; selBottom = bottom; }
      if (top < mr.y + mr.h && bottom > mr.y) {
        if (!_lowSeq) _lowSeq = m.seq;
        if (_divSeq && m.seq == _divSeq) {
          drawDivider(a, c, mr, top);
          top += divH(a);
        }
        bool sel = _sel && m.seq == _selSeq;
        drawMessage(a, c, m, id.kind == model::ThreadKind::Channel, sel, mr, top, f, textW);
        hitMessage(a, mr, top, h, i);
      }
      bottom = bottom - h;
    }
    _maxScroll = total > mr.h ? total - mr.h : 0;
    if (_scroll > _maxScroll) _scroll = _maxScroll;
    if (_sel && _ensureSel && selBottom) {
      // Bring the selected message into view (its top, if it is taller than the view).
      if (selTop < mr.y) _scroll += mr.y - selTop;
      else if (selBottom > mr.y + mr.h) _scroll -= selBottom - (mr.y + mr.h);
      if (selBottom - selTop > mr.h) _scroll += mr.y - selTop;
      if (_scroll < 0) _scroll = 0;
      if (_scroll > _maxScroll) _scroll = _maxScroll;
      _ensureSel = false;
      a.invalidate();
    }
    if (_scroll > 0 || !a.m().atNewest(id)) {
      // More below: in this window, or newer messages not loaded yet.
      c.fillRect(mr.x + mr.w / 2 - 12, mr.y + mr.h - 4, 24, 3, C_ACCENT);
    }
    drawScrollbar(c, mr, total, total - mr.h - _scroll, mr.h);

    if (_composing) drawCompose(a, c, r);
  }

  void onKey(App& a, const Key& k) override {
    if (_composing) { composeKey(a, k); return; }
    if (_sel && selectKey(a, k)) return;
    Rect r = a.content();
    switch (k.code) {
      case K_ESC: a.pop(); return;
      case K_RIGHT:   // pick a message: reply, details, thread
        if (_lowSeq && !k.repeat) { _sel = true; _selSeq = _lowSeq; _ensureSel = true; a.invalidate(); }
        return;
      case K_UP:
        if (_scroll >= _maxScroll && !k.repeat) {
          // At the top of what's loaded: bring in older messages.
          int n = a.m().loadOlder(id);
          if (n) _showOlder = n;
          else a.toast("Start of history", C_DIM);
          return;
        }
        _scroll += 12;
        if (_scroll > _maxScroll) _scroll = _maxScroll;
        return;
      case K_DOWN:
        if (_scroll == 0 && !k.repeat && !a.m().atNewest(id)) {
          // At the bottom of an older window: bring in newer messages and
          // keep the current last message in view.
          int n = a.m().loadNewer(id);
          _keepBottom = n;
          return;
        }
        _scroll -= 12;
        if (_scroll < 0) _scroll = 0;
        return;
      case K_PGUP: _scroll += r.h - 20; if (_scroll > _maxScroll) _scroll = _maxScroll; return;
      case K_PGDN: _scroll -= r.h - 20; if (_scroll < 0) _scroll = 0; return;
      case K_HALF_UP: _scroll += r.h / 2; if (_scroll > _maxScroll) _scroll = _maxScroll; return;
      case K_HALF_DN: _scroll -= r.h / 2; if (_scroll < 0) _scroll = 0; return;
      case K_END: _scroll = 0; return;
      case K_HOME: _scroll = _maxScroll; return;
      case K_TAB:
        if (id.kind == model::ThreadKind::Channel) openChannel(a, id.idx);
        else openContact(a, id.idx);
        return;
      case K_ENTER: case K_COMPOSE: startCompose(a); return;
      case K_SEND:
        // ZZ: send the draft left when writing stopped.
        if (!_draft[0]) { a.toast("No draft to send", C_DIM); return; }
        startCompose(a);
        composeKey(a, Key{K_ENTER, 0, false, 0});
        return;
      case K_DISCARD:
        if (!_draft[0]) return;
        _draft[0] = 0;
        _ed.attach(_draft, maxLen(a) + 1);
        a.toast("Draft discarded", C_DIM);
        return;
    }
    // Typing starts a message, except with vim keys, where letters are
    // commands and i, a or o start one.
    if (k.isChar() && !a.vimActive()) {
      startCompose(a);
      composeKey(a, k);
    }
  }

  // Dragging scrolls by the pixel, in the direction App::scrollDy gives (down is
  // toward the newest, as in lists); past the oldest loaded, older ones come in
  // (at most twice a second).
  void onTouch(App& a, const Gesture& g, const HitTarget* t) override {
    if (g.type == Gesture::SWIPE) return;
    if ((g.type == Gesture::TAP || g.type == Gesture::LONG) && !_composing && t && t->kind == HitTarget::ROW) {
      // A message: tap picks it, tapping the picked one (or holding) shows its details.
      model::Message m;
      if (!a.m().message(id, t->arg, m)) return;
      bool again = _sel && _selSeq == m.seq;
      _sel = true;
      _selSeq = m.seq;
      a.invalidate();
      if (again || g.type == Gesture::LONG) showDetails(a, m);
      return;
    }
    if (g.type != Gesture::DRAG) { Screen::onTouch(a, g, t); return; }
    const int dy = a.scrollDy(g.dy);   // positive: toward the newest, as in lists
    _scroll -= dy;
    if (_scroll > _maxScroll) {
      _scroll = _maxScroll;
      if (dy < 0 && a.nowMs() - _lastLoad > 500) {
        _lastLoad = a.nowMs();
        int n = a.m().loadOlder(id);
        if (n) _showOlder = n;
      }
    }
    if (_scroll < 0) {
      _scroll = 0;
      if (dy > 0 && !a.m().atNewest(id) && a.nowMs() - _lastLoad > 500) {
        _lastLoad = a.nowMs();
        _keepBottom = a.m().loadNewer(id);
      }
    }
  }

  const Hint* hints(App& a) override {
    static const Hint view[] = {{"ent", "write"}, {"^v", "scroll"}, {"tab", "info"}, {"esc", "back"}, {nullptr, nullptr}};
    static const Hint vim[] = {{"i", "write"}, {"^v", "scroll"}, {"tab", "info"}, {"q", "back"}, {nullptr, nullptr}};
    static const Hint draft[] = {{"i", "write"}, {"ZZ", "send"}, {"ZQ", "discard"}, {"q", "back"}, {nullptr, nullptr}};
    static const Hint pick[] = {{"^v", "move"}, {"ent", "details"}, {"r", "reply"}, {"t", "thread"}, {"esc", "done"}, {nullptr, nullptr}};
    if (_sel && !_composing) return pick;
    if (!_composing && a.vimActive()) return _draft[0] ? draft : vim;
    static const Hint comp[] = {{"ent", "send"}, {"tab", "name"}, {"esc", "stop"}, {"fn+,/", "cursor"}, {nullptr, nullptr}};
    return _composing ? comp : view;
  }

  // Where the new messages begin, as the chat opens (before it is marked
  // read). The line stays until the chat is left.
  void markNew(App& a) {
    _divSeq = 0;
    _toDivider = false;
    uint32_t readSeq;
    if (!a.m().unreadSince(id, readSeq)) return;
    // Bring in older messages until the window reaches the last one read,
    // within reason: a long backlog starts at the oldest of these.
    for (int page = 0; page < 4; page++) {
      model::Message m;
      if (!a.m().message(id, 0, m) || m.seq <= readSeq || !a.m().loadOlder(id)) break;
    }
    for (int i = 0, n = a.m().messageCount(id); i < n; i++) {
      model::Message m;
      if (a.m().message(id, i, m) && m.seq > readSeq) { _divSeq = m.seq; break; }
    }
    _toDivider = _divSeq != 0;
    _scroll = 0;
  }

  int newCount(App& a) {
    if (!_divSeq) return -1;
    int got = 0;
    for (int i = 0, n = a.m().messageCount(id); i < n; i++) {
      model::Message m;
      if (a.m().message(id, i, m) && m.seq >= _divSeq) got++;
    }
    return got;
  }

  void reset() {
    _divSeq = 0;
    _toDivider = false;
    _scroll = 0;
    _composing = false;
    _sel = false;
    _draft[0] = 0;
    _loginTried = false;
    _loggingIn = false;
  }
  static void s_roomLoginStarted();
  void _loginTriedReset() { _loginTried = false; }
  bool _loggingIn = false;

  // Jump to a message of this conversation and pick it (from the thread screen).
  void focus(uint32_t seq) {
    _sel = true;
    _selSeq = seq;
    _ensureSel = true;
  }

  void startCompose(App& a) {
    _sel = false;
    _composing = true;
    _scroll = 0;
    _ed.attach(_draft, maxLen(a) + 1);
  }

private:
  // Two lines of message text plus a margin.
  static int composeH(App& a) { return 2 * a.fBody()->height + 6; }

  int maxLen(App& a) {
    // Channel messages carry "name: " inside the payload.
    int lim = model::TEXT_LEN - 1;
    if (id.kind == model::ThreadKind::Channel) lim -= (int)strlen(a.m().prefs().nodeName) + 2;
    return lim;
  }

  // Picking a message: Up/Down move, Enter shows its details, r replies to its
  // sender, t follows the mentions back into a thread; Esc or Left puts it down.
  // Anything else goes on as usual (typing starts a message).
  bool selectKey(App& a, const Key& k) {
    int n = a.m().messageCount(id), at = -1;
    model::Message m;
    for (int i = 0; i < n; i++)
      if (a.m().message(id, i, m) && m.seq == _selSeq) { at = i; break; }
    if (at < 0) { _sel = false; return false; }   // gone from the window
    if (k.code == K_ESC || k.code == K_LEFT) { _sel = false; a.invalidate(); return true; }
    if (k.code == K_UP || k.code == K_DOWN) {
      int to = at + (k.code == K_UP ? -1 : 1);
      if (to < 0) {
        int got = a.m().loadOlder(id);
        if (!got) { a.toast("Start of history", C_DIM); return true; }
        to = got - 1;   // the message just above the old first one
      } else if (to >= n) {
        if (a.m().atNewest(id)) return true;
        int got = a.m().loadNewer(id);
        if (!got) return true;
        to = n;
      }
      model::Message nm;
      if (a.m().message(id, to, nm)) { _selSeq = nm.seq; _ensureSel = true; a.invalidate(); }
      return true;
    }
    if (!a.m().message(id, at, m)) return true;
    if (k.code == K_ENTER) { showDetails(a, m); return true; }
    if (k.code == 'r' && k.isChar()) { reply(a, m); return true; }
    if (k.code == 't' && k.isChar()) { openThreadFor(a, m.seq); return true; }
    if (k.code == K_TAB || k.code == K_PGUP || k.code == K_PGDN || k.code == K_HALF_UP || k.code == K_HALF_DN) return false;
    _sel = false;   // anything else: back to reading and writing
    return false;
  }

  // Start a message to the sender of `m`: @[Name] first.
  void reply(App& a, const model::Message& m) {
    if (m.outgoing || !m.sender[0]) { a.toast("Nobody to reply to here", C_DIM); return; }
    char mention[model::NAME_LEN + 6];
    snprintf(mention, sizeof mention, "@[%s] ", m.sender);
    startCompose(a);
    int len = (int)strlen(_draft), add = (int)strlen(mention);
    if (len && _draft[len - 1] != ' ') { _draft[len++] = ' '; _draft[len] = 0; }
    if (len + add > maxLen(a)) { a.toast("Draft too long for a reply", C_WARN); return; }
    memcpy(_draft + len, mention, add + 1);
    _ed.attach(_draft, maxLen(a) + 1);
  }

  // Everything known about a message, on a scrolling page.
  void showDetails(App& a, const model::Message& m) {
    static char body[360];
    int o = 0;
    char stamp[24], t[16];
    fmtStamp(stamp, sizeof stamp, a.m().now(), m.ts);
    snprintf(t, sizeof t, "%s", m.outgoing ? "me" : m.sender[0] ? m.sender : "");
    o += snprintf(body + o, sizeof body - o, "%s%s%s\n", t, t[0] ? " " : "", stamp);
    if (m.outgoing) {
      static const char* const kStatus[] = {"", "sending", "sent", "delivered", "failed"};
      o += snprintf(body + o, sizeof body - o, "%s", kStatus[(int)m.status % 5]);
      if (m.retries) o += snprintf(body + o, sizeof body - o, ", %u retr%s", m.retries, m.retries == 1 ? "y" : "ies");
      if (m.repeats) o += snprintf(body + o, sizeof body - o, ", heard %u repeat%s", m.repeats, m.repeats == 1 ? "" : "s");
      o += snprintf(body + o, sizeof body - o, "\n");
    } else {
      if (m.hops < 0) o += snprintf(body + o, sizeof body - o, "Route: direct\n");
      else if (m.hops == 0) o += snprintf(body + o, sizeof body - o, "Route: heard direct\n");
      else o += snprintf(body + o, sizeof body - o, "Route: %d hop%s\n", m.hops, m.hops == 1 ? "" : "s");
      if (m.pathSize && m.pathN) {
        o += snprintf(body + o, sizeof body - o, "Via:");
        for (int i = 0; i < m.pathN && o < (int)sizeof body - 40; i++) {
          const uint8_t* h = m.path + i * m.pathSize;
          char name[model::NAME_LEN];
          if (a.m().relayName(h, m.pathSize, name, sizeof name)) {
            o += snprintf(body + o, sizeof body - o, " %s", name);
          } else {
            o += snprintf(body + o, sizeof body - o, " ");
            for (int b = 0; b < m.pathSize; b++) o += snprintf(body + o, sizeof body - o, "%02x", h[b]);
          }
          if (i + 1 < m.pathN) o += snprintf(body + o, sizeof body - o, " >");
        }
        if (m.hops > m.pathN) o += snprintf(body + o, sizeof body - o, " ... (+%d)", m.hops - m.pathN);
        o += snprintf(body + o, sizeof body - o, "\n");
      }
      if (m.snr_x4 != INT8_MIN) {
        char snr[8];
        fmtSnr(snr, sizeof snr, m.snr_x4);
        o += snprintf(body + o, sizeof body - o, "Signal: %s dB SNR\n", snr);
      }
    }
    o += snprintf(body + o, sizeof body - o, "\n%s", m.text);
    openText(a, "Message", body);
  }

  struct Fetch { App* a; model::ThreadId id; };

  static bool fetchAt(void* ctx, int i, model::Message& m) {
    Fetch* f = (Fetch*)ctx;
    return f->a->m().message(f->id, i, m);
  }

  // The thread the message answers: follow the @mentions back (see ui/thread.h).
  void openThreadFor(App& a, uint32_t seq) {
    // Older messages may hold the start of it: bring a couple of pages in.
    for (int page = 0; page < 2; page++) {
      model::Message first;
      if (!a.m().message(id, 0, first) || !a.m().loadOlder(id)) break;
    }
    Fetch f{&a, id};
    int n = a.m().messageCount(id), at = -1;
    for (int i = 0; i < n && at < 0; i++) {
      model::Message m;
      if (a.m().message(id, i, m) && m.seq == seq) at = i;
    }
    int idx[THREAD_MAX];
    int cnt = at < 0 ? 0 : buildThread(fetchAt, &f, n, at, a.m().prefs().nodeName, idx, THREAD_MAX);
    if (cnt < 2) { a.toast("Mentions no one earlier", C_DIM); return; }
    showThread(a, id, idx, cnt);
  }

  // Tab in a message: finish the name after an @ (as @[Name] ), from those who wrote in this
  // chat and from the contacts; Tab again for the next one.
  void completeName(App& a) {
    const char* t = _ed.text();
    int cur = _ed.cursor(), from = _tabFrom;
    char partial[model::NAME_LEN];
    if (from >= 0) {
      snprintf(partial, sizeof partial, "%s", _tabPartial);
    } else {
      int j = cur - 1;
      while (j >= 0 && cur - j < (int)sizeof partial && t[j] != '@' && t[j] != ']' && t[j] != '\n') j--;
      if (j < 0 || t[j] != '@') { a.toast("Type @ and the start of a name, then Tab", C_DIM); return; }
      int start = j + 1;
      bool bracket = t[start] == '[' && start < cur;
      if (bracket) start++;
      snprintf(partial, sizeof partial, "%.*s", cur - start, t + start);
      if (!bracket && strchr(partial, ' ')) { a.toast("Type @ and the start of a name, then Tab", C_DIM); return; }
      from = j;
    }
    char names[12][model::NAME_LEN];
    int n = 0;
    auto offer = [&](const char* nm) {
      if (!nm[0] || n == 12 || !strcasecmp(nm, a.m().prefs().nodeName)) return;
      for (int i = 0; i < n; i++)
        if (!strcasecmp(names[i], nm)) return;
      snprintf(names[n++], model::NAME_LEN, "%s", nm);
    };
    // Prefix matches first (those who wrote lately before the contacts), then any part of a name.
    for (int pass = 0; pass < 2 && !n; pass++) {
      auto fits = [&](const char* nm) {
        if (!partial[0]) return true;
        if (pass == 0) return strncasecmp(nm, partial, strlen(partial)) == 0;
        return strcasestr(nm, partial) != nullptr;
      };
      for (int i = a.m().messageCount(id) - 1; i >= 0; i--) {
        model::Message m;
        if (a.m().message(id, i, m) && !m.outgoing && fits(m.sender)) offer(m.sender);
      }
      for (int i = 0, nc = a.m().contactCount(); i < nc; i++) {
        model::Contact ct;
        if (a.m().contact(i, ct) && fits(ct.name)) offer(ct.name);
      }
    }
    if (!n) { a.toast("No such name", C_DIM); return; }
    int idx = _tabFrom >= 0 ? (_tabIdx + 1) % n : 0;
    char text[model::NAME_LEN + 5];
    snprintf(text, sizeof text, "@[%s] ", names[idx]);
    if (!_ed.replaceBefore(from, text)) { a.toast("No room for that name", C_WARN); return; }
    _tabFrom = from;
    _tabIdx = idx;
    snprintf(_tabPartial, sizeof _tabPartial, "%s", partial);
  }

  void composeKey(App& a, const Key& k) {
    if (k.code == K_TAB) { completeName(a); return; }
    _tabFrom = -1;   // any other key ends a round of completions
    if (k.code == K_ESC) { _composing = false; return; }
    if (k.code == K_ENTER) {
      if (!_draft[0]) { _composing = false; return; }
      if (a.m().send(id, _draft)) {
        _draft[0] = 0;
        _ed.attach(_draft, maxLen(a) + 1);
        _composing = false;
        _scroll = 0;
      } else if (a.m().mode() == model::Mode::Remote && !a.m().link().synced) {
        a.toast("Node not connected: try later", C_WARN);   // the draft stays
      } else {
        a.toast("Send failed: no path or queue full", C_ERR);
      }
      return;
    }
    if (k.code == K_UP || k.code == K_DOWN) return;
    _ed.key(k);
  }

  int divH(App& a) const { return a.fSmall()->height + 4; }

  // A message's height in the list, with the "New" line if it sits above it.
  int rowH(App& a, const model::Message& m, const mc_font_t* f, int textW) const {
    int h = messageH(a, m, f, textW);
    return m.seq == _divSeq && _divSeq ? h + divH(a) : h;
  }

  void drawDivider(App& a, Canvas& c, const Rect& r, int top) {
    const mc_font_t* sf = a.fSmall();
    int y = top + divH(a) / 2 - 1;
    int x = r.x + 14;
    c.hline(r.x + 4, y, x - r.x - 7, C_BADGE);
    x = c.text(x, top + 1, "New", sf, C_BADGE) + 3;
    c.hline(x, y, r.x + r.w - 4 - x, C_BADGE);
  }

  void drawCompose(App& a, Canvas& c, const Rect& r) {
    const int CH = composeH(a);
    int y = r.y + r.h - CH;
    c.fillRect(r.x, y, r.w, CH, C_ZEBRA);
    c.hline(r.x, y, r.w, C_ACCENT);
    Rect er{r.x + 4, y + 3, r.w - 8, CH - 4};
    _ed.drawWrapped(a, c, er, a.fBody(), C_TEXT);
    char cnt[12];
    int len = _ed.length(), lim = maxLen(a);
    snprintf(cnt, sizeof cnt, "%d", lim - len);
    // On a tab above the box, so it never covers the text.
    const mc_font_t* sf = a.fSmall();
    int w = Canvas::textWidth(cnt, sf);
    c.fillRect(r.x + r.w - w - 7, y - sf->height - 1, w + 7, sf->height + 1, C_ZEBRA);
    c.vline(r.x + r.w - w - 7, y - sf->height - 1, sf->height + 1, C_ACCENT);
    c.hline(r.x + r.w - w - 7, y - sf->height - 1, w + 7, C_ACCENT);
    c.text(r.x + r.w - w - 3, y - sf->height, cnt, sf, lim - len < 20 ? C_WARN : C_DIM);
  }

  char _title[model::NAME_LEN];
  char _draft[model::TEXT_LEN] = {};
  LineEditor _ed;
  bool _composing = false;
  bool _loginTried = false;
  int _keepBottom = 0, _showOlder = 0;   // messages just loaded below / above
  int _scroll = 0, _maxScroll = 0;
  uint32_t _lastLoad = 0;                // a drag last brought in messages
  uint32_t _readRev = 0, _blink = 0;
  uint32_t _divSeq = 0;                  // the first new message (0: no "New" line)
  bool _toDivider = false;               // scroll to it on the next draw
  bool _sel = false, _ensureSel = false; // a message is picked (_selSeq), scroll it into view
  uint32_t _selSeq = 0, _lowSeq = 0;     // ... and the lowest message showing, which Right picks
  int _tabFrom = -1, _tabIdx = 0;        // Tab completion: where the @ is, which name is in
  char _tabPartial[model::NAME_LEN] = {};
};

ConversationScreen s_conv;
void ConversationScreen::s_roomLoginStarted() { s_conv._loggingIn = true; }

// A thread found by following mentions back: its messages from the oldest, one picked.
// Enter takes you to it in the whole conversation.
class ThreadScreen : public Screen {
public:
  model::ThreadId id;
  int idx[THREAD_MAX];   // the messages, as places in the conversation's window (it holds still meanwhile)
  int n = 0, sel = 0, scroll = 0;
  bool chan = false, ensure = true;

  const char* title(App&) override { return "Thread"; }

  void draw(App& a, Canvas& c, const Rect& mr) override {
    const mc_font_t* f = a.fBody();
    const int textW = mr.w - 14;
    int ys[THREAD_MAX], hs[THREAD_MAX], total = 0;
    model::Message m;
    for (int i = 0; i < n; i++) {
      ys[i] = total;
      if (!a.m().message(id, idx[i], m)) { hs[i] = 0; continue; }
      hs[i] = messageH(a, m, f, textW);
      total += hs[i];
    }
    int maxScroll = total > mr.h ? total - mr.h : 0;
    if (ensure && n) {
      if (ys[sel] < scroll) scroll = ys[sel];
      else if (ys[sel] + hs[sel] > scroll + mr.h) scroll = ys[sel] + hs[sel] - mr.h;
      ensure = false;
    }
    if (scroll > maxScroll) scroll = maxScroll;
    if (scroll < 0) scroll = 0;
    for (int i = 0; i < n; i++) {
      int top = mr.y + ys[i] - scroll;
      if (top >= mr.y + mr.h || top + hs[i] <= mr.y) continue;
      if (a.m().message(id, idx[i], m)) drawMessage(a, c, m, chan, i == sel, mr, top, f, textW);
      hitMessage(a, mr, top, hs[i], i);
    }
    drawScrollbar(c, mr, total, scroll, mr.h);
  }

  void onKey(App& a, const Key& k) override {
    switch (k.code) {
      case K_ESC: a.pop(); return;
      case K_UP: if (sel > 0) { sel--; ensure = true; a.invalidate(); } return;
      case K_DOWN: if (sel + 1 < n) { sel++; ensure = true; a.invalidate(); } return;
      case K_HOME: sel = 0; ensure = true; a.invalidate(); return;
      case K_END: sel = n - 1; ensure = true; a.invalidate(); return;
      case K_ENTER: goTo(a); return;
    }
  }

  void onTouch(App& a, const Gesture& g, const HitTarget* t) override {
    if ((g.type == Gesture::TAP || g.type == Gesture::LONG) && t && t->kind == HitTarget::ROW && t->arg < n) {
      if (t->arg == sel || g.type == Gesture::LONG) { sel = t->arg; goTo(a); return; }
      sel = t->arg;
      a.invalidate();
      return;
    }
    if (g.type == Gesture::DRAG) {
      scroll -= a.scrollDy(g.dy);
      a.invalidate();
      return;
    }
    if (g.type != Gesture::SWIPE) Screen::onTouch(a, g, t);
  }

  const Hint* hints(App&) override {
    static const Hint h[] = {{"^v", "move"}, {"ent", "in chat"}, {"esc", "back"}, {nullptr, nullptr}};
    return h;
  }

private:
  void goTo(App& a) {
    if (!n) return;
    model::Message m;
    if (!a.m().message(id, idx[sel], m)) return;
    uint32_t seq = m.seq;
    a.pop();
    s_conv.focus(seq);
    a.invalidate();
  }
};

ThreadScreen s_thread;

void showThread(App& a, model::ThreadId id, const int* idx, int n) {
  s_thread.id = id;
  s_thread.n = n < THREAD_MAX ? n : THREAD_MAX;
  memcpy(s_thread.idx, idx, s_thread.n * sizeof idx[0]);
  s_thread.sel = s_thread.n - 1;   // the message the thread was asked for
  s_thread.scroll = 0;
  s_thread.chan = id.kind == model::ThreadKind::Channel;
  s_thread.ensure = true;
  a.push(&s_thread);
}

// Conversations opened, oldest first, for Ctrl-o.
model::ThreadId s_jumps[8];
int s_nJumps = 0;

void noteJump(model::ThreadId t) {
  int n = 0;
  for (int i = 0; i < s_nJumps; i++)
    if (!(s_jumps[i] == t)) s_jumps[n++] = s_jumps[i];
  if (n == 8) { memmove(s_jumps, s_jumps + 1, sizeof s_jumps[0] * 7); n = 7; }
  s_jumps[n++] = t;
  s_nJumps = n;
}

// Open `t` in place of the conversation on top, or over whatever is there.
void switchTo(App& a, model::ThreadId t) {
  if (a.top() == &s_conv) a.pop();
  openThread(a, t);
}

}  // namespace

void openThread(App& a, model::ThreadId t) {
  if (!(s_conv.id == t)) s_conv.reset();
  s_conv._loginTriedReset();
  s_conv.id = t;
  s_conv.markNew(a);
  noteJump(t);
  a.push(&s_conv);
}

void jumpBack(App& a) {
  bool open = a.top() == &s_conv;
  for (int i = s_nJumps - 1; i >= 0; i--) {
    if (open && s_jumps[i] == s_conv.id) continue;
    // Gone since (a contact deleted, a channel left): skip it.
    bool live = false;
    model::Thread t;
    for (int j = 0, n = a.m().threadCount(); j < n && !live; j++) live = a.m().thread(j, t) && t.id == s_jumps[i];
    if (!live) continue;
    switchTo(a, s_jumps[i]);
    return;
  }
  a.toast("No earlier conversation", C_DIM);
}

void jumpUnread(App& a, int dir) {
  // Chats order: newest first. Start beside the open conversation, if any.
  int n = a.m().threadCount(), cur = -1;
  model::Thread t;
  if (a.top() == &s_conv)
    for (int i = 0; i < n; i++)
      if (a.m().thread(i, t) && t.id == s_conv.id) { cur = i; break; }
  for (int step = 1; step <= n; step++) {
    int i = cur < 0 ? (dir > 0 ? step - 1 : n - step) : ((cur + dir * step) % n + n) % n;
    if (i == cur) break;
    if (a.m().thread(i, t) && t.unread && !t.muted) { switchTo(a, t.id); return; }
  }
  a.toast("No unread messages", C_DIM);
}

void startCompose(App& a) {
  if (a.top() == &s_conv) s_conv.startCompose(a);
}

int chatNewCount(App& a) { return s_conv.newCount(a); }

}}  // namespace ui::screens
