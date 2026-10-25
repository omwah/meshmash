// What the local and the remote MeshModel have in common.
//
// The local model (MeshAdapter) runs MeshCore's companion MyMesh on this
// device; the remote one (RemoteAdapter) drives a companion node over its
// app protocol. Both keep the same things MeshCore does not: message history
// (MessageStore), delivery tracking with retries, signal history per
// contact, heard adverts, a packet log, admin request state, saved room
// passwords and the UI's own preferences. That part lives here; a subclass
// supplies contacts and channels and does the sending.
#pragma once
#include <stdint.h>
#include <Mesh.h>
#include "model/model.h"
#include "app/msg_store.h"
#include "app/platform.h"
#include "app/backup.h"

namespace app {

// Settings that belong to the UI rather than to MeshCore's NodePrefs.
struct UiPrefs {
  uint32_t magic;
  uint8_t version;
  uint8_t theme, timeZone, brightness;
  uint16_t screenTimeoutS, inputLockoutMs;
  uint8_t sound, textSize, gpsTimeSyncAtBoot, autoBackup, backupIdentity, backupMessages, bleEnabled;
  uint8_t presetIdx;
  uint16_t mutedChannels;        // bit per channel slot
  uint8_t favourites[16][4];     // unused: favourites moved to MeshCore's contact flag (kept for layout)
  uint8_t nFavourites;
  // Fields added later go here: older, shorter files load them as 0.
  uint8_t blePinRandom;
  uint8_t deviceMode;            // model::Mode
  uint8_t remoteGps;             // remote mode: this device's GPS on (local mode keeps it in NodePrefs)
  uint8_t pingBot;               // local mode: answer "ping" DMs
  uint8_t notifyMode;            // model::NotifyMode (0: LED only)
  uint8_t shortPreview;          // 0: chats list scrolls the whole last message
  uint8_t vimKeys;               // 0: the board's default, 1: off, 2: on
  uint8_t trackballOff;          // 1: the trackball's rolls are ignored
  uint8_t scrollNatural;         // 1: a drag moves the content with the finger; 0: as the arrow keys do
  uint8_t lockOff;               // 1: the screen timing out does not lock the keys (0: it does)
  uint8_t ledLevel;              // notification light brightness; 0: the default (128)
  uint8_t clock12;               // 1: times as 1:28 PM
};

// The mode the device boots in, from the UI prefs on flash (before any
// model starts).
model::Mode readDeviceMode(fs::FS& fs);

// A contact as the shared code needs it (MeshCore's ContactInfo, or the
// companion protocol's contact frame).
struct ContactRef {
  uint8_t key[PUB_KEY_SIZE];
  char name[32];
  uint8_t type;                  // ADV_TYPE_*
  uint8_t flags;                 // bit 0 favourite, bits 1-3 telemetry permissions
  uint8_t outPathLen;            // 0xFF (OUT_PATH_UNKNOWN): no route, flood
  uint8_t outPath[MAX_PATH_SIZE];
  uint32_t lastmod;
  int32_t lat, lon;
};

class AdapterBase : public model::MeshModel {
public:
  explicit AdapterBase(Platform& platform) : _plat(platform) {}

  // ---- model::MeshModel: the shared part ----
  uint32_t revision() const override { return _rev; }
  bool contact(int i, model::Contact& out) const override;

  int threadCount() const override;
  bool thread(int i, model::Thread& out) const override;
  int messageCount(model::ThreadId t) const override;
  bool message(model::ThreadId t, int i, model::Message& out) const override;
  void markRead(model::ThreadId t) override;
  bool unreadSince(model::ThreadId t, uint32_t& readSeq) const override;
  bool lastMessage(model::ThreadId t, char* out, int n) const override;
  bool threadHidden(model::ThreadId t) const override;
  void setThreadHidden(model::ThreadId t, bool on) override;
  uint16_t unreadIn(model::ThreadId t) const override;
  int loadOlder(model::ThreadId t) override;
  int loadNewer(model::ThreadId t) override;
  int history(model::ThreadId t, uint32_t upTo, model::Message* out, int max) override;
  int historyAround(model::ThreadId t, uint32_t seq, int before, int after, model::Message* out, int max) override;
  bool atNewest(model::ThreadId t) const override;
  void clearHistory(model::ThreadId t) override;
  uint16_t unreadTotal() const override;

  int heardCount() const override { return _nHeard; }
  bool heard(int i, model::Contact& out) const override;
  model::Req discoverState() const override { return _discoverState; }

  bool loggedIn(int contact) const override;
  bool hasSavedPassword(int contact) const override;
  bool loginSaved(int contact) override;
  void forgetPassword(int contact) override;
  model::Req requestState() const override { return _req.state; }
  const char* requestText() const override { return _req.text; }
  const model::RemoteStats& remoteStats() const override { return _remote; }
  int neighbourCount() const override { return _nNeighbours; }
  bool neighbour(int i, model::Neighbour& out) const override;
  uint32_t lastRttMs() const override { return _req.rttMs; }

  int presetCount() const override;
  const model::Preset& preset(int i) const override;

  int rxLogCount() const override { return _nLog; }
  bool rxLog(int i, model::RxLogEntry& out) const override;
  model::Gps gps() const override { return _plat.gps(); }
  bool gpsSyncTime() override;

  const model::Prefs& prefs() const override { return _prefs; }
  model::BackupStatus backupStatus() const override;
  void reboot() override { _plat.reboot(); }

  MessageStore& store() { return _msgs; }

protected:
  struct Pending {                 // outgoing DM waiting for its ACK
    uint32_t seq, ts;                // ts: retries resend with the original timestamp
    uint32_t ack, prevAck;           // an ACK for the previous attempt still counts
    uint32_t sentAt, timeout;
    uint8_t key[6];
    uint8_t attempt;
    bool used;
    bool phoneOwned;                 // sent by a phone app: it retries, we only show the outcome
  };
  struct PhoneSent {               // recent DMs a phone sent, to tell its retries from new messages
    uint8_t key[6];
    uint32_t ts, seq;
  };
  struct Signal {                  // last direct reception per contact
    uint8_t key[4];
    int8_t snr_x4;
    int16_t rssi;
    uint32_t at;
  };
  struct Heard {
    uint8_t pubkey[PUB_KEY_SIZE];
    char name[model::NAME_LEN];
    uint8_t type;
    int8_t hops, snr_x4;
    int16_t rssi;
    uint32_t at;
    bool hasPos;
    int32_t lat, lon;
  };
  enum class ReqKind : uint8_t { None, Login, Status, Telemetry, Neighbours, Cli, Trace, Ping };
  struct Request {
    ReqKind kind = ReqKind::None;
    model::Req state = model::Req::Idle;
    uint8_t key[PUB_KEY_SIZE];
    uint32_t tag = 0, startedAt = 0, deadline = 0, rttMs = 0;
    char text[200] = {};
    char password[16] = {};   // a login's password, saved if it works
  };
  struct SavedPassword { uint8_t key[6]; char pw[16]; };
  // Channel messages we sent recently, to count other nodes repeating them.
  struct SentChannelMsg {
    uint32_t seq, until;
    uint8_t hash[4];
    uint8_t repeats;
  };

  // ---- supplied by the subclass ----
  // Contacts by UI index (0 = first), and the index of a key prefix (-1: none).
  virtual bool contactAt(int i, ContactRef& c) const = 0;
  virtual int contactIndex(const uint8_t* key, int len = 6) const;
  // A channel slot's name ("" or false: empty slot).
  virtual bool channelName(int slot, char* name, int n) const = 0;
  virtual void setClock(uint32_t epoch) = 0;
  // Send one attempt of a DM: fill p.ack/prevAck, p.sentAt, p.timeout.
  virtual bool sendAttempt(Pending& p, const ContactRef& c, const char* text) = 0;
  // Before the last attempt of a DM: forget its route, so it floods.
  virtual void forgetRoute(const uint8_t* key) = 0;
  // Where this model's backup lives on the SD card (and its previous one).
  virtual const char* backupPath(bool previous) const { return previous ? Backup::PREV_PATH : Backup::PATH; }
  // A login worked; `keepAliveS` (0: none) as the server asked.
  virtual void onLoggedIn(const uint8_t* key, uint16_t keepAliveS) { (void)key; (void)keepAliveS; }

  // ---- shared helpers ----
  void resetRuntime();                      // everything but prefs and files, as after a restart
  void loadUiPrefs(fs::FS& fs);
  void saveUiPrefs();
  void uiToPrefs();                         // UiPrefs -> the UI fields of _prefs
  void prefsToUi(const model::Prefs& p);    // ...and back (does not save)
  bool isFavourite(const uint8_t* key) const;
  bool contactName(const uint8_t* key, int len, char* out, int n) const;
  bool threadKeyFor(model::ThreadId t, ThreadKey& k) const;

  // Store a received message (and keep unread counts right for muted channels).
  void storeDirect(const uint8_t* key, const model::Message& m);
  void storeChannel(uint8_t slot, const model::Message& m);
  // Outgoing: append to the open conversation (going back to its newest part).
  uint32_t appendOutgoing(const ThreadKey& k, model::Message& m);
  Pending* newPending();
  void ackReceived(uint32_t ack);
  void retryPending(uint32_t ms);
  void watchRepeats(uint32_t seq, const uint8_t* packetHash);

  // A received packet (raw, as it came off the air): log, heard adverts,
  // repeats of our channel messages, signal per contact.
  void rawRx(float snr, float rssi, const uint8_t* raw, int len, const uint8_t* selfKey);
  void noteSignal(const uint8_t* key, float snr, float rssi);
  const Signal* signalFor(const uint8_t* key) const;
  void putHeard(const Heard& h);
  void startDiscover(uint32_t tag, uint32_t listenMs);
  void discoverReply(float snr, float rssi, const uint8_t* payload, int len);

  bool startRequest(ReqKind k, int contact, uint32_t estTimeout);
  void finishRequest(model::Req st, const char* fmt, ...);
  bool requestIdle() const { return _req.state != model::Req::Busy; }
  void loginResult(const uint8_t* key, bool ok, bool admin, uint16_t keepAliveS);
  void statusReply(const uint8_t* stats, int len);      // a repeater/room's RepeaterStats
  void neighboursReply(const uint8_t* data, int len);   // after the tag
  void traceReply(uint8_t flags, const uint8_t* hashes, const uint8_t* snrs, uint8_t pathLen, float finalSnr);
  void pathReply(uint8_t inLen, uint8_t outLen);
  void decodeTelemetry(const uint8_t* lpp, int len);
  void requestTick(uint32_t ms);

  void loadPasswords();
  void savePasswords();
  const SavedPassword* passwordFor(const uint8_t* key) const;

  void configChanged();          // something a backup holds changed
  void refreshBackupInfo();
  void storageTick(uint32_t ms); // SD card changes, automatic backup
  void startBootGps() { _bootGpsPending = _ui.gpsTimeSyncAtBoot; }
  void gpsTick(uint32_t ms);
  bool uiMuted(int slot) const { return (_ui.mutedChannels >> slot) & 1; }
  void setUiMuted(int slot, bool muted);

  Platform& _plat;
  fs::FS* _fs = nullptr;
  uint32_t _rev = 1;
  mutable MessageStore _msgs;
  mutable ThreadKey _openKey{};
  mutable uint32_t _openRetry = 0;
  mutable bool _hasOpen = false;
  UiPrefs _ui{};
  model::Prefs _prefs{};

  static constexpr int MAX_PENDING = 6, MAX_SIGNALS = 48, MAX_HEARD = 24, MAX_LOG = 40, MAX_NEIGH = 16;
  static constexpr int MAX_DM_ATTEMPTS = 3;   // then one last flood if a path was in use
  Pending _pending[MAX_PENDING] = {};
  PhoneSent _phoneSent[4] = {};
  uint8_t _phoneSentNext = 0;
  Signal _signals[MAX_SIGNALS] = {};
  Heard _heard[MAX_HEARD];
  int _nHeard = 0;
  model::RxLogEntry _log[MAX_LOG];
  uint8_t _logHash[MAX_LOG][4];
  int _logHead = 0, _nLog = 0;
  Request _req;
  model::RemoteStats _remote{};
  model::Neighbour _neigh[MAX_NEIGH];
  int _nNeighbours = 0;
  model::Req _discoverState = model::Req::Idle;
  uint32_t _discoverTag = 0, _discoverUntil = 0;
  int16_t _lastRssi = 0;
  int8_t _lastSnr = 0;
  uint32_t _dupsSeen = 0;
  SentChannelMsg _sentChan[4] = {};

  // Backup state, cached so screens never trigger SD reads.
  struct {
    bool card;
    Backup::Info cur, prev;
    char error[32];
  } _bk{};
  uint32_t _cfgChangedAt = 0;      // 0: nothing waiting for an automatic backup
  uint32_t _lastBackupAt = 0;
  // Rooms/repeaters logged into this session, and passwords that worked.
  uint8_t _loggedIn[8][6] = {};
  SavedPassword _passwords[6] = {};
  uint32_t _bootGpsUntil = 0;     // boot-time GPS clock sync in progress until
  bool _bootGpsPending = false;   // ...to be started by the first loop()
};

// Hex of the first bytes of a key, for names we don't know.
void hexKey(char* out, size_t n, const uint8_t* b, int len);

}  // namespace app
