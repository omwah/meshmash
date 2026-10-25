// Main menu, Settings, Radio and Backup.
#include "gen/unicode_cfg.h"
#include "ui/screens/screens.h"
#include "ui/widgets.h"
#include "ui/tz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ui { namespace screens {

namespace {

const char* yesNo(bool b) { return b ? "on" : "off"; }

// ------------------------------------------------------------- main menu

enum MItem { M_HDR_MESH, M_JOIN, M_HEARD, M_DISCOVER, M_ADV0, M_ADVF,
             M_HDR_DEV, M_RADIO, M_DIAG, M_LOG, M_GPS,
             M_HDR_SYS, M_REMOTE, M_SETTINGS, M_BACKUP, M_ABOUT, M_REBOOT, M_N };

bool remoteMode(App& a) { return a.m().mode() == model::Mode::Remote; }

// Row -> item: "Remote node" is only there in remote mode.
int mItem(App& a, int i) { return !remoteMode(a) && i >= M_REMOTE ? i + 1 : i; }

class MainMenu : public MenuScreen {
public:
  const char* title(App&) override { return "Menu"; }

protected:
  int count(App& a) override { return remoteMode(a) ? M_N : M_N - 1; }
  void item(App& a, int i, MenuItem& it) override {
    switch (mItem(a, i)) {
      case M_HDR_MESH: it.label = "MESH"; it.header = true; break;
      case M_JOIN: it.label = "Join channel"; break;
      case M_HEARD: it.label = "Heard adverts"; snprintf(it.value, sizeof it.value, "%d", a.m().heardCount()); it.valueCol = C_DIM; break;
      case M_DISCOVER: it.label = "Discover repeaters"; break;
      case M_ADV0: it.label = "Advertise (zero hop)"; break;
      case M_ADVF: it.label = "Advertise (flood)"; break;
      case M_HDR_DEV: it.label = "RADIO & DIAGNOSTICS"; it.header = true; break;
      case M_RADIO: {
        it.label = "Radio";
        model::Radio r = a.m().radio();
        snprintf(it.value, sizeof it.value, "%.3f SF%u", r.freqMHz, r.sf);
        break;
      }
      case M_DIAG: it.label = "Diagnostics"; break;
      case M_LOG: it.label = "Packet log"; snprintf(it.value, sizeof it.value, "%d", a.m().rxLogCount()); it.valueCol = C_DIM; break;
      case M_GPS: {
        it.label = "GPS";
        model::Gps g = a.m().gps();
        snprintf(it.value, sizeof it.value, !g.enabled ? "off" : g.fix ? "fix" : "searching");
        it.valueCol = !g.enabled ? C_DIM : g.fix ? C_OK : C_WARN;
        break;
      }
      case M_HDR_SYS: it.label = "SYSTEM"; it.header = true; break;
      case M_REMOTE: {
        it.label = "Remote node";
        model::Link l = a.m().link();
        bool up = l.state == model::LinkState::Ready;
        snprintf(it.value, sizeof it.value, "%s", up ? "connected" : !l.saved ? "none" : "not connected");
        it.valueCol = up ? C_OK : C_WARN;
        break;
      }
      case M_SETTINGS: it.label = "Settings"; break;
      case M_BACKUP: {
        it.label = "Backup & restore";
        model::BackupStatus b = a.m().backupStatus();
        snprintf(it.value, sizeof it.value, !b.cardPresent ? "no card" : a.m().prefs().autoBackup ? "auto" : "manual");
        it.valueCol = b.cardPresent ? C_OK : C_DIM;
        break;
      }
      case M_ABOUT: it.label = "About"; break;
      case M_REBOOT: it.label = "Reboot"; break;
    }
  }
  void activate(App& a, int i) override {
    switch (mItem(a, i)) {
      case M_JOIN: openJoinChannel(a); break;
      case M_HEARD: openHeard(a, false); break;
      case M_DISCOVER:
        if (a.m().discoverRepeaters()) openHeard(a, true);
        else a.toast("Busy, try again", C_WARN);
        break;
      case M_ADV0: a.m().advert(false); a.toast("Advert sent (zero hop)", C_OK); break;
      case M_ADVF: a.m().advert(true); a.toast("Advert sent (flood)", C_OK); break;
      case M_RADIO: openRadio(a); break;
      case M_DIAG: openDiag(a); break;
      case M_LOG: openPacketLog(a); break;
      case M_GPS: openGps(a); break;
#if MC_UI_REMOTE
      case M_REMOTE: openRemote(a); break;
#endif
      case M_SETTINGS: openSettings(a); break;
      case M_BACKUP: openBackup(a); break;
      case M_ABOUT: {
        static char body[200];
        uint8_t k[6];
        a.m().selfKey(k);
        snprintf(body, sizeof body,
                 "MeshMash\nKeyboard-native MeshCore\n\nNode: %s\nKey: %02X%02X%02X%02X...\n\n"
                 "Firmware " MC_UI_VERSION "\nUnicode: " MC_UNICODE_PROFILE "\nMIT licensed.",
                 a.m().prefs().nodeName, k[0], k[1], k[2], k[3]);
        openText(a, "About", body);
        break;
      }
      case M_REBOOT:
        a.confirm("Reboot now?", [](App& app, int) { app.m().reboot(); });
        break;
    }
  }
};

MainMenu s_menu;

// ------------------------------------------------------------- settings

enum SItem { S_HDR_ID, S_NAME, S_MODE, S_REMOTE, S_HDR_TIME, S_TZ, S_HDR_DISP, S_THEME, S_BRIGHT, S_TIMEOUT, S_TEXT, S_PREVIEW,
             S_HDR_IN, S_VIM, S_BALL, S_LOCK, S_SCROLL, S_LOCKOUT, S_SOUND, S_NOTIFY,
             S_HDR_MESH, S_AUTOADD, S_PINGBOT, S_BLE, S_PIN, S_PINMODE,
             S_HDR_GPS, S_GPS, S_GPSADV, S_GPSTIME,
             S_HDR_TELEM, S_TBATT, S_TLOC, S_TENV,
             S_HDR_DANGER, S_FACTORY, S_N };

// Row -> item: "Trackball", "Lock when screen off" (the trackball's click unlocks) and
// "Touch Scrolling Direction" are only there on boards with one (the T-Deck, which is also the touch board).
int sItem(int i) { return !MC_HAS_TRACKBALL && i >= S_BALL ? i + 3 : i; }

const uint16_t kTimeouts[] = {0, 15, 30, 60, 120, 300};
const uint16_t kLockouts[] = {10, 30, 60, 100, 150, 250, 500};

template <typename T, int N>
int stepIn(const T (&arr)[N], T cur, int dir) {
  int i = 0;
  while (i < N - 1 && arr[i] < cur) i++;
  i += dir;
  if (i < 0) i = 0;
  if (i >= N) i = N - 1;
  return arr[i];
}

class SettingsScreen : public MenuScreen {
public:
  const char* title(App&) override { return "Settings"; }

protected:
  int count(App&) override { return MC_HAS_TRACKBALL ? S_N : S_N - 3; }
  void item(App& a, int i, MenuItem& it) override {
    i = sItem(i);
    const model::Prefs& p = a.m().prefs();
    switch (i) {
      case S_HDR_ID: it.label = "IDENTITY"; it.header = true; break;
      case S_NAME: it.label = "Node name"; snprintf(it.value, sizeof it.value, "%s", p.nodeName); break;
      case S_MODE:
        it.label = "Mesh node"; it.choice = true;
        it.enabled = MC_UI_REMOTE;   // a remote node needs Bluetooth
        snprintf(it.value, sizeof it.value, "%s", p.deviceMode == model::Mode::Remote ? "remote (BLE)" : "this device");
        break;
      case S_REMOTE: it.label = "Remote node"; it.enabled = remoteMode(a); break;
      case S_HDR_TIME: it.label = "TIME"; it.header = true; break;
      case S_TZ: it.label = "Time zone"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", kZones[p.timeZone % kZoneCount].name); break;
      case S_HDR_DISP: it.label = "DISPLAY"; it.header = true; break;
      case S_THEME: it.label = "Theme"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", kThemes[p.theme % kThemeCount].name); break;
      case S_BRIGHT: it.label = "Brightness"; it.choice = true; snprintf(it.value, sizeof it.value, "%d%%", (p.brightness * 100 + 127) / 255); break;
      case S_TIMEOUT:
        it.label = "Screen off after"; it.choice = true;
        if (p.screenTimeoutS) snprintf(it.value, sizeof it.value, "%us", p.screenTimeoutS);
        else snprintf(it.value, sizeof it.value, "never");
        break;
      case S_TEXT: {
        static const char* const kSizes[] = {"small", "medium", "large"};
        it.label = "Text size"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", kSizes[p.textSize % 3]);
        break;
      }
      case S_PREVIEW: it.label = "Chat preview"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", p.shortPreview ? "short" : "whole message"); break;
      case S_HDR_IN: it.label = "INPUT & SOUND"; it.header = true; break;
      case S_VIM: it.label = "Vim keys"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.vimKeys)); break;
      case S_BALL:
        it.label = "Trackball"; it.choice = true;
        snprintf(it.value, sizeof it.value, "%s", yesNo(p.trackball));
        break;
      case S_LOCK:
        it.label = "Lock when screen off"; it.choice = true;
        snprintf(it.value, sizeof it.value, "%s", yesNo(p.lockOnTimeout));
        break;
      case S_SCROLL:
        it.label = "Touch Scrolling Direction"; it.choice = true;
        snprintf(it.value, sizeof it.value, "%s", p.touchScrollNatural ? "opposite" : "same");
        break;
      case S_LOCKOUT: it.label = "Key filter"; it.choice = true; snprintf(it.value, sizeof it.value, "%u ms", p.inputLockoutMs); break;
      case S_SOUND: it.label = "Sound"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.sound)); break;
      case S_NOTIFY: {
        static const char* const kModes[] = {"LED only", "screen on", "stay dark", "screen + LED"};
        it.label = "New message"; it.choice = true;
        snprintf(it.value, sizeof it.value, "%s", kModes[(int)p.notify % 4]);
        break;
      }
      case S_HDR_MESH: it.label = "MESH & BLUETOOTH"; it.header = true; break;
      case S_AUTOADD: it.label = "Auto-add contacts"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.autoAddContacts)); break;
      case S_PINGBOT:
        it.label = "Answer \"ping\" DMs"; it.choice = true;
        it.enabled = a.m().pingBotAvailable();
        snprintf(it.value, sizeof it.value, "%s", !it.enabled ? "n/a" : yesNo(p.pingBot));
        break;
      case S_BLE:
        it.label = "Bluetooth (phone app)"; it.choice = true;
        it.enabled = a.m().bleAvailable();
        snprintf(it.value, sizeof it.value, "%s", !it.enabled ? "n/a" : !p.bleEnabled ? "off" : a.m().bleConnected() ? "connected" : "on");
        it.valueCol = a.m().bleConnected() ? C_OK : C_ACCENT;
        break;
      case S_PIN:
        it.label = "Bluetooth PIN";
        it.enabled = a.m().bleAvailable();
        snprintf(it.value, sizeof it.value, "%06u", (unsigned)a.m().bleActivePin());
        break;
      case S_PINMODE:
        it.label = "New PIN";
        it.choice = true;
        it.enabled = a.m().bleAvailable();
        snprintf(it.value, sizeof it.value, p.blePinRandom ? "each boot" : "fixed");
        break;
      case S_HDR_GPS: it.label = "GPS"; it.header = true; break;
      case S_GPS: it.label = "GPS receiver"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.gpsEnabled)); break;
      case S_GPSADV: it.label = "Share position in adverts"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.gpsShareInAdvert)); it.enabled = p.gpsEnabled; break;
      // Works with the receiver off too: it is powered just until the first fix.
      case S_GPSTIME: it.label = "Set clock from GPS at boot"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.gpsTimeSyncAtBoot)); break;
      case S_HDR_TELEM: it.label = "TELEMETRY: WHO MAY ASK"; it.header = true; break;
      case S_TBATT:
      case S_TLOC:
      case S_TENV: {
        static const char* const kLabels[] = {"Battery", "Location", "Sensors"};
        static const char* const kModes[] = {"no one", "per contact", "everyone"};
        model::TelemMode m = i == S_TBATT ? p.telemBattery : i == S_TLOC ? p.telemLocation : p.telemSensors;
        it.label = kLabels[i - S_TBATT];
        it.choice = true;
        snprintf(it.value, sizeof it.value, "%s", kModes[(int)m % 3]);
        it.valueCol = m == model::TelemMode::Everyone ? C_WARN : C_ACCENT;
        break;
      }
      case S_HDR_DANGER: it.label = "RESET"; it.header = true; break;
      case S_FACTORY: it.label = "Factory reset"; it.valueCol = C_ERR; break;
    }
  }

  void activate(App& a, int i) override {
    i = sItem(i);
    switch (i) {
      case S_NAME:
        a.prompt("Node name", a.m().prefs().nodeName, model::NAME_LEN - 1, [](App& app, int) {
          if (!app.promptText()[0]) return;
          model::Prefs p = app.m().prefs();
          snprintf(p.nodeName, sizeof p.nodeName, "%s", app.promptText());
          app.m().setPrefs(p);
          app.toast("Name saved: advertise to share it", C_OK);
        });
        break;
      case S_PIN:
        if (a.m().prefs().blePinRandom) {
          a.toast("PIN changes every boot: set New PIN to fixed to choose one", C_WARN);
          break;
        }
        a.prompt("6-digit PIN", "", 6, [](App& app, int) {
          const char* t = app.promptText();
          uint32_t v = 0;
          int n = 0;
          for (; t[n] >= '0' && t[n] <= '9'; n++) v = v * 10 + (t[n] - '0');
          if (n != 6 || t[n]) { app.toast("PIN must be 6 digits", C_ERR); return; }
          model::Prefs p = app.m().prefs();
          p.blePin = v;
          app.m().setPrefs(p);
          app.toast("PIN saved: restart, then re-pair the phone", C_OK);
        });
        break;
      case S_FACTORY:
        if (remoteMode(a)) {
          a.confirm("Erase this device's messages and settings? The node keeps its own.",
                    [](App& app, int) { app.m().factoryReset(); });
          break;
        }
        a.confirm("Erase identity, contacts, channels and settings? This cannot be undone.", [](App& app, int) {
          app.m().factoryReset();
        });
        break;
#if MC_UI_REMOTE
      case S_REMOTE: openRemote(a); break;
#endif
      case S_NOTIFY: a.flashLed(); break;   // a preview of the colours
      default:
        adjust(a, i, 1);
    }
  }

  void adjust(App& a, int i, int dir) override {
    i = sItem(i);
    model::Prefs p = a.m().prefs();
    switch (i) {
      case S_MODE:
        if (!MC_UI_REMOTE) return;
        a.confirm(p.deviceMode == model::Mode::Remote
                      ? "Restart as its own node, on this device's radio?"
                      : "Restart as the screen of another node, over Bluetooth? This device's own node pauses.",
                  [](App& app, int) {
                    model::Prefs q = app.m().prefs();
                    q.deviceMode = q.deviceMode == model::Mode::Remote ? model::Mode::Local : model::Mode::Remote;
                    app.m().setPrefs(q);
                    app.m().reboot();
                  });
        return;
      case S_TZ: p.timeZone = (uint8_t)((p.timeZone + kZoneCount + dir) % kZoneCount); break;
      case S_THEME: p.theme = (uint8_t)((p.theme + kThemeCount + dir) % kThemeCount); break;
      case S_BRIGHT: {
        int v = p.brightness + dir * 32;
        p.brightness = (uint8_t)(v < 31 ? 31 : v > 255 ? 255 : v);
        break;
      }
      case S_TIMEOUT: p.screenTimeoutS = (uint16_t)stepIn(kTimeouts, p.screenTimeoutS, dir); break;
      case S_TEXT: p.textSize = (uint8_t)((p.textSize + 3 + dir) % 3); break;
      case S_PREVIEW: p.shortPreview = !p.shortPreview; break;
      case S_VIM: p.vimKeys = !p.vimKeys; break;
      case S_BALL: p.trackball = !p.trackball; break;
      case S_LOCK: p.lockOnTimeout = !p.lockOnTimeout; break;
      case S_SCROLL: p.touchScrollNatural = !p.touchScrollNatural; break;
      case S_LOCKOUT: p.inputLockoutMs = (uint16_t)stepIn(kLockouts, p.inputLockoutMs, dir); break;
      case S_SOUND: p.sound = !p.sound; break;
      case S_NOTIFY: p.notify = (model::NotifyMode)(((int)p.notify + 4 + dir) % 4); break;
      case S_AUTOADD: p.autoAddContacts = !p.autoAddContacts; break;
      case S_PINGBOT: p.pingBot = !p.pingBot; break;
      case S_BLE: p.bleEnabled = !p.bleEnabled; break;
      case S_PINMODE:
        p.blePinRandom = !p.blePinRandom;
        a.toast("Applies after a restart", C_ACCENT);
        break;
      case S_GPS: p.gpsEnabled = !p.gpsEnabled; break;
      case S_GPSADV: p.gpsShareInAdvert = !p.gpsShareInAdvert; break;
      case S_GPSTIME: p.gpsTimeSyncAtBoot = !p.gpsTimeSyncAtBoot; break;
      case S_TBATT:
      case S_TLOC:
      case S_TENV: {
        model::TelemMode& m = i == S_TBATT ? p.telemBattery : i == S_TLOC ? p.telemLocation : p.telemSensors;
        m = (model::TelemMode)(((int)m + 3 + dir) % 3);
        break;
      }
      default: return;
    }
    a.m().setPrefs(p);
    a.applyPrefs();
  }
};

SettingsScreen s_settings;

// ------------------------------------------------------------- radio

enum RItem { R_PRESET, R_FREQ, R_BW, R_SF, R_CR, R_TX, R_HDR_ADV, R_REPEAT, R_BOOST, R_APPLY, R_N };

const float kBws[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};

class RadioScreen : public MenuScreen {
public:
  const char* title(App&) override { return _dirty ? "Radio (unsaved)" : "Radio"; }
  void onEnter(App& a) override {
    if (!_dirty) {
      _r = a.m().radio();
      _preset = a.m().prefs().presetIdx;
    }
    MenuScreen::onEnter(a);
  }

protected:
  int count(App&) override { return R_N; }
  void item(App& a, int i, MenuItem& it) override {
    switch (i) {
      case R_PRESET:
        it.label = "Preset"; it.choice = true;
        snprintf(it.value, sizeof it.value, "%s", _preset < a.m().presetCount() ? a.m().preset(_preset).name : "custom");
        break;
      case R_FREQ: it.label = "Frequency"; it.choice = true; snprintf(it.value, sizeof it.value, "%.3f MHz", _r.freqMHz); break;
      case R_BW: it.label = "Bandwidth"; it.choice = true; snprintf(it.value, sizeof it.value, "%g kHz", _r.bwKHz); break;
      case R_SF: it.label = "Spreading factor"; it.choice = true; snprintf(it.value, sizeof it.value, "SF%u", _r.sf); break;
      case R_CR: it.label = "Coding rate"; it.choice = true; snprintf(it.value, sizeof it.value, "4/%u", _r.cr); break;
      case R_TX: it.label = "TX power"; it.choice = true; snprintf(it.value, sizeof it.value, "%d dBm", _r.txDbm); it.valueCol = _r.txDbm > 20 ? C_WARN : C_ACCENT; break;
      case R_HDR_ADV: it.label = "ADVANCED"; it.header = true; break;
      case R_REPEAT: it.label = "Repeat packets"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(_r.repeat)); break;
      case R_BOOST: it.label = "RX boosted gain"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(_r.rxBoost)); break;
      case R_APPLY: it.label = _dirty ? "Apply changes" : "No changes"; it.enabled = _dirty; it.valueCol = C_WARN; break;
    }
  }
  void activate(App& a, int i) override {
    if (i == R_FREQ) {
      char cur[16];
      snprintf(cur, sizeof cur, "%.3f", _r.freqMHz);
      a.prompt("Frequency MHz", cur, 10, [](App& app, int) {
        // strtod rather than sscanf: MeshCore already links strtod, and no
        // scanf saves ~17 KB of flash.
        char* end = nullptr;
        float f = (float)strtod(app.promptText(), &end);
        if (end != app.promptText() && f > 100 && f < 2600) {
          s_radioSetFreq(f);
        } else {
          app.toast("Invalid frequency", C_ERR);
        }
      });
    } else if (i == R_APPLY) {
      a.confirm("Apply radio settings? You will only hear nodes using the same settings.", [](App& app, int) {
        s_radioApply(app);
      });
    } else {
      adjust(a, i, 1);
    }
  }
  void adjust(App& a, int i, int dir) override {
    switch (i) {
      case R_PRESET: {
        int n = a.m().presetCount();
        if (!n) return;
        _preset = (uint8_t)(((_preset >= n ? 0 : _preset) + n + dir) % n);
        const model::Preset& p = a.m().preset(_preset);
        _r.freqMHz = p.freqMHz; _r.bwKHz = p.bwKHz; _r.sf = p.sf; _r.cr = p.cr;
        break;
      }
      case R_FREQ: _r.freqMHz += dir * 0.025f; _preset = 0xFF; break;
      case R_BW: {
        int k = 0;
        while (k < 9 && kBws[k] + 0.01f < _r.bwKHz) k++;
        k += dir;
        _r.bwKHz = kBws[k < 0 ? 0 : k > 9 ? 9 : k];
        _preset = 0xFF;
        break;
      }
      case R_SF: _r.sf = (uint8_t)(_r.sf + dir < 5 ? 5 : _r.sf + dir > 12 ? 12 : _r.sf + dir); _preset = 0xFF; break;
      case R_CR: _r.cr = (uint8_t)(_r.cr + dir < 5 ? 5 : _r.cr + dir > 8 ? 8 : _r.cr + dir); _preset = 0xFF; break;
      case R_TX: {
        int v = _r.txDbm + dir;
        _r.txDbm = (int8_t)(v < -9 ? -9 : v > _r.txMaxDbm ? _r.txMaxDbm : v);
        break;
      }
      case R_REPEAT: _r.repeat = !_r.repeat; break;
      case R_BOOST: _r.rxBoost = !_r.rxBoost; break;
      default: return;
    }
    _dirty = true;
  }

public:
  static void s_radioSetFreq(float f);
  static void s_radioApply(App& a);
  model::Radio _r{};
  uint8_t _preset = 0;
  bool _dirty = false;
};

RadioScreen s_radio;

void RadioScreen::s_radioSetFreq(float f) {
  s_radio._r.freqMHz = f;
  s_radio._preset = 0xFF;
  s_radio._dirty = true;
}

void RadioScreen::s_radioApply(App& a) {
  if (!a.m().setRadio(s_radio._r)) { a.toast("Radio rejected these settings", C_ERR); return; }
  model::Prefs p = a.m().prefs();
  p.presetIdx = s_radio._preset;
  a.m().setPrefs(p);
  s_radio._dirty = false;
  a.toast("Radio settings applied", C_OK);
}

// ------------------------------------------------------------- backup

enum BItem { B_AUTO, B_IDENT, B_MSGS, B_NOW, B_RESTORE, B_PREV, B_N };

class BackupScreen : public MenuScreen {
public:
  const char* title(App&) override { return "Backup & restore"; }

protected:
  int drawHead(App& a, Canvas& c, const Rect& r) override {
    model::BackupStatus b = a.m().backupStatus();
    const mc_font_t* s = a.fSmall();
    const int lh = s->height + 1;
    c.fillRect(r.x, r.y, r.w, 2 * lh + 4, C_ZEBRA);
    char l[48];
    if (!b.cardPresent) snprintf(l, sizeof l, "No SD card inserted");
    else if (!b.hasBackup) snprintf(l, sizeof l, "SD card ready, no backup yet");
    else {
      char ago[8];
      fmtAgo(ago, sizeof ago, a.m().now(), b.lastBackupTs);
      snprintf(l, sizeof l, "Last backup %s ago%s", ago, b.hasPrevious ? " (+ previous)" : "");
    }
    c.text(r.x + 4, r.y + 2, l, s, b.cardPresent ? C_TEXT : C_WARN, -1, r.w - 8);
    if (b.lastError[0]) c.text(r.x + 4, r.y + 2 + lh, b.lastError, s, C_ERR, -1, r.w - 8);
    else c.text(r.x + 4, r.y + 2 + lh, remoteMode(a) ? "of the remote node, on this SD card" : "/meshmash/backup.bin", s,
                C_DIM, -1, r.w - 8);
    return 2 * lh + 6;
  }
  int count(App&) override { return B_N; }
  void item(App& a, int i, MenuItem& it) override {
    const model::Prefs& p = a.m().prefs();
    model::BackupStatus b = a.m().backupStatus();
    switch (i) {
      case B_AUTO: it.label = "Auto-backup to SD"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.autoBackup)); break;
      case B_IDENT: it.label = "Include identity key"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.backupIdentity)); it.valueCol = p.backupIdentity ? C_WARN : C_ACCENT; break;
      case B_MSGS: it.label = "Include messages"; it.choice = true; snprintf(it.value, sizeof it.value, "%s", yesNo(p.backupMessages)); break;
      case B_NOW: it.label = "Back up now"; it.enabled = b.cardPresent; break;
      case B_RESTORE: it.label = "Restore from SD"; it.enabled = b.hasBackup; break;
      case B_PREV: it.label = "Restore previous backup"; it.enabled = b.hasPrevious; break;
    }
  }
  void activate(App& a, int i) override {
    switch (i) {
      case B_NOW:
        if (a.m().backupNow()) a.toast("Backup written", C_OK);
        else a.toast("Backup failed", C_ERR);
        break;
      case B_RESTORE:
      case B_PREV:
        a.confirm(remoteMode(a) ? "Put this backup back on the node, replacing its settings, channels and contacts?"
                                : "Replace this device's settings with the backup and reboot?",
                  [](App& app, int prev) {
                    if (!app.m().restore(prev != 0)) app.toast("Restore failed: backup unreadable", C_ERR);
                    else if (remoteMode(app)) app.toast("Restoring to the node...", C_OK);
                  }, i == B_PREV);
        break;
      default:
        adjust(a, i, 1);
    }
  }
  void adjust(App& a, int i, int) override {
    model::Prefs p = a.m().prefs();
    if (i == B_AUTO) p.autoBackup = !p.autoBackup;
    else if (i == B_IDENT) {
      if (!p.backupIdentity) {
        a.confirm("Store the private key on the SD card? Anyone with the card can impersonate this node.", [](App& app, int) {
          model::Prefs q = app.m().prefs();
          q.backupIdentity = true;
          app.m().setPrefs(q);
        });
        return;
      }
      p.backupIdentity = false;
    } else if (i == B_MSGS) p.backupMessages = !p.backupMessages;
    else return;
    a.m().setPrefs(p);
  }
};

BackupScreen s_backup;

}  // namespace

void openMainMenu(App& a) {
  if (a.top() == &s_menu) a.pop();   // Menu again closes it
  else a.push(&s_menu);   // the cursor stays on the last item
}
bool mainMenuOpen(App& a) { return a.top() == &s_menu; }
void openSettings(App& a) { s_settings.resetNav(); a.push(&s_settings); }
void openRadio(App& a) { s_radio.resetNav(); a.push(&s_radio); }
void openBackup(App& a) { s_backup.resetNav(); a.push(&s_backup); }

}}  // namespace ui::screens
