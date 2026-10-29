// The UI's only view of the mesh. Implemented by the MeshCore adapter on
// devices (src/app) and by a scripted mock in the simulator. No Arduino or
// MeshCore types cross this boundary, so the UI builds and tests on a host.
//
// Lists are read by index into caller-provided structs: nothing here
// allocates, and the UI holds no copies beyond what is on screen.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace model {

constexpr int NAME_LEN = 32;
constexpr int TEXT_LEN = 160;     // MeshCore's max text payload, plus NUL

enum class NodeType : uint8_t { None = 0, Chat = 1, Repeater = 2, Room = 3, Sensor = 4 };

struct Contact {
  char name[NAME_LEN];
  uint8_t key[6];                 // public key prefix (display only)
  NodeType type;
  int8_t hops;                    // -1: no known path (flood)
  bool favourite;
  uint8_t telemetry;              // what this contact may request: TELEM_* bits
  bool hasPos;
  int32_t lat_e6, lon_e6;
  uint32_t lastHeard;             // epoch seconds of last advert/packet
  int8_t snr_x4;                  // last SNR heard directly, dB*4 (INT8_MIN: unknown)
  int16_t rssi;                   // last RSSI heard directly, dBm (0: unknown)
  uint8_t pathLen;                // bytes of stored path
  uint8_t path[16];               // first hops of the stored path (hash bytes)
};

// Telemetry categories (as MeshCore's companion firmware defines them).
enum : uint8_t { TELEM_BATTERY = 1, TELEM_LOCATION = 2, TELEM_SENSORS = 4 };
// Who may request a category: no one, contacts allowed one by one, everyone.
enum class TelemMode : uint8_t { NoOne = 0, PerContact = 1, Everyone = 2 };

struct Channel {
  uint8_t idx;
  char name[NAME_LEN];
  bool hashtag;                   // "#name" (key derived from name) vs private PSK
  bool muted;
};

enum class ThreadKind : uint8_t { Direct, Channel };

struct ThreadId {
  ThreadKind kind;
  uint16_t idx;                   // contact index, or channel index
  bool operator==(const ThreadId& o) const { return kind == o.kind && idx == o.idx; }
};

struct Thread {
  ThreadId id;
  char title[NAME_LEN];
  char preview[48];
  NodeType type;                  // for DMs: the contact's type
  uint32_t lastTs;
  uint16_t unread;
  bool favourite;
  bool muted;
};

enum class Delivery : uint8_t { None, Pending, Sent, Delivered, Failed };

// How many bytes of a message's route are kept: the T-Deck keeps up to 32 (32 relays with one-byte
// hashes), where a record cannot grow, the Cardputer 6.
#ifdef MC_LONG_PATH
constexpr int PATH_BYTES = 32;
#else
constexpr int PATH_BYTES = 6;
#endif

struct Message {
  uint32_t ts;                    // epoch seconds: when it arrived here (when it was sent, if ours)
  bool skewKnown;                 // incoming: `skew` is set (this device's clock was set when it arrived)
  int32_t skew;                   // incoming: the sender's clock minus ts, in seconds (rounded when kept)
  bool outgoing;
  Delivery status;                // outgoing only
  uint8_t retries;
  int8_t snr_x4;                  // incoming only, INT8_MIN unknown
  int8_t hops;                    // incoming: path length, -1 direct-unknown
  uint8_t repeats;                // outgoing channel msgs: times heard repeated
  uint32_t seq;                   // order in the history (newer is higher), 0 unknown
  uint8_t pathSize;               // incoming: bytes per relay hash in `path` (0: the path is not known)
  uint8_t pathN;                  // relay hashes in `path` (the first ones, if the route was longer than fits)
  uint8_t path[PATH_BYTES];       // incoming: the relays it passed, first relay first
  char sender[NAME_LEN];          // channel messages: who sent it
  char text[TEXT_LEN];
};

struct Radio {
  float freqMHz;
  float bwKHz;
  uint8_t sf, cr;
  int8_t txDbm;
  int8_t txMaxDbm;
  bool repeat;                    // client repeat
  bool rxBoost;
};

struct Preset {
  const char* name;
  float freqMHz, bwKHz;
  uint8_t sf, cr;
  uint8_t pathHashSize;           // bytes per path hop to use, 0 = leave as is
};

// What this device's power management has been doing since its counters were reset
// (the handheld's own, in remote mode too). Zero or false means not measured.
struct Power {
  uint32_t sinceS;                      // since the counters were reset (or boot)
  uint32_t cpuMhz, lowClockS;           // the CPU clock now, and seconds spent on the slower idle clock
  uint32_t screenOffS;
  bool sleepKnown;                      // light sleep exists on this build
  uint32_t sleepS, wakes;               // time inside sleep calls with the screen off, and how often they returned
  char block[16];                       // why sleep was last skipped
  uint16_t battMv;                      // the latest rested reading (0: none yet)
  bool charging;                        // the trend is paused: a host is attached or the voltage is rising
  bool rateKnown;
  int16_t mvPerHour;                    // change in rested voltage per hour (negative: draining)
  uint16_t nvsUsed = 0, nvsTotal = 0;   // entries in the flash settings store (0: unknown); bonds are written there
  uint8_t bleConnects = 0, bleAuthOk = 0, bleAuthFail = 0;   // phone connections since boot, and how their pairing ended
  uint8_t blePeerOta = 0xFF, blePeerId = 0xFF;                // address types of the last phone (0 public, 1 random)
  bool blePeerBonded = false;
  bool bleFailEnc = false, bleFailAuth = false, bleFailBond = false;   // what the last failed pairing had
  char bleRec[3][2][40] = {};                                  // the stored pairing record before off, after on, and now (see BleLastPeer)
  char bleHist[9] = {};                                       // last connections, two characters each (see BleLastPeer)
  int8_t bleBonds = -1;                    // phones paired with the Bluetooth stack (-1: none built); finding why a toggle loses the pairing
};

struct Diag {
  uint32_t uptimeS;
  int16_t noiseFloor;             // dBm
  int16_t lastRssi;               // dBm
  int8_t lastSnr_x4;
  uint32_t rxPackets, txPackets, rxFlood, rxDirect, txFlood, txDirect, dups, rxErrors;
  uint32_t airTxMs, airRxMs;      // cumulative airtime
  uint8_t dutyPct;                // tx airtime over the last hour
  uint16_t queueLen;              // outbound queue
  uint16_t offlineQueue;          // frames waiting for the phone app
  uint32_t heapFree, heapMin, heapMaxBlock;
  uint16_t battMv;
  uint8_t battPct;
  bool charging;
  uint16_t contacts, contactsMax;
  uint32_t flashUsedKb, flashTotalKb;   // the node's file system (0: unknown)
  // Message storage, for finding why history goes missing.
  uint32_t msgWriteFails;               // messages the file system would not keep
  uint32_t msgWrites, archived, archiveFails;
  uint8_t msgErr;                       // why the last write failed: 0 none, 1 open, 2 grow, 3 write, 4 lost
  uint32_t msgErrTs, msgOkTs;           // when the last write failed / was kept
  uint32_t msgSeqFlash, msgSeqNext, msgRingBytes;
  bool archiveOn;                       // a card was there at boot, so old messages are kept on it
  bool msgOnCard;                       // the history lives on the card, not in the flash
  uint32_t sdUsedKb, sdTotalKb;
  Power power;
  // The open conversation's window, against what the conversation holds.
  uint16_t winN;
  uint32_t winFirst, winLast, winThreadLast;
  bool winNewest;
  uint8_t channels, channelsMax;
};

struct RxLogEntry {
  uint32_t ms;                    // device uptime ms
  int8_t snr_x4;
  int16_t rssi;
  uint8_t payloadType;            // MeshCore PAYLOAD_TYPE_*
  uint8_t routeType;              // ROUTE_TYPE_*
  uint8_t pathLen;
  uint8_t len;
  uint8_t srcHash;                // first byte of sender hash when known
  bool dup;
};

struct Gps {
  bool enabled, fix;
  uint8_t sats;
  int32_t lat_e6, lon_e6;
  int32_t altCm;
  uint32_t lastFixS;              // epoch of last fix
  uint8_t hdop_x10;
};

// Which node the UI drives: the one on this device, or a companion node
// elsewhere, over Bluetooth (the device is then that node's screen).
enum class Mode : uint8_t { Local = 0, Remote = 1 };

enum class LinkState : uint8_t { Idle, Scanning, Connecting, Pairing, Ready, Lost };

// Remote mode: the Bluetooth link to the node.
struct Link {
  LinkState state;
  bool saved;                     // a node is chosen (and reconnected to)
  bool synced;                    // connected and its contacts etc. read
  char peer[NAME_LEN];            // the node's Bluetooth name
  char node[NAME_LEN];            // its mesh name
  int8_t rssi;
  uint16_t nodeBattMv;
  char error[40];
};

struct FoundNode {                // a scan result
  char name[NAME_LEN];
  int8_t rssi;
};

// What a new message does (besides the chime, which follows Prefs::sound).
enum class NotifyMode : uint8_t { Led, Screen, Blank, ScreenLed };

struct Prefs {
  char nodeName[NAME_LEN];
  uint8_t theme;
  uint8_t timeZone;               // index into ui::kZones
  uint8_t brightness;             // 0..255
  uint16_t screenTimeoutS;        // 0 = never
  uint16_t inputLockoutMs;
  bool sound;
  uint8_t textSize;               // 0 small, 1 medium, 2 large
  bool gpsEnabled;
  bool gpsShareInAdvert;
  bool gpsTimeSyncAtBoot;
  bool autoAddContacts;
  bool autoBackup;
  bool backupIdentity;
  bool backupMessages;
  bool bleEnabled;
  uint32_t blePin;                // the fixed PIN
  bool blePinRandom;              // a new random PIN at every boot
  uint8_t presetIdx;              // index into presets(), 0xFF custom
  TelemMode telemBattery, telemLocation, telemSensors;
  Mode deviceMode;                // takes effect after a restart
  bool pingBot;                   // answer "ping" DMs with the path they took
  NotifyMode notify;              // what a new message does
  uint8_t cpuSpeed;               // the CPU clock while the screen is on: 0 the board's own, 1 balanced, 2 battery saver
  bool clock12;                   // times as 1:28 PM; false: 13:28
  uint8_t ledBrightness;          // the notification light, 1-255 (the T-Deck's is the key backlight)
  bool shortPreview;              // chats list: scroll only the stored preview, not the whole message
  bool vimKeys;                   // h/j/k/l and friends move around (ui/input/vim.h)
  bool trackball;                 // boards with one: its rolls move (a worn one can be turned off)
  bool touchScrollNatural;        // touch boards: a drag moves the content with the finger (false: the
                                  // selection or view moves with it, as the arrow keys do)
  bool lockOnTimeout;             // boards with an unlock button: when the screen times out, keys and
                                  // touch are ignored until it is pressed (a pocket can't type)
};

// How many CPU speeds Settings offers (1: none). Only speeds validated on the board.
#ifndef MC_CPU_LEVELS
#define MC_CPU_LEVELS 3
#endif

#ifndef MC_HAS_TRACKBALL
#ifdef MC_BOARD_TDECK
#define MC_HAS_TRACKBALL 1
#else
#define MC_HAS_TRACKBALL 0
#endif
#endif

// Vim keys start on where the keyboard has no arrow keys.
#ifndef MC_VIM_KEYS_DEFAULT
#ifdef MC_BOARD_TDECK
#define MC_VIM_KEYS_DEFAULT 1
#else
#define MC_VIM_KEYS_DEFAULT 0
#endif
#endif

struct BackupStatus {
  bool cardPresent;
  bool hasBackup, hasPrevious;
  uint32_t lastBackupTs;          // epoch, 0 never
  char lastError[32];
};

// Results of a remote (repeater / room) request.
struct RemoteStats {
  bool valid;
  uint16_t battMv;
  uint32_t uptimeS;
  int16_t noiseFloor, lastRssi;
  int8_t lastSnr_x4;
  uint32_t rxPackets, txPackets, airTxS, airRxS;
  uint16_t queueLen;
  uint32_t floodsSent, directSent, dupsFlood, dupsDirect;
};

struct Neighbour {
  uint8_t key[4];
  char name[NAME_LEN];            // resolved from contacts, or empty
  int8_t snr_x4;
  uint32_t heardAgoS;
};

enum class Req : uint8_t { Idle, Busy, Ok, Failed, Timeout };

class MeshModel {
public:
  virtual ~MeshModel() {}

  // Bumped whenever anything the UI shows may have changed.
  virtual uint32_t revision() const = 0;
  virtual uint32_t now() const = 0;               // epoch seconds (RTC)
  virtual uint32_t uptimeMs() const = 0;

  // Identity.
  virtual void selfKey(uint8_t out[6]) const = 0;

  // Contacts.
  virtual int contactCount() const = 0;
  virtual bool contact(int i, Contact& out) const = 0;
  virtual void setFavourite(int i, bool fav) = 0;
  virtual bool removeContact(int i) = 0;
  virtual bool resetPath(int i) = 0;
  virtual void setTelemetry(int i, uint8_t telem) = 0;   // TELEM_* bits

  // Channels.
  virtual int channelCount() const = 0;
  virtual bool channel(int i, Channel& out) const = 0;
  virtual bool joinHashtag(const char* name) = 0;        // "#foo"
  virtual bool joinPrivate(const char* name, const uint8_t psk[16]) = 0;
  virtual bool createPrivate(const char* name) = 0;      // random PSK
  virtual bool channelKey(int i, uint8_t psk[16]) const = 0;
  virtual bool removeChannel(int i) = 0;
  virtual void setMuted(int i, bool muted) = 0;

  // Conversations, most recent first.
  virtual int threadCount() const = 0;
  virtual bool thread(int i, Thread& out) const = 0;
  virtual int messageCount(ThreadId t) const = 0;
  virtual bool message(ThreadId t, int i, Message& out) const = 0;  // 0 = oldest kept
  virtual void markRead(ThreadId t) = 0;
  // Unread messages in `t`: true, with `readSeq` the seq of the last one read
  // (messages after it are new).
  virtual bool unreadSince(ThreadId t, uint32_t& readSeq) const { (void)t; (void)readSeq; return false; }
  // The thread's last message in full (its preview may be cut short).
  virtual bool lastMessage(ThreadId t, char* out, int n) const = 0;
  // Kept off the chats list (history and membership stay). A new message
  // brings a DM or unmuted channel back; a muted channel stays off.
  virtual bool threadHidden(ThreadId t) const = 0;
  virtual void setThreadHidden(ThreadId t, bool on) = 0;
  // Unread messages in one conversation (muted and hidden ones included).
  virtual uint16_t unreadIn(ThreadId t) const = 0;
  // message() shows a window of a conversation: slide it to older or newer
  // messages; each returns how many came in (0 = no more that way).
  virtual int loadOlder(ThreadId t) = 0;
  virtual int loadNewer(ThreadId t) = 0;
  virtual bool atNewest(ThreadId t) const = 0;
  // Up to `max` messages of a conversation, oldest first, ending with the one at `upTo` (a
  // sequence number) or the last one before it; the window shown by message() stays put.
  virtual int history(ThreadId t, uint32_t upTo, Message* out, int max) = 0;
  // Up to `before` messages up to and including the one at `seq`, and up to `after` messages after it,
  // oldest first (at most `max` in all); the window shown by message() stays put.
  virtual int historyAround(ThreadId t, uint32_t seq, int before, int after, Message* out, int max) = 0;
  virtual void clearHistory(ThreadId t) = 0;
  virtual uint16_t unreadTotal() const = 0;
  virtual bool send(ThreadId t, const char* text) = 0;

  // Adverts and discovery.
  virtual bool advert(bool flood) = 0;
  virtual int heardCount() const = 0;                     // recently heard adverts
  virtual bool heard(int i, Contact& out) const = 0;
  virtual bool addHeard(int i) = 0;
  virtual bool discoverRepeaters() = 0;                   // zero-hop
  virtual Req discoverState() const = 0;

  // Repeater / room admin, one request in flight at a time.
  virtual bool login(int contact, const char* password) = 0;   // remembers the password if it works
  virtual bool loggedIn(int contact) const = 0;                  // this session
  virtual bool hasSavedPassword(int contact) const = 0;
  virtual bool loginSaved(int contact) = 0;                      // log in with the saved password
  virtual void forgetPassword(int contact) = 0;
  virtual bool requestStats(int contact) = 0;
  virtual bool requestNeighbours(int contact) = 0;
  virtual bool requestTelemetry(int contact) = 0;
  virtual bool sendCommand(int contact, const char* cmd) = 0;   // CLI text
  virtual bool tracePath(int contact) = 0;
  virtual bool ping(int contact) = 0;
  virtual Req requestState() const = 0;
  virtual const char* requestText() const = 0;            // last reply / error, for display
  virtual const RemoteStats& remoteStats() const = 0;
  virtual int neighbourCount() const = 0;
  virtual bool neighbour(int i, Neighbour& out) const = 0;
  virtual uint32_t lastRttMs() const = 0;

  // Radio.
  virtual Radio radio() const = 0;
  virtual bool setRadio(const Radio& r) = 0;
  virtual int presetCount() const = 0;
  virtual const Preset& preset(int i) const = 0;

  // Diagnostics.
  virtual Diag diag() const = 0;
  virtual void resetPower() {}                            // zero the Power counters
  // The screen is off: skip what only the screen shows (cosmetic polls, GPS nobody asked for).
  virtual void setQuiet(bool quiet) { (void)quiet; }
  virtual int rxLogCount() const = 0;
  virtual bool rxLog(int i, RxLogEntry& out) const = 0;  // 0 = newest
  virtual Gps gps() const = 0;
  virtual bool gpsSyncTime() = 0;

  // Settings.
  virtual const Prefs& prefs() const = 0;
  virtual void setPrefs(const Prefs& p) = 0;
  virtual bool bleConnected() const = 0;
  virtual bool bleAvailable() const = 0;                  // compiled in
  virtual uint32_t bleActivePin() const = 0;              // the PIN in use since boot
  virtual bool blePairing() const = 0;                    // a phone is connected, not yet authenticated
  virtual bool pingBotAvailable() const { return false; } // the local node only
  // The name of the one repeater or room whose key starts with these `size` bytes (a relay on a path).
  virtual bool relayName(const uint8_t* hash, int size, char* out, int n) const { (void)hash; (void)size; (void)out; (void)n; return false; }

  // Remote mode (the defaults are for a model without it).
  virtual Mode mode() const { return Mode::Local; }
  virtual Link link() const { return Link(); }
  virtual void scan(bool on) { (void)on; }
  virtual int foundCount() const { return 0; }
  virtual bool found(int i, FoundNode& out) const { (void)i; (void)out; return false; }
  virtual bool connectFound(int i, uint32_t pin) { (void)i; (void)pin; return false; }
  virtual void reconnect() {}
  virtual void disconnect() {}
  virtual void forgetNode() {}
  virtual bool rebootNode() { return false; }
  virtual bool resetNode() { return false; }

  // Backup.
  virtual BackupStatus backupStatus() const = 0;
  // True on a fresh install (no identity yet) when the SD card holds a backup.
  virtual bool restoreOffered() const = 0;
  virtual bool backupNow() = 0;
  virtual bool restore(bool previous) = 0;                // reboots on success
  virtual void factoryReset() = 0;
  virtual void reboot() = 0;
};

}  // namespace model
