#include "app/remote/remote_adapter.h"
#include <Arduino.h>
#include <Utils.h>
#include <helpers/BaseChatMesh.h>   // MAX_TEXT_LEN
#include <SHA256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace app {

namespace {

constexpr uint8_t REQ_GET_NEIGHBOURS = 0x06;
constexpr uint8_t CTL_DISCOVER_REQ = 0x80;
constexpr uint32_t DISCOVER_LISTEN_MS = 8000;
constexpr uint32_t STATS_EVERY_MS = 30000;
constexpr uint32_t CONTACTS_EVERY_MS = 60000;
constexpr uint32_t POLL_EVERY_MS = 15000;
const char* const kPeerFile = "/ui/peer.bin";

struct PeerFile {
  uint32_t magic;
  Peer peer;
  uint32_t pin;
};
constexpr uint32_t PEER_MAGIC = 0x52454550;   // "PEER"

int8_t hopsOf(uint8_t pathLen) { return pathLen == 0xFF ? -1 : (int8_t)(pathLen & 63); }

void toRef(const cp::Contact& c, ContactRef& r) {
  memcpy(r.key, c.key, PUB_KEY_SIZE);
  snprintf(r.name, sizeof r.name, "%s", c.name);
  r.type = c.type;
  r.flags = c.flags;
  r.outPathLen = c.outPathLen;
  memcpy(r.outPath, c.outPath, sizeof r.outPath);
  r.lastmod = c.lastAdvert ? c.lastAdvert : c.lastmod;
  r.lat = c.lat;
  r.lon = c.lon;
}

}  // namespace

RemoteAdapter::~RemoteAdapter() {
  free(_contacts);
  free(_chans);
  free(_seen);
}

bool RemoteAdapter::allocTables(int maxC, int maxCh) {
  if (maxCh > 16) maxCh = 16;   // what the UI's per-slot mute bits cover
  if (maxC != _maxContacts) {
    free(_contacts);
    free(_seen);
    _contacts = (cp::Contact*)calloc(maxC, sizeof(cp::Contact));
    _seen = (uint8_t*)calloc(maxC / 8 + 1, 1);
    _maxContacts = _contacts && _seen ? maxC : 0;
    _nContacts = 0;
  }
  if (maxCh != _maxChans) {
    free(_chans);
    _chans = (Chan*)calloc(maxCh, sizeof(Chan));
    _maxChans = _chans ? maxCh : 0;
  }
  return _maxContacts && _maxChans;
}

namespace {
struct CacheHeader {
  uint32_t magic;
  uint16_t maxContacts, nContacts;
  uint8_t maxChans, pad[3];
};
constexpr uint32_t CACHE_MAGIC = 0x4E434D52;   // "RMCN"
}  // namespace

void RemoteAdapter::loadCache() {
  char path[32];
  snprintf(path, sizeof path, "/ui/%s-node.bin", _histTag);
  File f = _fs->open(path, "r");
  if (!f) return;
  CacheHeader h{};
  bool ok = f.read((uint8_t*)&h, sizeof h) == sizeof h && h.magic == CACHE_MAGIC && h.nContacts <= h.maxContacts &&
            allocTables(h.maxContacts, h.maxChans) && h.maxChans == _maxChans &&
            f.read((uint8_t*)&_self, sizeof _self) == sizeof _self && f.read((uint8_t*)&_dev, sizeof _dev) == sizeof _dev &&
            f.read((uint8_t*)_chans, _maxChans * sizeof(Chan)) == _maxChans * sizeof(Chan) &&
            f.read((uint8_t*)_contacts, h.nContacts * sizeof(cp::Contact)) == h.nContacts * sizeof(cp::Contact);
  f.close();
  _nContacts = ok ? h.nContacts : 0;
  if (ok) {
    _haveSelf = true;
    syncPrefs();
  }
}

void RemoteAdapter::markCacheDirty() {
  _cacheDirty = true;
  _cacheChangedAt = millis();
}

void RemoteAdapter::saveCache() {
  if (!_maxContacts || !_haveSelf) return;
  char path[32];
  snprintf(path, sizeof path, "/ui/%s-node.bin", _histTag);
  File f = _fs->open(path, "w", true);
  if (!f) return;
  CacheHeader h{CACHE_MAGIC, (uint16_t)_maxContacts, (uint16_t)_nContacts, (uint8_t)_maxChans, {}};
  f.write((const uint8_t*)&h, sizeof h);
  f.write((const uint8_t*)&_self, sizeof _self);
  f.write((const uint8_t*)&_dev, sizeof _dev);
  f.write((const uint8_t*)_chans, _maxChans * sizeof(Chan));
  f.write((const uint8_t*)_contacts, _nContacts * sizeof(cp::Contact));
  f.close();
  _cacheDirty = false;
  _cacheSavedAt = millis() | 1;
}

// ------------------------------------------------------------------ setup

void RemoteAdapter::begin(fs::FS& fs) {
  resetRuntime();
  _client.reset();
  _wasReady = _synced = _haveSelf = false;
  _nContacts = 0;
  _contactsSince = _contactsAt = _contactsSoon = _pollAt = 0;
  _cacheDirty = _fullSync = false;
  _cacheSavedAt = 0;
  _histTag[0] = 0;
  _clockOffset = 0;
  _clockFromGps = false;
  _nodeBattMv = 0;
  memset(&_stats, 0, sizeof _stats);
  _havePrivKey = false;
  memset(&_rs, 0, sizeof _rs);
  _positionAt = 0;
  memset(_chanSends, 0, sizeof _chanSends);
  loadUiPrefs(fs);
  loadPasswords();
  loadPeer();
  openHistory();   // with no node saved yet, an empty placeholder
  uiToPrefs();
  _prefs.gpsEnabled = _ui.remoteGps;
  _plat.setGpsPower(_ui.remoteGps);
  startBootGps();
  refreshBackupInfo();
  if (_peerSaved) _link.connect(_peer, _pin);
  _rev++;
}

// History and backups per node, named by its Bluetooth address: they stay
// with the device even if it is erased and gets a new identity.
void RemoteAdapter::openHistory() {
  const uint8_t* a = _peer.addr;
  char tag[8];
  snprintf(tag, sizeof tag, "r%02x%02x%02x", a[2], a[1], a[0]);
  if (!strcmp(tag, _histTag)) return;
  snprintf(_histTag, sizeof _histTag, "%s", tag);
  snprintf(_bkPath[0], sizeof _bkPath[0], "%s/remote-%s.bin", Backup::DIR, tag);
  snprintf(_bkPath[1], sizeof _bkPath[1], "%s/remote-%s.bak", Backup::DIR, tag);
  fs::FS* card = _plat.sd();
  _msgs.setArchive(card);   // first: a ring of another size moves its extra records there
#ifdef MC_HISTORY_ON_SD
  if (card) _msgs.begin(card, tag, _fs);
  else _msgs.begin(_fs, tag);
#else
  _msgs.begin(_fs, tag);
#endif
  _hasOpen = false;
  loadCache();
}

void RemoteAdapter::loadPeer() {
  PeerFile pf{};
  File f = _fs->open(kPeerFile, "r");
  bool ok = f && f.read((uint8_t*)&pf, sizeof pf) == sizeof pf && pf.magic == PEER_MAGIC;
  if (f) f.close();
  _peerSaved = ok;
  if (!ok) return;
  _peer = pf.peer;
  _pin = pf.pin;
}

void RemoteAdapter::savePeer() {
  PeerFile pf{};
  pf.magic = PEER_MAGIC;
  pf.peer = _peer;
  pf.pin = _pin;
  File f = _fs->open(kPeerFile, "w", true);
  if (!f) return;
  f.write((const uint8_t*)&pf, sizeof pf);
  f.close();
}

void RemoteAdapter::connectTo(const Peer& p, uint32_t pin) {
  if (!_peerSaved || memcmp(p.addr, _peer.addr, 6)) {
    // Another node: its contacts and channels are not this one's.
    _nContacts = 0;
    _haveSelf = false;
  }
  _peer = p;
  _pin = pin;
  _peerSaved = true;
  savePeer();
  openHistory();
  refreshBackupInfo();
  _link.connect(p, pin);
  _rev++;
}

model::Link RemoteAdapter::link() const {
  model::Link l{};
  l.state = (model::LinkState)_link.state();
  l.saved = _peerSaved;
  l.synced = _synced;
  snprintf(l.peer, sizeof l.peer, "%s", _peerSaved ? _peer.name : "");
  snprintf(l.node, sizeof l.node, "%s", _haveSelf ? _self.name : "");
  l.rssi = _link.rssi();
  l.nodeBattMv = _nodeBattMv;
  snprintf(l.error, sizeof l.error, "%s", _link.lastError());
  return l;
}

void RemoteAdapter::scan(bool on) {
  if (on) _link.startScan();
  else _link.stopScan();
  _rev++;
}

bool RemoteAdapter::found(int i, model::FoundNode& out) const {
  Peer p;
  if (!_link.scanResult(i, p)) return false;
  memset(&out, 0, sizeof out);
  snprintf(out.name, sizeof out.name, "%s", p.name);
  out.rssi = p.rssi;
  return true;
}

bool RemoteAdapter::connectFound(int i, uint32_t pin) {
  Peer p;
  if (!_link.scanResult(i, p)) return false;
  _link.stopScan();
  connectTo(p, pin);
  return true;
}

void RemoteAdapter::reconnect() {
  if (_peerSaved) _link.connect(_peer, _pin);
  _rev++;
}

void RemoteAdapter::forgetPeer() {
  if (_peerSaved) _link.forget(_peer);
  _peerSaved = false;
  _fs->remove(kPeerFile);
  _rev++;
}

// ------------------------------------------------------------------ the link

bool RemoteAdapter::q(const uint8_t* f, int len, uint32_t cookie) {
  return _link.ready() && _client.queue(f, len, cookie);
}

// After connecting: who the node is, its contacts and channels, its clock,
// then whatever messages wait for us. Each reply asks for the next.
void RemoteAdapter::startSync() {
  _synced = false;
  uint8_t f[cp::MAX_FRAME];
  q(f, cp::encDeviceQuery(f), CK_SYNC_CHAIN);
}

void RemoteAdapter::syncMessages() {
  uint8_t f[1] = {cp::SYNC_NEXT_MESSAGE};
  if (!_client.pending(cp::SYNC_NEXT_MESSAGE)) q(f, 1);
}

void RemoteAdapter::loop() {
  uint32_t ms = millis();
  _link.loop(ms);
  // Redraw on link changes (status, header icon) and new scan results.
  FrameLink::State st = _link.state();
  int found = _link.scanCount();
  if (st != _lastState || found != _lastFound) {
    _lastState = st;
    _lastFound = found;
    _rev++;
  }
  bool ready = _link.ready();
  if (ready && !_wasReady) startSync();
  if (!ready && _wasReady) {
    _synced = false;
    _rev++;
  }
  _wasReady = ready;
  _client.loop(ms);

  if (_rs.active) restoreStep();
  retryPending(ms);
  requestTick(ms);
  gpsTick(ms);
  positionTick(ms);
  if (_synced) {
    uint8_t f[cp::MAX_FRAME];
    // The node's battery, for the header and Diagnostics.
    if (ms - _statsAt >= STATS_EVERY_MS) {
      _statsAt = ms;
      f[0] = cp::GET_BATT_AND_STORAGE;
      q(f, 1);
      for (uint8_t t = cp::STATS_CORE; t <= cp::STATS_PACKETS; t++) q(f, cp::encU8(f, cp::GET_STATS, t));
    }
    // Contacts added or changed since the last list (a lost push, an advert
    // from a node we don't have yet).
    if ((ms - _contactsAt >= CONTACTS_EVERY_MS || (_contactsSoon && (int32_t)(ms - _contactsSoon) >= 0)) &&
        !_client.pending(cp::GET_CONTACTS)) {
      _contactsAt = ms;
      _contactsSoon = 0;
      q(f, cp::encGetContacts(f, _contactsSince), CK_CONTACTS_SINCE);
    }
    // The flash copy: 10 s after the last change, at most every 2 minutes
    // (the table can be tens of KB: spare the flash).
    if (_cacheDirty && ms - _cacheChangedAt > 10000 && (!_cacheSavedAt || ms - _cacheSavedAt > 120000)) saveCache();
    // Messages whose "waiting" push was lost.
    if (ms - _pollAt >= POLL_EVERY_MS && _client.idle()) {
      _pollAt = ms;
      syncMessages();
    }
  }
  storageTick(ms);
}

// ------------------------------------------------------------------ replies

void RemoteAdapter::onReply(uint8_t cmd, uint32_t cookie, const uint8_t* f, int len) {
  uint8_t out[cp::MAX_FRAME];
  uint8_t code = f ? f[0] : 0xFF;   // 0xFF: no reply
  switch (cmd) {
    case cp::DEVICE_QUERY:
      if (code == cp::RC_DEVICE_INFO) {
        cp::parseDeviceInfo(f, len, _dev);
        allocTables(_dev.maxContacts ? _dev.maxContacts : 100, _dev.maxChannels ? _dev.maxChannels : 8);
        q(out, cp::encAppStart(out, "meshmash"), CK_SYNC_CHAIN);
      }
      break;
    case cp::APP_START:
      if (code == cp::RC_SELF_INFO && cp::parseSelfInfo(f, len, _self)) {
        _haveSelf = true;
        syncPrefs();
        if (cookie == CK_SYNC_CHAIN) q(out, cp::encGetContacts(out, 0), CK_SYNC_CHAIN);
        _rev++;
      }
      break;
    case cp::GET_CONTACTS:
      if (code == cp::RC_CONTACTS_START) {
        if (cookie == CK_SYNC_CHAIN && _seen) {   // the whole list follows
          memset(_seen, 0, _maxContacts / 8 + 1);
          _fullSync = true;
        }
      } else if (code == cp::RC_CONTACT) {
        cp::Contact c;
        if (cp::parseContact(f, len, c)) putContact(c);
      } else {
        if (_fullSync) {   // contacts the node no longer has
          _fullSync = false;
          if (code == cp::RC_END_OF_CONTACTS)
            for (int i = _nContacts - 1; i >= 0; i--)
              if (!(_seen[i / 8] & (1 << (i % 8)))) {
                memmove(&_contacts[i], &_contacts[i + 1], sizeof(cp::Contact) * (_nContacts - i - 1));
                _nContacts--;
              }
          markCacheDirty();
        }
        if (code == cp::RC_END_OF_CONTACTS && len >= 5) {
          uint32_t latest = cp::rd32(f + 1);
          if (latest > _contactsSince) _contactsSince = latest;
          _contactsAt = millis();
        }
        if (cookie == CK_SYNC_CHAIN) {
          _syncChan = 0;
          if (_maxChans) q(out, cp::encGetChannel(out, 0), CK_SYNC_CHAIN);
        }
      }
      _rev++;
      break;
    case cp::GET_CHANNEL: {
      int idx = _syncChan;
      if (code == cp::RC_CHANNEL_INFO && len >= 50 && f[1] < _maxChans) {
        idx = f[1];
        memcpy(_chans[idx].name, f + 2, 32);
        _chans[idx].name[31] = 0;
        memcpy(_chans[idx].secret, f + 34, 16);
      }
      if (cookie == CK_SYNC_CHAIN) {
        if (++_syncChan < _maxChans) {
          q(out, cp::encGetChannel(out, (uint8_t)_syncChan), CK_SYNC_CHAIN);
        } else {
          out[0] = cp::GET_DEVICE_TIME;
          q(out, 1, CK_SYNC_CHAIN);
        }
      }
      _rev++;
      break;
    }
    case cp::GET_DEVICE_TIME:
      if (code == cp::RC_CURR_TIME && len >= 5) {
        uint32_t t = cp::rd32(f + 1);
        // Our clock follows the node's, unless GPS set ours: then theirs follows.
        if (_clockFromGps) q(out, cp::encU32(out, cp::SET_DEVICE_TIME, now()));
        else if (t > 1600000000) _clockOffset = (int32_t)(t - millis() / 1000);
      }
      if (cookie == CK_SYNC_CHAIN) {
        _synced = true;
        _statsAt = millis() - STATS_EVERY_MS;   // battery right away
        syncMessages();
        // For backups that include the identity (if the node allows exporting it).
        if (_ui.backupIdentity && !_havePrivKey) {
          out[0] = cp::EXPORT_PRIVATE_KEY;
          q(out, 1);
        }
      }
      _rev++;
      break;
    case cp::SYNC_NEXT_MESSAGE:
      if (code == cp::RC_CONTACT_MSG_RECV_V3 || code == cp::RC_CHANNEL_MSG_RECV_V3) {
        cp::Msg m;
        if (cp::parseMsg(f, len, m)) gotMessage(m);
        syncMessages();   // until NO_MORE_MESSAGES
      }
      break;
    case cp::SEND_TXT_MSG: {
      if (cookie == CK_REQUEST) {   // a CLI command
        cp::Sent s;
        if (code == cp::RC_SENT && cp::parseSent(f, len, s)) _req.deadline = millis() + s.timeoutMs + 4000;
        else if (_req.state == model::Req::Busy) finishRequest(model::Req::Failed, "The node could not send it");
        break;
      }
      Pending* p = pendingBySeq(cookie);
      if (!p) break;
      cp::Sent s;
      if (code == cp::RC_SENT && cp::parseSent(f, len, s)) {
        p->prevAck = p->ack;
        p->ack = s.tag;
        p->sentAt = millis();
        p->timeout = s.timeoutMs + 1000;
      } else {
        // Refused, or no answer: retried after the timeout like a lost ACK.
        p->sentAt = millis();
        p->timeout = 3000;
      }
      break;
    }
    case cp::SEND_CHANNEL_TXT_MSG:
      for (auto& cs : _chanSends) {
        if (!cs.seq || cs.seq != cookie) continue;
        bool ok = code == cp::RC_OK;
        _msgs.setStatus(cs.seq, ok ? model::Delivery::Sent : model::Delivery::Failed, 0);
        if (ok && cs.slot < _maxChans) {
          uint8_t h[4];
          groupHash(_chans[cs.slot], cs.ts, cs.text, h);
          watchRepeats(cs.seq, h);
        }
        cs.seq = 0;
        _rev++;
      }
      break;
    case cp::GET_CONTACT_BY_KEY:
      if (code == cp::RC_CONTACT) {
        cp::Contact c;
        if (cp::parseContact(f, len, c)) putContact(c);
        _rev++;
      }
      break;
    case cp::SEND_LOGIN:
    case cp::SEND_STATUS_REQ:
    case cp::SEND_TELEMETRY_REQ:
    case cp::SEND_BINARY_REQ:
    case cp::SEND_TRACE_PATH:
    case cp::SEND_PATH_DISCOVERY_REQ: {
      cp::Sent s;
      if (code == cp::RC_SENT && cp::parseSent(f, len, s)) {
        if (cmd != cp::SEND_LOGIN && cmd != cp::SEND_STATUS_REQ && cmd != cp::SEND_TRACE_PATH) _req.tag = s.tag;
        _req.deadline = millis() + s.timeoutMs + 4000;
      } else if (_req.state == model::Req::Busy) {
        finishRequest(model::Req::Failed, code == 0xFF ? "No answer from the node" : "The node could not send it");
      }
      break;
    }
    case cp::EXPORT_PRIVATE_KEY:
      if (code == cp::RC_PRIVATE_KEY && len >= 65) {
        memcpy(_privKey, f + 1, 64);
        _havePrivKey = true;
      }
      break;
    case cp::GET_STATS:
      if (code != cp::RC_STATS || len < 2) break;
      if (f[1] == cp::STATS_CORE && len >= 11) {
        _nodeBattMv = cp::rd16(f + 2);
        _stats.uptimeS = cp::rd32(f + 4);
        _stats.queueLen = f[10];
      } else if (f[1] == cp::STATS_RADIO && len >= 14) {
        _stats.noiseFloor = (int16_t)cp::rd16(f + 2);
        _stats.lastRssi = (int8_t)f[4];
        _stats.lastSnr_x4 = (int8_t)f[5];
        _stats.txAirS = cp::rd32(f + 6);
        _stats.rxAirS = cp::rd32(f + 10);
      } else if (f[1] == cp::STATS_PACKETS && len >= 30) {
        _stats.recv = cp::rd32(f + 2);
        _stats.sent = cp::rd32(f + 6);
        _stats.sentFlood = cp::rd32(f + 10);
        _stats.sentDirect = cp::rd32(f + 14);
        _stats.recvFlood = cp::rd32(f + 18);
        _stats.recvDirect = cp::rd32(f + 22);
        _stats.recvErrors = cp::rd32(f + 26);
      }
      _rev++;
      break;
    case cp::GET_BATT_AND_STORAGE:
      if (code == cp::RC_BATT_AND_STORAGE && len >= 3) {
        _nodeBattMv = cp::rd16(f + 1);
        _rev++;
      }
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------------ pushes

void RemoteAdapter::onPush(const uint8_t* f, int len) {
  switch (f[0]) {
    case cp::P_MSG_WAITING:
      if (_synced) syncMessages();
      break;
    case cp::P_SEND_CONFIRMED:
      if (len >= 5) ackReceived(cp::rd32(f + 1));
      break;
    case cp::P_ADVERT:
    case cp::P_PATH_UPDATED:
    // A new node: added (auto-add), or only heard (then the fetch finds
    // nothing, and the heard list has it from the raw packet).
    case cp::P_NEW_ADVERT:
      if (len >= 1 + cp::KEY && _haveSelf) refreshContact(f + 1);
      break;
    case cp::P_CONTACT_DELETED:
      if (len >= 1 + cp::KEY) dropContact(f + 1);
      break;
    case cp::P_LOG_RX_DATA:
      if (len > 3) {
        int before = _nHeard;
        uint8_t first[PUB_KEY_SIZE];
        memcpy(first, _nHeard ? _heard[0].pubkey : first, PUB_KEY_SIZE);
        rawRx((int8_t)f[1] / 4.0f, (int8_t)f[2], f + 3, len - 3, _haveSelf ? _self.key : nullptr);
        // An advert from a node that isn't a contact (yet): the node may
        // have added it; ask a little later.
        if (_nHeard && (_nHeard != before || memcmp(first, _heard[0].pubkey, PUB_KEY_SIZE)) &&
            !findContact(_heard[0].pubkey, PUB_KEY_SIZE) && !_contactsSoon)
          _contactsSoon = (millis() + 2000) | 1;
      }
      break;
    case cp::P_CONTROL_DATA:
      if (len > 4) discoverReply((int8_t)f[1] / 4.0f, (int8_t)f[2], f + 4, len - 4);
      break;
    case cp::P_LOGIN_SUCCESS:
      if (_req.state == model::Req::Busy && _req.kind == ReqKind::Login && len >= 8 && !memcmp(f + 2, _req.key, 6))
        loginResult(_req.key, true, f[1] & 1, 0);   // the node keeps the session alive itself
      break;
    case cp::P_LOGIN_FAIL:
      if (_req.state == model::Req::Busy && _req.kind == ReqKind::Login && len >= 8 && !memcmp(f + 2, _req.key, 6))
        loginResult(_req.key, false, false, 0);
      break;
    case cp::P_STATUS_RESPONSE:
      if (_req.state == model::Req::Busy && (_req.kind == ReqKind::Status || _req.kind == ReqKind::Ping) &&
          len >= 8 && !memcmp(f + 2, _req.key, 6))
        statusReply(f + 8, len - 8);
      break;
    case cp::P_TELEMETRY_RESPONSE:
      if (_req.state == model::Req::Busy && _req.kind == ReqKind::Telemetry && len >= 8 && !memcmp(f + 2, _req.key, 6))
        decodeTelemetry(f + 8, len - 8);
      break;
    case cp::P_BINARY_RESPONSE:
      if (_req.state == model::Req::Busy && _req.kind == ReqKind::Neighbours && len >= 6 && cp::rd32(f + 2) == _req.tag)
        neighboursReply(f + 6, len - 6);
      break;
    case cp::P_TRACE_DATA: {
      // reserved, path length, flags, tag, auth, hashes, SNRs, final SNR
      if (len < 12 || _req.state != model::Req::Busy || cp::rd32(f + 4) != _req.tag) break;
      uint8_t pathLen = f[2], flags = f[3];
      int nSnr = pathLen >> (flags & 3);
      if (len < 12 + pathLen + nSnr + 1) break;
      traceReply(flags, f + 12, f + 12 + pathLen, pathLen, (int8_t)f[12 + pathLen + nSnr] / 4.0f);
      break;
    }
    case cp::P_PATH_DISCOVERY_RESPONSE: {
      // reserved, key prefix, out path length + path, in path length + path
      if (len < 9 || _req.state != model::Req::Busy || _req.kind != ReqKind::Ping || memcmp(f + 2, _req.key, 6)) break;
      uint8_t outLen = f[8];
      int i = 9 + (outLen & 63) * ((outLen >> 6) + 1);
      if (i >= len) break;
      pathReply(f[i], outLen);
      break;
    }
    default:
      break;
  }
}

// ------------------------------------------------------------------ messages

void RemoteAdapter::gotMessage(const cp::Msg& m) {
  model::Message out;
  memset(&out, 0, sizeof out);
  out.ts = m.ts;
  out.snr_x4 = m.snr_x4;
  out.hops = hopsOf(m.pathLen);
  if (m.channel) {
    const char* text = m.text;
    const char* sep = strstr(text, ": ");   // channel payloads are "sender: text"
    if (sep && sep - text < (int)sizeof out.sender) {
      snprintf(out.sender, sizeof out.sender, "%.*s", (int)(sep - text), text);
      text = sep + 2;
    }
    snprintf(out.text, sizeof out.text, "%s", text);
    storeChannel(m.channelIdx, out);
    return;
  }
  cp::Contact* from = findContact(m.keyPrefix, 6);
  if (m.pathLen == 0 || m.pathLen == 0xFF) noteSignal(m.keyPrefix, m.snr_x4 / 4.0f, _lastRssi);
  if (m.txtType == cp::TXT_CLI) {
    if (_req.state == model::Req::Busy && _req.kind == ReqKind::Cli && !memcmp(_req.key, m.keyPrefix, 6))
      finishRequest(model::Req::Ok, "%s", m.text);
    return;   // command replies are not conversation
  }
  if (m.hasExtra && !contactName(m.extra, 4, out.sender, sizeof out.sender))
    hexKey(out.sender, sizeof out.sender, m.extra, 4);   // a room post: its author
  snprintf(out.text, sizeof out.text, "%s", m.text);
  uint8_t key[PUB_KEY_SIZE] = {};
  memcpy(key, from ? from->key : m.keyPrefix, from ? PUB_KEY_SIZE : 6);
  storeDirect(key, out);
}

uint32_t RemoteAdapter::uniqueNow() {
  uint32_t t = now();
  if (t <= _lastTs) t = _lastTs + 1;
  _lastTs = t;
  return t;
}

RemoteAdapter::Pending* RemoteAdapter::pendingBySeq(uint32_t seq) {
  for (auto& p : _pending)
    if (p.used && p.seq == seq) return &p;
  return nullptr;
}

bool RemoteAdapter::sendAttempt(Pending& p, const ContactRef& c, const char* text) {
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encSendText(f, cp::TXT_PLAIN, p.attempt, p.ts, c.key, text), p.seq)) return false;
  // Until the node says how long to wait for the ACK.
  p.sentAt = millis();
  p.timeout = 30000;
  return true;
}

bool RemoteAdapter::send(model::ThreadId t, const char* text) {
  if (!text[0] || !_synced) return false;
  messageCount(t);   // the window on this conversation
  uint32_t ts = uniqueNow();
  model::Message m;
  memset(&m, 0, sizeof m);
  m.ts = ts;
  m.outgoing = true;
  m.snr_x4 = INT8_MIN;
  m.status = model::Delivery::Pending;
  snprintf(m.text, sizeof m.text, "%s", text);

  if (t.kind == model::ThreadKind::Channel) {
    if (t.idx >= _maxChans || !_chans[t.idx].name[0]) return false;
    ChanSend* cs = &_chanSends[0];
    for (auto& c : _chanSends)
      if (!c.seq) { cs = &c; break; }
    uint32_t seq = appendOutgoing(ThreadKey::channel((uint8_t)t.idx), m);
    if (!seq) return false;
    uint8_t f[cp::MAX_FRAME];
    if (!q(f, cp::encSendChannel(f, (uint8_t)t.idx, ts, text), seq)) {
      _msgs.setStatus(seq, model::Delivery::Failed, 0);
    } else {
      cs->seq = seq;
      cs->ts = ts;
      cs->slot = (uint8_t)t.idx;
      snprintf(cs->text, sizeof cs->text, "%s", text);
    }
    _rev++;
    return true;
  }

  ContactRef c;
  if (!contactAt(t.idx, c)) return false;
  Pending* p = newPending();
  if (!p) return false;
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

// The hash of the packet the node sends for a channel message, as MeshCore
// builds it (BaseChatMesh::sendGroupMessage, Mesh::createGroupDatagram), so
// repeats of it can be recognised in the node's raw RX log.
void RemoteAdapter::groupHash(const Chan& ch, uint32_t ts, const char* text, uint8_t out[4]) const {
  uint8_t plain[5 + MAX_TEXT_LEN + 32];
  memcpy(plain, &ts, 4);
  plain[4] = 0;   // TXT_TYPE_PLAIN
  int n = snprintf((char*)plain + 5, sizeof plain - 5, "%s: ", _self.name);
  int tl = (int)strlen(text);
  if (tl + n > MAX_TEXT_LEN) tl = MAX_TEXT_LEN - n;
  memcpy(plain + 5 + n, text, tl);
  uint8_t secret[PUB_KEY_SIZE] = {};
  memcpy(secret, ch.secret, 16);
  uint8_t payload[MAX_PACKET_PAYLOAD];
  mesh::Utils::sha256(payload, PATH_HASH_SIZE, secret, 16);   // the channel hash
  int len = PATH_HASH_SIZE + mesh::Utils::encryptThenMAC(secret, payload + PATH_HASH_SIZE, plain, 5 + n + tl);
  uint8_t type = PAYLOAD_TYPE_GRP_TXT;
  SHA256 sha;
  sha.update(&type, 1);
  sha.update(payload, len);
  uint8_t h[MAX_HASH_SIZE];
  sha.finalize(h, MAX_HASH_SIZE);
  memcpy(out, h, 4);
}

// ------------------------------------------------------------------ contacts

cp::Contact* RemoteAdapter::findContact(const uint8_t* key, int len) const {
  for (int i = 0; i < _nContacts; i++)
    if (!memcmp(_contacts[i].key, key, len)) return &_contacts[i];
  return nullptr;
}

int RemoteAdapter::contactIndex(const uint8_t* key, int len) const {
  for (int i = 0; i < _nContacts; i++)
    if (!memcmp(_contacts[i].key, key, len)) return i;
  return -1;
}

bool RemoteAdapter::contactAt(int i, ContactRef& r) const {
  if (i < 0 || i >= _nContacts) return false;
  toRef(_contacts[i], r);
  return true;
}

void RemoteAdapter::putContact(const cp::Contact& c) {
  int i = contactIndex(c.key, cp::KEY);
  if (i >= 0) _contacts[i] = c;
  else if (_nContacts < _maxContacts) _contacts[i = _nContacts++] = c;
  else return;
  // During a full list, mark it as still there. (Insertion order keeps the
  // marks valid: new contacts go at the end.)
  if (_fullSync) _seen[i / 8] |= (uint8_t)(1 << (i % 8));
  markCacheDirty();
  _rev++;
}

void RemoteAdapter::dropContact(const uint8_t* key) {
  int i = contactIndex(key, cp::KEY);
  if (i < 0) return;
  memmove(&_contacts[i], &_contacts[i + 1], sizeof(cp::Contact) * (_nContacts - i - 1));
  _nContacts--;
  markCacheDirty();
  _rev++;
}

void RemoteAdapter::updateContact(const cp::Contact& c) {
  uint8_t f[cp::MAX_FRAME];
  q(f, cp::encodeContact(f, c));
}

void RemoteAdapter::refreshContact(const uint8_t* key) {
  uint8_t f[cp::MAX_FRAME];
  q(f, cp::encKeyCmd(f, cp::GET_CONTACT_BY_KEY, key));
}

void RemoteAdapter::setFavourite(int i, bool fav) {
  if (i < 0 || i >= _nContacts) return;
  cp::Contact& c = _contacts[i];
  if ((bool)(c.flags & 1) == fav) return;
  c.flags = fav ? (c.flags | 1) : (c.flags & ~1);
  updateContact(c);
  configChanged();
  _rev++;
}

void RemoteAdapter::setTelemetry(int i, uint8_t telem) {
  if (i < 0 || i >= _nContacts) return;
  cp::Contact& c = _contacts[i];
  uint8_t flags = (uint8_t)((c.flags & ~0x0E) | ((telem & 7) << 1));
  if (flags == c.flags) return;
  c.flags = flags;
  updateContact(c);
  configChanged();
  _rev++;
}

bool RemoteAdapter::removeContact(int i) {
  if (i < 0 || i >= _nContacts) return false;
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encKeyCmd(f, cp::REMOVE_CONTACT, _contacts[i].key))) return false;
  _msgs.forget(ThreadKey::direct(_contacts[i].key));
  dropContact(_contacts[i].key);
  configChanged();
  return true;
}

bool RemoteAdapter::resetPath(int i) {
  if (i < 0 || i >= _nContacts) return false;
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encKeyCmd(f, cp::RESET_PATH, _contacts[i].key))) return false;
  _contacts[i].outPathLen = 0xFF;
  configChanged();
  _rev++;
  return true;
}

void RemoteAdapter::forgetRoute(const uint8_t* key) {
  cp::Contact* c = findContact(key, cp::KEY);
  if (!c) return;
  uint8_t f[cp::MAX_FRAME];
  q(f, cp::encKeyCmd(f, cp::RESET_PATH, c->key));
  c->outPathLen = 0xFF;
}

bool RemoteAdapter::addHeard(int i) {
  if (i < 0 || i >= _nHeard) return false;
  const Heard& h = _heard[i];
  if (findContact(h.pubkey, PUB_KEY_SIZE)) return false;
  cp::Contact c{};
  memcpy(c.key, h.pubkey, PUB_KEY_SIZE);
  if (h.name[0]) snprintf(c.name, sizeof c.name, "%s", h.name);
  else hexKey(c.name, sizeof c.name, h.pubkey, 4);
  c.type = h.type;
  c.outPathLen = 0xFF;
  c.lat = h.lat;
  c.lon = h.lon;
  c.lastmod = now();
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encodeContact(f, c))) return false;
  refreshContact(c.key);   // as the node stored it
  putContact(c);
  configChanged();
  return true;
}

// ------------------------------------------------------------------ channels

bool RemoteAdapter::channelName(int slot, char* name, int n) const {
  if (slot < 0 || slot >= _maxChans) return false;
  snprintf(name, n, "%s", _chans[slot].name);
  return true;
}

int RemoteAdapter::channelCount() const {
  int n = 0;
  for (int s = 0; s < _maxChans; s++)
    if (_chans[s].name[0]) n++;
  return n;
}

int RemoteAdapter::slotOf(int i) const {
  for (int s = 0; s < _maxChans; s++)
    if (_chans[s].name[0] && i-- == 0) return s;
  return -1;
}

int RemoteAdapter::freeSlot() const {
  for (int s = 0; s < _maxChans; s++)
    if (!_chans[s].name[0]) return s;
  return -1;
}

bool RemoteAdapter::channel(int i, model::Channel& out) const {
  int s = slotOf(i);
  if (s < 0) return false;
  memset(&out, 0, sizeof out);
  out.idx = (uint8_t)s;
  snprintf(out.name, sizeof out.name, "%s", _chans[s].name);
  out.hashtag = _chans[s].name[0] == '#';
  out.muted = uiMuted(s);
  return true;
}

void RemoteAdapter::setSlot(int slot, const char* name, const uint8_t secret[16]) {
  snprintf(_chans[slot].name, sizeof _chans[slot].name, "%s", name);
  memcpy(_chans[slot].secret, secret, 16);
}

bool RemoteAdapter::joinPrivate(const char* name, const uint8_t psk[16]) {
  int s = freeSlot();
  if (s < 0 || !name[0]) return false;
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encSetChannel(f, (uint8_t)s, name, psk))) return false;
  setSlot(s, name, psk);
  setUiMuted(s, false);
  configChanged();
  _rev++;
  return true;
}

bool RemoteAdapter::joinHashtag(const char* name) {
  // As the MeshCore apps do: the first 16 bytes of SHA-256 of "#name" (lower case).
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

bool RemoteAdapter::createPrivate(const char* name) {
  uint8_t key[16];
  _plat.randomBytes(key, sizeof key);
  return joinPrivate(name, key);
}

bool RemoteAdapter::channelKey(int i, uint8_t psk[16]) const {
  int s = slotOf(i);
  if (s < 0) return false;
  memcpy(psk, _chans[s].secret, 16);
  return true;
}

bool RemoteAdapter::removeChannel(int i) {
  int s = slotOf(i);
  if (s <= 0) return false;   // slot 0 (Public) stays
  static const uint8_t zero[16] = {};
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encSetChannel(f, (uint8_t)s, "", zero))) return false;
  setSlot(s, "", zero);
  _msgs.forget(ThreadKey::channel((uint8_t)s));
  configChanged();
  _rev++;
  return true;
}

void RemoteAdapter::setMuted(int i, bool muted) {
  int s = slotOf(i);
  if (s < 0) return;
  setUiMuted(s, muted);
  _rev++;
}

// ------------------------------------------------------------------ adverts, discovery

bool RemoteAdapter::advert(bool flood) {
  uint8_t f[2];
  bool ok = q(f, cp::encU8(f, cp::SEND_SELF_ADVERT, flood ? 1 : 0));
  _rev++;
  return ok;
}

bool RemoteAdapter::discoverRepeaters() {
  uint8_t data[10];
  data[0] = CTL_DISCOVER_REQ;
  data[1] = 1 << 2;   // ADV_TYPE_REPEATER
  _plat.randomBytes(&data[2], 4);
  memset(&data[6], 0, 4);   // since: everything
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encBytes(f, cp::SEND_CONTROL_DATA, data, sizeof data))) return false;
  startDiscover(cp::rd32(&data[2]), DISCOVER_LISTEN_MS);
  return true;
}

// ------------------------------------------------------------------ requests

bool RemoteAdapter::requestSent(const uint8_t* f, int len, ReqKind kind, int ci) {
  if (!requestIdle() || !_synced) return false;
  if (!startRequest(kind, ci, 20000)) return false;   // the node's estimate replaces this
  if (!q(f, len, CK_REQUEST)) {
    finishRequest(model::Req::Failed, "Busy, try again");
    return false;
  }
  return true;
}

bool RemoteAdapter::login(int ci, const char* password) {
  if (ci < 0 || ci >= _nContacts) return false;
  uint8_t f[cp::MAX_FRAME];
  int n = cp::encLogin(f, _contacts[ci].key, password);
  if (!requestSent(f, n, ReqKind::Login, ci)) return false;
  snprintf(_req.password, sizeof _req.password, "%s", password);
  return true;
}

bool RemoteAdapter::requestStats(int ci) {
  if (ci < 0 || ci >= _nContacts) return false;
  uint8_t f[cp::MAX_FRAME];
  return requestSent(f, cp::encKeyCmd(f, cp::SEND_STATUS_REQ, _contacts[ci].key), ReqKind::Status, ci);
}

bool RemoteAdapter::requestTelemetry(int ci) {
  if (ci < 0 || ci >= _nContacts) return false;
  uint8_t f[cp::MAX_FRAME];
  return requestSent(f, cp::encTelemetryReq(f, _contacts[ci].key), ReqKind::Telemetry, ci);
}

bool RemoteAdapter::requestNeighbours(int ci) {
  if (ci < 0 || ci >= _nContacts) return false;
  // version 0, count, offset (2), order (2 = strongest first), key prefix bytes, 4 random
  uint8_t req[11] = {REQ_GET_NEIGHBOURS, 0, MAX_NEIGH, 0, 0, 2, 4};
  _plat.randomBytes(&req[7], 4);
  uint8_t f[cp::MAX_FRAME];
  _nNeighbours = 0;
  return requestSent(f, cp::encBinaryReq(f, _contacts[ci].key, req, sizeof req), ReqKind::Neighbours, ci);
}

bool RemoteAdapter::sendCommand(int ci, const char* cmd) {
  if (ci < 0 || ci >= _nContacts) return false;
  uint8_t f[cp::MAX_FRAME];
  int n = cp::encSendText(f, cp::TXT_CLI, 0, uniqueNow(), _contacts[ci].key, cmd);
  return requestSent(f, n, ReqKind::Cli, ci);
}

bool RemoteAdapter::tracePath(int ci) {
  if (!requestIdle() || ci < 0 || ci >= _nContacts) return false;
  const cp::Contact& c = _contacts[ci];
  if (c.outPathLen == 0xFF) {
    snprintf(_req.text, sizeof _req.text, "No route known yet: send a message first");
    _req.state = model::Req::Failed;
    _rev++;
    return false;
  }
  // Out along the stored route (plus the node itself if it relays), then
  // back the same way: every relay appends the SNR it heard.
  bool relays = c.type == 2 || c.type == 3;   // ADV_TYPE_REPEATER, ADV_TYPE_ROOM
  int sz = (c.outPathLen >> 6) + 1, hops = c.outPathLen & 63;
  uint8_t path[cp::MAX_PATH];
  int n = 0;
  for (int i = 0; i < hops * sz && n < cp::MAX_PATH; i++) path[n++] = c.outPath[i];
  if (relays)
    for (int i = 0; i < sz && n < cp::MAX_PATH; i++) path[n++] = c.key[i];
  for (int h = hops - (relays ? 1 : 2); h >= 0; h--)
    for (int i = 0; i < sz && n < cp::MAX_PATH; i++) path[n++] = c.outPath[h * sz + i];
  if (n == 0) {
    snprintf(_req.text, sizeof _req.text, "Direct neighbour: no relays to trace");
    _req.state = model::Req::Failed;
    _rev++;
    return false;
  }
  uint32_t tag;
  _plat.randomBytes((uint8_t*)&tag, 4);
  uint8_t flags = (uint8_t)(sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3);
  uint8_t f[cp::MAX_FRAME];
  if (!requestSent(f, cp::encTrace(f, tag, 0, flags, path, n), ReqKind::Trace, ci)) return false;
  _req.tag = tag;
  return true;
}

bool RemoteAdapter::ping(int ci) {
  if (ci < 0 || ci >= _nContacts) return false;
  if (_contacts[ci].type != 1) {   // not a companion: repeaters, rooms, sensors answer status requests
    bool ok = requestStats(ci);
    if (ok) _req.kind = ReqKind::Ping;
    return ok;
  }
  // Companions: path discovery, as the MeshCore app does it.
  uint8_t f[cp::MAX_FRAME];
  return requestSent(f, cp::encPathDiscovery(f, _contacts[ci].key), ReqKind::Ping, ci);
}

// ------------------------------------------------------------------ radio, prefs

model::Radio RemoteAdapter::radio() const {
  model::Radio r{};
  r.freqMHz = _self.freqKHz / 1000.0f;
  r.bwKHz = _self.bwHz / 1000.0f;
  r.sf = _self.sf;
  r.cr = _self.cr;
  r.txDbm = _self.txDbm;
  r.txMaxDbm = _self.txMaxDbm;
  r.repeat = _dev.repeat;
  r.rxBoost = false;   // not something the app protocol sets
  return r;
}

bool RemoteAdapter::setRadio(const model::Radio& r) {
  if (r.freqMHz < 150 || r.freqMHz > 2500 || r.sf < 5 || r.sf > 12 || r.cr < 5 || r.cr > 8 || !_synced) return false;
  uint32_t freq = (uint32_t)(r.freqMHz * 1000 + 0.5f), bw = (uint32_t)(r.bwKHz * 1000 + 0.5f);
  uint8_t f[cp::MAX_FRAME];
  if (!q(f, cp::encRadio(f, freq, bw, r.sf, r.cr, r.repeat))) return false;
  int8_t tx = r.txDbm > _self.txMaxDbm ? _self.txMaxDbm : r.txDbm;
  q(f, cp::encU8(f, cp::SET_RADIO_TX_POWER, (uint8_t)tx));
  _self.freqKHz = freq;
  _self.bwHz = bw;
  _self.sf = r.sf;
  _self.cr = r.cr;
  _self.txDbm = tx;
  _dev.repeat = r.repeat;
  configChanged();
  _rev++;
  return true;
}

// The node's settings, as the UI's Prefs show them.
void RemoteAdapter::syncPrefs() {
  model::Prefs& p = _prefs;
  bool gps = p.gpsEnabled;
  uiToPrefs();
  p.gpsEnabled = gps;
  snprintf(p.nodeName, sizeof p.nodeName, "%s", _self.name);
  p.gpsShareInAdvert = _self.advertLocPolicy != 0;
  p.autoAddContacts = !(_self.manualAdd & 1);
  p.blePin = _dev.blePin;
  auto mode = [](uint8_t m) { return m == 2 ? model::TelemMode::Everyone : m == 1 ? model::TelemMode::PerContact
                                                                                  : model::TelemMode::NoOne; };
  p.telemBattery = mode(_self.telemModes & 3);
  p.telemLocation = mode((_self.telemModes >> 2) & 3);
  p.telemSensors = mode((_self.telemModes >> 4) & 3);
}

void RemoteAdapter::setPrefs(const model::Prefs& p) {
  uint8_t f[cp::MAX_FRAME];
  if (_synced) {
    if (strcmp(p.nodeName, _self.name)) {
      if (q(f, cp::encName(f, p.nodeName))) snprintf(_self.name, sizeof _self.name, "%s", p.nodeName);
    }
    auto mode = [](model::TelemMode m) { return (uint8_t)(m == model::TelemMode::Everyone ? 2
                                                : m == model::TelemMode::PerContact ? 1 : 0); };
    uint8_t telem = (uint8_t)(mode(p.telemBattery) | mode(p.telemLocation) << 2 | mode(p.telemSensors) << 4);
    uint8_t manual = (uint8_t)((_self.manualAdd & ~1) | (p.autoAddContacts ? 0 : 1));
    uint8_t loc = p.gpsShareInAdvert ? 1 : 0;
    if (telem != _self.telemModes || manual != _self.manualAdd || loc != _self.advertLocPolicy) {
      if (q(f, cp::encOtherParams(f, manual, telem, loc, _self.multiAcks))) {
        _self.telemModes = telem;
        _self.manualAdd = manual;
        _self.advertLocPolicy = loc;
      }
    }
    if (p.presetIdx != _ui.presetIdx && p.presetIdx < presetCount() && preset(p.presetIdx).pathHashSize) {
      f[0] = cp::SET_PATH_HASH_MODE;
      f[1] = 0;
      f[2] = (uint8_t)(preset(p.presetIdx).pathHashSize - 1);
      q(f, 3);
    }
  }
  // The GPS here is this device's own.
  if (p.gpsEnabled != _prefs.gpsEnabled) _plat.setGpsPower(p.gpsEnabled);
  prefsToUi(p);
  _ui.remoteGps = p.gpsEnabled;
  saveUiPrefs();
  syncPrefs();
  _prefs.gpsEnabled = p.gpsEnabled;
  configChanged();
  _rev++;
}

// ------------------------------------------------------------------ time

uint32_t RemoteAdapter::uptimeMs() const { return millis(); }

uint32_t RemoteAdapter::now() const {
  return _clockOffset ? (uint32_t)(_clockOffset + (int32_t)(millis() / 1000)) : (uint32_t)(millis() / 1000);
}

void RemoteAdapter::setClock(uint32_t epoch) {
  _clockOffset = (int32_t)(epoch - millis() / 1000);
  _clockFromGps = true;
  uint8_t f[5];
  q(f, cp::encU32(f, cp::SET_DEVICE_TIME, epoch));   // the node goes forward only
}

// ------------------------------------------------------------------ diagnostics

model::Diag RemoteAdapter::diag() const {
  model::Diag d;
  memset(&d, 0, sizeof d);
  // The node's radio and counters (CMD_GET_STATS), this device's heap.
  d.uptimeS = _stats.uptimeS;
  d.noiseFloor = _stats.noiseFloor;
  d.lastRssi = _stats.lastRssi ? _stats.lastRssi : _lastRssi;
  d.lastSnr_x4 = _stats.lastSnr_x4 ? _stats.lastSnr_x4 : _lastSnr;
  d.rxPackets = _stats.recv;
  d.txPackets = _stats.sent;
  d.rxFlood = _stats.recvFlood;
  d.rxDirect = _stats.recvDirect;
  d.txFlood = _stats.sentFlood;
  d.txDirect = _stats.sentDirect;
  d.rxErrors = _stats.recvErrors;
  d.airTxMs = _stats.txAirS * 1000;
  d.airRxMs = _stats.rxAirS * 1000;
  d.dutyPct = _stats.uptimeS ? (uint8_t)(_stats.txAirS * 100ULL / _stats.uptimeS) : 0;
  d.queueLen = _stats.queueLen;
  d.dups = _dupsSeen;
  d.heapFree = _plat.heapFree();
  d.heapMin = _plat.heapMin();
  d.heapMaxBlock = _plat.heapMaxBlock();
  d.battMv = _plat.batteryMv();   // this device's (the node's is in link())
  d.battPct = batteryPercent(d.battMv);
  d.charging = _plat.charging();
  d.contacts = (uint16_t)_nContacts;
  d.contactsMax = (uint16_t)_maxContacts;
  d.channels = (uint8_t)channelCount();
  d.channelsMax = (uint8_t)_maxChans;
  return d;
}

// ------------------------------------------------------------------ backup (the node's own)

const char* RemoteAdapter::backupPath(bool previous) const {
  return _bkPath[previous][0] ? _bkPath[previous] : (previous ? "/meshmash/remote.bak" : "/meshmash/remote.bin");
}

// What the node holds (settings, channels with their keys, contacts as the
// node keeps them, its identity if it lets us export it) plus our UI prefs,
// in a backup file of this node's own.
bool RemoteAdapter::backupNow() {
  fs::FS* sd = _plat.sd();
  _cfgChangedAt = 0;
  _lastBackupAt = millis() | 1;
  if (!sd) {
    snprintf(_bk.error, sizeof _bk.error, "No SD card");
    _bk.card = false;
    return false;
  }
  if (!_synced) {
    snprintf(_bk.error, sizeof _bk.error, "Node not connected");
    _rev++;
    return false;
  }
  NodeSettings ns{};
  snprintf(ns.name, sizeof ns.name, "%s", _self.name);
  ns.freqKHz = _self.freqKHz;
  ns.bwHz = _self.bwHz;
  ns.sf = _self.sf;
  ns.cr = _self.cr;
  ns.txDbm = _self.txDbm;
  ns.manualAdd = _self.manualAdd;
  ns.telemModes = _self.telemModes;
  ns.advertLocPolicy = _self.advertLocPolicy;
  ns.multiAcks = _self.multiAcks;
  ns.pathHashMode = _dev.pathHashMode;
  ns.repeat = _dev.repeat;
  Backup::Section sec[8];
  int n = 0;
  // This node's message history, streamed from flash, if asked for.
  char msgsPath[32], readPath[32], hiddenPath[32];
  snprintf(msgsPath, sizeof msgsPath, "/ui/%s-msgs.bin", _histTag);
  snprintf(readPath, sizeof readPath, "/ui/%s-read.bin", _histTag);
  snprintf(hiddenPath, sizeof hiddenPath, "/ui/%s-hidden.bin", _histTag);
  if (_ui.backupMessages) {
    sec[n++] = {msgsPath, nullptr, 0, _fs};
    sec[n++] = {readPath, nullptr, 0, _fs};
    if (_fs->exists(hiddenPath)) sec[n++] = {hiddenPath, nullptr, 0, _fs};
  }
  sec[n++] = {"node", (const uint8_t*)&ns, sizeof ns};
  sec[n++] = {"channels", (const uint8_t*)_chans, (uint32_t)(_maxChans * sizeof(Chan))};
  sec[n++] = {"contacts", (const uint8_t*)_contacts, (uint32_t)(_nContacts * sizeof(cp::Contact))};
  if (_ui.backupIdentity && _havePrivKey) sec[n++] = {"identity", _privKey, sizeof _privKey};
  sec[n++] = {"/ui/prefs.bin", (const uint8_t*)&_ui, sizeof _ui};
  bool ok = Backup::writeSections(*sd, backupPath(false), backupPath(true), sec, n, now(), _dev.version, _bk.error,
                                  sizeof _bk.error);
  if (ok) _bk.error[0] = 0;
  refreshBackupInfo();
  return ok;
}

bool RemoteAdapter::readBackupSection(void* ctx, const char* name, File& f, uint32_t len) {
  RemoteAdapter& a = *(RemoteAdapter*)ctx;
  if (!strcmp(name, "node") && len == sizeof(NodeSettings)) {
    a._rs.haveSet = f.read((uint8_t*)&a._rs.set, len) == len;
  } else if (!strcmp(name, "channels")) {
    memset(a._chans, 0, a._maxChans * sizeof(Chan));
    uint32_t n = len < a._maxChans * sizeof(Chan) ? len : a._maxChans * sizeof(Chan);
    return f.read((uint8_t*)a._chans, n) == n;
  } else if (!strcmp(name, "contacts")) {
    int count = (int)(len / sizeof(cp::Contact));
    if (count > a._maxContacts) count = a._maxContacts;
    a._nContacts = 0;
    if (count && f.read((uint8_t*)a._contacts, count * sizeof(cp::Contact)) != count * sizeof(cp::Contact)) return false;
    a._nContacts = count;
  } else if (!strcmp(name, "identity") && len == 64) {
    a._rs.haveKey = f.read(a._rs.key, 64) == 64;
  } else if (!strncmp(name, "/ui/r", 5) &&
             (strstr(name, "-msgs.bin") || strstr(name, "-read.bin") || strstr(name, "-hidden.bin"))) {
    // Message history: to this node's files (the backup may be from a node
    // with another address).
    char path[32];
    const char* suffix = strstr(name, "-msgs.bin") ? "-msgs.bin" : strstr(name, "-read.bin") ? "-read.bin" : "-hidden.bin";
    snprintf(path, sizeof path, "/ui/%s%s", a._histTag, suffix);
    File out = a._fs->open(path, "w", true);
    if (!out) return false;
    uint8_t buf[256];
    for (uint32_t done = 0; done < len;) {
      int c = len - done > sizeof buf ? (int)sizeof buf : (int)(len - done);
      if ((int)f.read(buf, c) != c || out.write(buf, c) != (size_t)c) { out.close(); return false; }
      done += c;
    }
    out.close();
    a._historyRestored = true;
  } else if (!strcmp(name, "/ui/prefs.bin")) {
    UiPrefs u{};
    uint32_t n = len < sizeof u ? len : sizeof u;
    if (f.read((uint8_t*)&u, n) == n && u.magic == a._ui.magic) {
      a._ui = u;
      a.saveUiPrefs();
    }
  }
  return true;   // a section we don't know: skipped
}

// Put a backup back on the node: identity first (the node reloads its
// contacts for it), then settings, channels, contacts; then sync again.
bool RemoteAdapter::restore(bool previous) {
  fs::FS* sd = _plat.sd();
  if (!sd || !_synced) {
    snprintf(_bk.error, sizeof _bk.error, sd ? "Node not connected" : "No SD card");
    _rev++;
    return false;
  }
  memset(&_rs, 0, sizeof _rs);
  _historyRestored = false;
  if (!Backup::readSections(*sd, backupPath(previous), readBackupSection, this, _bk.error, sizeof _bk.error)) {
    _rev++;
    return false;
  }
  if (_historyRestored) {   // read the restored history afresh
    _histTag[0] = 0;
    openHistory();
  }
  _rs.active = true;
  _bk.error[0] = 0;
  uiToPrefs();
  _rev++;
  return true;
}

void RemoteAdapter::restoreStep() {
  uint8_t f[cp::MAX_FRAME];
  const NodeSettings& ns = _rs.set;
  while (_rs.active && _client.queued() < CompanionClient::QUEUE / 2) {
    switch (_rs.step) {
      case 0:
        if (_rs.haveKey) q(f, cp::encBytes(f, cp::IMPORT_PRIVATE_KEY, _rs.key, 64));
        memset(_rs.key, 0, sizeof _rs.key);
        break;
      case 1: if (_rs.haveSet) q(f, cp::encName(f, ns.name)); break;
      case 2: if (_rs.haveSet) q(f, cp::encRadio(f, ns.freqKHz, ns.bwHz, ns.sf, ns.cr, ns.repeat)); break;
      case 3: if (_rs.haveSet) q(f, cp::encU8(f, cp::SET_RADIO_TX_POWER, (uint8_t)ns.txDbm)); break;
      case 4:
        if (_rs.haveSet) q(f, cp::encOtherParams(f, ns.manualAdd, ns.telemModes, ns.advertLocPolicy, ns.multiAcks));
        break;
      case 5:
        if (_rs.haveSet) {
          f[0] = cp::SET_PATH_HASH_MODE;
          f[1] = 0;
          f[2] = ns.pathHashMode;
          q(f, 3);
        }
        break;
      case 6:   // every channel slot, empty ones too
        if (_rs.idx < _maxChans) {
          q(f, cp::encSetChannel(f, (uint8_t)_rs.idx, _chans[_rs.idx].name, _chans[_rs.idx].secret));
          _rs.idx++;
          continue;
        }
        _rs.idx = 0;
        break;
      case 7:
        if (_rs.idx < _nContacts) {
          q(f, cp::encodeContact(f, _contacts[_rs.idx]));
          _rs.idx++;
          continue;
        }
        break;
      default:
        _rs.active = false;
        _havePrivKey = false;
        startSync();   // read it all back as the node now has it
        _rev++;
        return;
    }
    _rs.step++;
  }
}

bool RemoteAdapter::rebootNode() {
  uint8_t f[8];
  return q(f, cp::encText(f, cp::REBOOT, "reboot"));
}

bool RemoteAdapter::resetNode() {
  uint8_t f[8];
  return q(f, cp::encText(f, cp::FACTORY_RESET, "reset"));
}

void RemoteAdapter::factoryReset() {
  // This device's own data for this mode; the node is reset separately
  // (resetNode).
  _msgs.wipe();
  _fs->remove("/ui/prefs.bin");
  _fs->remove(kPeerFile);
  _plat.reboot();
}

// Share this device's GPS position as the node's, when the node shares its
// position in adverts: on a fix, then when it moves or every 10 minutes.
void RemoteAdapter::positionTick(uint32_t ms) {
  if (!_synced || !_self.advertLocPolicy || ms - _positionAt < 30000) return;
  model::Gps g = _plat.gps();
  if (!g.fix) return;
  int32_t dLat = g.lat_e6 - _sentLat, dLon = g.lon_e6 - _sentLon;
  bool moved = dLat > 1000 || dLat < -1000 || dLon > 1000 || dLon < -1000;   // ~100 m
  if (!moved && _positionAt && ms - _positionAt < 600000) return;
  uint8_t f[cp::MAX_FRAME];
  if (q(f, cp::encLatLon(f, g.lat_e6, g.lon_e6))) {
    _positionAt = ms | 1;
    _sentLat = g.lat_e6;
    _sentLon = g.lon_e6;
  }
}

}  // namespace app
