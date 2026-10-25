// The UI's MeshModel on top of stock MeshCore's companion MyMesh, running on
// this device (the local node).
//
// It is also MyMesh's UI task: MeshCore calls the event hooks (patch 0001)
// as messages, ACKs and responses arrive. What it keeps beyond MeshCore
// (history, delivery tracking, heard adverts...) is AdapterBase's, shared
// with the remote model.
#pragma once
#include <stdint.h>
#include "MyMesh.h"
#include "AbstractUITask.h"
#include "app/adapter_base.h"

namespace app {

class MeshAdapter : public AbstractUITask, public AdapterBase {
public:
  MeshAdapter(mesh::MainBoard* board, MultiSerialInterface* ifaces, Platform& platform)
      : AbstractUITask(board, ifaces), AdapterBase(platform) {}

  // Call after MyMesh::begin(): `fs` is the flash filesystem MeshCore uses.
  // `freshInstall`: there was no identity on flash before MyMesh::begin().
  void begin(MyMesh& mesh, DataStore& store, mesh::RTCClock& rtc, fs::FS& fs, bool bleBuilt, bool freshInstall);

  // ---- AbstractUITask (called by MyMesh) ----
  void onMessageRecv(mesh::Packet*, const ContactInfo&, uint8_t, uint32_t, const char*) override {}
  void onChannelMessageRecv(mesh::Packet*, ChannelDetails&, const char*) override {}
  void onQueueSizeChanged(int) override {}
  void notify(UIEventType) override { _rev++; }
  void loop() override;   // retries, request timeouts
  void onDirectMessage(const ContactInfo& from, uint8_t txt_type, uint8_t path_len, float snr,
                       uint32_t sender_ts, const uint8_t* extra, int extra_len, const char* text) override;
  void onChannelMessage(uint8_t channel_idx, uint8_t path_len, float snr, uint32_t ts, const char* text) override;
  void onAckReceived(const uint8_t* ack) override;
  void onSentMessage(const uint8_t* pub_key_prefix, uint8_t channel_idx, uint8_t attempt, uint32_t timestamp,
                     uint32_t expected_ack, uint32_t est_timeout, const char* text) override;
  void onContactResponse(const ContactInfo& from, const uint8_t* data, uint8_t len) override;
  void onTraceData(uint32_t tag, uint8_t flags, const uint8_t* hashes, const uint8_t* snrs, uint8_t path_len,
                   float final_snr) override;
  void onRawRx(float snr, float rssi, const uint8_t* raw, int len) override;
  void onControlData(float snr, float rssi, uint8_t path_len, const uint8_t* payload, int len) override;
  void onPathResponse(const ContactInfo& from, uint8_t in_path_len, uint8_t out_path_len, uint8_t extra_type,
                      const uint8_t* extra, uint8_t extra_len) override;

  // ---- model::MeshModel ----
  uint32_t now() const override;
  uint32_t uptimeMs() const override;
  void selfKey(uint8_t out[6]) const override;

  int contactCount() const override;
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

  void setPrefs(const model::Prefs& p) override;
  bool bleConnected() const override { return hasConnection(); }
  bool bleAvailable() const override { return _bleBuilt; }
  bool pingBotAvailable() const override { return true; }
  uint32_t bleActivePin() const override { return _activePin; }
  bool relayName(const uint8_t* hash, int size, char* out, int n) const override;
  bool blePairing() const override { return _bleBuilt && _plat.blePairing(); }
  // The PIN for this boot (fixed, or new random if so set); call once,
  // before starting the Bluetooth interface.
  uint32_t choosePinForBoot();

  bool restoreOffered() const override { return _fresh && _bk.cur.valid; }
  bool backupNow() override;
  bool restore(bool previous) override;
  void factoryReset() override;

protected:
  // ---- AdapterBase ----
  bool contactAt(int i, ContactRef& c) const override;
  bool channelName(int slot, char* name, int n) const override;
  void setClock(uint32_t epoch) override { _rtc->setCurrentTime(epoch); }
  bool sendAttempt(Pending& p, const ContactRef& c, const char* text) override;
  void forgetRoute(const uint8_t* key) override;
  void onLoggedIn(const uint8_t* key, uint16_t keepAliveS) override;

private:
  // MeshCore reserves the first MAX_ANON_CONTACTS slots; UI index 0 is the
  // first real contact.
  bool getContact(int i, ContactInfo& c) const { return i >= 0 && _mesh->getContactByIdx(MAX_ANON_CONTACTS + i, c); }
  ContactInfo* contactPtr(int i) const;
  void syncPrefsFromMesh();
  bool sendDirect(const ContactRef& c, model::Message& m);
  void pingReceived(const ContactInfo& from, uint8_t pathLen, float snr);
  void pingTick(uint32_t ms);

  MyMesh* _mesh = nullptr;
  DataStore* _store = nullptr;
  mesh::RTCClock* _rtc = nullptr;
  bool _bleBuilt = false;
  bool _fresh = false;
  int _seenContacts = -1;
  uint32_t _activePin = 0;
  uint32_t _prefsCheckAt = 0;

  // The ping bot.
  static constexpr uint32_t PING_DELAY_MS = 1000, PING_QUIET_MS = 10000;
  struct {                           // the path of the last DM packet heard
    bool valid;
    uint8_t src, pathLen;
    uint8_t path[MAX_PATH_SIZE];
  } _dmPath = {};
  struct {                           // the path of the last DM or channel packet heard: for the message that follows
    bool valid;
    uint8_t pathLen;
    uint8_t path[6];
  } _rxPath = {};
  void noteSenderClock(model::Message& m, uint32_t senderTs);
  void attachPath(model::Message& m, uint8_t pathLen);
  struct PingAnswered { uint8_t key[6]; uint32_t at; };
  PingAnswered _answered[4] = {};
  struct {                           // the reply waiting to go
    uint8_t key[6];
    uint32_t due;                    // 0: none
    char text[model::TEXT_LEN];
  } _pong = {};
};

}  // namespace app
