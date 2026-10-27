// simnet: the real UI on real MeshCore companion nodes over a simulated LoRa
// medium, all in one process on a virtual clock.
//
//   simnet [-s script] [-o outdir] [--data dir]
//   simnet --smoke
//   simnet --remote-smoke
//   simnet --remote ...   the UI drives "Cardputer" as a remote node, over
//                         simulated Bluetooth (RemoteAdapter)
//
// The UI node is "Cardputer" (id 0). Commands besides runner.cpp's:
//   node <name> [snr] [echo] [telem]   add a companion node linked to the UI node
//   repeater <name> [snr] [password]   add a stock MeshCore repeater
//   room <name> [snr] [password]       add a stock MeshCore room server
//   as <name> login <room> <password>  a companion logs in to a room/repeater
//   link <a> <b> <snr> | unlink <a> <b>
//   as <name> advert [flood]
//   as <name> dm <to> <text>      a direct message (to = node name)
//   as <name> chan <slot> <text>  post to channel slot (0 = Public)
//   as <name> join <#hashtag>
//   reboot                        restart the UI node on its existing flash
//   wipe                          erase the UI node's flash and reboot (fresh install)
//   sd on|off                     insert or remove the SD card (sim-data/.../sd)
//   ble on|off                    remote mode: drop or restore the Bluetooth link
//   phone on|off                  local mode: a phone app connects to (or leaves) the node
//   phone dm <node> <text>        the phone sends a DM through the node's own command handler
//   phone retry                   it sends the last DM again, same timestamp, next attempt
//   phone chan <slot> <text>      it sends to a channel
//   nodewipe                      remote mode: erase the node (do it with the link off)
//   nopair (before anything else) remote mode: don't connect by itself; the UI chooses the node
//   blepin <pin>                  remote mode: the node's Bluetooth PIN (default 123456)
//   expect identity same|new      UI node key vs. before the last wipe
//   expect screen on|off
//   expect pinmode random|fixed   the Bluetooth PIN mode (a fresh device: random)
//   expect sounds <n>          notification chimes played so far
//   expect request idle|busy|ok|failed|timeout
//   expect repeats <n>         repeats heard of the last message we sent in the open chat
//   expect contacts|unread|heard|threads <n>
//   expect status <pending|sent|delivered|failed>   last message we sent in the open chat
//   dump                          print the UI node's view to stdout
#ifndef PIO_UNIT_TESTING
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <string>
#include "ui/app.h"
#include "ui/screens/screens.h"
#include "app/mesh_adapter.h"
#include "app/remote/remote_adapter.h"
#include "pipe_link.h"
#include "runner.h"
#include "sim_node.h"
#include "bot.h"
#include "sim_repeater.h"
#include "sim_room.h"

int simnetSmoke();
int simnetRemoteSmoke();

namespace {

constexpr uint32_t kEpoch = 1791727500;   // 2026-10-11 14:05 UTC

class SimSound : public hal::Sound {
public:
  void play(const hal::Tone*, int) override { played++; }
  int played = 0;
};
SimSound g_sound;

// Counts bursts: each time it lights up from dark.
class SimLed : public hal::Led {
public:
  void set(uint8_t r, uint8_t g, uint8_t b) override {
    bool lit = r || g || b;
    if (lit && !_lit) bursts++;
    _lit = lit;
  }
  int bursts = 0;

private:
  bool _lit = false;
};
SimLed g_led;

class SimPlatform : public app::Platform {
public:
  fs::FS* sd() override { return card ? &sdFs : nullptr; }
  fs::FS sdFs;
  bool card = true;
  uint32_t heapFree() override { return 118 * 1024; }
  uint32_t heapMin() override { return 96 * 1024; }
  uint32_t heapMaxBlock() override { return 64 * 1024; }
  uint16_t batteryMv() override { return 3950; }
  void reboot() override { rebootRequested = true; }
  bool rebootRequested = false;
};

struct Bot {
  std::unique_ptr<BotTask> task;
  std::unique_ptr<SimNode> node;
};

class NetWorld : public sim::World {
public:
  // `remote`: the UI is a RemoteAdapter, talking over a simulated Bluetooth
  // link (PipeLink) to "Cardputer", which is then a plain companion node.
  NetWorld(const std::string& dir, app::MeshAdapter& local, app::RemoteAdapter* remote, PipeLink& pipe,
           MultiSerialInterface& ifaces, SimPlatform& plat)
      : _dir(dir), _ui(remote ? (app::AdapterBase&)*remote : (app::AdapterBase&)local), _local(local), _remote(remote),
        _pipe(pipe), _ifaces(ifaces), _plat(plat) {
    _plat.sdFs.setRoot(dir + "/sd");
    _plat.sdFs.mkdir("/");
  }

  void attach(ui::App& app, ui::Screen* first) override {
    _app = &app;
    _first = first;
  }

  void start() override {
    bootUiNode(_fresh);
    _ids["Cardputer"] = 0;
  }

  // Start (or restart) the UI node the way the firmware's setup() does.
  void bootUiNode(bool format) {
    _plat.rebootRequested = false;
    if (_remote) {
      // This device restarts; the node it is connected to keeps running
      // (and stays out of reach if the scenario said "ble off").
      bool reachable = !_bleOff;
      _pipe.linkUp(false);
      if (reachable) _pipe.linkUp(true), _pipe.disconnect();
      if (!_uiNode) {
        _uiNode.reset(new SimNode(_medium, 0, "Cardputer", _dir + "/node", kEpoch, nullptr, format, &_pipe));
        _uiNode->begin();
      }
      snprintf(_pipe.peerName, sizeof _pipe.peerName, "Cardputer");   // its Bluetooth name: MeshCore-<node name>
      _remoteFs.setRoot(_dir + "/ui");
      if (format) _remoteFs.format();
      else _remoteFs.mkdir("/");
      _remote->begin(_remoteFs);
      app::Peer p;
      if (_autoPair && !_remote->hasSavedPeer() && _pipe.scanResult(0, p)) _remote->connectTo(p, _pipe.pin);
      return;
    }
    _uiNode.reset();
    _phone.linkUp(false);   // a restart drops the phone
    _uiNode.reset(new SimNode(_medium, 0, "Cardputer", _dir + "/ui", kEpoch, &_local, format, &_phone));
    bool fresh = !_uiNode->fs().exists("/identity/_main.id");
    _uiNode->begin();
    _local.begin(_uiNode->mesh(), _uiNode->store(), *rtcOf(*_uiNode), _uiNode->fs(), false, fresh);
  }

  void reboot(bool format) {
    bootUiNode(format);
    if (_app) _app->begin(_first);   // the UI starts again at its boot screen
  }

  bool setup(const char* cmd, const char*) override {
    if (!strcmp(cmd, "keep")) { _fresh = false; return true; }   // reuse flash from a previous run
    if (!strcmp(cmd, "nopair")) { _autoPair = false; return true; }   // remote mode: leave choosing the node to the UI
    return false;
  }

  void step(uint32_t now) override {
    sim_setMillis(now);
    if (_plat.rebootRequested) reboot(false);
    if (_uiNode) {
      _uiNode->loop();
      if (_remote) _remote->loop();
      else _local.loop();
    }
    for (auto& b : _bots) {
      b.second.node->loop();
      b.second.task->loop();
    }
    for (auto& r : _rpts) r.second->loop();
    for (auto& r : _rooms) r.second->loop();
    if (_uiNode) _uiNode->select();   // the UI reads its node's globals
  }

  bool command(const char* cmd, const char* arg, char* err, size_t n) override {
    if (!strcmp(cmd, "node")) {
      char name[32] = {}, flag[8] = {}, flag2[8] = {};
      float snr = 8.0f;
      sscanf(arg, "%31s %f %7s %7s", name, &snr, flag, flag2);
      int id = _nextId++;
      Bot b;
      b.task.reset(new BotTask(&board, &_ifaces));
      b.node.reset(new SimNode(_medium, id, name, _dir + "/" + name, kEpoch, b.task.get()));
      b.node->begin();
      b.task->attach(&b.node->mesh(), rtcOf(*b.node));
      b.task->echo = !strcmp(flag, "echo") || !strcmp(flag2, "echo");
      if (!strcmp(flag, "telem") || !strcmp(flag2, "telem")) {   // shares base telemetry: answers pings
        b.node->select();
        b.node->mesh().getNodePrefs()->telemetry_mode_base = TELEM_MODE_ALLOW_ALL;
      }
      _medium.link(0, id, snr);
      _ids[name] = id;
      _bots[name] = std::move(b);
      return true;
    }
    if (!strcmp(cmd, "room")) {
      char name[32] = {}, pw[32] = "password";
      float snr = 8.0f;
      sscanf(arg, "%31s %f %31s", name, &snr, pw);
      int id = _nextId++;
      _rooms[name] = SimRoom::create(_medium, id, name, _dir + "/" + name, kEpoch, pw);
      _medium.link(0, id, snr);
      _ids[name] = id;
      return true;
    }
    if (!strcmp(cmd, "repeater")) {
      char name[32] = {}, pw[32] = "password";
      float snr = 8.0f;
      sscanf(arg, "%31s %f %31s", name, &snr, pw);
      int id = _nextId++;
      _rpts[name] = SimRepeater::create(_medium, id, name, _dir + "/" + name, kEpoch, pw);
      _medium.link(0, id, snr);
      _ids[name] = id;
      return true;
    }
    if (!strcmp(cmd, "link") || !strcmp(cmd, "unlink")) {
      char a[32], b[32];
      float snr = 8.0f;
      if (sscanf(arg, "%31s %31s %f", a, b, &snr) < 2 || !_ids.count(a) || !_ids.count(b)) {
        snprintf(err, n, "unknown node");
        return true;
      }
      if (cmd[0] == 'l') _medium.link(_ids[a], _ids[b], snr);
      else _medium.unlink(_ids[a], _ids[b]);
      return true;
    }
    if (!strcmp(cmd, "as")) return asNode(arg, err, n);
    if (!strcmp(cmd, "reboot")) { reboot(false); return true; }
    if (!strcmp(cmd, "wipe")) {
      memcpy(_keyBeforeWipe, _uiNode->mesh().self_id.pub_key, sizeof _keyBeforeWipe);
      reboot(true);
      return true;
    }
    if (!strcmp(cmd, "sd")) { _plat.card = !strcmp(arg, "on"); return true; }
    if (!strcmp(cmd, "phone")) return phone(arg, err, n);
    if (!strcmp(cmd, "ble")) {   // remote mode: the Bluetooth link
      _bleOff = strcmp(arg, "on") != 0;
      _pipe.linkUp(!_bleOff);
      return true;
    }
    if (!strcmp(cmd, "blepin")) { _pipe.pin = (uint32_t)atol(arg); return true; }
    if (!strcmp(cmd, "nodewipe") && _remote) {   // remote mode: erase the node (new identity, no contacts)
      memcpy(_keyBeforeWipe, _uiNode->mesh().self_id.pub_key, sizeof _keyBeforeWipe);
      _uiNode.reset(new SimNode(_medium, 0, "Cardputer", _dir + "/node", kEpoch, nullptr, true, &_pipe));
      _uiNode->begin();
      return true;
    }
    if (!strcmp(cmd, "pref")) {   // pref pingbot on|off, pref notify <mode>, pref pin random|fixed, pref lock on|off
      char what[16] = {}, val[8] = {};
      sscanf(arg, "%15s %7s", what, val);
      model::Prefs p = _ui.prefs();
      if (!strcmp(what, "pingbot")) p.pingBot = !strcmp(val, "on");
      else if (!strcmp(what, "pin")) p.blePinRandom = !strcmp(val, "random");   // pref pin random|fixed
      else if (!strcmp(what, "lock")) p.lockOnTimeout = !strcmp(val, "on");   // pref lock on|off
      else if (!strcmp(what, "notify")) {   // pref notify led|screen|dark|both
        static const char* const modes[] = {"led", "screen", "dark", "both"};
        int m = 0;
        while (m < 4 && strcmp(val, modes[m])) m++;
        if (m == 4) { snprintf(err, n, "unknown notify mode '%s'", val); return true; }
        p.notify = (model::NotifyMode)m;
      }
      else { snprintf(err, n, "unknown pref '%s'", what); return true; }
      _ui.setPrefs(p);
      return true;
    }
    if (!strcmp(cmd, "expect")) return expect(arg, err, n);
    if (!strcmp(cmd, "dump")) { dump(); return true; }
    return false;
  }

private:
  static mesh::RTCClock* rtcOf(SimNode& n) { return n.mesh().getRTCClock(); }

  // A phone app on the node's Bluetooth: it speaks MeshCore's companion protocol, so
  // its sends reach MyMesh's own command handler (CMD_SEND_TXT_MSG, CMD_SEND_CHANNEL_TXT_MSG).
  bool phone(const char* arg, char* err, size_t n) {
    char what[8] = {};
    int used = 0;
    sscanf(arg, "%7s %n", what, &used);
    const char* rest = arg + used;
    if (_remote) { snprintf(err, n, "phone: local mode only"); return true; }
    if (!strcmp(what, "on") || !strcmp(what, "off")) { _phone.linkUp(what[1] == 'n'); return true; }
    uint8_t f[MAX_FRAME_SIZE];
    int len = 0;
    uint32_t ts = _phoneTs;
    if (!strcmp(what, "dm")) {
      char name[32] = {};
      int u2 = 0;
      sscanf(rest, "%31s %n", name, &u2);
      auto it = _bots.find(name);
      if (it == _bots.end()) { snprintf(err, n, "no node '%s'", name); return true; }
      memcpy(_phoneKey, it->second.node->mesh().self_id.pub_key, 6);
      snprintf(_phoneText, sizeof _phoneText, "%s", rest + u2);
      ts = _phoneTs = _uiNode->mesh().getRTCClock()->getCurrentTimeUnique();
      _phoneAttempt = 0;
    } else if (!strcmp(what, "retry")) {
      _phoneAttempt++;
    } else if (!strcmp(what, "chan")) {
      int slot = 0, u2 = 0;
      sscanf(rest, "%d %n", &slot, &u2);
      f[len++] = 3;   // CMD_SEND_CHANNEL_TXT_MSG
      f[len++] = 0;   // TXT_TYPE_PLAIN
      f[len++] = (uint8_t)slot;
      uint32_t t = _uiNode->mesh().getRTCClock()->getCurrentTimeUnique();
      memcpy(&f[len], &t, 4); len += 4;
      int tl = (int)strlen(rest + u2);
      memcpy(&f[len], rest + u2, tl); len += tl;
      if (!_phone.send(f, len)) snprintf(err, n, "phone is not connected");
      return true;
    } else {
      snprintf(err, n, "phone what? '%s'", what);
      return true;
    }
    f[len++] = 2;   // CMD_SEND_TXT_MSG
    f[len++] = 0;   // TXT_TYPE_PLAIN
    f[len++] = _phoneAttempt;
    memcpy(&f[len], &ts, 4); len += 4;
    memcpy(&f[len], _phoneKey, 6); len += 6;
    int tl = (int)strlen(_phoneText);
    memcpy(&f[len], _phoneText, tl); len += tl;
    if (!_phone.send(f, len)) snprintf(err, n, "phone is not connected");
    return true;
  }

  bool asNode(const char* arg, char* err, size_t n) {
    char name[32] = {}, what[16] = {};
    int used = 0;
    sscanf(arg, "%31s %15s %n", name, what, &used);
    const char* rest = arg + used;
    auto rm = _rooms.find(name);
    if (rm != _rooms.end()) {
      if (strcmp(what, "advert")) { snprintf(err, n, "rooms only advert"); return true; }
      rm->second->advert(strstr(rest, "flood") != nullptr);
      if (_uiNode) _uiNode->select();
      return true;
    }
    auto rp = _rpts.find(name);
    if (rp != _rpts.end()) {
      if (strcmp(what, "advert")) { snprintf(err, n, "repeaters only advert"); return true; }
      rp->second->advert(strstr(rest, "flood") != nullptr);
      if (_uiNode) _uiNode->select();
      return true;
    }
    auto it = _bots.find(name);
    if (it == _bots.end()) { snprintf(err, n, "no node '%s'", name); return true; }
    SimNode& node = *it->second.node;
    node.select();
    MyMesh& m = node.mesh();
    if (!strcmp(what, "advert")) {
      m.uiSendAdvert(strstr(rest, "flood") != nullptr);
    } else if (!strcmp(what, "dm")) {
      char to[32] = {};
      int u = 0;
      sscanf(rest, "%31s %n", to, &u);
      ContactInfo* c = m.searchContactsByPrefix(to);
      if (!c) { snprintf(err, n, "%s has no contact '%s' (advertise first)", name, to); return true; }
      uint32_t ack, est;
      m.sendMessage(*c, rtcOf(node)->getCurrentTimeUnique(), 0, rest + u, ack, est);
    } else if (!strcmp(what, "chan")) {
      int slot = 0, u = 0;
      sscanf(rest, "%d %n", &slot, &u);
      ChannelDetails d;
      if (!m.getChannel(slot, d) || !d.name[0]) { snprintf(err, n, "%s has no channel %d", name, slot); return true; }
      const char* text = rest + u;
      m.sendGroupMessage(rtcOf(node)->getCurrentTimeUnique(), d.channel, m.getNodePrefs()->node_name, text,
                         (int)strlen(text));
    } else if (!strcmp(what, "login")) {
      char to[32] = {}, pw[32] = {};
      sscanf(rest, "%31s %31s", to, pw);
      ContactInfo* c = m.searchContactsByPrefix(to);
      if (!c) { snprintf(err, n, "%s has no contact '%s'", name, to); return true; }
      uint32_t est;
      m.sendLogin(*c, pw, est);
    } else if (!strcmp(what, "join")) {
      ChannelDetails d;
      memset(&d, 0, sizeof d);
      snprintf(d.name, sizeof d.name, "%s", rest);
      mesh::Utils::sha256(d.channel.secret, 16, (const uint8_t*)rest, (int)strlen(rest));
      int slot = 1;
      ChannelDetails e;
      while (m.getChannel(slot, e) && e.name[0]) slot++;
      m.setChannel(slot, d);
      m.uiSaveChannels();
    } else {
      snprintf(err, n, "unknown action '%s'", what);
    }
    if (_uiNode) _uiNode->select();
    return true;
  }

  bool expect(const char* arg, char* err, size_t n) {
    char what[16] = {}, val[16] = {};
    sscanf(arg, "%15s %15s", what, val);
    long want = atol(val);
    long got = -1;
    if (!strcmp(what, "got")) {   // expect got <node> <text>: the node's last DM contains the text
      int used = 0;
      sscanf(arg, "%*15s %*15s %n", &used);
      auto it = _bots.find(val);
      if (it == _bots.end()) { snprintf(err, n, "no node '%s'", val); return true; }
      const std::string& g = it->second.task->lastText;
      if (!used || !strstr(g.c_str(), arg + used)) snprintf(err, n, "%s last got '%s'", val, g.c_str());
      return true;
    }
    if (!strcmp(what, "lastmsg")) {   // expect lastmsg <text>: the first chat's last message, in full, ends so
      const char* want = arg + strlen("lastmsg");
      while (*want == ' ') want++;
      model::Thread t;
      char full[model::TEXT_LEN + 40] = "";
      if (!_ui.thread(0, t) || !_ui.lastMessage(t.id, full, sizeof full)) { snprintf(err, n, "no chats"); return true; }
      size_t fl = strlen(full), wl = strlen(want);
      if (fl < wl || strcmp(full + fl - wl, want)) snprintf(err, n, "last message is '%s'", full);
      return true;
    }
    if (!strcmp(what, "dms")) {   // expect dms <node> <n>: direct messages the node has got
      long want = -1;
      sscanf(arg, "%*15s %*15s %ld", &want);
      auto it = _bots.find(val);
      if (it == _bots.end()) { snprintf(err, n, "no node '%s'", val); return true; }
      if (it->second.task->received != want) snprintf(err, n, "%s got %d DMs, not %ld", val, it->second.task->received, want);
      return true;
    }
    if (!strcmp(what, "pinmode")) {   // expect pinmode random|fixed
      const bool random = _ui.prefs().blePinRandom;
      if (random != !strcmp(val, "random")) snprintf(err, n, "the PIN mode is %s", random ? "random" : "fixed");
      return true;
    }
    if (!strcmp(what, "contacts")) got = _ui.contactCount();
    else if (!strcmp(what, "unread")) got = _ui.unreadTotal();
    else if (!strcmp(what, "heard")) got = _ui.heardCount();
    else if (!strcmp(what, "threads")) got = _ui.threadCount();
    else if (!strcmp(what, "channels")) got = _ui.channelCount();
    else if (!strcmp(what, "linked")) got = _ui.bleConnected();   // remote mode: link up (1) or not (0)
    else if (!strcmp(what, "sounds")) got = g_sound.played;
    else if (!strcmp(what, "leds")) got = g_led.bursts;   // LED bursts since the start
    else if (!strcmp(what, "chanunread")) {   // expect chanunread <slot> <n>: one channel's own count
      long slot = atol(val), want2 = -1;
      sscanf(arg, "%*15s %*15s %ld", &want2);
      got = _ui.unreadIn({model::ThreadKind::Channel, (uint16_t)slot});
      want = want2;
    }
    else if (!strcmp(what, "divider")) got = _app ? ui::screens::chatNewCount(*_app) : -1;   // from the "New" line down
    else if (!strcmp(what, "request")) {
      static const char* const st[] = {"idle", "busy", "ok", "failed", "timeout"};
      const char* g = st[(int)_ui.requestState()];
      if (strcmp(g, val)) snprintf(err, n, "request is %s (%s)", g, _ui.requestText());
      return true;
    }
    else if (!strcmp(what, "window")) got = _ui.store().count();   // messages of the open chat in RAM
    else if (!strcmp(what, "first") || !strcmp(what, "last")) {      // text in the window's oldest/newest
      app::MessageStore& s = _ui.store();
      const char* g = s.count() ? s.at(what[0] == 'f' ? 0 : s.count() - 1).text : "(none)";
      if (!strstr(g, val)) snprintf(err, n, "%s message is '%s'", what, g);
      return true;
    }
    else if (!strcmp(what, "repeats")) {   // last message we sent in the open chat
      app::MessageStore& s = _ui.store();
      got = -1;
      for (int i = s.count() - 1; i >= 0; i--)
        if (s.at(i).outgoing) { got = s.at(i).repeats; break; }
    }
    else if (!strcmp(what, "screen")) {
      bool on = _app && !_app->asleep();
      if (on != !strcmp(val, "on")) snprintf(err, n, "screen is %s", on ? "on" : "off");
      return true;
    }
    else if (!strcmp(what, "identity")) {
      bool same = !memcmp(_keyBeforeWipe, _uiNode->mesh().self_id.pub_key, sizeof _keyBeforeWipe);
      if (same != !strcmp(val, "same")) snprintf(err, n, "identity is %s", same ? "the same" : "new");
      return true;
    }
    else if (!strcmp(what, "status")) {
      static const char* const names[] = {"none", "pending", "sent", "delivered", "failed"};
      app::MessageStore& s = _ui.store();
      const char* g = "no outgoing message";
      for (int i = s.count() - 1; i >= 0; i--)
        if (s.at(i).outgoing) { g = names[(int)s.at(i).status]; break; }
      if (strcmp(g, val)) snprintf(err, n, "expected status %s, got %s", val, g);
      return true;
    } else {
      snprintf(err, n, "unknown expectation '%s'", what);
      return true;
    }
    if (got != want) snprintf(err, n, "expected %s %ld, got %ld", what, want, got);
    return true;
  }

  void dump() {
    printf("contacts %d, channels %d, threads %d, unread %u, heard %d\n", _ui.contactCount(), _ui.channelCount(),
           _ui.threadCount(), _ui.unreadTotal(), _ui.heardCount());
    for (int i = 0; i < _ui.threadCount(); i++) {
      model::Thread t;
      _ui.thread(i, t);
      printf("  thread %-16s unread %u  %s\n", t.title, t.unread, t.preview);
    }
    model::Diag d = _ui.diag();
    printf("  rx %u tx %u flood %u/%u direct %u/%u dups %u air %lums\n", d.rxPackets, d.txPackets, d.rxFlood,
           d.txFlood, d.rxDirect, d.txDirect, d.dups, (unsigned long)d.airTxMs);
    if (_remote) printf("  link: node frames dropped %u\n", _pipe.dropped);
  }

  std::string _dir;
  app::AdapterBase& _ui;
  app::MeshAdapter& _local;
  app::RemoteAdapter* _remote;
  PipeLink& _pipe;
  PipeLink _phone;   // local mode: the phone app's end of the node's serial interface
  uint32_t _phoneTs = 0;
  uint8_t _phoneKey[6] = {}, _phoneAttempt = 0;
  char _phoneText[160] = "";
  fs::FS _remoteFs;
  MultiSerialInterface& _ifaces;
  SimMedium _medium;
  std::unique_ptr<SimNode> _uiNode;
  std::map<std::string, Bot> _bots;
  std::map<std::string, std::unique_ptr<SimRepeater>> _rpts;
  std::map<std::string, std::unique_ptr<SimRoom>> _rooms;
  std::map<std::string, int> _ids;
  bool _fresh = true;
  bool _autoPair = true;
  bool _bleOff = false;
  SimPlatform& _plat;
  ui::App* _app = nullptr;
  ui::Screen* _first = nullptr;
  uint8_t _keyBeforeWipe[PUB_KEY_SIZE] = {};
  int _nextId = 1;   // 0 is the UI node
};

uint32_t g_now = 0;

}  // namespace

int main(int argc, char** argv) {
  const char* script = nullptr;
  const char* out = ".";
  std::string data = "sim-data/simnet";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--smoke")) return simnetSmoke();
    if (!strcmp(argv[i], "--remote-smoke")) return simnetRemoteSmoke();
    if (!strcmp(argv[i], "-s") && i + 1 < argc) script = argv[++i];
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
  }
  FILE* in = script ? fopen(script, "r") : stdin;
  if (!in) { perror(script); return 1; }

  static MultiSerialInterface ifaces;
  static SimPlatform platform;
  static app::MeshAdapter adapter(&board, &ifaces, platform);
  static PipeLink pipe;
  static app::RemoteAdapter remoteAdapter(pipe, platform);
  app::RemoteAdapter* remote = nullptr;
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--remote")) remote = &remoteAdapter;
  model::MeshModel& model = remote ? (model::MeshModel&)remoteAdapter : (model::MeshModel&)adapter;
  static sim::SimDisplay display;
  static sim::SimKeyboard kb(&g_now);
  static ui::App ui(model, display, kb, &g_sound);
  ui.setLed(&g_led);
  static NetWorld world(data, adapter, remote, pipe, ifaces, platform);
  return sim::runScript(in, out, ui, ui::screens::boot(), kb, display, g_now, world);
}
#endif
