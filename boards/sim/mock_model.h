// A scripted, deterministic stand-in for the mesh, for the simulator and
// screenshot tests. All names and keys are fictional.
#pragma once
#include "model/model.h"
#include <stdio.h>

namespace sim {

class MockModel : public model::MeshModel {
public:
  MockModel();
  void setClock(const uint32_t* nowMs) { _ms = nowMs; }
  void step();   // advance pending deliveries/requests against the clock

  // Script hooks.
  void rxDirect(int contact, const char* text);
  void rxChannel(int slot, const char* sender, const char* text);
  void setBle(bool connected) { _ble = connected; _rev++; }
  void setGpsFix(bool fix) { _gps.fix = fix; _rev++; }
  void setFresh(bool fresh) { _fresh = fresh; }
  void setEmpty();   // no contacts/messages (first boot look)

  uint32_t revision() const override { return _rev; }
  uint32_t now() const override { return kEpoch + *_ms / 1000; }
  uint32_t uptimeMs() const override { return *_ms + 3 * 3600 * 1000; }
  void selfKey(uint8_t out[6]) const override;

  int contactCount() const override { return _nContacts; }
  bool contact(int i, model::Contact& out) const override;
  void setFavourite(int i, bool fav) override;
  bool removeContact(int i) override;
  bool resetPath(int i) override;
  void setTelemetry(int i, uint8_t telem) override;

  int channelCount() const override { return _nChannels; }
  bool channel(int i, model::Channel& out) const override;
  bool joinHashtag(const char* name) override;
  bool joinPrivate(const char* name, const uint8_t psk[16]) override;
  bool createPrivate(const char* name) override;
  bool channelKey(int i, uint8_t psk[16]) const override;
  bool removeChannel(int i) override;
  void setMuted(int i, bool muted) override;

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
  int loadOlder(model::ThreadId) override { return 0; }   // the mock shows everything at once
  int loadNewer(model::ThreadId) override { return 0; }
  bool atNewest(model::ThreadId) const override { return true; }
  void clearHistory(model::ThreadId t) override;
  uint16_t unreadTotal() const override;
  bool send(model::ThreadId t, const char* text) override;

  bool advert(bool flood) override;
  int heardCount() const override { return _nHeard; }
  bool heard(int i, model::Contact& out) const override;
  bool addHeard(int i) override;
  bool discoverRepeaters() override;
  model::Req discoverState() const override { return _discover; }

  bool login(int contact, const char* password) override;
  bool loggedIn(int contact) const override { return contact == _loggedInto; }
  bool hasSavedPassword(int) const override { return false; }
  bool loginSaved(int contact) override { return login(contact, ""); }
  void forgetPassword(int) override {}
  bool requestStats(int contact) override;
  bool requestNeighbours(int contact) override;
  bool requestTelemetry(int contact) override;
  bool sendCommand(int contact, const char* cmd) override;
  bool tracePath(int contact) override;
  bool ping(int contact) override;
  model::Req requestState() const override { return _req; }
  const char* requestText() const override { return _reqText; }
  const model::RemoteStats& remoteStats() const override { return _stats; }
  int neighbourCount() const override { return 3; }
  bool neighbour(int i, model::Neighbour& out) const override;
  uint32_t lastRttMs() const override { return 1840; }

  model::Radio radio() const override { return _radio; }
  bool setRadio(const model::Radio& r) override { _radio = r; _rev++; return true; }
  int presetCount() const override;
  const model::Preset& preset(int i) const override;

  model::Diag diag() const override;
  int rxLogCount() const override { return _nLog; }
  bool rxLog(int i, model::RxLogEntry& out) const override;
  model::Gps gps() const override;
  bool gpsSyncTime() override { return _gps.fix; }

  const model::Prefs& prefs() const override { return _prefs; }
  void setPrefs(const model::Prefs& p) override { _prefs = p; _rev++; }
  bool bleConnected() const override { return _ble; }
  bool bleAvailable() const override { return true; }
  bool pingBotAvailable() const override { return true; }
  bool relayName(const uint8_t* hash, int size, char* out, int n) const override {
    if (size != 1 || hash[0] != 0xA1) return false;
    snprintf(out, n, "Hilltop");
    return true;
  }
  uint32_t bleActivePin() const override { return _prefs.blePinRandom ? 482913 : _prefs.blePin; }
  bool blePairing() const override { return _pairing; }
  void setPairing(bool on) { _pairing = on; _rev++; }

  model::BackupStatus backupStatus() const override;
  bool restoreOffered() const override { return _fresh; }
  bool backupNow() override { _lastBackup = now(); _rev++; return true; }
  bool restore(bool) override { _fresh = false; _rev++; return true; }
  void factoryReset() override { setEmpty(); }
  void reboot() override {}

  static constexpr uint32_t kEpoch = 1791727500;  // 2026-10-11 14:05 UTC

private:
  struct Msg {
    model::ThreadId t;
    model::Message m;
  };
  struct ThreadState {
    model::ThreadId id;
    uint16_t unread;
    bool used;
    bool hidden;                   // kept off the chats list
  };
  bool chanMuted(int slot) const;
  void addMsg(model::ThreadId t, uint32_t ts, bool out, const char* sender, const char* text,
              int8_t snr_x4, int8_t hops, model::Delivery st);
  ThreadState* threadFor(model::ThreadId t);
  void startReq(const char* reply, uint32_t delayMs);

  static constexpr int MAX_C = 40, MAX_CH = 8, MAX_MSG = 160, MAX_T = 24, MAX_LOG = 48, MAX_H = 16;
  static constexpr uint32_t kZero = 0;
  const uint32_t* _ms = &kZero;
  uint32_t _rev = 1;
  model::Contact _contacts[MAX_C];
  int _nContacts = 0;
  model::Channel _channels[MAX_CH];
  uint8_t _keys[MAX_CH][16];
  int _nChannels = 0;
  Msg _msgs[MAX_MSG];
  int _nMsgs = 0;
  uint32_t _lastSeq = 0;
  ThreadState _threads[MAX_T];
  model::Contact _heard[MAX_H];
  int _nHeard = 0;
  model::RxLogEntry _log[MAX_LOG];
  int _nLog = 0;
  model::Radio _radio;
  model::Prefs _prefs;
  model::Gps _gps;
  model::RemoteStats _stats;
  bool _ble = false, _fresh = false, _pairing = false;
  int _loggedInto = -1;
  model::Req _req = model::Req::Idle, _discover = model::Req::Idle;
  uint32_t _reqDone = 0, _discoverDone = 0;
  char _reqText[200] = {};
  char _pendingText[200] = {};
  uint32_t _lastBackup = 0;
};

}  // namespace sim
