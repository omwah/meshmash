// A conversation (DM, room or channel): messages bottom-anchored like a chat
// app, a meta line per message (sender, time, delivery or signal), and a
// compose box. Up/Down scroll; Enter or typing starts composing. A "New" line
// marks where the messages that came in since the last visit begin.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include <stdio.h>
#include <string.h>

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
    for (int i = n - 1; i >= 0; i--) {
      model::Message m;
      if (!a.m().message(id, i, m)) continue;
      int h = rowH(a, m, f, textW);
      total += h;
      int top = bottom - h;
      if (top < mr.y + mr.h && bottom > mr.y) {
        if (_divSeq && m.seq == _divSeq) {
          drawDivider(a, c, mr, top);
          top += divH(a);
        }
        drawMessage(a, c, m, mr, top, f, textW);
      }
      bottom = bottom - h;
    }
    _maxScroll = total > mr.h ? total - mr.h : 0;
    if (_scroll > _maxScroll) _scroll = _maxScroll;
    if (_scroll > 0 || !a.m().atNewest(id)) {
      // More below: in this window, or newer messages not loaded yet.
      c.fillRect(mr.x + mr.w / 2 - 12, mr.y + mr.h - 4, 24, 3, C_ACCENT);
    }
    drawScrollbar(c, mr, total, total - mr.h - _scroll, mr.h);

    if (_composing) drawCompose(a, c, r);
  }

  void onKey(App& a, const Key& k) override {
    if (_composing) { composeKey(a, k); return; }
    Rect r = a.content();
    switch (k.code) {
      case K_ESC: a.pop(); return;
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
    if (g.type != Gesture::DRAG || _composing) { Screen::onTouch(a, g, t); return; }
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
    if (!_composing && a.vimActive()) return _draft[0] ? draft : vim;
    static const Hint comp[] = {{"ent", "send"}, {"esc", "stop"}, {"fn+,/", "cursor"}, {nullptr, nullptr}};
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
    _draft[0] = 0;
    _loginTried = false;
    _loggingIn = false;
  }
  static void s_roomLoginStarted();
  void _loginTriedReset() { _loginTried = false; }
  bool _loggingIn = false;

  void startCompose(App& a) {
    (void)a;
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

  void composeKey(App& a, const Key& k) {
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
    int h = a.mx().meta + wrapText(m.text, f, textW, nullptr, nullptr) * f->height + GAP;
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

  void drawMessage(App& a, Canvas& c, const model::Message& m, const Rect& r, int top,
                   const mc_font_t* f, int textW) {
    bool chan = id.kind == model::ThreadKind::Channel;
    uint8_t bar = m.outgoing ? C_ACCENT : chan ? C_CHAN : C_CHAT;
    int lines = wrapText(m.text, f, textW, nullptr, nullptr);
    int h = a.mx().meta + lines * f->height;
    if (m.outgoing) c.fillRect(r.x + 4, top, r.w - 7, h + 1, C_ZEBRA);
    c.fillRect(r.x + 4, top, 2, h + 1, bar);

    // Meta line: who, when, then delivery state or signal.
    const mc_font_t* sf = a.fSmall();
    char t[16];
    fmtStamp(t, sizeof t, a.m().now(), m.ts);
    int x = r.x + 9;
    if (m.outgoing) {
      x = c.text(x, top + 1, "me", sf, C_ACCENT) + 5;
    } else if (m.sender[0]) {   // channels and rooms carry the author
      x = c.text(x, top + 1, m.sender, sf, C_CHAT, -1, 100) + 5;
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
};

ConversationScreen s_conv;
void ConversationScreen::s_roomLoginStarted() { s_conv._loggingIn = true; }

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
