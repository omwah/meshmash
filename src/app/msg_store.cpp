#include "app/msg_store.h"
#include <string.h>
#include <stdio.h>

namespace app {


struct __attribute__((packed)) MessageStore::Rec {
  uint32_t seq;          // 0 = empty slot
  ThreadKey key;
  uint8_t flags;         // bit0 outgoing, bit1 the incoming path is stored, bits 2-3 its hash size - 1,
                         // bits 4-7 with `status`: the sender's clock offset of an incoming message (packSkew)
  uint8_t status, retries, repeats;
  int8_t snr_x4, hops;
  uint32_t ts;
  uint32_t ack;          // outgoing: its ack; incoming: with retries and repeats, 6 bytes of path (see packPath)
  char sender[24];
  char text[model::TEXT_LEN];
#ifdef MC_LONG_PATH
  uint8_t path[model::PATH_BYTES];   // incoming: the relays it passed (flag bit 1: stored; bits 2-3: hash size - 1)
#endif
};
static_assert(sizeof(MessageStore::Rec) == MessageStore::RECORD_BYTES, "record size");

#ifdef MC_LONG_PATH
// Records written before the route had a field of its own are 209 bytes: the same fields, with an
// incoming message's first six route bytes packed into ack, retries and repeats. They are converted
// when a ring file is rebuilt and when an archive file is first used.
static constexpr size_t kOldRec = 209;
static void upgrade(const uint8_t* old, MessageStore::Rec& r) {
  memset(&r, 0, sizeof r);
  memcpy(&r, old, kOldRec);
  if (!(r.flags & 1) && (r.flags & 2)) {
    memcpy(r.path, &r.ack, 4);
    r.path[4] = r.retries;
    r.path[5] = r.repeats;
    r.ack = 0;
    r.retries = r.repeats = 0;
  }
}
#endif

// The record size of a ring file (or of one being adopted): the current one, or the old one.
static size_t srcRecSize(size_t fileSize) {
#ifdef MC_LONG_PATH
  if (fileSize != (size_t)MessageStore::CAPACITY * sizeof(MessageStore::Rec) && fileSize % kOldRec == 0) return kOldRec;
#endif
  return sizeof(MessageStore::Rec);
}

// Record at byte `at` of a file whose records are `rs` bytes, in the current layout.
static bool readSrc(File& f, size_t at, size_t rs, MessageStore::Rec& r) {
  if (!f.seek(at)) return false;
  if (rs == sizeof r) return f.read((uint8_t*)&r, sizeof r) == sizeof r;
#ifdef MC_LONG_PATH
  uint8_t buf[kOldRec];
  if (rs != kOldRec || f.read(buf, kOldRec) != (int)kOldRec) return false;
  upgrade(buf, r);
  return true;
#else
  return false;
#endif
}

// An incoming message has no delivery status either, so those 8 bits and 4 flag bits hold how far
// the sender's clock was from ours: known (1 bit), sign, a unit (minutes, hours, days, weeks) and
// 8 bits of count. Rounded, which is plenty: usually it is seconds, or hundreds of days.
static uint16_t packSkew(int32_t d) {
  uint32_t mag = d < 0 ? (uint32_t)(-(int64_t)d) : (uint32_t)d;
  static const uint32_t kUnit[4] = {60, 3600, 86400, 604800};
  int u = 0;
  while (u < 3 && mag / kUnit[u] > 255) u++;
  uint32_t v = mag / kUnit[u];
  if (v > 255) v = 255;
  return (uint16_t)(0x800 | (d < 0 ? 0x400 : 0) | (u << 8) | v);
}

static int32_t unpackSkew(uint16_t code) {
  static const uint32_t kUnit[4] = {60, 3600, 86400, 604800};
  int32_t mag = (int32_t)((code & 0xFF) * kUnit[(code >> 8) & 3]);
  return code & 0x400 ? -mag : mag;
}

// An incoming message has no ack, retries or repeats, so those six bytes hold the
// relays it passed (records from before this stay readable: no flag, no path).
void MessageStore::packPath(Rec& r, const model::Message& m) {
  if (!m.pathSize || !m.pathN) return;
#ifdef MC_LONG_PATH
  memcpy(r.path, m.path, sizeof r.path);
#else
  uint8_t b[6] = {};
  memcpy(b, m.path, sizeof b);
  memcpy(&r.ack, b, 4);
  r.retries = b[4];
  r.repeats = b[5];
#endif
  r.flags |= 2 | (uint8_t)((m.pathSize - 1) << 2);
}

void MessageStore::unpackPath(const Rec& r, model::Message& m) {
  m.retries = m.repeats = 0;
  m.status = model::Delivery::None;
  if (!(r.flags & 2)) return;
#ifdef MC_LONG_PATH
  memcpy(m.path, r.path, sizeof m.path);
#else
  uint8_t b[6];
  memcpy(b, &r.ack, 4);
  b[4] = r.retries;
  b[5] = r.repeats;
  memcpy(m.path, b, sizeof b);
#endif
  m.pathSize = (uint8_t)(((r.flags >> 2) & 3) + 1);
  int n = m.hops > 0 ? m.hops : 0;
  m.pathN = (uint8_t)(n * m.pathSize > (int)sizeof m.path ? sizeof m.path / m.pathSize : n);
}

bool ThreadKey::operator==(const ThreadKey& o) const { return kind == o.kind && !memcmp(id, o.id, 6); }

ThreadKey ThreadKey::direct(const uint8_t* pk) {
  ThreadKey k{};
  k.kind = 0;
  memcpy(k.id, pk, 6);
  return k;
}

ThreadKey ThreadKey::channel(uint8_t slot) {
  ThreadKey k{};
  k.kind = 1;
  k.id[0] = slot;
  return k;
}

bool MessageStore::readRec(int slot, Rec& r) {
  File f = _fs->open(_file, "r");
  if (!f || !f.seek((uint32_t)slot * sizeof(Rec))) return false;
  bool ok = f.read((uint8_t*)&r, sizeof r) == sizeof r;
  f.close();
  return ok;
}

// Only builds with a ring other than the default size carry this (the Cardputer's flash is tight
// and its ring has never changed size).
#if MSG_RING_RECORDS != 512
// The ring's size is a build setting. A file made for another size is rebuilt once, at boot: each
// record goes to the slot its seq gives here, and the older one of two that share a slot goes to
// the card (kept in the archive, shown when scrolling back).
void MessageStore::fitRing() {
  File f = _fs->exists(_file) ? _fs->open(_file, "r") : File();
  if (!f) return;
  const size_t want = (size_t)CAPACITY * sizeof(Rec), have = f.size();
  const size_t rs = srcRecSize(have);
  if (!have || have == want || have % rs) { f.close(); return; }
  f.close();
  if (rebuildRing()) return;   // through the card: the old file goes before the new one is written
  f = _fs->open(_file, "r");
  if (!f) return;
  char tmp[40];
  snprintf(tmp, sizeof tmp, "%s.new", _file);
  _fs->remove(tmp);
  Rec z, r, occ;
  memset(&z, 0, sizeof z);
  File o = _fs->open(tmp, "w", true);
  bool ok = (bool)o;
  for (int i = 0; ok && i < CAPACITY; i++) ok = o.write((const uint8_t*)&z, sizeof z) == sizeof z;
  if (o) o.close();
  o = ok ? _fs->open(tmp, "r+") : File();
  ok = ok && (bool)o;
  uint32_t moved = 0;
  for (size_t at = 0; ok && at < have; at += rs) {
    if (!readSrc(f, at, rs, r)) { ok = false; break; }
    if (!r.seq) continue;
    uint32_t pos = (uint32_t)(r.seq % CAPACITY) * sizeof(Rec);
    ok = o.seek(pos) && o.read((uint8_t*)&occ, sizeof occ) == sizeof occ;
    if (!ok) break;
    if (occ.seq && occ.seq > r.seq) {   // the newer one is there already
      if (_sd) archive(r);
    } else {
      if (occ.seq && _sd) archive(occ);
      ok = o.seek(pos) && o.write((const uint8_t*)&r, sizeof r) == sizeof r;
    }
    moved++;
  }
  f.close();
  if (o) o.close();
  if (ok) {
    _fs->remove(_file);
    ok = _fs->rename(tmp, _file);
  }
  if (!ok) _fs->remove(tmp);   // the old file stays as it was
  else _stats.resized = moved;
}

// Rebuild the ring file through the card, which holds the new ring while the flash file is deleted
// and written again: the flash never has to hold both, and a file the flash will no longer open
// for writing is replaced by a fresh one. Records that share a slot keep the newer one; the older
// goes to the archive. Needs the card.
bool MessageStore::rebuildRing() {
  if (!_sd) return false;
  File f = _fs->exists(_file) ? _fs->open(_file, "r") : File();
  if (!f) return false;
  const size_t have = f.size(), want = (size_t)CAPACITY * sizeof(Rec);
  const size_t rs = srcRecSize(have);
  if (!have || have % rs) { f.close(); return false; }
  const char* tmp = "/meshmash/ring.tmp";
  _sd->mkdir("/meshmash");
  _sd->remove(tmp);
  Rec z, r, occ;
  memset(&z, 0, sizeof z);
  File o = _sd->open(tmp, "w", true);
  bool ok = (bool)o;
  for (int i = 0; ok && i < CAPACITY; i++) ok = o.write((const uint8_t*)&z, sizeof z) == sizeof z;
  if (o) o.close();
  o = ok ? _sd->open(tmp, "r+") : File();
  ok = ok && (bool)o;
  uint32_t moved = 0;
  for (size_t at = 0; ok && at < have; at += rs) {
    if (!readSrc(f, at, rs, r)) { ok = false; break; }
    if (!r.seq) continue;
    uint32_t pos = (uint32_t)(r.seq % CAPACITY) * sizeof(Rec);
    ok = o.seek(pos) && o.read((uint8_t*)&occ, sizeof occ) == sizeof occ;
    if (!ok) break;
    if (occ.seq && occ.seq > r.seq) {
      archive(r);
    } else {
      if (occ.seq) archive(occ);
      ok = o.seek(pos) && o.write((const uint8_t*)&r, sizeof r) == sizeof r;
    }
    moved++;
  }
  f.close();
  if (o) o.close();
  if (!ok) { _sd->remove(tmp); return false; }   // the old file is untouched
  // Replace the flash file. From here the card's copy is the only whole one until the end.
  _fs->remove(_file);
  File n = _fs->open(_file, "w", true);
  File c = _sd->open(tmp, "r");
  ok = n && c;
  for (size_t at = 0; ok && at < want; at += sizeof(Rec))
    ok = c.read((uint8_t*)&r, sizeof r) == sizeof r && n.write((const uint8_t*)&r, sizeof r) == sizeof r;
  if (n) n.close();
  if (c) c.close();
  if (ok) {
    File chk = _fs->open(_file, "r");
    ok = chk && chk.size() == want;
    if (chk) chk.close();
  }
  if (ok) _sd->remove(tmp);   // else it stays on the card for recovery
  else _err = 5;
  if (ok) _stats.resized = moved;
  return ok;
}
#else
void MessageStore::fitRing() {}
bool MessageStore::rebuildRing() { return false; }
#endif

bool MessageStore::writeRec(const Rec& r) {
  // Open read/write so the rest of the ring is kept; create on first use.
  File f = _fs->exists(_file) ? _fs->open(_file, "r+") : _fs->open(_file, "w", true);
  if (!f) { _err = 1; return false; }
  uint32_t at = (uint32_t)(r.seq % CAPACITY) * sizeof(Rec);
  // SPIFFS can't seek past the end of a file: grow it with empty records
  // up to this slot first.
  size_t size = f.size();
  // A different (older) record in this slot is about to go: archive it once the new one is in.
  Rec old;
  bool haveOld = _sd && at + sizeof(Rec) <= size && f.seek(at) && f.read((uint8_t*)&old, sizeof old) == sizeof old &&
                 old.seq && old.seq != r.seq;
  bool ok = true;
  if (at > size) {
    static const uint8_t zeros[64] = {};
    ok = f.seek(size);
    for (size_t left = at - size; ok && left;) {
      size_t n = left < sizeof zeros ? left : sizeof zeros;
      ok = f.write(zeros, n) == n;
      left -= n;
    }
    if (!ok) _err = 2;
  }
  if (ok) {
    ok = f.seek(at) && f.write((const uint8_t*)&r, sizeof r) == sizeof r;
    if (!ok) _err = 3;
  }
  _stats.ringBytes = (uint32_t)(at + sizeof(Rec) > size ? at + sizeof(Rec) : size);
  f.close();
  // A full flash can take the write and lose it at close: read the record back.
  if (ok) {
    Rec back;
    ok = readRec((int)(r.seq % CAPACITY), back) && back.seq == r.seq && back.key == r.key;
    if (!ok) _err = 4;
  }
  if (ok && haveOld) archive(old) ? _stats.archived++ : _stats.archiveFails++;
  return ok;
}

#ifdef MC_HISTORY_ON_SD
// The history moved to the card: what the flash still holds (the ring, the read marks, the hidden
// list) is merged in and then deleted there, which frees the flash. A file that cannot be moved
// stays where it is.
void MessageStore::adoptFrom(fs::FS* flash, const char* tag) {
  const char* tp = tag && tag[0] ? tag : "";
  const char* dash = tag && tag[0] ? "-" : "";
  char fr[40], fread[40], fhid[40];
  snprintf(fr, sizeof fr, "/ui/%s%smsgs.bin", tp, dash);
  snprintf(fread, sizeof fread, "/ui/%s%sread.bin", tp, dash);
  snprintf(fhid, sizeof fhid, "/ui/%s%shidden.bin", tp, dash);
  const size_t want = (size_t)CAPACITY * sizeof(Rec);
  if (flash->exists(fr)) {
    File src = flash->open(fr, "r");
    const size_t rs = src ? srcRecSize(src.size()) : sizeof(Rec);
    bool ok = src && src.size() % rs == 0;
    if (ok && !_fs->exists(_file)) {   // no ring on the card yet: an empty one
      Rec z;
      memset(&z, 0, sizeof z);
      File n = _fs->open(_file, "w", true);
      ok = (bool)n;
      for (int i = 0; ok && i < CAPACITY; i++) ok = n.write((const uint8_t*)&z, sizeof z) == sizeof z;
      if (n) n.close();
    }
    File o = ok ? _fs->open(_file, "r+") : File();
    ok = ok && o && o.size() == want;
    Rec r, occ;
    uint32_t moved = 0;
    for (size_t at = 0; ok && at < src.size(); at += rs) {
      if (!readSrc(src, at, rs, r)) { ok = false; break; }
      if (!r.seq) continue;
      uint32_t pos = (uint32_t)(r.seq % CAPACITY) * sizeof(Rec);
      ok = o.seek(pos) && o.read((uint8_t*)&occ, sizeof occ) == sizeof occ;
      if (!ok) break;
      if (occ.seq == r.seq) { moved++; continue; }   // already there
      if (occ.seq && occ.seq > r.seq) {
        archive(r);
      } else {
        if (occ.seq) archive(occ);
        ok = o.seek(pos) && o.write((const uint8_t*)&r, sizeof r) == sizeof r;
      }
      moved++;
    }
    if (src) src.close();
    if (o) o.close();
    if (ok) {
      flash->remove(fr);
      _stats.resized += moved;
    }
  }
  auto move = [&](const char* from, const char* to) {
    if (!flash->exists(from)) return;
    bool ok = true;
    if (!_fs->exists(to)) {
      File a = flash->open(from, "r");
      File b = _fs->open(to, "w", true);
      ok = a && b;
      uint8_t buf[64];
      for (int n; ok && (n = (int)a.read(buf, sizeof buf)) > 0;) ok = b.write(buf, n) == (size_t)n;
      if (a) a.close();
      if (b) b.close();
    }
    if (ok) flash->remove(from);
  };
  move(fread, _readFile);
  move(fhid, _hiddenFile);
}
#endif

bool MessageStore::begin(fs::FS* fs, const char* tag, fs::FS* flash) {
  _fs = fs;
  _onCard = flash != nullptr;
  const char* dir = _onCard ? "/meshmash" : "/ui";
  // The local node's history keeps its original file names.
  if (tag && tag[0]) {
    snprintf(_file, sizeof _file, "%s/%s-msgs.bin", dir, tag);
    snprintf(_readFile, sizeof _readFile, "%s/%s-read.bin", dir, tag);
    snprintf(_hiddenFile, sizeof _hiddenFile, "%s/%s-hidden.bin", dir, tag);
    snprintf(_archDir, sizeof _archDir, "/meshmash/history/%s", tag);
  } else {
    snprintf(_file, sizeof _file, "%s/msgs.bin", dir);
    snprintf(_readFile, sizeof _readFile, "%s/read.bin", dir);
    snprintf(_hiddenFile, sizeof _hiddenFile, "%s/hidden.bin", dir);
    snprintf(_archDir, sizeof _archDir, "/meshmash/history");
  }
  _nThreads = 0;
  _nextSeq = 1;
  _stats = Stats{};
  _stats.onCard = _onCard;
  _fs->mkdir(dir);
  fitRing();
#ifdef MC_HISTORY_ON_SD
  if (flash) adoptFrom(flash, tag);
#endif
  loadRead();
  loadHidden();
  {
    // A ring that could not be brought to this build's record size is put aside, not misread.
    File chk = _fs->exists(_file) ? _fs->open(_file, "r") : File();
    size_t sz = chk ? chk.size() : 0;
    if (chk) chk.close();
    if (sz && (sz % sizeof(Rec) || sz > (size_t)CAPACITY * sizeof(Rec))) {
      char old[44];
      snprintf(old, sizeof old, "%s.old", _file);
      _fs->remove(old);
      _fs->rename(_file, old);
    }
  }
  File f = _fs->open(_file, "r");
  if (f) {
    Rec r;
    _stats.ringBytes = (uint32_t)f.size();
    while (f.read((uint8_t*)&r, sizeof r) == sizeof r) {
      if (!r.seq) continue;
      if (r.seq >= _nextSeq) _nextSeq = r.seq + 1;
      ThreadInfo* t = touch(r.key);
      if (!t) continue;
      if (!(r.flags & 1) && r.seq > t->readSeq) t->unread++;
      if (r.seq > t->lastSeq) {
        t->lastSeq = r.seq;
        t->lastTs = r.ts;
        t->lastOut = r.flags & 1;
        snprintf(t->preview, sizeof t->preview, "%s%s%s", r.sender, r.sender[0] ? ": " : "", r.text);
      }
    }
    f.close();
  }
  // Drop read markers of conversations whose messages were all overwritten.
  for (int i = 0; i < _nThreads;)
    if (!_threads[i].lastSeq) _threads[i] = _threads[--_nThreads];
    else i++;
  sortThreads();
  _stats.flashSeq = _nextSeq - 1;
  return true;
}

ThreadInfo* MessageStore::touch(const ThreadKey& k) {
  for (int i = 0; i < _nThreads; i++)
    if (_threads[i].key == k) return &_threads[i];
  if (_nThreads == MAX_THREADS) {
    // Evict the oldest conversation summary (its records stay on flash).
    int old = 0;
    for (int i = 1; i < _nThreads; i++)
      if (_threads[i].lastSeq < _threads[old].lastSeq) old = i;
    _threads[old] = _threads[--_nThreads];
  }
  ThreadInfo* t = &_threads[_nThreads++];
  memset(t, 0, sizeof *t);
  t->key = k;
  return t;
}

void MessageStore::sortThreads() {
  for (int i = 1; i < _nThreads; i++) {
    ThreadInfo v = _threads[i];
    int j = i - 1;
    while (j >= 0 && _threads[j].lastSeq < v.lastSeq) { _threads[j + 1] = _threads[j]; j--; }
    _threads[j + 1] = v;
  }
}

const ThreadInfo* MessageStore::thread(int i) const { return i >= 0 && i < _nThreads ? &_threads[i] : nullptr; }

const ThreadInfo* MessageStore::find(const ThreadKey& k) const {
  for (int i = 0; i < _nThreads; i++)
    if (_threads[i].key == k) return &_threads[i];
  return nullptr;
}

uint16_t MessageStore::unreadTotal() const {
  uint32_t n = 0;
  for (int i = 0; i < _nThreads; i++) n += _threads[i].unread;
  return n > 0xFFFF ? 0xFFFF : (uint16_t)n;
}

uint32_t MessageStore::append(const ThreadKey& k, const model::Message& m, uint32_t ack) {
  Rec r;
  memset(&r, 0, sizeof r);
  r.seq = _nextSeq++;
  r.key = k;
  r.flags = m.outgoing ? 1 : 0;
  r.status = (uint8_t)m.status;
  if (!m.outgoing && m.skewKnown) {
    uint16_t code = packSkew(m.skew);
    r.status = (uint8_t)(code & 0xFF);
    r.flags |= (uint8_t)((code >> 8) << 4);
  }
  r.retries = m.retries;
  r.repeats = m.repeats;
  r.snr_x4 = m.snr_x4;
  r.hops = m.hops;
  r.ts = m.ts;
  r.ack = ack;
  if (!m.outgoing) packPath(r, m);
  snprintf(r.sender, sizeof r.sender, "%s", m.sender);
  snprintf(r.text, sizeof r.text, "%s", m.text);
  // Is the open window showing this conversation's newest message? (Checked
  // before the new message updates the summary.)
  bool wasNewest = _hasOpen && _open == k && atNewest();
  // A write that fails (the flash is full) still shows: the message stays in the window and the
  // chats list, and the failure is counted for Diagnostics.
  bool saved = writeRec(r);
  if (!saved && _err == 1 && !_rebuilt && _sd) {   // the flash will not open the ring: start it afresh
    _rebuilt = true;
    if (rebuildRing()) saved = writeRec(r);
  }
  if (saved) {
    _stats.writes++;
    _stats.flashSeq = r.seq;
    _stats.lastOkTs = m.ts;
  } else {
    _stats.writeFails++;
    _stats.lastErr = _err;
    _stats.lastErrTs = m.ts;
  }

  ThreadInfo* t = touch(k);
  t->lastSeq = r.seq;
  t->lastTs = m.ts;
  t->lastOut = m.outgoing;
  if (m.outgoing) {
    t->readSeq = r.seq;   // replying marks the conversation read
    t->unread = 0;
  } else {
    t->unread++;
  }
  snprintf(t->preview, sizeof t->preview, "%s%s%s", m.sender, m.sender[0] ? ": " : "", m.text);
  sortThreads();

  // Add to the open window only if it shows the newest messages; when the
  // user has scrolled back, the new message waits below (newer()).
  if (wasNewest) {
    if (_nCache == CACHE) {
      memmove(_cache, _cache + 1, sizeof(_cache[0]) * (CACHE - 1));
      memmove(_cacheSeq, _cacheSeq + 1, sizeof(_cacheSeq[0]) * (CACHE - 1));
      _nCache--;
    }
    _cache[_nCache] = m;
    _cache[_nCache].seq = r.seq;
    _cacheSeq[_nCache++] = r.seq;
  }
  return r.seq;
}

bool MessageStore::setStatus(uint32_t seq, model::Delivery st, uint8_t retries) {
  Rec r;
  if (!readRec(seq % CAPACITY, r) || r.seq != seq) return false;
  r.status = (uint8_t)st;
  r.retries = retries;
  for (int i = 0; i < _nCache; i++)
    if (_cacheSeq[i] == seq) { _cache[i].status = st; _cache[i].retries = retries; }
  return writeRec(r);
}

bool MessageStore::setRepeats(uint32_t seq, uint8_t repeats) {
  Rec r;
  if (!readRec(seq % CAPACITY, r) || r.seq != seq) return false;
  r.repeats = repeats;
  for (int i = 0; i < _nCache; i++)
    if (_cacheSeq[i] == seq) _cache[i].repeats = repeats;
  return writeRec(r);
}

bool MessageStore::text(uint32_t seq, char* out, int n) {
  Rec r;
  if (!readRec(seq % CAPACITY, r) || r.seq != seq) return false;
  snprintf(out, n, "%s", r.text);
  return true;
}

bool MessageStore::lastText(const ThreadKey& k, char* out, int n) {
  const ThreadInfo* t = find(k);
  Rec r;
  if (!t || !readRec(t->lastSeq % CAPACITY, r) || r.seq != t->lastSeq) return false;
  snprintf(out, n, "%s%s%s%s", t->lastOut ? "You: " : "", r.sender, r.sender[0] ? ": " : "", r.text);
  return true;
}

bool MessageStore::ackOf(uint32_t seq, uint32_t& ack) {
  Rec r;
  if (!readRec(seq % CAPACITY, r) || r.seq != seq) return false;
  ack = r.ack;
  return true;
}

void MessageStore::open(const ThreadKey& k) {
  _open = k;
  _hasOpen = true;
  _nCache = 0;
  File f = _fs->open(_file, "r");
  if (!f) return;
  // Keep the newest CACHE records of this thread, in sequence order.
  Rec r;
  while (f.read((uint8_t*)&r, sizeof r) == sizeof r) {
    if (!r.seq || !(r.key == k)) continue;
    int pos = _nCache;
    if (_nCache == CACHE) {
      if (r.seq < _cacheSeq[0]) continue;
      memmove(_cache, _cache + 1, sizeof(_cache[0]) * (CACHE - 1));
      memmove(_cacheSeq, _cacheSeq + 1, sizeof(_cacheSeq[0]) * (CACHE - 1));
      pos = --_nCache;
    }
    while (pos > 0 && _cacheSeq[pos - 1] > r.seq) {
      _cache[pos] = _cache[pos - 1];
      _cacheSeq[pos] = _cacheSeq[pos - 1];
      pos--;
    }
    toMessage(r, _cache[pos]);
    _cacheSeq[pos] = r.seq;
    _nCache++;
  }
  f.close();
}

void MessageStore::toMessage(const Rec& r, model::Message& m) {
  memset(&m, 0, sizeof m);
  m.ts = r.ts;
  m.outgoing = r.flags & 1;
  m.status = (model::Delivery)r.status;
  m.retries = r.retries;
  m.repeats = r.repeats;
  m.snr_x4 = r.snr_x4;
  m.hops = r.hops;
  m.seq = r.seq;
  if (!m.outgoing) {
    unpackPath(r, m);
    uint16_t code = (uint16_t)(r.status | ((r.flags >> 4) << 8));
    if (code & 0x800) {
      m.skewKnown = true;
      m.skew = unpackSkew(code);
    }
  }
  snprintf(m.sender, sizeof m.sender, "%s", r.sender);
  snprintf(m.text, sizeof m.text, "%s", r.text);
}

// ---- archive (SD card)

void MessageStore::archivePath(const ThreadKey& k, char* out, int n) const {
#ifdef MC_LONG_PATH
  const char* ext = "m2";   // records with the whole route; the old ones were .bin
#else
  const char* ext = "bin";
#endif
  snprintf(out, n, "%s/%c%02x%02x%02x%02x%02x%02x.%s", _archDir, k.kind ? 'c' : 'd', k.id[0], k.id[1], k.id[2],
           k.id[3], k.id[4], k.id[5], ext);
}

// The archive file of a conversation; an older one (209-byte records) is converted the first time.
void MessageStore::archiveFile(const ThreadKey& k, char* out, int n) {
  archivePath(k, out, n);
#ifdef MC_LONG_PATH
  if (!_sd || _sd->exists(out)) return;
  char old[64];
  snprintf(old, sizeof old, "%.*sbin", (int)strlen(out) - 2, out);   // ...m2 -> ...bin
  if (!_sd->exists(old)) return;
  File a = _sd->open(old, "r");
  File b = a && a.size() % kOldRec == 0 ? _sd->open(out, "w", true) : File();
  bool ok = a && b;
  uint8_t buf[kOldRec];
  Rec r;
  size_t count = 0;
  while (ok && a.read(buf, kOldRec) == (int)kOldRec) {
    upgrade(buf, r);
    ok = b.write((const uint8_t*)&r, sizeof r) == sizeof r;
    count++;
  }
  if (a) a.close();
  if (b) b.close();
  if (ok) {
    File chk = _sd->open(out, "r");
    ok = chk && chk.size() == count * sizeof(Rec);
    if (chk) chk.close();
  }
  if (ok) _sd->remove(old);
  else _sd->remove(out);   // the old file stays, and is tried again next time
#endif
}

bool MessageStore::archive(const Rec& r) {
  char path[64];
  archiveFile(r.key, path, sizeof path);
  _sd->mkdir("/meshmash");
  _sd->mkdir("/meshmash/history");
  _sd->mkdir(_archDir);
  File f = _sd->open(path, "a", true);
  if (!f) return false;
  bool ok = f.write((const uint8_t*)&r, sizeof r) == sizeof r;   // appended in order: the file is sorted by seq
  f.close();
  return ok;
}

MessageStore::Stats MessageStore::stats() const {
  Stats s = _stats;
  s.nextSeq = _nextSeq;
  s.archiveOn = _sd != nullptr;
  return s;
}

// ---- the window

int MessageStore::findSeqs(const ThreadKey& k, uint32_t from, bool older, uint32_t* out, int max) {
  // Keep the `max` candidates closest to `from`, sorted ascending.
  int n = 0;
  auto offer = [&](uint32_t seq) {
    if (older ? seq >= from : seq <= from) return;
    for (int i = 0; i < n; i++)
      if (out[i] == seq) return;
    if (n == max) {
      // Older: drop the smallest (furthest back); newer: drop the largest.
      if (older ? seq < out[0] : seq > out[n - 1]) return;
      if (older) memmove(out, out + 1, sizeof(uint32_t) * --n);
      else n--;
    }
    int p = n++;
    while (p > 0 && out[p - 1] > seq) { out[p] = out[p - 1]; p--; }
    out[p] = seq;
  };
  Rec r;
  File f = _fs->open(_file, "r");
  if (f) {
    while (f.read((uint8_t*)&r, sizeof r) == sizeof r)
      if (r.seq && r.key == k) offer(r.seq);
    f.close();
  }
  if (older && n < max && _sd) {
    char path[64];
    archiveFile(k, path, sizeof path);
    File a = _sd->exists(path) ? _sd->open(path, "r") : File();
    if (a) {
      while (a.read((uint8_t*)&r, sizeof r) == sizeof r)
        if (r.seq && r.key == k) offer(r.seq);
      a.close();
    }
  }
  return n;
}

bool MessageStore::loadSeq(const ThreadKey& k, uint32_t seq, model::Message& m) {
  Rec r;
  if (readRec(seq % CAPACITY, r) && r.seq == seq && r.key == k) {
    toMessage(r, m);
    return true;
  }
  if (!_sd) return false;
  char path[64];
  archiveFile(k, path, sizeof path);
  File a = _sd->exists(path) ? _sd->open(path, "r") : File();
  bool found = false;
  while (a && !found && a.read((uint8_t*)&r, sizeof r) == sizeof r)
    if (r.seq == seq) { toMessage(r, m); found = true; }
  if (a) a.close();
  return found;
}

int MessageStore::history(const ThreadKey& k, uint32_t upTo, model::Message* out, int max) {
  if (max < 1 || max > 128) return 0;
  uint32_t seqs[128];
  int n = findSeqs(k, upTo + 1, true, seqs, max);
  int got = 0;
  bool have[128] = {};
  auto take = [&](const Rec& r) {
    if (!r.seq || !(r.key == k)) return;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {   // seqs is sorted ascending
      int mid = (lo + hi) / 2;
      if (seqs[mid] == r.seq) {
        if (!have[mid]) { have[mid] = true; toMessage(r, out[mid]); got++; }
        return;
      }
      if (seqs[mid] < r.seq) lo = mid + 1; else hi = mid - 1;
    }
  };
  Rec r;
  File f = _fs->open(_file, "r");
  if (f) {
    while (f.read((uint8_t*)&r, sizeof r) == sizeof r) take(r);
    f.close();
  }
  if (got < n && _sd) {
    char path[64];
    archiveFile(k, path, sizeof path);
    File a = _sd->exists(path) ? _sd->open(path, "r") : File();
    if (a) {
      while (a.read((uint8_t*)&r, sizeof r) == sizeof r) take(r);
      a.close();
    }
  }
  int w = 0;   // close the gaps of records that vanished between the scans
  for (int i = 0; i < n; i++)
    if (have[i]) { if (w != i) out[w] = out[i]; w++; }
  return w;
}

int MessageStore::around(const ThreadKey& k, uint32_t seq, int before, int after, model::Message* out, int max) {
  if (max < 1 || before < 1) return 0;
  if (before > max) before = max;
  int n = history(k, seq, out, before);
  if (n < 1 || out[n - 1].seq != seq) return n;   // the message is not there: no context after it either
  uint32_t seqs[64];
  if (after > 64) after = 64;
  if (after > max - n) after = max - n;
  int m = after > 0 ? findSeqs(k, seq, false, seqs, after) : 0;
  for (int i = 0; i < m && n < max; i++)
    if (loadSeq(k, seqs[i], out[n])) n++;
  return n;
}

int MessageStore::older() {
  if (!_hasOpen || !_nCache) return 0;
  uint32_t seqs[PAGE];
  int n = findSeqs(_open, _cacheSeq[0], true, seqs, PAGE);
  if (!n) return 0;
  // Make room at the front, dropping the newest messages off the end.
  int keep = _nCache + n > CACHE ? CACHE - n : _nCache;
  memmove(_cache + n, _cache, sizeof(_cache[0]) * keep);
  memmove(_cacheSeq + n, _cacheSeq, sizeof(_cacheSeq[0]) * keep);
  int got = 0;
  for (int i = 0; i < n; i++)
    if (loadSeq(_open, seqs[i], _cache[got])) _cacheSeq[got++] = seqs[i];
  if (got < n) {   // a record vanished between the scans: close the gap
    memmove(_cache + got, _cache + n, sizeof(_cache[0]) * keep);
    memmove(_cacheSeq + got, _cacheSeq + n, sizeof(_cacheSeq[0]) * keep);
  }
  _nCache = got + keep;
  return got;
}

int MessageStore::newer() {
  if (!_hasOpen || !_nCache || atNewest()) return 0;
  uint32_t seqs[PAGE];
  int n = findSeqs(_open, _cacheSeq[_nCache - 1], false, seqs, PAGE);
  if (!n) return 0;
  // Drop the oldest messages off the front to make room at the end.
  int drop = _nCache + n > CACHE ? _nCache + n - CACHE : 0;
  memmove(_cache, _cache + drop, sizeof(_cache[0]) * (_nCache - drop));
  memmove(_cacheSeq, _cacheSeq + drop, sizeof(_cacheSeq[0]) * (_nCache - drop));
  _nCache -= drop;
  int got = 0;
  for (int i = 0; i < n; i++)
    if (loadSeq(_open, seqs[i], _cache[_nCache])) { _cacheSeq[_nCache++] = seqs[i]; got++; }
  return got;
}

bool MessageStore::atNewest() const {
  const ThreadInfo* t = find(_open);
  return !t || !_nCache || _cacheSeq[_nCache - 1] >= t->lastSeq;
}

void MessageStore::clear(const ThreadKey& k) {
  File f = _fs->exists(_file) ? _fs->open(_file, "r+") : File();
  if (f) {
    Rec r;
    for (uint32_t slot = 0; f.seek(slot * sizeof(Rec)) && f.read((uint8_t*)&r, sizeof r) == sizeof r; slot++) {
      if (!r.seq || !(r.key == k)) continue;
      r.seq = 0;   // an empty slot
      if (f.seek(slot * sizeof(Rec))) f.write((const uint8_t*)&r, sizeof r);
    }
    f.close();
  }
  if (_sd) {
    char path[64];
    archivePath(k, path, sizeof path);
    _sd->remove(path);
#ifdef MC_LONG_PATH
    snprintf(path + strlen(path) - 2, 4, "bin");   // an older archive file not converted yet
    _sd->remove(path);
#endif
  }
  if (_hasOpen && _open == k) _nCache = 0;
  forget(k);
}

void MessageStore::markRead(const ThreadKey& k) {
  for (int i = 0; i < _nThreads; i++) {
    ThreadInfo& t = _threads[i];
    if (!(t.key == k) || (!t.unread && t.readSeq == t.lastSeq)) continue;
    t.unread = 0;
    t.readSeq = t.lastSeq;
    saveRead();
  }
}

void MessageStore::forget(const ThreadKey& k) {
  for (int i = 0; i < _nThreads; i++)
    if (_threads[i].key == k) {
      memmove(&_threads[i], &_threads[i + 1], sizeof(ThreadInfo) * (_nThreads - i - 1));
      _nThreads--;
      saveRead();
      return;
    }
}

// Read positions: one (key, seq) pair per thread.
void MessageStore::saveRead() {
  File f = _fs->open(_readFile, "w", true);
  if (!f) return;
  for (int i = 0; i < _nThreads; i++) {
    f.write((const uint8_t*)&_threads[i].key, sizeof(ThreadKey));
    f.write((const uint8_t*)&_threads[i].readSeq, 4);
  }
  f.close();
}

void MessageStore::loadRead() {
  File f = _fs->open(_readFile, "r");
  if (!f) return;
  ThreadKey k;
  uint32_t seq;
  while (f.read((uint8_t*)&k, sizeof k) == sizeof k && f.read((uint8_t*)&seq, 4) == 4) {
    ThreadInfo* t = touch(k);
    if (t) t->readSeq = seq;
  }
  f.close();
}

bool MessageStore::hidden(const ThreadKey& k) const {
  for (int i = 0; i < _nHidden; i++)
    if (_hidden[i] == k) return true;
  return false;
}

void MessageStore::setHidden(const ThreadKey& k, bool on) {
  if (hidden(k) == on) return;
  if (on) {
    if (_nHidden == MAX_HIDDEN) {   // full: the oldest entry goes
      memmove(_hidden, _hidden + 1, sizeof(_hidden[0]) * (MAX_HIDDEN - 1));
      _nHidden--;
    }
    _hidden[_nHidden++] = k;
  } else {
    int w = 0;
    for (int i = 0; i < _nHidden; i++)
      if (!(_hidden[i] == k)) _hidden[w++] = _hidden[i];
    _nHidden = w;
  }
  File f = _fs->open(_hiddenFile, "w", true);
  if (!f) return;
  f.write((const uint8_t*)_hidden, sizeof(_hidden[0]) * _nHidden);
  f.close();
}

void MessageStore::loadHidden() {
  _nHidden = 0;
  File f = _fs->open(_hiddenFile, "r");
  if (!f) return;
  while (_nHidden < MAX_HIDDEN && f.read((uint8_t*)&_hidden[_nHidden], sizeof(ThreadKey)) == sizeof(ThreadKey)) _nHidden++;
  f.close();
}

void MessageStore::wipe() {
  _fs->remove(_file);
  _fs->remove(_readFile);
  _fs->remove(_hiddenFile);
  _nHidden = 0;
  _nThreads = 0;
  _nCache = 0;
  _nextSeq = 1;
}

}  // namespace app
