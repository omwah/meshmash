// Message history on flash, for the UI (MeshCore keeps none for it).
//
// A fixed-size ring of records in one file: record i lives at slot
// seq % CAPACITY, so writes never grow the file and the oldest messages are
// overwritten. RAM holds only a summary per conversation (built by scanning
// the file at boot) and a window of messages of the conversation that is
// open, which can slide back to older messages and forward again.
//
// With an archive filesystem (an SD card), a record about to be overwritten
// in the ring is first appended to its conversation's archive file there, in
// its final state (delivered, repeats...), so history goes back further than
// the ring.
#pragma once
#include <stdint.h>
#include <FS.h>
#include "model/model.h"

namespace app {

// Which conversation: a contact (by public key prefix) or a channel slot.
struct ThreadKey {
  uint8_t kind;          // 0 direct, 1 channel
  uint8_t id[6];
  bool operator==(const ThreadKey& o) const;
  static ThreadKey direct(const uint8_t* pubkey);
  static ThreadKey channel(uint8_t slot);
};

struct ThreadInfo {
  ThreadKey key;
  uint32_t lastTs, lastSeq, readSeq;
  uint16_t unread;
  bool lastOut;
  char preview[44];
};

class MessageStore {
public:
#ifndef MSG_RING_RECORDS
#define MSG_RING_RECORDS 512
#endif
  static constexpr int CAPACITY = MSG_RING_RECORDS;   // records kept on flash (512: ~108 KB)
  static constexpr int MAX_THREADS = 40;
  static constexpr int MAX_HIDDEN = 32;
  static constexpr int CACHE = 24;         // messages of the open conversation in RAM
  static constexpr int PAGE = 12;          // messages loaded per step back or forward

  // `tag` names a separate history (a remote node's: files /ui/<tag>-*.bin and
  // an archive folder <tag>/ on the card); null for the local node's.
  // With `flash` the history lives on the card (`fs`, files in /meshmash) and whatever an earlier
  // run left in the flash (/ui) is moved over, then deleted from it; without, on `fs` itself.
  bool begin(fs::FS* fs, const char* tag = nullptr, fs::FS* flash = nullptr);
  // Removable storage for the archive, or null (no card).
  void setArchive(fs::FS* sd) { _sd = sd; }
  // Append a message; returns its sequence number (0 on failure).
  uint32_t append(const ThreadKey& k, const model::Message& m, uint32_t ack = 0);
  bool setStatus(uint32_t seq, model::Delivery st, uint8_t retries);
  bool setRepeats(uint32_t seq, uint8_t repeats);
  bool text(uint32_t seq, char* out, int n);
  // A thread's last message in full, as its preview reads ("You: ",
  // "sender: "), from flash.
  bool lastText(const ThreadKey& k, char* out, int n);
  bool ackOf(uint32_t seq, uint32_t& ack);

  // What the ring and the archive have done since boot, for Diagnostics.
  struct Stats {
    uint32_t writes, writeFails;       // appends the flash kept / refused
    uint32_t archived, archiveFails;   // records moved to the card / that could not be
    uint8_t lastErr;                   // why the last write failed: 0 none, 1 open, 2 grow, 3 write, 4 lost (not there on reading back), 5 rebuild
    uint32_t lastErrTs, lastOkTs;      // message time of the last failed / kept write
    uint32_t flashSeq, nextSeq;        // last seq in the ring / next to hand out
  uint32_t resized;                  // records moved when the ring changed size at boot (0: none)
    uint32_t ringBytes;                // size of the ring file as last seen
    bool archiveOn;                    // a card was there when the store started
  bool onCard;                       // the ring and read marks are on the card, not the flash
  };
  Stats stats() const;
  uint32_t writeFailures() const { return _stats.writeFails; }

  int threadCount() const { return _nThreads; }
  const ThreadInfo* thread(int i) const;          // most recent first
  const ThreadInfo* find(const ThreadKey& k) const;
  void markRead(const ThreadKey& k);
  uint16_t unreadTotal() const;
  void forget(const ThreadKey& k);                // drop a thread (contact/channel removed)
  // Conversations kept off the chats list (history and membership stay).
  bool hidden(const ThreadKey& k) const;
  void setHidden(const ThreadKey& k, bool on);

  // The open conversation: a window of up to CACHE messages, oldest first.
  void open(const ThreadKey& k);
  int count() const { return _nCache; }
  const model::Message& at(int i) const { return _cache[i]; }  // 0 = oldest cached
  // Slide the window: returns how many messages came in (0 = none there).
  int older();
  int newer();
  bool atNewest() const;     // the window ends with the conversation's last message
  // The `max` messages of a conversation with seq <= upTo, oldest first, read from the ring and
  // the archive without touching the window; returns how many.
  int history(const ThreadKey& k, uint32_t upTo, model::Message* out, int max);
  // Delete a conversation's messages (ring and archive).
  void clear(const ThreadKey& k);

  void wipe();

private:
  struct Rec;
  bool readRec(int slot, Rec& r);
  bool rebuildRing();
  void adoptFrom(fs::FS* flash, const char* tag);
  void fitRing();   // an older ring file of another size: move its records into this size
  bool archive(const Rec& r);
  void archivePath(const ThreadKey& k, char* out, int n) const;
  // Up to `max` sequence numbers of this conversation, the ones closest to
  // `from` going back (older) or forward; from the ring, then the archive.
  int findSeqs(const ThreadKey& k, uint32_t from, bool older, uint32_t* out, int max);
  bool loadSeq(const ThreadKey& k, uint32_t seq, model::Message& m);
  static void toMessage(const Rec& r, model::Message& m);
  static void packPath(Rec& r, const model::Message& m);
  static void unpackPath(const Rec& r, model::Message& m);
  bool writeRec(const Rec& r);
  ThreadInfo* touch(const ThreadKey& k);
  void sortThreads();
  void saveRead();
  void loadRead();
  void loadHidden();

  fs::FS* _fs = nullptr;
  fs::FS* _sd = nullptr;
  char _file[32] = "/ui/msgs.bin", _readFile[32] = "/ui/read.bin", _hiddenFile[32] = "/ui/hidden.bin";
  char _archDir[48] = "/meshmash/history";
  uint32_t _nextSeq = 1;
  Stats _stats{};
  bool _onCard = false;
  uint8_t _err = 0;
  bool _rebuilt = false;   // the ring was rebuilt since boot after a failed open (once)
  ThreadInfo _threads[MAX_THREADS];
  int _nThreads = 0;
  ThreadKey _hidden[MAX_HIDDEN];
  int _nHidden = 0;
  ThreadKey _open{};
  bool _hasOpen = false;
  model::Message _cache[CACHE];
  uint32_t _cacheSeq[CACHE];
  int _nCache = 0;
};

}  // namespace app
