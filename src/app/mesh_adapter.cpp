#include "app/mesh_adapter.h"
#include "app/ping_bot.h"
#include "app/radio_presets.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace app {

namespace {

constexpr uint8_t REQ_GET_STATUS = 0x01;
constexpr uint8_t REQ_GET_TELEMETRY = 0x03;
constexpr uint8_t REQ_GET_NEIGHBOURS = 0x06;
constexpr uint8_t CTL_DISCOVER_REQ = 0x80;
constexpr uint32_t DISCOVER_LISTEN_MS = 8000;

int8_t hopsOf(uint8_t path_len) {
  if (path_len == 0xFF) return -1;           // came by a known direct route
  return (int8_t)(path_len & 63);
}

uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

void toRef(const ContactInfo& c, ContactRef& r) {
  memcpy(r.key, c.id.pub_key, PUB_KEY_SIZE);
  snprintf(r.name, sizeof r.name, "%s", c.name);
  r.type = c.type;
  r.flags = c.flags;
  r.outPathLen = c.out_path_len;
  memcpy(r.outPath, c.out_path, sizeof r.outPath);
  r.lastmod = c.lastmod;
  r.lat = c.gps_lat;
  r.lon = c.gps_lon;
}

}  // namespace

// ------------------------------------------------------------------ setup

void MeshAdapter::begin(MyMesh& mesh, DataStore& store, mesh::RTCClock& rtc, fs::FS& fs, bool bleBuilt, bool freshInstall) {
  _mesh = &mesh;
  _store = &store;
  _rtc = &rtc;
  _bleBuilt = bleBuilt;
  loadUiPrefs(fs);
  fs::FS* card = _plat.sd();
  _msgs.setArchive(card);   // before begin: a ring of another size moves its extra records there
#ifdef MC_HISTORY_ON_SD
  // With a card the history lives on it and the flash is left alone; without, in the flash.
  if (card) _msgs.begin(card, nullptr, &fs);
  else _msgs.begin(&fs);
#else
  _msgs.begin(&fs);
#endif
  loadPasswords();
  syncPrefsFromMesh();
  _fresh = freshInstall;
  // Favourites used to be kept here: move them to MeshCore's contact flag.
  if (_ui.nFavourites) {
    for (int i = 0; i < _ui.nFavourites && i < 16; i++) {
      ContactInfo* c = _mesh->lookupContactByPubKey(_ui.favourites[i], 4);
      if (c) c->flags |= 1;
    }
    _ui.nFavourites = 0;
    saveUiPrefs();
    _mesh->uiMarkContactsDirty();
  }
  _seenContacts = _mesh->getNumContacts();
  // "Set clock from GPS at boot": started from the first loop(): the sensor
  // manager starts after this and switches GPS to its saved setting.
  startBootGps();
  refreshBackupInfo();
  _rev++;
}

void MeshAdapter::syncPrefsFromMesh() {
  NodePrefs* np = _mesh->getNodePrefs();
  model::Prefs& p = _prefs;
  uiToPrefs();
  snprintf(p.nodeName, sizeof p.nodeName, "%s", np->node_name);
  p.gpsEnabled = np->gps_enabled;
  p.gpsShareInAdvert = np->advert_loc_policy != ADVERT_LOC_NONE;
  p.autoAddContacts = !np->manual_add_contacts;
  p.blePin = np->ble_pin ? np->ble_pin : _mesh->getBLEPin();
  auto mode = [](uint8_t m) { return m == TELEM_MODE_ALLOW_ALL ? model::TelemMode::Everyone
                                    : m == TELEM_MODE_ALLOW_FLAGS ? model::TelemMode::PerContact : model::TelemMode::NoOne; };
  p.telemBattery = mode(np->telemetry_mode_base);
  p.telemLocation = mode(np->telemetry_mode_loc);
  p.telemSensors = mode(np->telemetry_mode_env);
}

void MeshAdapter::setPrefs(const model::Prefs& p) {
  NodePrefs* np = _mesh->getNodePrefs();
  snprintf(np->node_name, sizeof np->node_name, "%s", p.nodeName);
  np->gps_enabled = p.gpsEnabled;
  np->advert_loc_policy = p.gpsShareInAdvert ? ADVERT_LOC_SHARE : ADVERT_LOC_NONE;
  np->manual_add_contacts = !p.autoAddContacts;
  np->ble_pin = p.blePin;
  auto mode = [](model::TelemMode m) { return (uint8_t)(m == model::TelemMode::Everyone ? TELEM_MODE_ALLOW_ALL
                                              : m == model::TelemMode::PerContact ? TELEM_MODE_ALLOW_FLAGS : TELEM_MODE_DENY); };
  np->telemetry_mode_base = mode(p.telemBattery);
  np->telemetry_mode_loc = mode(p.telemLocation);
  np->telemetry_mode_env = mode(p.telemSensors);
  if (p.presetIdx != _ui.presetIdx && p.presetIdx < kRadioPresetCount && kRadioPresets[p.presetIdx].pathHashSize)
    np->path_hash_mode = kRadioPresets[p.presetIdx].pathHashSize - 1;
  _mesh->savePrefs();

  if (_bleBuilt && (bool)_ui.bleEnabled != p.bleEnabled) {
    if (p.bleEnabled) enableBluetooth();
    else disableBluetooth();
  }
  if ((bool)_prefs.gpsEnabled != p.gpsEnabled) _plat.setGpsPower(p.gpsEnabled);

  // A new name is not advertised here: the UI tells the user to advertise;
  // we don't spend airtime unasked.
  prefsToUi(p);
  saveUiPrefs();
  syncPrefsFromMesh();
  configChanged();
  _rev++;
}

uint32_t MeshAdapter::choosePinForBoot() {
  if (_ui.blePinRandom) {
    uint32_t r;
    _mesh->getRNG()->random((uint8_t*)&r, sizeof r);
    _activePin = 100000 + r % 900000;
  } else {
    _activePin = _prefs.blePin ? _prefs.blePin : 123456;
  }
  _mesh->setBLEPin(_activePin);  // keep MeshCore's reported PIN in sync with the BLE interface
  return _activePin;
}

// ------------------------------------------------------------------ time

uint32_t MeshAdapter::now() const { return _rtc->getCurrentTime(); }
uint32_t MeshAdapter::uptimeMs() const { return millis(); }

void MeshAdapter::selfKey(uint8_t out[6]) const { memcpy(out, _mesh->self_id.pub_key, 6); }

// ------------------------------------------------------------------ contacts

bool MeshAdapter::contactAt(int i, ContactRef& r) const {
  ContactInfo c;
  if (!getContact(i, c)) return false;
  toRef(c, r);
  return true;
}

ContactInfo* MeshAdapter::contactPtr(int i) const {
  ContactInfo c;
  if (i < 0 || !getContact(i, c)) return nullptr;
  return _mesh->lookupContactByPubKey(c.id.pub_key, PUB_KEY_SIZE);
}

int MeshAdapter::contactCount() const { return _mesh->getNumContacts(); }

void MeshAdapter::setFavourite(int i, bool fav) {
  ContactInfo* p = contactPtr(i);
  if (!p || (bool)(p->flags & 1) == fav) return;
  p->flags = fav ? (p->flags | 1) : (p->flags & ~1);
  _mesh->uiMarkContactsDirty();
  configChanged();
  _rev++;
}

bool MeshAdapter::removeContact(int i) {
  ContactInfo* c = contactPtr(i);
  if (!c) return false;
  uint8_t key[PUB_KEY_SIZE];
  memcpy(key, c->id.pub_key, PUB_KEY_SIZE);
  if (!_mesh->removeContact(*c)) return false;
  _mesh->uiDeleteContactBlob(key);
  _mesh->uiMarkContactsDirty();
  _msgs.forget(ThreadKey::direct(key));
  configChanged();
  _rev++;
  return true;
}

void MeshAdapter::setTelemetry(int i, uint8_t telem) {
  ContactInfo* c = contactPtr(i);
  if (!c) return;
  uint8_t flags = (uint8_t)((c->flags & ~0x0E) | ((telem & 7) << 1));
  if (flags == c->flags) return;
  c->flags = flags;
  _mesh->uiMarkContactsDirty();
  configChanged();
  _rev++;
}

bool MeshAdapter::resetPath(int i) {
  ContactInfo* c = contactPtr(i);
  if (!c) return false;
  c->out_path_len = OUT_PATH_UNKNOWN;
  _mesh->uiMarkContactsDirty();
  configChanged();
  _rev++;
  return true;
}

void MeshAdapter::forgetRoute(const uint8_t* key) {
  ContactInfo* c = _mesh->lookupContactByPubKey(key, PUB_KEY_SIZE);
  if (!c) return;
  c->out_path_len = OUT_PATH_UNKNOWN;
  _mesh->uiMarkContactsDirty();
}

// ------------------------------------------------------------------ channels

int MeshAdapter::channelCount() const {
  int n = 0;
  ChannelDetails d;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++)
    if (_mesh->getChannel(i, d) && d.name[0]) n++;
  return n;
}

bool MeshAdapter::channelName(int slot, char* name, int n) const {
  ChannelDetails d;
  if (slot < 0 || slot >= MAX_GROUP_CHANNELS || !_mesh->getChannel(slot, d)) return false;
  snprintf(name, n, "%s", d.name);
  return true;
}

// List index -> channel slot (slots can have gaps after a channel is left).
static int slotOf(MyMesh* m, int i) {
  ChannelDetails d;
  for (int s = 0; s < MAX_GROUP_CHANNELS; s++)
    if (m->getChannel(s, d) && d.name[0] && i-- == 0) return s;
  return -1;
}

bool MeshAdapter::channel(int i, model::Channel& out) const {
  int s = slotOf(_mesh, i);
  ChannelDetails d;
  if (s < 0 || !_mesh->getChannel(s, d)) return false;
  memset(&out, 0, sizeof out);
  out.idx = (uint8_t)s;
  snprintf(out.name, sizeof out.name, "%s", d.name);
  out.hashtag = d.name[0] == '#';
  out.muted = uiMuted(s);
  return true;
}

static int freeSlot(MyMesh* m) {
  ChannelDetails d;
  for (int s = 0; s < MAX_GROUP_CHANNELS; s++)
    if (m->getChannel(s, d) && !d.name[0]) return s;
  return -1;
}

bool MeshAdapter::joinPrivate(const char* name, const uint8_t psk[16]) {
  int s = freeSlot(_mesh);
  if (s < 0 || !name[0]) return false;
  ChannelDetails d;
  memset(&d, 0, sizeof d);
  snprintf(d.name, sizeof d.name, "%s", name);
  memcpy(d.channel.secret, psk, 16);
  if (!_mesh->setChannel(s, d)) return false;
  _mesh->uiSaveChannels();
  setUiMuted(s, false);
  configChanged();
  _rev++;
  return true;
}

bool MeshAdapter::joinHashtag(const char* name) {
  // Hashtag channels derive their key from the name, as the MeshCore apps
  // do: the first 16 bytes of SHA-256 of "#name" (lower case).
  char n[model::NAME_LEN];
  int o = 0;
  if (name[0] != '#') n[o++] = '#';
  for (int i = 0; name[i] && o < (int)sizeof n - 1; i++) {
    char c = name[i];
    n[o++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  n[o] = 0;
  uint8_t key[16];
  mesh::Utils::sha256(key, sizeof key, (const uint8_t*)n, o);
  return joinPrivate(n, key);
}

bool MeshAdapter::createPrivate(const char* name) {
  uint8_t key[16];
  _mesh->getRNG()->random(key, sizeof key);
  return joinPrivate(name, key);
}

bool MeshAdapter::channelKey(int i, uint8_t psk[16]) const {
  int s = slotOf(_mesh, i);
  ChannelDetails d;
  if (s < 0 || !_mesh->getChannel(s, d)) return false;
  memcpy(psk, d.channel.secret, 16);
  return true;
}

bool MeshAdapter::removeChannel(int i) {
  int s = slotOf(_mesh, i);
  if (s <= 0) return false;   // slot 0 (Public) stays
  ChannelDetails d;
  memset(&d, 0, sizeof d);
  if (!_mesh->setChannel(s, d)) return false;
  _mesh->uiSaveChannels();
  _msgs.forget(ThreadKey::channel((uint8_t)s));
  configChanged();
  _rev++;
  return true;
}

void MeshAdapter::setMuted(int i, bool muted) {
  int s = slotOf(_mesh, i);
  if (s < 0) return;
  setUiMuted(s, muted);
  configChanged();
  _rev++;
}

// ------------------------------------------------------------------ sending

bool MeshAdapter::sendAttempt(Pending& p, const ContactRef& r, const char* text) {
  ContactInfo* c = _mesh->lookupContactByPubKey(r.key, PUB_KEY_SIZE);
  if (!c) return false;
  uint32_t ack = 0, timeout = 0;
  int res = _mesh->sendMessage(*c, p.ts, p.attempt, text, ack, timeout);
  if (res == MSG_SEND_FAILED) return false;
  p.prevAck = p.ack;
  p.ack = ack;
  p.sentAt = millis();
  p.timeout = timeout + 1000;
  return true;
}

bool MeshAdapter::send(model::ThreadId t, const char* text) {
  if (!text[0]) return false;
  messageCount(t);   // the window on this conversation
  uint32_t ts = _rtc->getCurrentTimeUnique();
  model::Message m;
  memset(&m, 0, sizeof m);
  m.ts = ts;
  m.outgoing = true;
  m.snr_x4 = INT8_MIN;
  snprintf(m.text, sizeof m.text, "%s", text);

  if (t.kind == model::ThreadKind::Channel) {
    ChannelDetails d;
    if (!_mesh->getChannel(t.idx, d) || !d.name[0]) return false;
    const char* name = _mesh->getNodePrefs()->node_name;
    uint8_t hash[MAX_HASH_SIZE];
    bool ok = _mesh->uiSendGroupMessage(ts, d.channel, name, text, (int)strlen(text), hash);
    m.status = ok ? model::Delivery::Sent : model::Delivery::Failed;
    uint32_t seq = appendOutgoing(ThreadKey::channel((uint8_t)t.idx), m);
    if (ok && seq) watchRepeats(seq, hash);
    _rev++;
    return ok;
  }

  ContactRef c;
  if (!contactAt(t.idx, c)) return false;
  return sendDirect(c, m);
}

// A DM, tracked until its ACK (retried if none). `m` holds the text and ts.
bool MeshAdapter::sendDirect(const ContactRef& c, model::Message& m) {
  Pending* p = newPending();
  if (!p) return false;

  const char* text = m.text;
  uint32_t ts = m.ts;
  m.status = model::Delivery::Pending;
  uint32_t seq = appendOutgoing(ThreadKey::direct(c.key), m);
  if (!seq) return false;
  memset(p, 0, sizeof *p);
  p->seq = seq;
  p->ts = ts;
  memcpy(p->key, c.key, 6);
  p->used = true;
  if (!sendAttempt(*p, c, text)) {
    p->used = false;
    _msgs.setStatus(seq, model::Delivery::Failed, 0);
  }
  _rev++;
  return true;
}

void MeshAdapter::onAckReceived(const uint8_t* ack) { ackReceived(rd32(ack)); }

// A message a phone app sent through the node: show it as ours. The phone retries a DM
// itself (same timestamp, a higher attempt), so a retry only renews the wait for its ACK.
void MeshAdapter::onSentMessage(const uint8_t* key, uint8_t channel_idx, uint8_t attempt, uint32_t ts,
                                uint32_t expected_ack, uint32_t est_timeout, const char* text) {
  if (!text[0]) return;
  model::Message m;
  memset(&m, 0, sizeof m);
  m.ts = ts;
  m.outgoing = true;
  m.snr_x4 = INT8_MIN;
  snprintf(m.text, sizeof m.text, "%s", text);

  if (!key) {   // a channel: no ACK to wait for
    messageCount(model::ThreadId{model::ThreadKind::Channel, channel_idx});
    m.status = model::Delivery::Sent;
    appendOutgoing(ThreadKey::channel(channel_idx), m);
    _rev++;
    return;
  }

  PhoneSent* known = nullptr;
  for (auto& ps : _phoneSent)
    if (ps.seq && ps.ts == ts && !memcmp(ps.key, key, 6)) known = &ps;
  uint32_t seq;
  if (known) {
    seq = known->seq;
  } else {
    m.status = model::Delivery::Pending;
    seq = appendOutgoing(ThreadKey::direct(key), m);
    if (!seq) return;
    PhoneSent& slot = _phoneSent[_phoneSentNext++ % 4];
    memcpy(slot.key, key, 6);
    slot.ts = ts;
    slot.seq = seq;
  }
  Pending* p = nullptr;
  for (auto& q : _pending)
    if (q.used && q.seq == seq) p = &q;
  if (!p) p = newPending();
  if (!p) return;
  if (p->used) p->prevAck = p->ack;
  else { memset(p, 0, sizeof *p); p->seq = seq; p->ts = ts; memcpy(p->key, key, 6); p->used = true; p->phoneOwned = true; }
  p->attempt = attempt;
  p->ack = expected_ack;
  p->sentAt = millis();
  p->timeout = est_timeout + 1000;
  if (known) _msgs.setStatus(seq, model::Delivery::Pending, attempt);
  _rev++;
}

void MeshAdapter::loop() {
  uint32_t ms = millis();
  retryPending(ms);
  requestTick(ms);
  gpsTick(ms);

  // A phone app can change MeshCore's prefs (name, GPS, PIN...) at any time:
  // pick up what it changed.
  if (ms - _prefsCheckAt >= 1000) {
    _prefsCheckAt = ms;
    model::Prefs before = _prefs;
    syncPrefsFromMesh();
    if (memcmp(&before, &_prefs, sizeof before) != 0) _rev++;
  }

  // Contacts MeshCore adds or drops by itself (auto-add) also count.
  int nc = _mesh->getNumContacts();
  if (nc != _seenContacts) {
    _seenContacts = nc;
    configChanged();
  }
  storageTick(ms);
  pingTick(ms);
}

// ------------------------------------------------------------------ receiving

// How far the sender's clock was from ours, when ours is set and the time was not taken from them.
void MeshAdapter::noteSenderClock(model::Message& m, uint32_t senderTs) {
  if (m.ts == senderTs || m.ts < 1700000000u) return;
  m.skewKnown = true;
  m.skew = (int32_t)(senderTs - m.ts);
}

void MeshAdapter::onDirectMessage(const ContactInfo& from, uint8_t txt_type, uint8_t path_len, float snr,
                                  uint32_t sender_ts, const uint8_t* extra, int extra_len, const char* text) {
  if (path_len == 0 || path_len == 0xFF) noteSignal(from.id.pub_key, snr, _lastRssi);
  if (txt_type == TXT_TYPE_CLI_DATA) {
    if (_req.state == model::Req::Busy && _req.kind == ReqKind::Cli && !memcmp(_req.key, from.id.pub_key, 6))
      finishRequest(model::Req::Ok, "%s", text);
    return;   // command replies are not conversation
  }
  model::Message m;
  memset(&m, 0, sizeof m);
  // A room's posts keep the time the room gave them; everything else gets the time it arrived.
  m.ts = txt_type == TXT_TYPE_SIGNED_PLAIN ? sender_ts : receivedAt(_rtc->getCurrentTime(), sender_ts);
  noteSenderClock(m, sender_ts);
  m.snr_x4 = (int8_t)(snr * 4);
  m.hops = hopsOf(path_len);
  attachPath(m, path_len);
  if (txt_type == TXT_TYPE_SIGNED_PLAIN && extra_len >= 4) {
    // Room server post: extra holds the author's key prefix.
    if (!contactName(extra, 4, m.sender, sizeof m.sender)) hexKey(m.sender, sizeof m.sender, extra, 4);
  }
  snprintf(m.text, sizeof m.text, "%s", text);
  storeDirect(from.id.pub_key, m);
  if (_ui.pingBot && txt_type == TXT_TYPE_PLAIN && isPing(text)) pingReceived(from, path_len, snr);
}

// The relays a message passed, from the packet heard just before it was decoded
// (MeshCore's hook has only the length); as many as the store keeps.
void MeshAdapter::attachPath(model::Message& m, uint8_t pathLen) {
  bool known = _rxPath.valid && _rxPath.pathLen == pathLen;
  _rxPath.valid = false;
  int size = (pathLen >> 6) + 1, hops = pathLen & 63;
  if (pathLen == 0xFF || hops == 0 || !known) return;
  m.pathSize = (uint8_t)size;
  m.pathN = (uint8_t)(hops * size > (int)sizeof m.path ? sizeof m.path / size : hops);
  memcpy(m.path, _rxPath.path, m.pathN * size);
}

void MeshAdapter::onChannelMessage(uint8_t idx, uint8_t path_len, float snr, uint32_t ts, const char* text) {
  model::Message m;
  memset(&m, 0, sizeof m);
  m.ts = receivedAt(_rtc->getCurrentTime(), ts);
  noteSenderClock(m, ts);
  m.snr_x4 = (int8_t)(snr * 4);
  m.hops = hopsOf(path_len);
  attachPath(m, path_len);
  // Channel payloads are "sender: text".
  const char* sep = strstr(text, ": ");
  if (sep && sep - text < (int)sizeof m.sender) {
    snprintf(m.sender, sizeof m.sender, "%.*s", (int)(sep - text), text);
    text = sep + 2;
  }
  snprintf(m.text, sizeof m.text, "%s", text);
  storeChannel(idx, m);
}

// ------------------------------------------------------------------ ping bot

namespace {

// A relay's name, if exactly one repeater or room contact has that hash.
bool findRelay(void* ctx, const uint8_t* hash, int size, char* out, int n) {
  MyMesh* mesh = (MyMesh*)ctx;
  ContactInfo c;
  const char* found = nullptr;
  char name[sizeof c.name];
  for (int i = 0; mesh->getContactByIdx(i, c); i++) {
    if ((c.type != ADV_TYPE_REPEATER && c.type != ADV_TYPE_ROOM) || memcmp(c.id.pub_key, hash, size)) continue;
    if (found) return false;   // two share it: can't tell which
    snprintf(name, sizeof name, "%s", c.name);
    found = name;
  }
  if (found) snprintf(out, n, "%s", found);
  return found != nullptr;
}

}  // namespace

bool MeshAdapter::relayName(const uint8_t* hash, int size, char* out, int n) const {
  return findRelay(_mesh, hash, size, out, n);
}

void MeshAdapter::pingReceived(const ContactInfo& from, uint8_t pathLen, float snr) {
  uint32_t ms = millis();
  // One answer per contact every few seconds, whatever it sends.
  PingAnswered* slot = &_answered[0];
  for (auto& a : _answered) {
    if (!memcmp(a.key, from.id.pub_key, 6)) {
      if (a.at && ms - a.at < PING_QUIET_MS) return;
      slot = &a;
      break;
    }
    if (a.at < slot->at) slot = &a;   // else reuse the oldest
  }
  memcpy(slot->key, from.id.pub_key, 6);
  slot->at = ms | 1;

  memcpy(_pong.key, from.id.pub_key, 6);
  if (pathLen == 0xFF) {
    // Sent by a direct route: the relays used up its path on the way. Ours
    // to the sender, reversed, is most likely the one it took.
    uint8_t rev[MAX_PATH_SIZE];
    uint8_t len = from.out_path_len;
    if (len != OUT_PATH_UNKNOWN) {
      int size = (len >> 6) + 1, hops = len & 63;
      if (hops * size > (int)sizeof rev) hops = sizeof rev / size;
      for (int h = 0; h < hops; h++) memcpy(rev + h * size, from.out_path + (hops - 1 - h) * size, size);
      len = (uint8_t)((len & 0xC0) | hops);
    }
    formatPong(_pong.text, sizeof _pong.text, len, rev, true, snr, _lastRssi, findRelay, _mesh);
  } else {
    // The path of this DM's packet, if the one just heard was it.
    bool known = _dmPath.valid && _dmPath.src == from.id.pub_key[0] && _dmPath.pathLen == pathLen;
    formatPong(_pong.text, sizeof _pong.text, pathLen, known ? _dmPath.path : nullptr, false, snr, _lastRssi,
               findRelay, _mesh);
  }
  _dmPath.valid = false;
  _pong.due = (ms + PING_DELAY_MS) | 1;   // after MeshCore has finished with the packet
}

void MeshAdapter::pingTick(uint32_t ms) {
  if (!_pong.due || (int32_t)(ms - _pong.due) < 0) return;
  _pong.due = 0;
  ContactInfo* c = _mesh->lookupContactByPubKey(_pong.key, 6);
  if (!c || !_ui.pingBot) return;
  ContactRef r;
  toRef(*c, r);
  model::Message m;
  memset(&m, 0, sizeof m);
  m.ts = _rtc->getCurrentTimeUnique();
  m.outgoing = true;
  m.snr_x4 = INT8_MIN;
  snprintf(m.text, sizeof m.text, "%s", _pong.text);
  sendDirect(r, m);
}

void MeshAdapter::onRawRx(float snr, float rssi, const uint8_t* raw, int len) {
  rawRx(snr, rssi, raw, len, _mesh->self_id.pub_key);
  // MeshCore hands a packet to us here before decrypting it: if it is a DM,
  // keep its path for the ping bot (the message hook has only its length).
  mesh::Packet pkt;
  bool ok = len > 0 && len <= 255 && pkt.readFrom(raw, (uint8_t)len);
  if (ok && (pkt.getPayloadType() == PAYLOAD_TYPE_TXT_MSG || pkt.getPayloadType() == PAYLOAD_TYPE_GRP_TXT)) {
    int n = pkt.getPathByteLen();
    if (n > (int)sizeof _rxPath.path) n = sizeof _rxPath.path;
    _rxPath.pathLen = pkt.path_len;
    memcpy(_rxPath.path, pkt.path, n);
    _rxPath.valid = true;
  }
  if (ok && pkt.getPayloadType() == PAYLOAD_TYPE_TXT_MSG && pkt.payload_len > 1) {
    int n = pkt.getPathByteLen();
    if (n > (int)sizeof _dmPath.path) n = sizeof _dmPath.path;
    _dmPath.src = pkt.payload[1];
    _dmPath.pathLen = pkt.path_len;
    memcpy(_dmPath.path, pkt.path, n);
    _dmPath.valid = true;
  }
}

bool MeshAdapter::addHeard(int i) {
  if (i < 0 || i >= _nHeard) return false;
  const Heard& h = _heard[i];
  if (_mesh->lookupContactByPubKey(h.pubkey, PUB_KEY_SIZE)) return false;
  ContactInfo c;
  memset(&c, 0, sizeof c);
  c.id = mesh::Identity(h.pubkey);
  if (h.name[0]) snprintf(c.name, sizeof c.name, "%s", h.name);
  else hexKey(c.name, sizeof c.name, h.pubkey, 4);
  c.type = h.type;
  c.out_path_len = OUT_PATH_UNKNOWN;
  c.lastmod = now();
  c.gps_lat = h.lat;
  c.gps_lon = h.lon;
  if (!_mesh->addContact(c)) return false;
  _mesh->uiMarkContactsDirty();
  configChanged();
  _rev++;
  return true;
}

bool MeshAdapter::advert(bool flood) {
  bool ok = _mesh->uiSendAdvert(flood);
  _rev++;
  return ok;
}

bool MeshAdapter::discoverRepeaters() {
  uint8_t data[10];
  data[0] = CTL_DISCOVER_REQ;
  data[1] = 1 << ADV_TYPE_REPEATER;
  _mesh->getRNG()->random(&data[2], 4);
  memset(&data[6], 0, 4);   // since: everything
  mesh::Packet* pkt = _mesh->createControlData(data, sizeof data);
  if (!pkt) return false;
  _mesh->sendZeroHop(pkt);
  startDiscover(rd32(&data[2]), DISCOVER_LISTEN_MS);
  return true;
}

void MeshAdapter::onControlData(float snr, float rssi, uint8_t, const uint8_t* p, int len) {
  discoverReply(snr, rssi, p, len);
}

// ------------------------------------------------------------------ requests

bool MeshAdapter::login(int ci, const char* password) {
  if (!requestIdle()) return false;
  ContactInfo* c = contactPtr(ci);
  uint32_t est = 0;
  if (!c || _mesh->sendLogin(*c, password, est) == MSG_SEND_FAILED) return false;
  if (!startRequest(ReqKind::Login, ci, est)) return false;
  snprintf(_req.password, sizeof _req.password, "%s", password);
  return true;
}

void MeshAdapter::onLoggedIn(const uint8_t* key, uint16_t keepAliveS) {
  // Keep the connection alive as the app's login does, so a room keeps
  // pushing posts.
  ContactInfo* c = _mesh->lookupContactByPubKey(key, PUB_KEY_SIZE);
  if (c) _mesh->uiStartConnection(*c, keepAliveS);
}

bool MeshAdapter::requestStats(int ci) {
  if (!requestIdle()) return false;
  ContactInfo* c = contactPtr(ci);
  uint32_t est = 0, tag = 0;
  if (!c || _mesh->sendRequest(*c, REQ_GET_STATUS, tag, est) == MSG_SEND_FAILED) return false;
  _req.tag = tag;
  return startRequest(ReqKind::Status, ci, est);
}

bool MeshAdapter::requestTelemetry(int ci) {
  if (!requestIdle()) return false;
  ContactInfo* c = contactPtr(ci);
  uint32_t est = 0, tag = 0;
  if (!c || _mesh->sendRequest(*c, REQ_GET_TELEMETRY, tag, est) == MSG_SEND_FAILED) return false;
  _req.tag = tag;
  return startRequest(ReqKind::Telemetry, ci, est);
}

bool MeshAdapter::requestNeighbours(int ci) {
  if (!requestIdle()) return false;
  ContactInfo* c = contactPtr(ci);
  // version 0, count, offset (2), order (2 = strongest first), key prefix bytes, 4 random
  uint8_t req[11] = {REQ_GET_NEIGHBOURS, 0, MAX_NEIGH, 0, 0, 2, 4};
  _mesh->getRNG()->random(&req[7], 4);
  uint32_t est = 0, tag = 0;
  if (!c || _mesh->sendRequest(*c, req, sizeof req, tag, est) == MSG_SEND_FAILED) return false;
  _req.tag = tag;
  _nNeighbours = 0;
  return startRequest(ReqKind::Neighbours, ci, est);
}

bool MeshAdapter::sendCommand(int ci, const char* cmd) {
  if (!requestIdle()) return false;
  ContactInfo* c = contactPtr(ci);
  uint32_t est = 0;
  if (!c || _mesh->sendCommandData(*c, _rtc->getCurrentTimeUnique(), 0, TXT_TYPE_CLI_DATA, cmd, est) == MSG_SEND_FAILED) return false;
  return startRequest(ReqKind::Cli, ci, est);
}

bool MeshAdapter::tracePath(int ci) {
  if (!requestIdle()) return false;
  ContactInfo* c = contactPtr(ci);
  if (!c) return false;
  if (c->out_path_len == OUT_PATH_UNKNOWN) {
    snprintf(_req.text, sizeof _req.text, "No route known yet: send a message first");
    _req.state = model::Req::Failed;
    _rev++;
    return false;
  }
  // Out along the stored route (plus the node itself if it relays), then
  // back the same way: every relay appends the SNR it heard.
  int sz = (c->out_path_len >> 6) + 1, hops = c->out_path_len & 63;
  uint8_t path[MAX_PATH_SIZE];
  int n = 0;
  for (int i = 0; i < hops * sz && n < MAX_PATH_SIZE; i++) path[n++] = c->out_path[i];
  if (c->type == ADV_TYPE_REPEATER || c->type == ADV_TYPE_ROOM)
    for (int i = 0; i < sz && n < MAX_PATH_SIZE; i++) path[n++] = c->id.pub_key[i];
  for (int h = hops - (c->type == ADV_TYPE_REPEATER || c->type == ADV_TYPE_ROOM ? 1 : 2); h >= 0; h--)
    for (int i = 0; i < sz && n < MAX_PATH_SIZE; i++) path[n++] = c->out_path[h * sz + i];
  if (n == 0) {
    snprintf(_req.text, sizeof _req.text, "Direct neighbour: no relays to trace");
    _req.state = model::Req::Failed;
    _rev++;
    return false;
  }
  uint32_t tag;
  _mesh->getRNG()->random((uint8_t*)&tag, 4);
  uint8_t flags = (uint8_t)(sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3);
  mesh::Packet* pkt = _mesh->createTrace(tag, 0, flags);
  if (!pkt) return false;
  _mesh->sendDirect(pkt, path, (uint8_t)n);
  _req.tag = tag;
  uint32_t est = radio_driver.getEstAirtimeFor(20 + n) * (uint32_t)(n + 1) * 2;
  return startRequest(ReqKind::Trace, ci, est);
}

bool MeshAdapter::ping(int ci) {
  if (!requestIdle()) return false;
  ContactInfo* c = contactPtr(ci);
  if (!c) return false;
  if (c->type != ADV_TYPE_CHAT) {
    bool ok = requestStats(ci);   // repeaters, rooms, sensors answer status requests
    if (ok) _req.kind = ReqKind::Ping;
    return ok;
  }
  // Companions: path discovery, as the MeshCore app does it: a flooded
  // request for basic telemetry, answered with the route both ways.
  uint8_t req[9];
  req[0] = REQ_GET_TELEMETRY;
  req[1] = (uint8_t)~TELEM_PERM_BASE;   // inverse mask: only base telemetry
  memset(&req[2], 0, 3);
  _mesh->getRNG()->random(&req[5], 4);
  uint8_t saved = c->out_path_len;
  c->out_path_len = OUT_PATH_UNKNOWN;   // force a flood
  uint32_t tag = 0, est = 0;
  int r = _mesh->sendRequest(*c, req, sizeof req, tag, est);
  c->out_path_len = saved;
  if (r == MSG_SEND_FAILED) return false;
  _req.tag = tag;
  return startRequest(ReqKind::Ping, ci, est);
}

void MeshAdapter::onPathResponse(const ContactInfo& from, uint8_t in_len, uint8_t out_len, uint8_t extra_type,
                                 const uint8_t* extra, uint8_t extra_len) {
  if (_req.state != model::Req::Busy || _req.kind != ReqKind::Ping || memcmp(from.id.pub_key, _req.key, PUB_KEY_SIZE))
    return;
  if (extra_type != PAYLOAD_TYPE_RESPONSE || extra_len < 4 || rd32(extra) != _req.tag) return;
  pathReply(in_len, out_len);
}

void MeshAdapter::onTraceData(uint32_t tag, uint8_t flags, const uint8_t* hashes, const uint8_t* snrs, uint8_t path_len,
                              float final_snr) {
  if (_req.state != model::Req::Busy || tag != _req.tag) return;
  traceReply(flags, hashes, snrs, path_len, final_snr);
}

void MeshAdapter::onContactResponse(const ContactInfo& from, const uint8_t* data, uint8_t len) {
  if (_req.state != model::Req::Busy || memcmp(from.id.pub_key, _req.key, PUB_KEY_SIZE) || len < 4) return;
  uint32_t tag = rd32(data);
  switch (_req.kind) {
    case ReqKind::Login:
      if (len >= 6 && !memcmp(&data[4], "OK", 2)) loginResult(from.id.pub_key, true, false, 0);
      else if (len >= 7 && data[4] == RESP_SERVER_LOGIN_OK)
        loginResult(from.id.pub_key, true, data[6] & 1, (uint16_t)data[5] * 16);
      else loginResult(from.id.pub_key, false, false, 0);
      break;
    case ReqKind::Ping:
    case ReqKind::Status:
      statusReply(data + 4, len - 4);
      break;
    case ReqKind::Telemetry:
      if (tag != _req.tag) return;
      decodeTelemetry(data + 4, len - 4);
      break;
    case ReqKind::Neighbours:
      if (tag != _req.tag || len < 8) return;
      neighboursReply(data + 4, len - 4);
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------------ radio

model::Radio MeshAdapter::radio() const {
  NodePrefs* p = _mesh->getNodePrefs();
  model::Radio r;
  r.freqMHz = p->freq;
  r.bwKHz = p->bw;
  r.sf = p->sf;
  r.cr = p->cr;
  r.txDbm = p->tx_power_dbm;
  r.txMaxDbm = MAX_LORA_TX_POWER;
  r.repeat = p->isRepeatEn();
  r.rxBoost = p->rx_boosted_gain;
  return r;
}

bool MeshAdapter::setRadio(const model::Radio& r) {
  if (r.freqMHz < 150 || r.freqMHz > 2500 || r.sf < 5 || r.sf > 12 || r.cr < 5 || r.cr > 8) return false;
  NodePrefs* p = _mesh->getNodePrefs();
  p->freq = r.freqMHz;
  p->bw = r.bwKHz;
  p->sf = r.sf;
  p->cr = r.cr;
  p->tx_power_dbm = r.txDbm > MAX_LORA_TX_POWER ? MAX_LORA_TX_POWER : r.txDbm;
  p->setRepeatEn(r.repeat);
  p->rx_boosted_gain = r.rxBoost;
  _mesh->savePrefs();
  radio_driver.setParams(p->freq, p->bw, p->sf, p->cr);
  radio_driver.setTxPower(p->tx_power_dbm);
  radio_driver.setRxBoostedGainMode(p->rx_boosted_gain);
  configChanged();
  _rev++;
  return true;
}

// ------------------------------------------------------------------ diagnostics

model::Diag MeshAdapter::diag() const {
  model::Diag d;
  memset(&d, 0, sizeof d);
  d.uptimeS = millis() / 1000;
  d.noiseFloor = (int16_t)radio_driver.getNoiseFloor();
  d.lastRssi = _lastRssi;
  d.lastSnr_x4 = _lastSnr;
  d.rxPackets = radio_driver.getPacketsRecv();
  d.txPackets = radio_driver.getPacketsSent();
  d.rxFlood = _mesh->getNumRecvFlood();
  d.rxDirect = _mesh->getNumRecvDirect();
  d.txFlood = _mesh->getNumSentFlood();
  d.txDirect = _mesh->getNumSentDirect();
  d.dups = _dupsSeen;
  d.rxErrors = radio_driver.getPacketsRecvErrors();
  d.airTxMs = _mesh->getTotalAirTime();
  d.airRxMs = _mesh->getReceiveAirTime();
  uint32_t up = millis();
  d.dutyPct = up ? (uint8_t)(d.airTxMs * 100ULL / up) : 0;
  d.heapFree = _plat.heapFree();
  d.heapMin = _plat.heapMin();
  d.heapMaxBlock = _plat.heapMaxBlock();
#ifdef MC_BOARD_TDECK   // only the T-Deck's Diagnostics has the storage page
  d.flashUsedKb = _store->getStorageUsedKb();
  d.flashTotalKb = _store->getStorageTotalKb();
  MessageStore::Stats ms = _msgs.stats();
  d.msgWriteFails = ms.writeFails;
  d.msgWrites = ms.writes;
  d.archived = ms.archived;
  d.archiveFails = ms.archiveFails;
  d.msgErr = ms.lastErr;
  d.msgErrTs = ms.lastErrTs;
  d.msgOkTs = ms.lastOkTs;
  d.msgSeqFlash = ms.flashSeq;
  d.msgSeqNext = ms.nextSeq;
  d.msgRingBytes = ms.ringBytes;
  d.archiveOn = ms.archiveOn;
  d.msgOnCard = ms.onCard;
  _plat.sdSpace(d.sdUsedKb, d.sdTotalKb);
  d.cpuMhz = _plat.cpuMhz();
  d.lowClockS = _plat.lowClockSeconds();
  if (_hasOpen) {
    d.winN = (uint16_t)_msgs.count();
    d.winFirst = d.winN ? _msgs.at(0).seq : 0;
    d.winLast = d.winN ? _msgs.at(d.winN - 1).seq : 0;
    d.winNewest = _msgs.atNewest();
    const ThreadInfo* ti = _msgs.find(_openKey);
    d.winThreadLast = ti ? ti->lastSeq : 0;
  }
#endif
  d.battMv = getBattMilliVolts();
  d.battPct = batteryPercent(d.battMv);
  d.charging = _plat.charging();
  d.contacts = (uint16_t)_mesh->getNumContacts();
  d.contactsMax = MAX_CONTACTS;
  d.channels = (uint8_t)channelCount();
  d.channelsMax = MAX_GROUP_CHANNELS;
  return d;
}

// ------------------------------------------------------------------ backup

bool MeshAdapter::backupNow() {
  fs::FS* sd = _plat.sd();
  if (!sd) {
    snprintf(_bk.error, sizeof _bk.error, "No SD card");
    _bk.card = false;
    return false;
  }
  _mesh->uiFlushContacts();   // MeshCore saves contacts lazily
  Backup::Options o{_ui.backupIdentity != 0, _ui.backupMessages != 0};
  bool ok = Backup::write(*_fs, *sd, o, now(), FIRMWARE_VERSION, _bk.error, sizeof _bk.error);
  if (ok) _bk.error[0] = 0;
  _cfgChangedAt = 0;
  _lastBackupAt = millis() | 1;
  refreshBackupInfo();
  return ok;
}

bool MeshAdapter::restore(bool previous) {
  fs::FS* sd = _plat.sd();
  if (!sd) return false;
  if (!Backup::restore(*_fs, *sd, previous, _bk.error, sizeof _bk.error)) {
    _rev++;
    return false;
  }
  _fresh = false;
  _plat.reboot();   // MeshCore reloads everything from flash
  return true;
}

void MeshAdapter::factoryReset() {
  _msgs.wipe();
  _fs->remove("/ui/prefs.bin");
  _store->formatFileSystem();
  _plat.reboot();
}

}  // namespace app
