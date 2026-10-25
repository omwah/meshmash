// Home: tabs for Chats (conversations, most recent first), Contacts (people),
// Nodes (repeaters, rooms, sensors) and Channels. Typing filters the list.
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include <stdio.h>
#include <string.h>

namespace ui { namespace screens {

namespace {

enum Tab { T_CHATS, T_CONTACTS, T_NODES, T_CHANNELS, T_COUNT };
const char* const kTabNames[T_COUNT] = {"Chats", "Contacts", "Nodes", "Channels"};
const char* const kTabShort[T_COUNT] = {"Chats", "People", "Nodes", "Chans"};   // when the full names don't fit

// Rows a tab can list: every contact and channel the build keeps.
#ifndef MC_LIST_MAX
#define MC_LIST_MAX 256
#endif
constexpr int MAX_ORDER = MC_LIST_MAX;

bool matches(const char* name, const char* q) {
  if (!q[0]) return true;
  for (const char* p = name; *p; p++) {
    int i = 0;
    while (q[i] && p[i] && ((p[i] | 0x20) == (q[i] | 0x20))) i++;
    if (!q[i]) return true;
  }
  return false;
}

class HomeScreen : public Screen {
public:
  const char* title(App& a) override {
    // Remote mode: whose screen this is.
    if (a.m().mode() != model::Mode::Remote) return a.showBar() ? kTabNames[_tab] : "MeshCore";
    return a.m().prefs().nodeName[0] ? a.m().prefs().nodeName : "Remote node";
  }

  void onHome(App&) override {
    _tab = T_CHATS;
    _query[0] = 0;
    _typing = false;
    _nav[T_CHATS] = ListNav();
  }

  void onEnter(App& a) override {
    _builtRev = ~0u;
    rebuild(a);
  }

  bool tick(App& a, uint32_t) override {
    if (a.m().revision() != _builtRev) { rebuild(a); return true; }
    return false;
  }

  void draw(App& a, Canvas& c, const Rect& r) override {
    const Metrics& m = a.mx();
    const int th = tabH(a);
    if (th) drawTabs(a, c, r);
    Rect lr{r.x, r.y + th, r.w, r.h - th};
    ListNav& nav = _nav[_tab];
    const int baseH = _tab == T_CHATS ? m.chatRow : m.row;
    int vis = lr.h / baseH;
    // The leftover pixels below the last whole row go to the rows, so the
    // list reaches the bottom of its area instead of ending in a blank band.
    const int rowH = baseH + (vis ? (lr.h % baseH) / vis : 0);
    _rowH = rowH;
    const int pad = (rowH - baseH) / 2;
    nav.clamp(_n, vis);
    if (_n == 0) {
      const char* msg = _query[0] ? "No matches" :
                        _tab == T_CHATS ? "No conversations yet" :
                        _tab == T_CHANNELS ? "No channels: tab to join" : "Nobody heard yet";
      c.text(r.x + (r.w - Canvas::textWidth(msg, a.fMed())) / 2, lr.y + (lr.h - a.fMed()->height) / 2, msg, a.fMed(),
             C_DIM, -1, r.w - 8);
      return;
    }
    for (int row = 0; row < vis && nav.top + row < _n; row++) {
      int i = nav.top + row;
      int y = lr.y + row * rowH;
      bool sel = i == nav.sel;
      if (sel) {
        c.fillRect(lr.x, y, lr.w - 3, rowH, C_SEL);
      } else if (row & 1) {
        c.fillRect(lr.x, y, lr.w - 3, rowH, C_ZEBRA);
      }
      a.hit({lr.x, y, lr.w, rowH}, HitTarget::ROW, i);
      if (_tab == T_CHATS) drawChat(a, c, y + pad, baseH, _order[i], sel, pad);
      else if (_tab == T_CHANNELS) drawChannel(a, c, y + pad, baseH, _order[i], sel);
      else drawContact(a, c, y + pad, baseH, _order[i], sel);
    }
    drawScrollbar(c, lr, _n, nav.top, vis);
  }

  void onKey(App& a, const Key& k) override {
    ListNav& nav = _nav[_tab];
    Rect r = a.content();
    int vis = (r.h - tabH(a)) / (_tab == T_CHATS ? a.mx().chatRow : a.mx().row);
    int tab = -1;
    if ((k.code == K_LEFT || k.code == K_RIGHT) && !k.repeat)   // one tab per press
      tab = (_tab + (k.code == K_RIGHT ? 1 : T_COUNT - 1)) % T_COUNT;
    else if (k.code == K_TAB_NEXT || k.code == K_TAB_PREV)
      tab = (_tab + (k.code == K_TAB_NEXT ? 1 : T_COUNT - 1)) % T_COUNT;
    else if (k.code == K_TAB_N && k.count <= T_COUNT)
      tab = k.count - 1;
    if (tab >= 0 || k.code == K_LEFT || k.code == K_RIGHT) {
      if (tab < 0) return;
      _tab = tab;
      _query[0] = 0;
      _typing = false;
      rebuild(a);
      return;
    }
    if (nav.key(k, _n, vis)) return;
    if (k.code == K_ESC) {
      // Esc never opens anything here, so pressing it repeatedly to get
      // "back home" can't overshoot into a menu.
      _typing = false;
      if (_query[0]) { _query[0] = 0; rebuild(a); }
      return;
    }
    // Vim keys: / starts a search (letters are commands otherwise), Enter
    // ends it keeping the matches, n and N step through them.
    if (k.code == K_SEARCH) {
      _typing = true;
      _query[0] = 0;
      rebuild(a);
      return;
    }
    if (_typing && k.code == K_ENTER) { _typing = false; return; }
    if ((k.code == K_NEXT_MATCH || k.code == K_PREV_MATCH) && _n) {
      nav.sel = (nav.sel + (k.code == K_NEXT_MATCH ? 1 : _n - 1)) % _n;
      nav.clamp(_n, vis);
      return;
    }
    if (k.code == K_BKSP) {
      int l = (int)strlen(_query);
      if (l) { _query[l - 1] = 0; rebuild(a); }
      return;
    }
    if (k.isChar() && k.code != ' ' && !a.vimActive()) {
      int l = (int)strlen(_query);
      if (l < (int)sizeof(_query) - 1) { _query[l] = (char)k.code; _query[l + 1] = 0; nav.sel = 0; rebuild(a); }
      return;
    }
    if (k.code == K_TAB && _tab == T_CHANNELS && _n == 0) { openJoinChannel(a); return; }
    if (_n == 0 || nav.sel >= _n) return;
    int idx = _order[nav.sel];
    if (k.code == K_ENTER) open(a, idx);
    else if (k.code == K_TAB) details(a, idx);
    else if (k.code == K_COMPOSE) { open(a, idx); startCompose(a); }
  }

  bool textMode(App&) override { return _typing; }

  bool selectRow(App&, int i) override {
    if (i < 0 || i >= _n) return false;
    _nav[_tab].sel = i;
    return true;
  }

  void showTab(App& a, int tab) {
    _tab = tab;
    _query[0] = 0;
    _typing = false;
    rebuild(a);
  }
  int tab() const { return _tab; }
  int dragRowPx(App& a) override { return _rowH ? _rowH : Screen::dragRowPx(a); }

  const Hint* hints(App& a) override {
    // Most important first: larger text sizes show fewer hints.
    static const Hint chats[] = {{"ent", "open"}, {"opt", "menu"}, {"<>", "tabs"}, {"tab", "info"}, {"a-z", "find"}, {nullptr, nullptr}};
    static const Hint vimChats[] = {{"ent", "open"}, {"opt", "menu"}, {"<>", "tabs"}, {"tab", "info"}, {"/", "find"}, {nullptr, nullptr}};
    static const Hint search[] = {{"ent", "open"}, {"bksp", "edit"}, {"esc", "clear"}, {nullptr, nullptr}};
    static const Hint typing[] = {{"ent", "done"}, {"bksp", "edit"}, {"esc", "clear"}, {nullptr, nullptr}};
    static const Hint found[] = {{"ent", "open"}, {"n/N", "next/prev"}, {"esc", "clear"}, {nullptr, nullptr}};
    static const Hint chan[] = {{"ent", "open"}, {"opt", "menu"}, {"<>", "tabs"}, {"tab", "manage"}, {nullptr, nullptr}};
    if (_typing) return typing;
    if (_query[0]) return a.m().prefs().vimKeys ? found : search;
    if (a.m().prefs().vimKeys && _tab != T_CHANNELS) return vimChats;
    return _tab == T_CHANNELS ? chan : chats;
  }

private:
  // The tab strip; with the shortcut bar its icons are the tabs, and the
  // strip is there only for a search.
  int tabH(App& a) const { return a.showBar() && !_query[0] && !_typing ? 0 : a.mx().tab; }

  void drawTabs(App& a, Canvas& c, const Rect& r) {
    const int TH = a.mx().tab;
    const mc_font_t* f = a.fMed();
    const int ty = r.y + (TH - f->height) / 2;
    c.fillRect(r.x, r.y, r.w, TH, C_ZEBRA);
    if (_query[0] || _typing) {
      int x = c.text(r.x + 4, ty, "Find: ", f, C_DIM);
      x = c.text(x, ty, _query, f, C_ACCENT);
      if ((_typing || !a.m().prefs().vimKeys) && !((a.nowMs() / 500) & 1)) c.fillRect(x, ty, 1, f->height, C_ACCENT);
      char n[12];
      snprintf(n, sizeof n, "%d", _n);
      c.text(r.x + r.w - 4 - Canvas::textWidth(n, f), ty, n, f, C_DIM);
      return;
    }
    // Full names if they fit, short ones otherwise (larger text sizes).
    int total = 0;
    for (int t = 0; t < T_COUNT; t++) total += Canvas::textWidth(kTabNames[t], f) + 12;
    const char* const* names = total <= r.w ? kTabNames : kTabShort;
    int x = r.x + 1;
    for (int t = 0; t < T_COUNT; t++) {
      int w = Canvas::textWidth(names[t], f) + 10;
      bool on = t == _tab;
      if (on) c.pill(x, r.y + 1, w, TH - 2, C_ACCENT);
      c.text(x + 5, ty, names[t], f, on ? C_BG : C_DIM);
      a.hit({x, r.y, w + 2, TH}, HitTarget::SHORTCUT, SC_CHATS + t);
      // Unread dot on Chats when looking elsewhere.
      if (t == T_CHATS && !on && a.m().unreadTotal()) c.fillRect(x + w - 4, r.y + 3, 3, 3, C_BADGE);
      x += w + 2;
    }
  }

  void drawChat(App& a, Canvas& c, int y, int rowH, int i, bool sel, int pad) {
    model::Thread t;
    if (!a.m().thread(i, t)) return;
    const mc_font_t* f = a.fMed();
    const mc_font_t* sm = a.fSmall();
    const int line2 = y + 1 + f->height;
    bool chan = t.id.kind == model::ThreadKind::Channel;
    uint8_t col = chan ? C_CHAN : typeColor(t.type);
    c.fillRect(0, y - pad, 3, rowH + 2 * pad, col);
    // Channels: # for hashtag channels, a lock for ones with a private key.
    int iy = y + 1 + (f->height - 8) / 2;
    c.bitmap(6, iy, chan ? (t.title[0] == '#' ? icon::hash : icon::lock) : typeIcon(t.type), 8, 8, col);
    // Right side: age on the first line, unread badge on the second.
    char ts[12];
    fmtAgo(ts, sizeof ts, a.m().now(), t.lastTs);
    int rx = Canvas::W - 6 - Canvas::textWidth(ts, sm);
    // A muted channel's count waits quietly: dimmed.
    bool alerting = t.unread && !t.muted;
    c.text(rx, y + 1 + (f->height - sm->height) / 2, ts, sm, alerting ? C_BADGE : C_DIM);
    int tx = Canvas::W - 4;
    if (t.unread)
      tx = drawBadge(a, Canvas::W - 5, line2 + (f->height - sm->height - 2) / 2, t.unread, alerting ? C_BADGE : C_DIM) - 2;
    int ix = rx - 10;
    if (t.muted) { c.bitmap(ix, iy, icon::mute, 8, 8, C_DIM); ix -= 10; }
    if (t.favourite) { c.bitmap(ix, iy, icon::star, 8, 8, C_WARN); ix -= 10; }
    a.fitText(c, 17, y + 1, t.title, f, C_TEXT, ix - 19, sel);
    const char* preview = t.preview;
    if (sel && !a.m().prefs().shortPreview) {
      // The selected chat scrolls through its whole last message (the
      // stored preview is cut short): read it once per change.
      uint32_t rev = a.m().revision();
      if (!(_fullId == t.id) || rev != _fullRev) {
        _fullId = t.id;
        _fullRev = rev;
        if (!a.m().lastMessage(t.id, _full, sizeof _full)) snprintf(_full, sizeof _full, "%s", t.preview);
      }
      preview = _full;
    }
    a.fitText(c, 17, line2, preview, f, sel ? C_TEXT : C_DIM, tx - 21, sel);
  }

  void drawContact(App& a, Canvas& c, int y, int rowH, int i, bool sel) {
    model::Contact ct;
    if (!a.m().contact(i, ct)) return;
    const mc_font_t* sm = a.fSmall();
    const int iy = y + (rowH - 8) / 2, sy = y + (rowH - sm->height) / 2;
    uint8_t col = typeColor(ct.type);
    c.bitmap(4, iy, typeIcon(ct.type), 8, 8, col);
    // Right: age, signal, hops pill.
    char ago[8];
    fmtAgo(ago, sizeof ago, a.m().now(), ct.lastHeard);
    int x = Canvas::W - 6 - Canvas::textWidth(ago, sm);
    c.text(x, sy, ago, sm, C_DIM);
    x -= 13;
    drawSignal(c, x, iy, ct.snr_x4);
    char hp[6];
    if (ct.hops < 0) snprintf(hp, sizeof hp, "fl");
    else snprintf(hp, sizeof hp, "%d", ct.hops);
    int hw = Canvas::textWidth(hp, sm) + 4;
    x -= hw + 3;
    c.pill(x, sy - 1, hw, sm->height + 1, ct.hops == 0 ? C_OK : ct.hops < 0 ? C_DIM : C_ACCENT);
    c.text(x + 2, sy, hp, sm, C_BG);
    if (ct.favourite) { x -= 10; c.bitmap(x, iy, icon::star, 8, 8, C_WARN); }
    a.fitText(c, 16, y + (rowH - a.fList()->height) / 2, ct.name, a.fList(), C_TEXT, x - 20, sel);
  }

  void drawChannel(App& a, Canvas& c, int y, int rowH, int i, bool sel) {
    model::Channel ch;
    if (!a.m().channel(i, ch)) return;
    const int iy = y + (rowH - 8) / 2;
    c.bitmap(4, iy, ch.hashtag ? icon::hash : icon::lock, 8, 8, C_CHAN);
    int x = Canvas::W - 6;
    // Unread, also for muted channels (dimmed) and ones kept off Chats.
    uint16_t unread = a.m().unreadIn({model::ThreadKind::Channel, ch.idx});
    if (unread) x = drawBadge(a, x, y + (rowH - a.fSmall()->height - 2) / 2, unread, ch.muted ? C_DIM : C_BADGE) - 3;
    if (ch.muted) { x -= 10; c.bitmap(x, iy, icon::mute, 8, 8, C_DIM); }
    a.fitText(c, 16, y + (rowH - a.fList()->height) / 2, ch.name, a.fList(), C_TEXT, x - 20, sel);
  }

  void rebuild(App& a) {
    _builtRev = a.m().revision();
    _n = 0;
    model::MeshModel& m = a.m();
    if (_tab == T_CHATS) {
      for (int i = 0, n = m.threadCount(); i < n && _n < MAX_ORDER; i++) {
        model::Thread t;
        if (m.thread(i, t) && matches(t.title, _query)) _order[_n++] = (uint16_t)i;
      }
    } else if (_tab == T_CHANNELS) {
      for (int i = 0, n = m.channelCount(); i < n && _n < MAX_ORDER; i++) {
        model::Channel ch;
        if (m.channel(i, ch) && matches(ch.name, _query)) _order[_n++] = (uint16_t)i;
      }
    } else {
      bool people = _tab == T_CONTACTS;
      for (int i = 0, n = m.contactCount(); i < n && _n < MAX_ORDER; i++) {
        model::Contact ct;
        if (!m.contact(i, ct)) continue;
        bool isPerson = ct.type == model::NodeType::Chat || ct.type == model::NodeType::None;
        if (isPerson == people && matches(ct.name, _query)) _order[_n++] = (uint16_t)i;
      }
      // Favourites first, then most recently heard (insertion sort: n is small).
      for (int i = 1; i < _n; i++) {
        uint16_t v = _order[i];
        model::Contact cv;
        m.contact(v, cv);
        int j = i - 1;
        while (j >= 0) {
          model::Contact cj;
          m.contact(_order[j], cj);
          bool before = cv.favourite != cj.favourite ? cv.favourite : cv.lastHeard > cj.lastHeard;
          if (!before) break;
          _order[j + 1] = _order[j];
          j--;
        }
        _order[j + 1] = v;
      }
    }
  }

  void open(App& a, int idx) {
    if (_tab == T_CHATS) {
      model::Thread t;
      if (a.m().thread(idx, t)) openThread(a, t.id);
    } else if (_tab == T_CHANNELS) {
      model::Channel ch;
      if (a.m().channel(idx, ch)) openThread(a, {model::ThreadKind::Channel, ch.idx});
    } else if (_tab == T_CONTACTS) {
      openThread(a, {model::ThreadKind::Direct, (uint16_t)idx});
    } else {
      model::Contact ct;
      if (a.m().contact(idx, ct) && ct.type == model::NodeType::Room) openThread(a, {model::ThreadKind::Direct, (uint16_t)idx});
      else openContact(a, idx);
    }
  }

  void details(App& a, int idx) {
    if (_tab == T_CHATS) {
      model::Thread t;
      if (!a.m().thread(idx, t)) return;
      if (t.id.kind == model::ThreadKind::Channel) openChannel(a, t.id.idx);
      else openContact(a, t.id.idx);
    } else if (_tab == T_CHANNELS) {
      openChannel(a, idx);
    } else {
      openContact(a, idx);
    }
  }

  int _tab = T_CHATS;
  int _rowH = 0;   // the list's row height as last drawn
  ListNav _nav[T_COUNT];
  char _query[16] = {};
  bool _typing = false;                // vim keys: typing a search after /
  uint16_t _order[MAX_ORDER];
  int _n = 0;
  uint32_t _builtRev = ~0u;
  char _full[model::TEXT_LEN + 40];   // the selected chat's last message, in full
  model::ThreadId _fullId{model::ThreadKind::Direct, 0xFFFF};
  uint32_t _fullRev = 0;
};

HomeScreen s_home;

}  // namespace

Screen* home() { return &s_home; }

void shortcut(App& a, int sc) {
  if (sc == SC_MENU) {
    if (!mainMenuOpen(a)) openMainMenu(a);
    return;
  }
  // Home, on that tab; from anywhere, as long Esc does.
  if (a.top() != &s_home) a.popToRoot();
  if (a.top() == &s_home) s_home.showTab(a, sc - SC_CHATS);
  a.invalidate();
}

int shortcutShowing(App& a) {
  if (a.top() == &s_home) return SC_CHATS + s_home.tab();
  return mainMenuOpen(a) ? SC_MENU : -1;
}

}}  // namespace ui::screens
