#include "mock_model.h"
#include "app/radio_presets.h"
#include <stdio.h>
#include <string.h>

namespace sim {

using namespace model;

namespace {

Contact mk(const char* name, NodeType t, int8_t hops, float snr, int16_t rssi, uint32_t agoS, bool fav,
           uint8_t k0) {
  Contact c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", name);
  c.type = t;
  c.hops = hops;
  c.snr_x4 = (int8_t)(snr * 4);
  c.rssi = rssi;
  c.lastHeard = MockModel::kEpoch - agoS;
  c.favourite = fav;
  for (int i = 0; i < 6; i++) c.key[i] = (uint8_t)(k0 * 37 + i * 91);
  c.pathLen = hops > 0 ? hops : 0;
  for (int i = 0; i < c.pathLen && i < 16; i++) c.path[i] = (uint8_t)(0x3A + i * 0x29 + k0);
  return c;
}

}  // namespace

MockModel::MockModel() {
  memset(&_prefs, 0, sizeof _prefs);
  snprintf(_prefs.nodeName, sizeof _prefs.nodeName, "Cardputer-7F");
  _prefs.theme = 0;
  _prefs.timeZone = 1;   // US Pacific, to exercise DST in the simulator
  _prefs.brightness = 192;
  _prefs.screenTimeoutS = 60;
  _prefs.inputLockoutMs = 60;
  _prefs.ledBrightness = 128;
  _prefs.sound = true;
  _prefs.textSize = 1;
  _prefs.vimKeys = MC_VIM_KEYS_DEFAULT;
  _prefs.trackball = true;
  _prefs.lockOnTimeout = true;
  _prefs.autoAddContacts = true;
  _prefs.autoBackup = true;
  _prefs.backupIdentity = true;
  _prefs.bleEnabled = true;
  _prefs.blePin = 123456;
  _prefs.blePinRandom = true;   // as a fresh device
  _prefs.presetIdx = 0;
  _prefs.telemBattery = model::TelemMode::PerContact;

  const Preset& p = app::kRadioPresets[0];
  _radio = {p.freqMHz, p.bwKHz, p.sf, p.cr, 22, 22, false, true};

  memset(&_gps, 0, sizeof _gps);
  _gps.fix = true;
  _gps.sats = 9;
  _gps.lat_e6 = 32715000;  // fictional point
  _gps.lon_e6 = -117161000;
  _gps.altCm = 2100;
  _gps.hdop_x10 = 9;

  memset(&_stats, 0, sizeof _stats);
  memset(_threads, 0, sizeof _threads);

  // Contacts: people first, then infrastructure. Fictional names.
  Contact cs[] = {
      mk("Alice Kestrel", NodeType::Chat, 1, 6.5f, -92, 120, true, 1),
      mk("Bob Rivera", NodeType::Chat, 0, 9.25f, -71, 40, false, 2),
      mk("Carmen Ortiz", NodeType::Chat, 3, -4.0f, -109, 3600 * 3, false, 3),
      mk("Dmitri V", NodeType::Chat, -1, -12.5f, -118, 86400 * 2, false, 4),
      mk("Eve (mobile)", NodeType::Chat, 2, -1.0f, -104, 900, true, 5),
      mk("Farid Haddad", NodeType::Chat, 4, -8.5f, -113, 7200, false, 6),
      mk("Grace Park", NodeType::Chat, 1, 2.0f, -98, 300, false, 7),
      mk("Hilltop RPT", NodeType::Repeater, 0, 10.0f, -64, 600, true, 8),
      mk("Harbor Repeater", NodeType::Repeater, 0, -6.0f, -111, 1800, false, 9),
      mk("Ridge 900", NodeType::Repeater, 1, -2.0f, -105, 5400, false, 10),
      mk("Makers Room", NodeType::Room, 1, 3.5f, -96, 2400, false, 11),
      mk("Weather-01", NodeType::Sensor, 2, -9.0f, -115, 3600, false, 12),
  };
  _nContacts = sizeof cs / sizeof cs[0];
  memcpy(_contacts, cs, sizeof cs);
  _contacts[0].hasPos = true;
  _contacts[0].lat_e6 = 32720512;
  _contacts[0].lon_e6 = -117150311;

  const char* chans[][2] = {{"Public", ""}, {"#local", "#"}, {"#emergency", "#"}, {"Family", ""}};
  for (auto& c : chans) {
    Channel& ch = _channels[_nChannels];
    ch.idx = (uint8_t)_nChannels;
    snprintf(ch.name, sizeof ch.name, "%s", c[0]);
    ch.hashtag = c[1][0] == '#';
    ch.muted = _nChannels == 2;
    for (int b = 0; b < 16; b++) _keys[_nChannels][b] = (uint8_t)(b * 17 + _nChannels * 3);
    _nChannels++;
  }

  // Conversation history (oldest first).
  ThreadId pub{ThreadKind::Channel, 0}, loc{ThreadKind::Channel, 1}, fam{ThreadKind::Channel, 3};
  ThreadId alice{ThreadKind::Direct, 0}, bob{ThreadKind::Direct, 1}, eve{ThreadKind::Direct, 4},
      room{ThreadKind::Direct, 10};
  uint32_t t = kEpoch;
  addMsg(pub, t - 5400, false, "Ridge 900", "Ridge repeater back online after the battery swap.", -8, 2, Delivery::None);
  addMsg(pub, t - 3000, false, "Grace Park", "Anyone hearing me from the north side?", 6, 1, Delivery::None);
  addMsg(pub, t - 2900, true, "", "Loud and clear via Hilltop, -2 dB here.", 0, 0, Delivery::Sent);
  _msgs[_nMsgs - 1].m.repeats = 3;
  addMsg(pub, t - 400, false, "Farid Haddad", "Net check-in at 19:00 tonight on #local.", -20, 4, Delivery::None);

  addMsg(loc, t - 7000, false, "Carmen Ortiz", "Trail is muddy past the second bridge.", -14, 3, Delivery::None);
  addMsg(loc, t - 6800, false, "Alice Kestrel", "Thanks! Bringing boots.", 22, 1, Delivery::None);

  addMsg(fam, t - 86400, false, "Eve (mobile)", "Landed safe.", 4, 2, Delivery::None);

  addMsg(alice, t - 4000, false, "", "Are you coming to the meetup Saturday?", 26, 1, Delivery::None);
  addMsg(alice, t - 3900, true, "", "Yes! I'll bring the new Cardputer with the LoRa cap.", 0, 0, Delivery::Delivered);
  addMsg(alice, t - 3800, false, "", "Nice. Can you test range from the parking lot to the ridge? I want to know if the Hilltop repeater covers the whole route.", 26, 1, Delivery::None);
  addMsg(alice, t - 200, true, "", "Will do, I'll log SNR along the way.", 0, 0, Delivery::Pending);
  addMsg(alice, t - 150, false, "", "Perfect, thanks!", 24, 1, Delivery::None);
  addMsg(alice, t - 120, false, "", "Also bring a spare antenna if you have one.", 26, 1, Delivery::None);

  addMsg(bob, t - 9000, true, "", "Did the solar node survive the storm?", 0, 0, Delivery::Delivered);
  addMsg(bob, t - 8800, false, "", "Yep, battery at 78%.", 37, 0, Delivery::None);
  addMsg(bob, t - 60, true, "", "Great. Swapping the antenna tomorrow.", 0, 0, Delivery::Failed);

  addMsg(eve, t - 900, false, "", "Running late, 20 min.", -4, 2, Delivery::None);
  addMsg(room, t - 2400, false, "Grace Park", "Workshop slides are up on the room.", 14, 1, Delivery::None);

  // Unread state.
  threadFor(alice)->unread = 2;
  threadFor(pub)->unread = 1;
  threadFor(eve)->unread = 1;

  // Heard adverts (not all are contacts).
  Contact hs[] = {
      mk("Hilltop RPT", NodeType::Repeater, 0, 10.0f, -64, 600, false, 8),
      mk("Harbor Repeater", NodeType::Repeater, 0, -6.0f, -111, 1800, false, 9),
      mk("Mesa Relay", NodeType::Repeater, 0, -11.25f, -117, 240, false, 21),
      mk("kayak-node", NodeType::Chat, 2, -3.0f, -106, 75, false, 22),
      mk("Library Room", NodeType::Room, 3, -9.5f, -114, 4000, false, 23),
  };
  _nHeard = sizeof hs / sizeof hs[0];
  memcpy(_heard, hs, sizeof hs);

  // Packet log (newest first when read).
  static const uint8_t types[] = {4, 2, 3, 5, 4, 8, 5, 2, 3, 4, 9, 5, 0, 1, 4, 5};
  for (int i = 0; i < 24; i++) {
    RxLogEntry& e = _log[_nLog++];
    e.ms = uptimeMs() - (uint32_t)(24 - i) * 47000u;
    e.payloadType = types[i % 16];
    e.routeType = (i % 3) ? 1 : 2;
    e.pathLen = (uint8_t)(i % 5);
    e.snr_x4 = (int8_t)(((i * 7) % 40 - 20) * 2);
    e.rssi = (int16_t)(-60 - (i * 13) % 60);
    e.len = (uint8_t)(20 + (i * 11) % 120);
    e.srcHash = (uint8_t)(i * 53);
    e.dup = i % 7 == 3;
  }
}

void MockModel::setEmpty() {
  _nContacts = 0;
  _nChannels = 1;
  _nMsgs = 0;
  _nHeard = 0;
  _nLog = 0;
  memset(_threads, 0, sizeof _threads);
  _rev++;
}

void MockModel::selfKey(uint8_t out[6]) const {
  static const uint8_t k[6] = {0x7F, 0x3C, 0x91, 0x0A, 0xD2, 0x55};
  memcpy(out, k, 6);
}

MockModel::ThreadState* MockModel::threadFor(ThreadId t) {
  for (auto& s : _threads)
    if (s.used && s.id == t) return &s;
  for (auto& s : _threads)
    if (!s.used) { s.used = true; s.id = t; s.unread = 0; s.hidden = false; return &s; }
  return &_threads[0];
}

void MockModel::addMsg(ThreadId t, uint32_t ts, bool out, const char* sender, const char* text, int8_t snr_x4,
                       int8_t hops, Delivery st) {
  if (_nMsgs == MAX_MSG) {
    memmove(_msgs, _msgs + 1, sizeof(Msg) * (MAX_MSG - 1));
    _nMsgs--;
  }
  Msg& m = _msgs[_nMsgs++];
  memset(&m, 0, sizeof m);
  m.t = t;
  m.m.seq = ++_lastSeq;
  m.m.ts = ts;
  m.m.outgoing = out;
  m.m.status = st;
  m.m.snr_x4 = out ? INT8_MIN : snr_x4;
  m.m.hops = hops;
  snprintf(m.m.sender, sizeof m.m.sender, "%s", sender);
  snprintf(m.m.text, sizeof m.m.text, "%s", text);
  threadFor(t);
  _rev++;
}

void MockModel::step() {
  uint32_t ms = *_ms;
  // Pending outgoing DMs are delivered 1.5 s after sending.
  for (int i = 0; i < _nMsgs; i++) {
    Message& m = _msgs[i].m;
    if (m.outgoing && m.status == Delivery::Pending && (now() - m.ts) >= 2) {
      m.status = _msgs[i].t.kind == ThreadKind::Channel ? Delivery::Sent : Delivery::Delivered;
      if (_msgs[i].t.kind == ThreadKind::Channel) m.repeats = 2;
      _rev++;
    }
  }
  if (_req == Req::Busy && (int32_t)(ms - _reqDone) >= 0) {
    _req = Req::Ok;
    snprintf(_reqText, sizeof _reqText, "%s", _pendingText);
    _rev++;
  }
  if (_discover == Req::Busy && (int32_t)(ms - _discoverDone) >= 0) {
    _discover = Req::Ok;
    _rev++;
  }
}

bool MockModel::contact(int i, Contact& out) const {
  if (i < 0 || i >= _nContacts) return false;
  out = _contacts[i];
  return true;
}

void MockModel::setFavourite(int i, bool fav) {
  if (i >= 0 && i < _nContacts) { _contacts[i].favourite = fav; _rev++; }
}

bool MockModel::removeContact(int i) {
  if (i < 0 || i >= _nContacts) return false;
  memmove(&_contacts[i], &_contacts[i + 1], sizeof(Contact) * (_nContacts - i - 1));
  _nContacts--;
  _rev++;
  return true;
}

void MockModel::setTelemetry(int i, uint8_t telem) {
  if (i >= 0 && i < _nContacts) { _contacts[i].telemetry = telem; _rev++; }
}

bool MockModel::resetPath(int i) {
  if (i < 0 || i >= _nContacts) return false;
  _contacts[i].hops = -1;
  _contacts[i].pathLen = 0;
  _rev++;
  return true;
}

bool MockModel::channel(int i, Channel& out) const {
  if (i < 0 || i >= _nChannels) return false;
  out = _channels[i];
  return true;
}

bool MockModel::joinHashtag(const char* name) {
  if (_nChannels == MAX_CH) return false;
  Channel& c = _channels[_nChannels];
  c.idx = (uint8_t)_nChannels;
  snprintf(c.name, sizeof c.name, "%s", name);
  c.hashtag = true;
  c.muted = false;
  _nChannels++;
  _rev++;
  return true;
}

bool MockModel::joinPrivate(const char* name, const uint8_t psk[16]) {
  if (!joinHashtag(name)) return false;
  _channels[_nChannels - 1].hashtag = false;
  memcpy(_keys[_nChannels - 1], psk, 16);
  return true;
}

bool MockModel::createPrivate(const char* name) {
  uint8_t k[16];
  for (int i = 0; i < 16; i++) k[i] = (uint8_t)(i * 29 + 7);
  return joinPrivate(name, k);
}

bool MockModel::channelKey(int i, uint8_t psk[16]) const {
  if (i < 0 || i >= _nChannels) return false;
  memcpy(psk, _keys[i], 16);
  return true;
}

bool MockModel::removeChannel(int i) {
  if (i <= 0 || i >= _nChannels) return false;
  memmove(&_channels[i], &_channels[i + 1], sizeof(Channel) * (_nChannels - i - 1));
  _nChannels--;
  _rev++;
  return true;
}

void MockModel::setMuted(int i, bool muted) {
  if (i >= 0 && i < _nChannels) { _channels[i].muted = muted; _rev++; }
}

int MockModel::threadCount() const {
  int n = 0;
  for (auto& s : _threads) n += s.used && !s.hidden;
  return n;
}

bool MockModel::chanMuted(int slot) const {
  for (int c = 0; c < _nChannels; c++)
    if (_channels[c].idx == slot) return _channels[c].muted;
  return false;
}

bool MockModel::threadHidden(ThreadId t) const {
  for (auto& s : _threads)
    if (s.used && s.id == t) return s.hidden;
  return false;
}

void MockModel::setThreadHidden(ThreadId t, bool on) {
  ThreadState* s = threadFor(t);
  if (!s) return;
  s->hidden = on;
  if (on) s->unread = 0;
  _rev++;
}

uint16_t MockModel::unreadIn(ThreadId t) const {
  for (auto& s : _threads)
    if (s.used && s.id == t) return s.unread;
  return 0;
}

bool MockModel::thread(int i, Thread& out) const {
  // Order threads by last message time, newest first.
  int order[MAX_T], n = 0;
  uint32_t last[MAX_T];
  for (int k = 0; k < MAX_T; k++) {
    if (!_threads[k].used || _threads[k].hidden) continue;
    uint32_t ts = 0;
    for (int j = 0; j < _nMsgs; j++)
      if (_msgs[j].t == _threads[k].id && _msgs[j].m.ts > ts) ts = _msgs[j].m.ts;
    int p = n++;
    while (p > 0 && last[p - 1] < ts) { order[p] = order[p - 1]; last[p] = last[p - 1]; p--; }
    order[p] = k;
    last[p] = ts;
  }
  if (i < 0 || i >= n) return false;
  const ThreadState& s = _threads[order[i]];
  memset(&out, 0, sizeof out);
  out.id = s.id;
  out.unread = s.unread;
  out.lastTs = last[i];
  if (s.id.kind == ThreadKind::Channel) {
    for (int c = 0; c < _nChannels; c++)
      if (_channels[c].idx == s.id.idx) {
        snprintf(out.title, sizeof out.title, "%s", _channels[c].name);
        out.muted = _channels[c].muted;
      }
  } else if (s.id.idx < _nContacts) {
    snprintf(out.title, sizeof out.title, "%s", _contacts[s.id.idx].name);
    out.type = _contacts[s.id.idx].type;
    out.favourite = _contacts[s.id.idx].favourite;
  }
  for (int j = _nMsgs - 1; j >= 0; j--)
    if (_msgs[j].t == s.id) {
      const Message& m = _msgs[j].m;
      if (m.outgoing) snprintf(out.preview, sizeof out.preview, "You: %s", m.text);
      else if (m.sender[0] && s.id.kind == ThreadKind::Channel) snprintf(out.preview, sizeof out.preview, "%s: %s", m.sender, m.text);
      else snprintf(out.preview, sizeof out.preview, "%s", m.text);
      break;
    }
  return true;
}

int MockModel::messageCount(ThreadId t) const {
  int n = 0;
  for (int j = 0; j < _nMsgs; j++) n += _msgs[j].t == t;
  return n;
}

bool MockModel::message(ThreadId t, int i, Message& out) const {
  for (int j = 0; j < _nMsgs; j++)
    if (_msgs[j].t == t && i-- == 0) { out = _msgs[j].m; return true; }
  return false;
}

void MockModel::clearHistory(ThreadId t) {
  int w = 0;
  for (int r = 0; r < _nMsgs; r++)
    if (!(_msgs[r].t == t)) _msgs[w++] = _msgs[r];
  _nMsgs = w;
  _rev++;
}

bool MockModel::lastMessage(ThreadId t, char* out, int n) const {
  for (int j = _nMsgs - 1; j >= 0; j--) {
    if (!(_msgs[j].t == t)) continue;
    const Message& m = _msgs[j].m;
    if (m.outgoing) snprintf(out, n, "You: %s", m.text);
    else if (m.sender[0] && t.kind == ThreadKind::Channel) snprintf(out, n, "%s: %s", m.sender, m.text);
    else snprintf(out, n, "%s", m.text);
    return true;
  }
  return false;
}

bool MockModel::unreadSince(ThreadId t, uint32_t& readSeq) const {
  int unread = 0;
  for (auto& s : _threads)
    if (s.used && s.id == t) unread = s.unread;
  if (!unread) return false;
  // The thread's last `unread` messages are the new ones.
  int n = messageCount(t), i = 0;
  readSeq = 0;
  for (int j = 0; j < _nMsgs; j++)
    if (_msgs[j].t == t && i++ < n - unread) readSeq = _msgs[j].m.seq;
  return true;
}

void MockModel::markRead(ThreadId t) {
  for (auto& s : _threads)
    if (s.used && s.id == t && s.unread) { s.unread = 0; _rev++; }
}

uint16_t MockModel::unreadTotal() const {
  uint16_t n = 0;
  for (auto& s : _threads)
    if (s.used && !s.hidden && !(s.id.kind == ThreadKind::Channel && chanMuted(s.id.idx))) n += s.unread;
  return n;
}

bool MockModel::send(ThreadId t, const char* text) {
  addMsg(t, now(), true, "", text, 0, 0, Delivery::Pending);
  return true;
}

void MockModel::rxDirect(int c, const char* text) {
  ThreadId t{ThreadKind::Direct, (uint16_t)c};
  addMsg(t, now(), false, "", text, 20, _contacts[c].hops, Delivery::None);
  threadFor(t)->hidden = false;
  threadFor(t)->unread++;
}

void MockModel::rxChannel(int slot, const char* sender, const char* text) {
  ThreadId t{ThreadKind::Channel, (uint16_t)slot};
  addMsg(t, now(), false, sender, text, -6, 2, Delivery::None);
  if (!chanMuted(slot)) threadFor(t)->hidden = false;
  threadFor(t)->unread++;
}

bool MockModel::advert(bool) { _rev++; return true; }

bool MockModel::heard(int i, Contact& out) const {
  if (i < 0 || i >= _nHeard) return false;
  out = _heard[i];
  return true;
}

bool MockModel::addHeard(int i) {
  if (i < 0 || i >= _nHeard || _nContacts == MAX_C) return false;
  for (int c = 0; c < _nContacts; c++)
    if (!strcmp(_contacts[c].name, _heard[i].name)) return false;
  _contacts[_nContacts++] = _heard[i];
  _rev++;
  return true;
}

bool MockModel::discoverRepeaters() {
  _discover = Req::Busy;
  _discoverDone = *_ms + 3000;
  _rev++;
  return true;
}

void MockModel::startReq(const char* reply, uint32_t delayMs) {
  _req = Req::Busy;
  _reqDone = *_ms + delayMs;
  snprintf(_pendingText, sizeof _pendingText, "%s", reply);
  _rev++;
}

bool MockModel::login(int c, const char*) {
  _loggedInto = c;
  startReq("Logged in as admin", 800);
  return true;
}

bool MockModel::requestStats(int) {
  _stats.valid = true;
  _stats.battMv = 4012;
  _stats.uptimeS = 86400 * 12 + 3600 * 5;
  _stats.noiseFloor = -112;
  _stats.lastRssi = -94;
  _stats.lastSnr_x4 = 22;
  _stats.rxPackets = 48211;
  _stats.txPackets = 13970;
  _stats.airTxS = 2310;
  _stats.airRxS = 9120;
  _stats.queueLen = 0;
  _stats.floodsSent = 11021;
  _stats.directSent = 2949;
  _stats.dupsFlood = 30122;
  _stats.dupsDirect = 410;
  startReq("ok", 900);
  return true;
}

bool MockModel::requestNeighbours(int) { startReq("ok", 900); return true; }

bool MockModel::requestTelemetry(int) {
  startReq("Battery: 4.01 V\nTemperature: 21.5 C\nHumidity: 48 %\nPressure: 1012.4 hPa", 1100);
  return true;
}

bool MockModel::sendCommand(int, const char* cmd) {
  char r[120];
  snprintf(r, sizeof r, "> %s\nOK", cmd);
  startReq(r, 700);
  return true;
}

bool MockModel::tracePath(int) {
  startReq("Path to Alice Kestrel:\n7F (you)\n  > 8A Hilltop RPT  snr 9.5\n  > 3C Alice Kestrel  snr 6.5\nBack:\n  > 8A snr 7.0\n  > 7F snr 10.0", 1500);
  return true;
}

bool MockModel::ping(int) { startReq("pong", 1800); return true; }

bool MockModel::neighbour(int i, Neighbour& out) const {
  static const char* const names[] = {"Harbor Repeater", "Ridge 900", ""};
  if (i < 0 || i >= 3) return false;
  memset(&out, 0, sizeof out);
  for (int b = 0; b < 4; b++) out.key[b] = (uint8_t)(i * 71 + b * 13);
  snprintf(out.name, sizeof out.name, "%s", names[i]);
  out.snr_x4 = (int8_t)(-8 + i * 14);
  out.heardAgoS = 120 + i * 900;
  return true;
}

int MockModel::presetCount() const { return app::kRadioPresetCount; }

const Preset& MockModel::preset(int i) const { return app::kRadioPresets[i]; }

Diag MockModel::diag() const {
  Diag d;
  memset(&d, 0, sizeof d);
  d.uptimeS = uptimeMs() / 1000;
  d.noiseFloor = -116;
  d.lastRssi = -94;
  d.lastSnr_x4 = 26;
  d.rxPackets = 1824;
  d.txPackets = 211;
  d.rxFlood = 1610;
  d.rxDirect = 214;
  d.txFlood = 140;
  d.txDirect = 71;
  d.dups = 712;
  d.rxErrors = 3;
  d.airTxMs = 61000;
  d.airRxMs = 498000;
  d.dutyPct = 2;
  d.queueLen = 1;
  d.offlineQueue = 0;
  d.heapFree = 86 * 1024;
  d.heapMin = 61 * 1024;
  d.heapMaxBlock = 52 * 1024;
  d.battMv = 3948;
  d.battPct = 71;
  d.charging = false;
  d.contacts = (uint16_t)_nContacts;
  d.contactsMax = 150;
  d.channels = (uint8_t)_nChannels;
  d.channelsMax = 8;
  return d;
}

bool MockModel::rxLog(int i, RxLogEntry& out) const {
  if (i < 0 || i >= _nLog) return false;
  out = _log[_nLog - 1 - i];
  return true;
}

Gps MockModel::gps() const {
  Gps g = _gps;
  g.enabled = _prefs.gpsEnabled;
  return g;
}

BackupStatus MockModel::backupStatus() const {
  BackupStatus b;
  memset(&b, 0, sizeof b);
  b.cardPresent = true;
  b.hasBackup = true;
  b.hasPrevious = true;
  b.lastBackupTs = _lastBackup ? _lastBackup : kEpoch - 1260;
  return b;
}

}  // namespace sim
