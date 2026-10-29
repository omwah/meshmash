#include "app/adapter_base.h"
#include "app/radio_presets.h"
#include "app/lock_pref.h"
#include <Arduino.h>
#include <helpers/AdvertDataHelpers.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef UI_DEFAULT_TEXT_SIZE
#define UI_DEFAULT_TEXT_SIZE 1   // medium
#endif

namespace app {

namespace {

constexpr uint32_t UI_PREFS_MAGIC = 0x4955434D;   // "MCUI"
constexpr uint8_t UI_PREFS_VERSION = 2;   // 2: text size levels
const char* const kUiPrefsFile = "/ui/prefs.bin";
constexpr uint8_t CTL_DISCOVER_RESP = 0x90;

uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

}  // namespace

void hexKey(char* out, size_t n, const uint8_t* b, int len) {
  size_t o = 0;
  if (n) out[0] = 0;
  for (int i = 0; i < len && o + 2 < n; i++) o += snprintf(out + o, n - o, "%02X", b[i]);
}

uint8_t batteryPercent(uint16_t mv) {
  // As esp-vim configures the Cardputer (and M5Stack's library draws it): a
  // straight line from 3300 mV (0 %) to 4100 mV (100 %).
  if (mv <= 3300) return 0;
  if (mv >= 4100) return 100;
  return (uint8_t)((mv - 3300) * 100 / 800);
}

// ------------------------------------------------------------------ state

void AdapterBase::resetRuntime() {
  memset(_pending, 0, sizeof _pending);
  memset(_signals, 0, sizeof _signals);
  _nHeard = 0;
  _logHead = _nLog = 0;
  _req = Request();
  _remote = model::RemoteStats();
  _nNeighbours = 0;
  _discoverState = model::Req::Idle;
  _lastRssi = 0;
  _lastSnr = 0;
  _dupsSeen = 0;
  memset(_sentChan, 0, sizeof _sentChan);
  memset(_loggedIn, 0, sizeof _loggedIn);
  _cfgChangedAt = _lastBackupAt = 0;
  _bootGpsUntil = 0;
  _bootGpsPending = false;
  _hasOpen = false;
  _rev++;
}

// ------------------------------------------------------------------ UI prefs

void AdapterBase::loadUiPrefs(fs::FS& fs) {
  _fs = &fs;
  memset(&_ui, 0, sizeof _ui);
  File f = _fs->open(kUiPrefsFile, "r");
  // A file from an older build is shorter: the fields it lacks stay 0.
  size_t got = f ? f.read((uint8_t*)&_ui, sizeof _ui) : 0;
  bool ok = got >= offsetof(UiPrefs, blePinRandom) && _ui.magic == UI_PREFS_MAGIC;
  if (f) f.close();
  const bool fresh = !ok || _ui.version != UI_PREFS_VERSION;
  if (fresh) {
    memset(&_ui, 0, sizeof _ui);
    _ui.magic = UI_PREFS_MAGIC;
    _ui.version = UI_PREFS_VERSION;
    _ui.brightness = 192;
    _ui.screenTimeoutS = 60;
    _ui.inputLockoutMs = 60;
    _ui.sound = 1;
    _ui.textSize = UI_DEFAULT_TEXT_SIZE;
    _ui.autoBackup = 1;
    _ui.backupIdentity = 1;
    _ui.bleEnabled = 1;
    _ui.blePinRandom = 0;   // one PIN, random the first time and then kept (123456 is the first one anyone tries)
    _ui.presetIdx = 0;
  }
  // A new install locks when the screen times out; one from before the Cardputer's G0 keeps its any-key wake.
  settleLockPref(_ui.lockOff, _ui.lockSet, fresh, MC_HAS_TRACKBALL);
}

model::Mode readDeviceMode(fs::FS& fs) {
  UiPrefs u{};
  File f = fs.open(kUiPrefsFile, "r");
  size_t got = f ? f.read((uint8_t*)&u, sizeof u) : 0;
  if (f) f.close();
  bool ok = got > offsetof(UiPrefs, deviceMode) && u.magic == UI_PREFS_MAGIC && u.version == UI_PREFS_VERSION;
  return ok && u.deviceMode == 1 ? model::Mode::Remote : model::Mode::Local;
}

void AdapterBase::saveUiPrefs() {
  File f = _fs->open(kUiPrefsFile, "w", true);
  if (!f) return;
  f.write((const uint8_t*)&_ui, sizeof _ui);
  f.close();
}

void AdapterBase::uiToPrefs() {
  model::Prefs& p = _prefs;
  p.theme = _ui.theme;
  p.timeZone = _ui.timeZone;
  p.brightness = _ui.brightness;
  p.screenTimeoutS = _ui.screenTimeoutS;
  p.inputLockoutMs = _ui.inputLockoutMs;
  p.sound = _ui.sound;
  p.textSize = _ui.textSize;
  p.gpsTimeSyncAtBoot = _ui.gpsTimeSyncAtBoot;
  p.autoBackup = _ui.autoBackup;
  p.backupIdentity = _ui.backupIdentity;
  p.backupMessages = _ui.backupMessages;
  p.bleEnabled = _ui.bleEnabled;
  p.presetIdx = _ui.presetIdx;
  p.blePinRandom = _ui.blePinRandom;
  p.deviceMode = _ui.deviceMode == 1 ? model::Mode::Remote : model::Mode::Local;
  p.pingBot = _ui.pingBot;
  p.shortPreview = _ui.shortPreview;
  p.vimKeys = _ui.vimKeys ? _ui.vimKeys == 2 : MC_VIM_KEYS_DEFAULT;
  p.trackball = !_ui.trackballOff;
  p.lockOnTimeout = !_ui.lockOff;
  p.touchScrollNatural = _ui.scrollNatural;
  p.ledBrightness = _ui.ledLevel ? _ui.ledLevel : 128;
  p.clock12 = _ui.clock12;
  p.notify = _ui.notifyMode <= 3 ? (model::NotifyMode)_ui.notifyMode : model::NotifyMode::Led;
  p.cpuSpeed = _ui.cpuSpeed < MC_CPU_LEVELS ? _ui.cpuSpeed : 0;
}

void AdapterBase::prefsToUi(const model::Prefs& p) {
  _ui.theme = p.theme;
  _ui.timeZone = p.timeZone;
  _ui.brightness = p.brightness;
  _ui.screenTimeoutS = p.screenTimeoutS;
  _ui.inputLockoutMs = p.inputLockoutMs;
  _ui.sound = p.sound;
  _ui.textSize = p.textSize > 2 ? 2 : p.textSize;
  _ui.gpsTimeSyncAtBoot = p.gpsTimeSyncAtBoot;
  _ui.autoBackup = p.autoBackup;
  _ui.backupIdentity = p.backupIdentity;
  _ui.backupMessages = p.backupMessages;
  _ui.bleEnabled = p.bleEnabled;
  _ui.presetIdx = p.presetIdx;
  _ui.blePinRandom = p.blePinRandom;
  _ui.deviceMode = (uint8_t)p.deviceMode;
  _ui.pingBot = p.pingBot;
  _ui.notifyMode = (uint8_t)p.notify;
  _ui.shortPreview = p.shortPreview;
  _ui.vimKeys = p.vimKeys ? 2 : 1;
  _ui.trackballOff = !p.trackball;
  _ui.lockOff = !p.lockOnTimeout;
  _ui.scrollNatural = p.touchScrollNatural;
  _ui.ledLevel = p.ledBrightness;
  _ui.clock12 = p.clock12;
  _ui.cpuSpeed = p.cpuSpeed < MC_CPU_LEVELS ? p.cpuSpeed : 0;
}

void AdapterBase::setUiMuted(int slot, bool muted) {
  if (muted) _ui.mutedChannels |= (uint16_t)(1u << slot);
  else _ui.mutedChannels &= (uint16_t)~(1u << slot);
  saveUiPrefs();
}

// ------------------------------------------------------------------ contacts

int AdapterBase::contactIndex(const uint8_t* key, int len) const {
  ContactRef c;
  for (int i = 0, n = contactCount(); i < n; i++)
    if (contactAt(i, c) && !memcmp(c.key, key, len)) return i;
  return -1;
}

bool AdapterBase::contactName(const uint8_t* key, int len, char* out, int n) const {
  ContactRef c;
  int i = contactIndex(key, len);
  if (i < 0 || !contactAt(i, c)) return false;
  snprintf(out, n, "%s", c.name);
  return true;
}

// Favourites live in MeshCore's contact flags (bit 0), shared with phone apps.
bool AdapterBase::isFavourite(const uint8_t* key) const {
  ContactRef c;
  int i = contactIndex(key, 6);
  return i >= 0 && contactAt(i, c) && (c.flags & 1);
}

bool AdapterBase::contact(int i, model::Contact& out) const {
  ContactRef c;
  if (i < 0 || !contactAt(i, c)) return false;
  memset(&out, 0, sizeof out);
  snprintf(out.name, sizeof out.name, "%s", c.name);
  memcpy(out.key, c.key, 6);
  out.type = c.type <= ADV_TYPE_SENSOR ? (model::NodeType)c.type : model::NodeType::None;
  if (c.outPathLen == 0xFF) {
    out.hops = -1;
  } else {
    out.hops = (int8_t)(c.outPathLen & 63);
    int bytes = out.hops * ((c.outPathLen >> 6) + 1);
    out.pathLen = (uint8_t)(bytes < 16 ? bytes : 16);
    memcpy(out.path, c.outPath, out.pathLen);
  }
  out.favourite = c.flags & 1;
  out.telemetry = (uint8_t)((c.flags >> 1) & 7);   // flags: bit 0 favourite, bits 1-3 telemetry
  out.hasPos = c.lat || c.lon;
  out.lat_e6 = c.lat;
  out.lon_e6 = c.lon;
  out.lastHeard = c.lastmod;
  out.snr_x4 = INT8_MIN;
  if (const Signal* s = signalFor(c.key)) {
    out.snr_x4 = s->snr_x4;
    out.rssi = s->rssi;
    if (s->at > out.lastHeard) out.lastHeard = s->at;
  }
  return true;
}

void AdapterBase::noteSignal(const uint8_t* key, float snr, float rssi) {
  Signal* slot = nullptr;
  for (auto& s : _signals)
    if (!memcmp(s.key, key, 4)) { slot = &s; break; }
  if (!slot) {
    slot = &_signals[0];
    for (auto& s : _signals)
      if (s.at < slot->at) slot = &s;
    memcpy(slot->key, key, 4);
  }
  slot->snr_x4 = (int8_t)(snr * 4);
  slot->rssi = (int16_t)rssi;
  slot->at = now();
}

const AdapterBase::Signal* AdapterBase::signalFor(const uint8_t* key) const {
  for (auto& s : _signals)
    if (s.at && !memcmp(s.key, key, 4)) return &s;
  return nullptr;
}

// ------------------------------------------------------------------ threads

bool AdapterBase::threadKeyFor(model::ThreadId t, ThreadKey& k) const {
  if (t.kind == model::ThreadKind::Channel) {
    k = ThreadKey::channel((uint8_t)t.idx);
    return true;
  }
  ContactRef c;
  if (!contactAt(t.idx, c)) return false;
  k = ThreadKey::direct(c.key);
  return true;
}

int AdapterBase::threadCount() const {
  // Conversations with contacts or channels that were removed are hidden.
  int n = 0;
  char name[32];
  for (int i = 0; i < _msgs.threadCount(); i++) {
    const ThreadInfo* t = _msgs.thread(i);
    if (_msgs.hidden(t->key)) continue;   // kept off the chats list
    if (t->key.kind == 1 ? channelName(t->key.id[0], name, sizeof name) && name[0] : contactIndex(t->key.id) >= 0) n++;
  }
  return n;
}

bool AdapterBase::thread(int i, model::Thread& out) const {
  for (int k = 0; k < _msgs.threadCount(); k++) {
    const ThreadInfo* t = _msgs.thread(k);
    int ci = -1;
    char chName[32] = "";
    if (_msgs.hidden(t->key)) continue;
    if (t->key.kind == 0) {
      ci = contactIndex(t->key.id);
      if (ci < 0) continue;
    } else if (!channelName(t->key.id[0], chName, sizeof chName) || !chName[0]) {
      continue;
    }
    if (i-- != 0) continue;
    memset(&out, 0, sizeof out);
    out.lastTs = t->lastTs;
    out.unread = t->unread;
    snprintf(out.preview, sizeof out.preview, "%s%s", t->lastOut ? "You: " : "", t->preview);
    if (t->key.kind == 1) {
      out.id = {model::ThreadKind::Channel, t->key.id[0]};
      snprintf(out.title, sizeof out.title, "%s", chName);
      out.muted = uiMuted(t->key.id[0]);
    } else {
      ContactRef c;
      contactAt(ci, c);
      out.id = {model::ThreadKind::Direct, (uint16_t)ci};
      snprintf(out.title, sizeof out.title, "%s", c.name);
      out.type = c.type <= ADV_TYPE_SENSOR ? (model::NodeType)c.type : model::NodeType::None;
      out.favourite = c.flags & 1;
    }
    return true;
  }
  return false;
}

int AdapterBase::messageCount(model::ThreadId t) const {
  ThreadKey k;
  if (!threadKeyFor(t, k)) return 0;
  // An empty window for a conversation that has messages (a read that failed): try again, once
  // per change, instead of staying empty until another chat is opened.
  bool empty = _hasOpen && _openKey == k && !_msgs.count() && _openRetry != _rev;
  if (empty) {
    const ThreadInfo* ti = _msgs.find(k);
    empty = ti && ti->lastSeq;
  }
  if (!_hasOpen || !(_openKey == k) || empty) {
    _openRetry = _rev;
    _msgs.open(k);
    _openKey = k;
    _hasOpen = true;
  }
  return _msgs.count();
}

bool AdapterBase::message(model::ThreadId t, int i, model::Message& out) const {
  int n = messageCount(t);
  if (i < 0 || i >= n) return false;
  out = _msgs.at(i);
  return true;
}

int AdapterBase::loadOlder(model::ThreadId t) {
  messageCount(t);   // make sure the window is on this conversation
  int n = _msgs.older();
  if (n) _rev++;
  return n;
}

int AdapterBase::loadNewer(model::ThreadId t) {
  messageCount(t);
  int n = _msgs.newer();
  if (n) _rev++;
  return n;
}

int AdapterBase::history(model::ThreadId t, uint32_t upTo, model::Message* out, int max) {
  ThreadKey k;
  if (!threadKeyFor(t, k)) return 0;
  return _msgs.history(k, upTo, out, max);
}

int AdapterBase::historyAround(model::ThreadId t, uint32_t seq, int before, int after, model::Message* out, int max) {
  ThreadKey k;
  if (!threadKeyFor(t, k)) return 0;
  return _msgs.around(k, seq, before, after, out, max);
}

bool AdapterBase::atNewest(model::ThreadId t) const {
  messageCount(t);
  return _msgs.atNewest();
}

void AdapterBase::clearHistory(model::ThreadId t) {
  ThreadKey k;
  if (!threadKeyFor(t, k)) return;
  _msgs.clear(k);
  _hasOpen = false;
  _rev++;
}

bool AdapterBase::unreadSince(model::ThreadId t, uint32_t& readSeq) const {
  ThreadKey k;
  if (!threadKeyFor(t, k)) return false;
  const ThreadInfo* ti = _msgs.find(k);
  if (!ti || !ti->unread) return false;
  readSeq = ti->readSeq;
  return true;
}

bool AdapterBase::threadHidden(model::ThreadId t) const {
  ThreadKey k;
  return threadKeyFor(t, k) && _msgs.hidden(k);
}

void AdapterBase::setThreadHidden(model::ThreadId t, bool on) {
  ThreadKey k;
  if (!threadKeyFor(t, k)) return;
  _msgs.setHidden(k, on);
  if (on && _msgs.find(k)) _msgs.markRead(k);
  if (_ui.backupMessages) configChanged();
  _rev++;
}

uint16_t AdapterBase::unreadIn(model::ThreadId t) const {
  ThreadKey k;
  const ThreadInfo* ti = threadKeyFor(t, k) ? _msgs.find(k) : nullptr;
  return ti ? ti->unread : 0;
}

bool AdapterBase::lastMessage(model::ThreadId t, char* out, int n) const {
  ThreadKey k;
  return threadKeyFor(t, k) && _msgs.lastText(k, out, n);
}

void AdapterBase::markRead(model::ThreadId t) {
  ThreadKey k;
  if (!threadKeyFor(t, k)) return;
  const ThreadInfo* ti = _msgs.find(k);
  if (ti && ti->unread) {
    _msgs.markRead(k);
    _rev++;
  }
}

uint16_t AdapterBase::unreadTotal() const {
  uint32_t n = 0;
  for (int i = 0; i < _msgs.threadCount(); i++) {
    const ThreadInfo* t = _msgs.thread(i);
    // Muted channels keep a count of their own (Channels tab), but don't
    // alert; neither do conversations kept off the chats list.
    if ((t->key.kind == 1 && uiMuted(t->key.id[0])) || _msgs.hidden(t->key)) continue;
    n += t->unread;
  }
  return n > 0xFFFF ? 0xFFFF : (uint16_t)n;
}

void AdapterBase::storeDirect(const uint8_t* key, const model::Message& m) {
  ThreadKey k = ThreadKey::direct(key);
  _msgs.setHidden(k, false);   // a new message brings a hidden conversation back
  _msgs.append(k, m);
  if (_ui.backupMessages) configChanged();
  _rev++;
}

void AdapterBase::storeChannel(uint8_t slot, const model::Message& m) {
  ThreadKey k = ThreadKey::channel(slot);
  // An unmuted channel kept off the chats list comes back with a new post; a
  // muted one stays off, counting its posts unread quietly.
  if (!uiMuted(slot)) _msgs.setHidden(k, false);
  _msgs.append(k, m);
  if (_ui.backupMessages) configChanged();
  _rev++;
}

// ------------------------------------------------------------------ sending

uint32_t AdapterBase::appendOutgoing(const ThreadKey& k, model::Message& m) {
  // Sending from an older part of the conversation: go back to the newest
  // messages, so what is sent shows up.
  if (_hasOpen && _openKey == k && !_msgs.atNewest()) _msgs.open(_openKey);
  return _msgs.append(k, m);
}

AdapterBase::Pending* AdapterBase::newPending() {
  for (auto& q : _pending)
    if (!q.used) return &q;
  return nullptr;   // too many messages waiting for ACKs
}

void AdapterBase::ackReceived(uint32_t a) {
  for (auto& p : _pending) {
    if (!p.used || (a != p.ack && a != p.prevAck)) continue;
    _msgs.setStatus(p.seq, model::Delivery::Delivered, p.attempt);
    p.used = false;
    _rev++;
  }
}

void AdapterBase::retryPending(uint32_t ms) {
  for (auto& p : _pending) {
    if (!p.used || ms - p.sentAt < p.timeout) continue;
    if (p.phoneOwned) {   // the phone retries (and a retry or late ACK revives it); we only say it failed
      _msgs.setStatus(p.seq, model::Delivery::Failed, p.attempt);
      p.used = false;
      _rev++;
      continue;
    }
    int ci = contactIndex(p.key);
    ContactRef c;
    bool have = ci >= 0 && contactAt(ci, c);
    char text[model::TEXT_LEN];
    p.attempt++;
    bool more = have && p.attempt < MAX_DM_ATTEMPTS + (c.outPathLen != 0xFF ? 1 : 0);
    if (more && p.attempt == MAX_DM_ATTEMPTS) {
      // The stored route seems broken: forget it and flood one last time.
      forgetRoute(c.key);
      c.outPathLen = 0xFF;
    }
    if (more && _msgs.text(p.seq, text, sizeof text) && sendAttempt(p, c, text)) {
      _msgs.setStatus(p.seq, model::Delivery::Pending, p.attempt);
    } else {
      _msgs.setStatus(p.seq, model::Delivery::Failed, p.attempt);
      p.used = false;
    }
    _rev++;
  }
}

void AdapterBase::watchRepeats(uint32_t seq, const uint8_t* hash) {
  // Listen for repeats of it for a minute (the oldest entry is reused).
  SentChannelMsg* slot = &_sentChan[0];
  for (auto& sc : _sentChan)
    if (sc.until < slot->until) slot = &sc;
  slot->seq = seq;
  slot->until = millis() + 60000;
  slot->repeats = 0;
  memcpy(slot->hash, hash, 4);
}

// ------------------------------------------------------------------ receiving

void AdapterBase::rawRx(float snr, float rssi, const uint8_t* raw, int len, const uint8_t* selfKey) {
  _lastSnr = (int8_t)(snr * 4);
  _lastRssi = (int16_t)rssi;
  mesh::Packet pkt;
  if (len <= 0 || len > 255 || !pkt.readFrom(raw, (uint8_t)len)) return;

  uint8_t hash[MAX_HASH_SIZE];
  pkt.calculatePacketHash(hash);
  bool dup = false;
  for (int i = 0; i < _nLog; i++)
    if (!memcmp(_logHash[i], hash, 4)) { dup = true; break; }
  if (dup) _dupsSeen++;

  model::RxLogEntry& e = _log[_logHead];
  memcpy(_logHash[_logHead], hash, 4);
  e.ms = millis();
  e.snr_x4 = _lastSnr;
  e.rssi = _lastRssi;
  e.payloadType = pkt.getPayloadType();
  e.routeType = pkt.getRouteType();
  e.pathLen = pkt.getPathHashCount();
  e.len = (uint8_t)len;
  e.dup = dup;
  uint8_t t = e.payloadType;
  e.srcHash = pkt.payload_len > 1 && (t == PAYLOAD_TYPE_TXT_MSG || t == PAYLOAD_TYPE_REQ || t == PAYLOAD_TYPE_RESPONSE ||
                                      t == PAYLOAD_TYPE_PATH)
                  ? pkt.payload[1]
                  : pkt.payload_len ? pkt.payload[0] : 0;
  _logHead = (_logHead + 1) % MAX_LOG;
  if (_nLog < MAX_LOG) _nLog++;

  if (t == PAYLOAD_TYPE_ADVERT && !dup) {
    // ADVERT payload: pub_key, timestamp, signature, app data.
    const int hdr = PUB_KEY_SIZE + 4 + SIGNATURE_SIZE;
    if (pkt.payload_len > hdr && !(selfKey && !memcmp(pkt.payload, selfKey, PUB_KEY_SIZE))) {
      AdvertDataParser parser(&pkt.payload[hdr], (uint8_t)(pkt.payload_len - hdr));
      if (parser.isValid()) {
        Heard h;
        memset(&h, 0, sizeof h);
        memcpy(h.pubkey, pkt.payload, PUB_KEY_SIZE);
        snprintf(h.name, sizeof h.name, "%s", parser.hasName() ? parser.getName() : "");
        h.type = parser.getType();
        h.hops = pkt.isRouteFlood() ? (int8_t)pkt.getPathHashCount() : 0;
        h.snr_x4 = (int8_t)(snr * 4);
        h.rssi = (int16_t)rssi;
        h.at = now();
        h.hasPos = parser.hasLatLon();
        h.lat = parser.getIntLat();
        h.lon = parser.getIntLon();
        if (h.hops == 0) noteSignal(h.pubkey, snr, rssi);
        putHeard(h);
      }
    }
  }
  if (t == PAYLOAD_TYPE_GRP_TXT) {
    for (auto& sc : _sentChan)
      if (sc.seq && (int32_t)(sc.until - e.ms) > 0 && !memcmp(sc.hash, hash, 4) && sc.repeats < 255) {
        _msgs.setRepeats(sc.seq, ++sc.repeats);   // "heard N repeats"
      }
  }
  _rev++;
}

bool AdapterBase::rxLog(int i, model::RxLogEntry& out) const {
  if (i < 0 || i >= _nLog) return false;
  out = _log[(_logHead - 1 - i + MAX_LOG) % MAX_LOG];
  return true;
}

void AdapterBase::putHeard(const Heard& h) {
  int slot = -1;
  for (int i = 0; i < _nHeard; i++)
    if (!memcmp(_heard[i].pubkey, h.pubkey, PUB_KEY_SIZE)) { slot = i; break; }
  if (slot < 0) {
    if (_nHeard < MAX_HEARD) slot = _nHeard++;
    else {
      slot = 0;
      for (int i = 1; i < _nHeard; i++)
        if (_heard[i].at < _heard[slot].at) slot = i;
    }
  } else if (!h.name[0]) {
    // A discovery reply carries no name: keep the one from the advert.
    Heard keep = _heard[slot];
    _heard[slot] = h;
    memcpy(_heard[slot].name, keep.name, sizeof keep.name);
    _heard[slot].hasPos = keep.hasPos;
    _heard[slot].lat = keep.lat;
    _heard[slot].lon = keep.lon;
    goto sort;
  }
  _heard[slot] = h;
sort:
  // Most recent first.
  for (int i = 1; i < _nHeard; i++) {
    Heard v = _heard[i];
    int j = i - 1;
    while (j >= 0 && _heard[j].at < v.at) { _heard[j + 1] = _heard[j]; j--; }
    _heard[j + 1] = v;
  }
}

bool AdapterBase::heard(int i, model::Contact& out) const {
  if (i < 0 || i >= _nHeard) return false;
  const Heard& h = _heard[i];
  memset(&out, 0, sizeof out);
  if (h.name[0]) snprintf(out.name, sizeof out.name, "%s", h.name);
  else if (!contactName(h.pubkey, PUB_KEY_SIZE, out.name, sizeof out.name)) hexKey(out.name, sizeof out.name, h.pubkey, 4);
  memcpy(out.key, h.pubkey, 6);
  out.type = h.type <= ADV_TYPE_SENSOR ? (model::NodeType)h.type : model::NodeType::None;
  out.hops = h.hops;
  out.snr_x4 = h.snr_x4;
  out.rssi = h.rssi;
  out.lastHeard = h.at;
  out.hasPos = h.hasPos;
  out.lat_e6 = h.lat;
  out.lon_e6 = h.lon;
  return true;
}

void AdapterBase::startDiscover(uint32_t tag, uint32_t listenMs) {
  _discoverTag = tag;
  _discoverState = model::Req::Busy;
  _discoverUntil = millis() + listenMs;
  _rev++;
}

void AdapterBase::discoverReply(float snr, float rssi, const uint8_t* p, int len) {
  if (len < 6 + PUB_KEY_SIZE || (p[0] & 0xF0) != CTL_DISCOVER_RESP) return;
  if (_discoverState != model::Req::Busy || rd32(&p[2]) != _discoverTag) return;
  Heard h;
  memset(&h, 0, sizeof h);
  memcpy(h.pubkey, &p[6], PUB_KEY_SIZE);
  h.type = p[0] & 0x0F;
  h.hops = 0;
  h.snr_x4 = (int8_t)(snr * 4);
  h.rssi = (int16_t)rssi;
  h.at = now();
  noteSignal(h.pubkey, snr, rssi);
  putHeard(h);
  _rev++;
}

// ------------------------------------------------------------------ requests

bool AdapterBase::startRequest(ReqKind k, int contact, uint32_t est) {
  ContactRef c;
  if (!contactAt(contact, c)) return false;
  _req.kind = k;
  _req.state = model::Req::Busy;
  memcpy(_req.key, c.key, PUB_KEY_SIZE);
  _req.startedAt = millis();
  _req.deadline = millis() + est + 4000;
  _req.text[0] = 0;
  _rev++;
  return true;
}

void AdapterBase::finishRequest(model::Req st, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(_req.text, sizeof _req.text, fmt, ap);
  va_end(ap);
  _req.state = st;
  _req.rttMs = millis() - _req.startedAt;
  _rev++;
}

void AdapterBase::requestTick(uint32_t ms) {
  if (_req.state == model::Req::Busy && (int32_t)(ms - _req.deadline) >= 0) {
    _req.state = model::Req::Timeout;
    snprintf(_req.text, sizeof _req.text, _req.kind == ReqKind::Ping ? "No reply (it may not share telemetry)" : "No reply");
    _rev++;
  }
  if (_discoverState == model::Req::Busy && (int32_t)(ms - _discoverUntil) >= 0) {
    _discoverState = model::Req::Ok;
    _rev++;
  }
}

bool AdapterBase::loggedIn(int ci) const {
  ContactRef c;
  if (!contactAt(ci, c)) return false;
  for (auto& k : _loggedIn)
    if (!memcmp(k, c.key, 6)) return true;
  return false;
}

const AdapterBase::SavedPassword* AdapterBase::passwordFor(const uint8_t* key) const {
  for (auto& p : _passwords)
    if (p.pw[0] && !memcmp(p.key, key, 6)) return &p;
  return nullptr;
}

bool AdapterBase::hasSavedPassword(int ci) const {
  ContactRef c;
  return contactAt(ci, c) && passwordFor(c.key);
}

bool AdapterBase::loginSaved(int ci) {
  ContactRef c;
  if (!contactAt(ci, c)) return false;
  const SavedPassword* p = passwordFor(c.key);
  char pw[16];
  snprintf(pw, sizeof pw, "%s", p ? p->pw : "");
  return login(ci, pw);
}

void AdapterBase::forgetPassword(int ci) {
  ContactRef c;
  if (!contactAt(ci, c)) return;
  for (auto& p : _passwords)
    if (!memcmp(p.key, c.key, 6)) memset(&p, 0, sizeof p);
  savePasswords();
  _rev++;
}

// Passwords that worked, so logins can be automatic. On internal flash only;
// they are not included in SD backups.
void AdapterBase::loadPasswords() {
  memset(_passwords, 0, sizeof _passwords);
  File f = _fs->open("/ui/pw.bin", "r");
  if (!f) return;
  f.read((uint8_t*)_passwords, sizeof _passwords);
  f.close();
}

void AdapterBase::savePasswords() {
  File f = _fs->open("/ui/pw.bin", "w", true);
  if (!f) return;
  f.write((const uint8_t*)_passwords, sizeof _passwords);
  f.close();
}

void AdapterBase::loginResult(const uint8_t* key, bool ok, bool admin, uint16_t keepAliveS) {
  if (!ok) {
    finishRequest(model::Req::Failed, "Login refused: wrong password?");
    memset(_req.password, 0, sizeof _req.password);
    return;
  }
  if (keepAliveS) onLoggedIn(key, keepAliveS);
  finishRequest(model::Req::Ok, admin ? "Logged in as admin" : "Logged in");
  uint8_t (*slot)[6] = &_loggedIn[0];
  for (auto& k : _loggedIn)
    if (!memcmp(k, key, 6) || !k[0]) { slot = &k; break; }
  memcpy(*slot, key, 6);
  if (_req.password[0]) {
    SavedPassword* p = const_cast<SavedPassword*>(passwordFor(key));
    for (auto& q : _passwords)
      if (!p && !q.pw[0]) p = &q;
    if (!p) p = &_passwords[0];
    memcpy(p->key, key, 6);
    snprintf(p->pw, sizeof p->pw, "%s", _req.password);
    savePasswords();
  }
  memset(_req.password, 0, sizeof _req.password);
}

void AdapterBase::statusReply(const uint8_t* s, int len) {
  if (_req.kind == ReqKind::Ping) { finishRequest(model::Req::Ok, "pong"); return; }
  if (len < 56) { finishRequest(model::Req::Failed, "Short status reply"); return; }
  // simple_repeater's RepeaterStats
  _remote.valid = true;
  _remote.battMv = rd16(s + 0);
  _remote.queueLen = rd16(s + 2);
  _remote.noiseFloor = (int16_t)rd16(s + 4);
  _remote.lastRssi = (int16_t)rd16(s + 6);
  _remote.rxPackets = rd32(s + 8);
  _remote.txPackets = rd32(s + 12);
  _remote.airTxS = rd32(s + 16);
  _remote.uptimeS = rd32(s + 20);
  _remote.floodsSent = rd32(s + 24);
  _remote.directSent = rd32(s + 28);
  _remote.lastSnr_x4 = (int8_t)(int16_t)rd16(s + 42);
  _remote.dupsDirect = rd16(s + 44);
  _remote.dupsFlood = rd16(s + 46);
  _remote.airRxS = rd32(s + 48);
  finishRequest(model::Req::Ok, "ok");
}

void AdapterBase::neighboursReply(const uint8_t* data, int len) {
  // count (2), then per neighbour: key prefix (4), heard ago (4), snr (1)
  if (len < 4) return;
  int count = rd16(data + 2);
  const uint8_t* e = data + 4;
  _nNeighbours = 0;
  for (int i = 0; i < count && _nNeighbours < MAX_NEIGH && e + 9 <= data + len; i++, e += 9) {
    model::Neighbour& n = _neigh[_nNeighbours++];
    memset(&n, 0, sizeof n);
    memcpy(n.key, e, 4);
    n.heardAgoS = rd32(e + 4);
    n.snr_x4 = (int8_t)e[8];
    contactName(e, 4, n.name, sizeof n.name);
  }
  finishRequest(model::Req::Ok, "%d neighbours", _nNeighbours);
}

void AdapterBase::traceReply(uint8_t flags, const uint8_t* hashes, const uint8_t* snrs, uint8_t path_len,
                             float final_snr) {
  if (_req.kind == ReqKind::Ping) { finishRequest(model::Req::Ok, "pong"); return; }
  int sz = 1 << (flags & 3);
  int hops = path_len / sz;
  char out[200];
  int o = snprintf(out, sizeof out, "Round trip %u ms, %d relay%s\n", (unsigned)(millis() - _req.startedAt), hops,
                   hops == 1 ? "" : "s");
  for (int h = 0; h < hops && o < (int)sizeof out - 40; h++) {
    char id[12], name[24] = "", cn[32];
    hexKey(id, sizeof id, &hashes[h * sz], sz);
    if (contactName(&hashes[h * sz], sz, cn, sizeof cn)) snprintf(name, sizeof name, " %.18s", cn);
    int s = (int8_t)snrs[h];
    o += snprintf(out + o, sizeof out - o, "> %s%s  snr %d.%d\n", id, name, s / 4, (s < 0 ? -s : s) % 4 * 25 / 10);
  }
  snprintf(out + o, sizeof out - o, "back to us  snr %.1f", final_snr);
  finishRequest(model::Req::Ok, "%s", out);
}

void AdapterBase::pathReply(uint8_t in_len, uint8_t out_len) {
  finishRequest(model::Req::Ok, "Reply in %u ms: %d hop%s there, %d back", (unsigned)(millis() - _req.startedAt),
                out_len & 63, (out_len & 63) == 1 ? "" : "s", in_len & 63);
}

bool AdapterBase::neighbour(int i, model::Neighbour& out) const {
  if (i < 0 || i >= _nNeighbours) return false;
  out = _neigh[i];
  return true;
}

// Cayenne LPP: channel, type, big-endian value. Only common types are named.
void AdapterBase::decodeTelemetry(const uint8_t* p, int len) {
  char out[200];
  int o = 0;
  out[0] = 0;
  for (int i = 0; i + 2 <= len && o < (int)sizeof out - 40;) {
    uint8_t ch = p[i], type = p[i + 1];
    i += 2;
    auto be = [&](int n) { int32_t v = 0; for (int k = 0; k < n; k++) v = (v << 8) | p[i + k]; return v; };
    const char* label = nullptr;
    int size = 0;
    double val = 0;
    const char* unit = "";
    switch (type) {
      case 116: label = "Voltage"; size = 2; val = (uint16_t)be(2) / 100.0; unit = " V"; break;
      case 103: label = "Temperature"; size = 2; val = (int16_t)be(2) / 10.0; unit = " C"; break;
      case 104: label = "Humidity"; size = 1; val = be(1) / 2.0; unit = " %"; break;
      case 115: label = "Pressure"; size = 2; val = (uint16_t)be(2) / 10.0; unit = " hPa"; break;
      case 117: label = "Current"; size = 2; val = (uint16_t)be(2) / 1000.0; unit = " A"; break;
      case 128: label = "Power"; size = 2; val = (uint16_t)be(2); unit = " W"; break;
      case 2: label = "Analog"; size = 2; val = (int16_t)be(2) / 100.0; break;
      case 136: {   // GPS: lat, lon (0.0001 deg), alt (0.01 m), 3 bytes each
        if (i + 9 > len) { i = len; break; }
        int32_t la = be(3), lo;
        if (la & 0x800000) la |= ~0xFFFFFF;
        i += 3;
        lo = be(3);
        if (lo & 0x800000) lo |= ~0xFFFFFF;
        i += 6;
        o += snprintf(out + o, sizeof out - o, "Position %.4f, %.4f\n", la / 10000.0, lo / 10000.0);
        continue;
      }
      default: i = len; continue;   // unknown size: stop
    }
    if (i + size > len) break;
    i += size;
    o += snprintf(out + o, sizeof out - o, "%s%s: %.2f%s\n", ch > 1 ? "  " : "", label, val, unit);
  }
  finishRequest(model::Req::Ok, "%s", o ? out : "No telemetry shared");
}

// ------------------------------------------------------------------ radio presets

int AdapterBase::presetCount() const { return kRadioPresetCount; }
const model::Preset& AdapterBase::preset(int i) const { return kRadioPresets[i]; }

// ------------------------------------------------------------------ GPS

bool AdapterBase::gpsSyncTime() {
  model::Gps g = _plat.gps();
  if (!g.fix || !g.lastFixS) return false;
  setClock(g.lastFixS);
  _rev++;
  return true;
}

void AdapterBase::gpsTick(uint32_t ms) {
  // "Set clock from GPS at boot": run the receiver until the first fix (at
  // most 10 minutes) even if GPS is otherwise off.
  if (_bootGpsPending) {
    _bootGpsPending = false;
    _bootGpsUntil = ms + 600000UL;
    if (!_prefs.gpsEnabled) _plat.setGpsPower(true);
  }
  if (_bootGpsUntil) {
    model::Gps g = _plat.gps();
    bool done = g.fix && g.lastFixS;
    if (done) gpsSyncTime();
    if (done || (int32_t)(ms - _bootGpsUntil) >= 0) {
      _bootGpsUntil = 0;
      if (!_prefs.gpsEnabled) _plat.setGpsPower(false);
    }
  }
}

// ------------------------------------------------------------------ backup

void AdapterBase::refreshBackupInfo() {
  fs::FS* sd = _plat.sd();
  _bk.card = sd != nullptr;
  memset(&_bk.cur, 0, sizeof _bk.cur);
  memset(&_bk.prev, 0, sizeof _bk.prev);
  if (sd) {
    _bk.cur = Backup::infoAt(*sd, backupPath(false));
    _bk.prev = Backup::infoAt(*sd, backupPath(true));
  }
  _rev++;
}

model::BackupStatus AdapterBase::backupStatus() const {
  model::BackupStatus b;
  memset(&b, 0, sizeof b);
  b.cardPresent = _bk.card;
  b.hasBackup = _bk.cur.valid;
  b.hasPrevious = _bk.prev.valid;
  b.lastBackupTs = _bk.cur.valid ? _bk.cur.created : 0;
  snprintf(b.lastError, sizeof b.lastError, "%s", _bk.error);
  return b;
}

void AdapterBase::configChanged() {
  _cfgChangedAt = millis() | 1;
  _rev++;
}

void AdapterBase::storageTick(uint32_t ms) {
  // Card inserted or removed: refresh what the Backup screen shows, and
  // where old messages are archived.
  fs::FS* sd = _plat.sd();
  if ((sd != nullptr) != _bk.card) refreshBackupInfo();
  _msgs.setArchive(sd);
  // Automatic backup: 10 s after the last change, at most once a minute.
  if (_ui.autoBackup && _cfgChangedAt && _bk.card && ms - _cfgChangedAt > 10000 &&
      (!_lastBackupAt || ms - _lastBackupAt > 60000))
    backupNow();
}

}  // namespace app
