// Power counters: how long the screen has been off, and a battery-voltage trend
// taken only from rested readings. Plain arithmetic on a clock the caller passes
// in, so it runs the same on the host. Nothing here is persisted or logged.
#pragma once
#include <stdint.h>
#include "model/model.h"

namespace app {

class PowerStats {
public:
  static constexpr uint32_t kSettleMs = 30000;          // screen off this long before a reading is rested
  static constexpr uint32_t kSampleEveryMs = 300000;    // between rested readings
  static constexpr uint32_t kRateMinMs = 3600000;       // a rate needs this much trend...
  static constexpr int kNoiseMv = 15;                   // ...and a rise above this is charging, not ADC noise

  PowerStats() { reset(0); }

  void reset(uint32_t now) {
    bool off = _off;
    *this = PowerStats(Tag{});
    _t0 = now;
    _off = off;
    _offSince = now;
  }

  // The screen went off or came on. Calling it with the state it already has does nothing.
  void screen(bool off, uint32_t now) {
    if (off == _off) return;
    if (off) _offSince = now;
    else _offMs += now - _offSince;
    _off = off;
    _ivN = 0;                                           // a new screen-off interval starts
  }

  // Is it time for a rested reading? `quiet`: no transmission in the last few seconds and no
  // audio. Meant to be asked at a wake that is happening anyway, never to cause one.
  bool sampleDue(uint32_t now, bool quiet) const {
    if (!_off || !quiet || now - _offSince < kSettleMs) return false;
    return !_haveSample || now - _lastTry >= kSampleEveryMs;
  }

  // Record a reading (the caller averages several). `usbHost`: a computer is attached.
  void sample(uint32_t mv, bool usbHost, uint32_t now) {
    _haveSample = true;
    _lastTry = now;
    if (usbHost) { startCharging(mv); return; }
    if (_charging) {
      if ((int)mv > (int)_prev + kNoiseMv) { _prev = mv; return; }   // still rising
      _charging = false;                                // it has ended: the trend restarts at the next reading
      _prev = mv;
      return;
    }
    if (_n && (int)mv > (int)_latest + kNoiseMv) { startCharging(mv); return; }
    if (!_n) { _first = mv; _firstMs = now; }
    _latest = mv;
    _latestMs = now;
    _n++;
    if (!_ivN) { _ivFirst = mv; _ivFirstMs = now; }
    _ivLast = mv;
    _ivLastMs = now;
    _ivN++;
  }

  model::Power snapshot(uint32_t now) const {
    model::Power p;
    p = model::Power();
    p.sinceS = (now - _t0) / 1000;
    p.screenOffS = (_offMs + (_off ? now - _offSince : 0)) / 1000;
    p.battMv = _n ? _latest : 0;
    p.charging = _charging;
    uint32_t span = _latestMs - _firstMs;
    if (_n >= 2 && span >= kRateMinMs) {
      p.rateKnown = true;
      p.mvPerHour = (int16_t)(((int64_t)_latest - (int64_t)_first) * 3600000 / (int64_t)span);
    }
    return p;
  }

  // The latest screen-off interval: first and last rested reading in it (for a detail page).
  bool interval(uint16_t& startMv, uint16_t& endMv, uint32_t& spanS) const {
    if (_ivN < 1) return false;
    startMv = _ivFirst;
    endMv = _ivLast;
    spanS = (_ivLastMs - _ivFirstMs) / 1000;
    return true;
  }

private:
  struct Tag {};
  explicit PowerStats(Tag) {}

  void startCharging(uint32_t mv) { _charging = true; _prev = mv; _n = 0; _ivN = 0; }

  uint32_t _t0 = 0;
  bool _off = false;
  uint32_t _offSince = 0, _offMs = 0;
  bool _haveSample = false, _charging = false;
  uint32_t _lastTry = 0;
  uint16_t _prev = 0;
  int _n = 0;
  uint16_t _first = 0, _latest = 0;
  uint32_t _firstMs = 0, _latestMs = 0;
  int _ivN = 0;
  uint16_t _ivFirst = 0, _ivLast = 0;
  uint32_t _ivFirstMs = 0, _ivLastMs = 0;
};

}  // namespace app
