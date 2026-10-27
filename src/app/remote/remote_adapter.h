// The UI's MeshModel for a companion node elsewhere: this device is the
// node's keyboard and screen over Bluetooth, as the phone app would be,
// speaking stock companion firmware's app protocol (CompanionClient).
//
// The node owns contacts, channels, radio and identity; we mirror them,
// and keep what the app keeps (history, delivery tracking, heard adverts,
// a packet log from the node's raw RX pushes) in AdapterBase, with a
// history of its own per node.
#pragma once
#include <stdint.h>
#include "app/adapter_base.h"
#include "app/remote/companion_client.h"

namespace app {

class RemoteAdapter : public AdapterBase, private CompanionClient::Handler {
public:
  RemoteAdapter(FrameLink& link, Platform& platform) : AdapterBase(platform), _link(link), _client(link, *this) {}
  ~RemoteAdapter();

  // `fs`: this device's flash (UI prefs, history, the saved node).
  void begin(fs::FS& fs);
  void loop();

  // ---- the link (the Remote node screen) ----
  FrameLink& link() { return _link; }
  bool hasSavedPeer() const { return _peerSaved; }
  const Peer& savedPeer() const { return _peer; }
  uint32_t savedPin() const { return _pin; }
  void connectTo(const Peer& p, uint32_t pin);   // and remember it
  void forgetPeer();
  bool synced() const { return _synced; }

  // ---- model::MeshModel: remote mode ----
  model::Mode mode() const override { return model::Mode::Remote; }
  model::Link link() const override;
  void scan(bool on) override;
  int foundCount() const override { return _link.scanCount(); }
  bool found(int i, model::FoundNode& out) const override;
  bool connectFound(int i, uint32_t pin) override;
  void reconnect() override;
  void disconnect() override { _link.disconnect(); _rev++; }
  void forgetNode() override { forgetPeer(); }
  // The node itself: restart it, or erase it (identity, contacts, channels).
  bool rebootNode() override;
  bool resetNode() override;
  const cp::DeviceInfo& deviceInfo() const { return _dev; }

  // ---- model::MeshModel ----
  uint32_t now() const override;
  uint32_t uptimeMs() const override;
  void selfKey(uint8_t out[6]) const override { memcpy(out, _self.key, 6); }

  int contactCount() const override { return _nContacts; }
  void setFavourite(int i, bool fav) override;
  bool removeContact(int i) override;
  bool resetPath(int i) override;
  void setTelemetry(int i, uint8_t telem) override;

  int channelCount() const override;
  bool channel(int i, model::Channel& out) const override;
  bool joinHashtag(const char* name) override;
  bool joinPrivate(const char* name, const uint8_t psk[16]) override;
  bool createPrivate(const char* name) override;
  bool channelKey(int i, uint8_t psk[16]) const override;
  bool removeChannel(int i) override;
  void setMuted(int i, bool muted) override;

  bool send(model::ThreadId t, const char* text) override;

  bool advert(bool flood) override;
  bool addHeard(int i) override;
  bool discoverRepeaters() override;

  bool login(int contact, const char* password) override;
  bool requestStats(int contact) override;
  bool requestNeighbours(int contact) override;
  bool requestTelemetry(int contact) override;
  bool sendCommand(int contact, const char* cmd) override;
  bool tracePath(int contact) override;
  bool ping(int contact) override;

  model::Radio radio() const override;
  bool setRadio(const model::Radio& r) override;

  model::Diag diag() const override;
  void resetPower() override { _plat.resetPower(); }

  void setPrefs(const model::Prefs& p) override;
  bool bleConnected() const override { return _link.ready(); }
  bool bleAvailable() const override { return false; }   // no phone link of our own in this mode
  uint32_t bleActivePin() const override { return 0; }
  bool blePairing() const override { return false; }

  bool restoreOffered() const override { return false; }
  bool backupNow() override;
  bool restore(bool previous) override;
  void factoryReset() override;

protected:
  // ---- AdapterBase ----
  bool contactAt(int i, ContactRef& c) const override;
  int contactIndex(const uint8_t* key, int len = 6) const override;
  bool channelName(int slot, char* name, int n) const override;
  void setClock(uint32_t epoch) override;
  bool sendAttempt(Pending& p, const ContactRef& c, const char* text) override;
  void forgetRoute(const uint8_t* key) override;
  const char* backupPath(bool previous) const override;

private:
  struct Chan {
    char name[32];
    uint8_t secret[16];
  };
  // The node's own settings, as a backup holds them.
  struct NodeSettings {
    char name[32];
    uint32_t freqKHz, bwHz;
    uint8_t sf, cr;
    int8_t txDbm;
    uint8_t manualAdd, telemModes, advertLocPolicy, multiAcks, pathHashMode, repeat;
  } __attribute__((packed));
  struct Stats {
    uint32_t uptimeS, txAirS, rxAirS;
    uint16_t queueLen;
    int16_t noiseFloor;
    int8_t lastRssi, lastSnr_x4;
    uint32_t recv, sent, sentFlood, sentDirect, recvFlood, recvDirect, recvErrors;
  };
  // Cookies tell replies apart: what a command was for.
  // Message sends use the message's sequence number, far below these.
  enum : uint32_t { CK_NONE = 0, CK_SYNC_CHAIN = 0xFFFFFFFF, CK_REQUEST = 0xFFFFFFFE, CK_CONTACTS_SINCE = 0xFFFFFFFD };

  // ---- CompanionClient::Handler ----
  void onReply(uint8_t cmd, uint32_t cookie, const uint8_t* f, int len) override;
  void onPush(const uint8_t* f, int len) override;

  bool q(const uint8_t* f, int len, uint32_t cookie = CK_NONE);
  void startSync();
  void syncMessages();
  void gotMessage(const cp::Msg& m);
  void putContact(const cp::Contact& c);
  void dropContact(const uint8_t* key);
  cp::Contact* findContact(const uint8_t* key, int len) const;
  void updateContact(const cp::Contact& c);   // to the node
  void refreshContact(const uint8_t* key);    // from the node
  int slotOf(int i) const;
  int freeSlot() const;
  void setSlot(int slot, const char* name, const uint8_t secret[16]);
  void groupHash(const Chan& ch, uint32_t ts, const char* text, uint8_t out[4]) const;
  uint32_t uniqueNow();
  bool requestSent(const uint8_t* f, int len, ReqKind kind, int contact);
  void syncPrefs();
  void openHistory();
  // This node's contacts, channels and self info on flash, so they show
  // before the link is up.
  void loadCache();
  void saveCache();
  void markCacheDirty();
  bool allocTables(int maxContacts, int maxChans);
  static bool readBackupSection(void* ctx, const char* name, File& f, uint32_t len);
  void restoreStep();
  void positionTick(uint32_t ms);
  void loadPeer();
  void savePeer();
  Pending* pendingBySeq(uint32_t seq);

  FrameLink& _link;
  CompanionClient _client;
  bool _wasReady = false, _synced = false;
  FrameLink::State _lastState = FrameLink::State::Idle;
  int _lastFound = 0;
  cp::DeviceInfo _dev{};
  cp::SelfInfo _self{};
  bool _haveSelf = false;
  cp::Contact* _contacts = nullptr;
  int _nContacts = 0, _maxContacts = 0;
  Chan* _chans = nullptr;
  int _maxChans = 0;
  int _syncChan = 0;
  // The node's send queue is a few frames deep: pushes get lost when it is
  // busy. Contacts changed since the last list, and waiting messages, are
  // also asked for now and then.
  uint32_t _contactsSince = 0, _contactsAt = 0, _contactsSoon = 0, _pollAt = 0;
  // The node's battery and stats, refreshed now and then.
  uint16_t _nodeBattMv = 0;
  uint32_t _statsAt = 0;
  Stats _stats{};
  // The node's private key, for backups (if it lets us export it).
  uint8_t _privKey[64];
  bool _havePrivKey = false;
  // A restore in progress: commands go out a few at a time.
  struct {
    bool active;
    int step, idx;
    NodeSettings set;
    bool haveSet, haveKey;
    uint8_t key[64];
  } _rs{};
  char _bkPath[2][48] = {};
  bool _historyRestored = false;
  bool _cacheDirty = false;
  uint32_t _cacheSavedAt = 0, _cacheChangedAt = 0;
  // A full contact list replaces ours: contacts not in it go at its end
  // (rather than the list emptying while it comes in).
  uint8_t* _seen = nullptr;
  bool _fullSync = false;
  uint32_t _positionAt = 0;
  int32_t _sentLat = 0, _sentLon = 0;
  int32_t _clockOffset = 0;        // epoch - uptime seconds; 0: not set yet
  bool _clockFromGps = false;
  uint32_t _lastTs = 0;
  // The saved node.
  Peer _peer{};
  uint32_t _pin = 123456;
  bool _peerSaved = false;
  char _histTag[8] = "";
  // Channel sends waiting for the node's OK: seq -> slot, timestamp.
  struct ChanSend { uint32_t seq, ts; uint8_t slot; char text[model::TEXT_LEN]; };
  ChanSend _chanSends[4] = {};
};

}  // namespace app
