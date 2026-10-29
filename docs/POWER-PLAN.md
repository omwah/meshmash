# Battery and sleep implementation plan

Status: Draft for review. No power-management implementation changes yet.

## Goals and constraints

- Reduce locked/screen-off battery use substantially beyond lowering CPU frequency.
- Keep local MeshCore reception and required protocol processing working while locked.
- Require explicit user action to unlock input; preserve configured notification screen wakes without unlocking.
- Offer an active-UI CPU-speed setting so users can trade responsiveness for battery life.
- Limit screen-off background activity to required mesh work and explicitly enabled services.
- Validate through software counters, a battery-voltage trend on a Power Diagnostics page, and functional tests. Counters demonstrate reduced activity and sleep behaviour; the voltage trend is a rough real-world drain indicator, not measured current or guaranteed battery life.

## Key distinction: CPU wake, screen wake, input unlock

These are separate events. The CPU may wake briefly for radio interrupts and necessary timers without lighting the screen. A notification configured to light the screen may wake the display while input stays locked; "stay dark" never lights it. Only explicit user action unlocks input.

A CPU that wakes only on user input cannot process mesh reception, acknowledgements, retries, and scheduled work, so deep sleep is not the target for the listening mode. An offline sleep mode would be a separate feature with explicit loss-of-reception semantics.

## Framework constraints

Pinned: `platformio/espressif32@6.11.0` → arduino-esp32 2.0.17 (IDF 4.4.7), NimBLE-Arduino 1.4.3. The precompiled ESP32-S3 sdkconfig has:

```text
# CONFIG_PM_ENABLE is not set
# CONFIG_BT_CTRL_MODEM_SLEEP is not set
```

Consequences:

- **No automatic light sleep or tickless idle** without rebuilding the Arduino libraries. Explicit `esp_light_sleep_start()` (as used by MeshCore's `ESP32Board::sleep()`) is the only light-sleep path.
- **No light sleep while the BT controller is running.** Without controller modem sleep, light sleep cannot keep advertising or a connection alive. With BLE on, savings come only from clock policy and reduced background work (Phases 3–4).
- **"Bluetooth off" must stop the controller.** Today `disableBluetooth()` stops advertising and the service but leaves the controller up (nothing in `boards/common/esp32/nimble_*.cpp` deinitializes it). Phase 5 depends on fixing this.
- **USB:** every build uses USB Serial/JTAG (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`), and light sleep drops that link. Do not sleep while `HWCDC::isConnected()` reports a host; report it as a blocker. A charger alone does not block sleep. Verify that `isConnected()` goes false promptly on unplug and on a charger, since it is based on USB frame activity.
- **Audio:** I2S playback blocks sleep until it finishes.
- **GPS UART:** receiver output is lost during light sleep. Acceptable under the Phase 4 GPS policy; an active GPS request blocks sleep.

A custom framework build enabling PM and BT modem sleep is out of scope. Revisit only if BLE-on battery life proves to be the dominant complaint after this plan ships.

ESP32-S3 reference (closest published version to the pinned 4.4.7): https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/system/power_management.html

## Current implementation findings

Recheck during implementation:

- Each board has its own Arduino `loop()`: `boards/tdeck/main.cpp`, `boards/cardputer_adv/main.cpp`. In local mode the loop runs MeshCore, interface/sensor/adapter processing, and UI; remote mode runs the BLE remote adapter instead. The loop keeps running with the display off, with idle `delay()` capped at 10 ms. `delay()` already lets the idle task halt the CPU, so the remaining cost is the wake every 10 ms and the full CPU clock, not busy-waiting.
- Display/backlight sleep exists; rendering and marquee frames are largely suppressed while off.
- T-Deck lowers its screen-off clock to 160 MHz (`boards/tdeck/tdeck_idle.h`) and slows key/touch polling. 160 MHz was chosen so advert verification finishes before the radio's single packet buffer is overwritten. Cardputer has no equivalent.
- T-Deck unlocks with a trackball click (GPIO0). Cardputer wakes on any key without locking; this plan adds G0 (also GPIO0) unlocking.
- Notifications and pairing can light a locked screen.
- Adapter work includes GPS, retries, synchronization, storage, and automatic backups, including while the screen is off.
- Remote mode sleeps the local radio, but BLE transport and synchronization keep running.
- T-Deck's shared peripheral power rail also powers the radio: do not switch it off while listening.
- UI CPU frequency is not a persistent setting.

Relevant locations:

- `boards/tdeck/main.cpp`, `boards/cardputer_adv/main.cpp`, `boards/tdeck/tdeck_idle.h`, board input/display drivers
- `src/hal/idle.h`, `src/ui/app.cpp`
- `src/app/mesh_adapter.cpp`, `src/app/adapter_base.{h,cpp}`, `src/app/remote/remote_adapter.cpp`
- `boards/common/esp32/nimble_serial.{h,cpp}`, `boards/common/esp32/nimble_central.{h,cpp}`
- `src/model/model.h`, `src/ui/screens/menu.cpp`, `src/ui/screens/diag.cpp`
- `external/MeshCore/src/helpers/ESP32Board.{h,cpp}`

## What to learn from nRF MeshCore

The nRF52 companion calls `board.sleep(0)` when idle, using SoftDevice event waiting / WFE: interrupt-driven CPU idle with the radio still receiving. On ESP32-S3 the equivalent deeper step is explicit light sleep with DIO1 GPIO wake, which `ESP32Board::sleep()` already implements. nRF results do not transfer to ESP32 handhelds with different display, input, regulator, and BLE behaviour.

Upstream context (design references only, not merge status or performance on our boards):

- https://github.com/meshcore-dev/MeshCore/pull/1238 — interrupt-driven nRF repeater idle.
- https://github.com/meshcore-dev/MeshCore/pull/1122 — DC/DC, LED, and shutdown improvements. Its ~15 µA figure is powered-off, not listening.

## Power states

| State | Behaviour |
| --- | --- |
| Active UI | Normal interaction at the selected UI CPU speed |
| Locked, BLE off | Display off; light sleep between radio events, unlock, and deadlines |
| Locked, BLE on | Display off; reduced clock and background work; no light sleep |
| Locked, remote node | Display off; local radio asleep; BLE link and message delivery preserved; no light sleep |
| Locked, notification lit | Display on for the notification interval; input locked; then back to the applicable locked state |

Keep display/lock state, CPU frequency policy, and enabled services separate. Screen-off must not silently disable services the user enabled.

## Phase 1: minimal Power Diagnostics

Add **Power** as a logical page after the last existing Diagnostics group (after Storage on T-Deck, after System on Cardputer), using the existing Diagnostics entry, navigation, and text-size-aware pagination. Move the T-Deck `cpu <MHz> MHz idle clock <duration>` line from Storage to Power.

Summary (illustrative values):

```text
Diagnostics: Power
Since reset 1h 00m
CPU         160 MHz
Screen off  58m 12s
Sleep calls 54m 08s  93%
Wakes 126   Block: BLE on
Battery     3912 mV  -18 mV/h
```

- `Since reset`: local elapsed time since counters were reset (or boot).
- `Sleep calls`: time inside successful light-sleep calls while the display was off; percentage of screen-off time. `n/a` until Phase 5, `—` for a zero denominator.
- `Wakes`: sleep returns. `Block:` shows the most recent reason sleep was skipped (BLE on, USB, audio, GPS request, pending work, inhibited).
- `Battery`: latest rested reading and drain rate over the interval (see below).
- Unsupported values show `n/a`, not zero.
- In remote mode all fields describe the handheld; never substitute remote-node values. Supply them through a shared local platform power snapshot in both adapters (`src/app/platform.h`, `boards/common/esp32/esp32_platform.{h,cpp}`).
- One action: Enter/trackball-click → `Reset counters?` confirm. Resets only Power counters; RAM-only, also reset on reboot.
- Refresh only while the page is visible. No serial logging, flash writes, or sampling timer.

### Battery voltage trend

The only measured quantity available; uses the existing `batteryMv()` platform reading.

- **Rested samples only.** Take a sample with the display off, no radio TX in the last few seconds, and no audio playing, averaging several ADC reads. Screen-on and TX readings sag under load and are not used for the trend.
- **Cadence.** There is no periodic battery sampling today (`battMv` is read only when diagnostics/status are built). Sample on a wake that is already happening (radio, deadline, or the 1 s sleep cap) when at least 5 minutes have passed since the last rested sample; never add a wake just to sample. Keep the first rested sample after reset, the latest, and their timestamps, plus the first and last rested samples of the most recent screen-off interval. RAM only.
- **Rate.** `(first − latest) / elapsed` in mV/h. Show `—` until at least 60 min and 2 samples have elapsed, since ADC noise of several mV makes shorter rates meaningless.
- **Charging.** If a USB host is attached or the voltage rose by more than the noise threshold, show `charging` and restart the trend from the next rested sample after it ends.
- **Interpretation.** Li-ion voltage is flat across much of the charge range, so mV/h depends on state of charge. Compare runs that start at similar voltages and under the same settings; do not convert mV/h into hours remaining or mA.
- Detail page (later): last screen-off interval's start/end mV and rate, alongside its sleep-call percentage, so a single locked stretch can be compared directly.

Deferred until sleep exists and the summary proves insufficient: low-clock time (time at the reduced screen-off clock, labelled as **not** idle or sleep), per-frequency residency, wake counts by cause, blocker counts by reason, GPS/BLE active time, background-task and retry counts, frozen/live snapshots, and a "last screen-off interval" view.

Tests: simulator layout for both boards and all text sizes; Storage no longer shows the CPU line; reset confirmation.

## Phase 2: separate screen wake from input unlock

- Centralize wake policy distinguishing CPU wake, display wake, and input unlock.
- Incoming messages always update model state and unread counts. Notification illumination never unlocks input.
- Preserve existing notification preferences: screen-light actions may wake the display; "stay dark" must not, including through Cardputer's shared LED/backlight behaviour. Preserve configurable LED and sound alerts.
- When a notification lights a dark screen, keep it lit for a short notification interval (initially a fixed 6 s, restarted by each newer eligible message; a reading-time rule and a dedicated preview screen are in `docs/NOTIFY-PREVIEW-PLAN.md`), then return to the dark, locked state without user interaction. This applies even when the normal timeout is "never" and the device was manually locked. If the screen was already on under always-on operation, add no timer. No pre-timeout dimming during the interval.
- Render only what the notification needs; it is not an interactive session.
- A user unlock returns control to the normal screen-timeout policy; notification timers must not turn off a screen the user is now using.
- Keep T-Deck trackball-click unlock and pocket-input filtering, including while a notification lights the screen.
- **Pairing:** keep its display wake without unlocking input. Keep the display on while pairing is actually in progress (tied to the pairing lifecycle, not a visible PIN or an established connection). When it completes, fails, or is cancelled, restart the inactivity timeout; if the normal timeout is "never" and the device was manually locked, use 60 s.

Acceptance: each notification action behaves as configured; lit locked screens reject pocket input; timeout returns to the dark state; unlock stays responsive.

### Cardputer lock and unlock

Interaction:

- **Unlock:** press G0 while locked.
- **Manual lock:** hold G0 while unlocked, mirroring T-Deck's hold-to-lock gesture and threshold. No menu action on either board.
- A short G0 press while active does nothing. If the display is off but unlocked, G0 wakes it and is consumed until release.
- **`Lock when screen off`** preference (Cardputer only; T-Deck keeps always locking on timeout). On: screen timeout locks input. Off: keep today's any-key wake, consuming the first key-down. Default on for new installs; existing installs keep any-key wake through preference migration. Manual lock always needs G0.
- While locked, keys neither unlock nor light the display. Active-UI shortcuts are unchanged.
- On unlock, restore the previous screen and UI CPU speed; consume the G0 press.
- Holding G0 at power-on still enters download mode.
- Lock is a pocket lock, not authentication.

Button and key state:

- Debounce G0; one action per press/release. A lock hold must be released before the next press unlocks, and an unlock press must be released before a hold can lock again. If G0 is held when lock is entered another way, require release and a fresh press.
- On lock, discard queued keyboard actions. On unlock, drain the TCA8418 FIFO (including overflow) and resynchronize modifiers so nothing typed while locked replays.

Wake integration (with Phase 5):

- Read G0 through the board input layer; no M5Cardputer dependency (https://docs.m5stack.com/en/arduino/m5cardputer/button).
- GPIO0 is the unlock input on both boards, so one shared low-level GPIO wake path serves both. Verify polarity and pull configuration per board.
- Do not enable G0 wake while G0 is held (avoids an immediate-wake loop); wait for release first.
- While locked, suspend keyboard scanning and do not enable keyboard wake.

Acceptance:

- Auto-lock on: timeout locks; no key or chord unlocks or lights the screen; a fresh G0 press unlocks exactly once, also from a notification-lit screen.
- Hold-to-lock, menu lock, debounce, and release gating; holding either press never toggles twice.
- No leaked text, modifiers, or actions after unlock, including FIFO overflow.
- Auto-lock off: any-key wake with first-key consumption; manual lock still needs G0.
- Preference migration and new-install default.
- Extend `test/scenarios/cardputer-sleep.sim` and `test/scenarios/tdeck-lock.sim`.
- Normal boot and G0 download mode unchanged.

## Phase 3: active-UI CPU setting

Persistent UI preference (not a MeshCore radio setting), applied **only while the screen is on and unlocked**:

- Fast: current board speed (default for new and existing installs).
- Balanced: 160 MHz.
- Battery saver: 80 MHz.

Screen-off and locked operation never go below 160 MHz, so advert verification keeps its timing margin. Cardputer gains the same 160 MHz screen-off clock T-Deck already uses.

- Expose only speeds validated per board and build variant.
- One board power-policy interface owns clock changes; UI settings and idle hooks do not set the clock independently.
- Preserve old preference-file compatibility.
- Validate display, BLE, GPS, USB, storage, and incoming-message handling while the UI is open at each speed. If 80 MHz drops packets during active use, raise the clock while processing radio work or drop Battery saver.

Lower frequency is a user tradeoff, not a guaranteed saving.

## Phase 4: reduce screen-off work

| Activity | Locked policy |
| --- | --- |
| Radio RX/TX, ACKs, retries, protocol deadlines | Preserve |
| UI animation, rendering | Suspend; minimal rendering during notification illumination |
| Key/touch scans | Suspend while locked except the unlock input |
| Battery and diagnostic sampling | Reduce; refresh on unlock |
| GPS | Only for enabled location reporting or an outstanding explicit request |
| Preference/contact/message persistence | Unchanged; combine redundant writes only with no added data-loss risk |
| Automatic backups | Unchanged; they are infrequent and run as now while locked |
| Remote status polling | Reduce cosmetic polls; preserve message delivery and connection work |
| LED and sound | Follow notification policy, including "stay dark" |

GPS:

- GPS enabled is permission to use it, not a reason to poll. Poll only for enabled location reporting (at its cadence) or an outstanding explicit request (GPS screen refresh, GPS time update). Bound each request with a timeout; locking never creates or restarts one.
- Treat boot GPS time acquisition as a bounded one-shot.
- Opening unrelated UI creates no GPS demand; a GPS screen shows cached data with age.
- Stopping polling does not power down the receiver; use supported receiver idle controls where available and keep unused UART traffic from causing wakeups or unbounded buffering.

Build an inventory of every timer: owner, next deadline, and whether it is essential while locked. Adapter work is not optional just because it is outside MeshCore.

### Timer inventory (as of Phase 4)

| Timer | Owner | Period | Screen off |
| --- | --- | --- | --- |
| Main loop | `loop()` in each board | at most 10 ms, none while MeshCore has work | Unchanged until Phase 5 (light sleep) |
| MeshCore retries, ACK timeouts, CAD, advert schedule | MeshCore | its own | Preserved |
| Pending-send retry, request deadline | `AdapterBase`/adapters | per message | Preserved |
| MeshCore prefs sync | `MeshAdapter::loop` | 1 s | Unchanged (a memcmp) |
| Contact-count check, ping reply | `MeshAdapter` | every loop / 2 s one-shot | Preserved |
| Auto backup, config save | `storageTick` | 10 s after a change, at most 1/min | Unchanged |
| Boot GPS clock sync | `gpsTick` | one-shot, 10 min cap | Preserved |
| GPS NMEA read | `sensors.loop()` | every loop | Skipped unless boot sync or position sharing (`gpsWanted()`); the UART buffer drops what it cannot hold |
| Remote message poll | `RemoteAdapter` | 15 s | Preserved (delivery) |
| Remote battery/stats poll | `RemoteAdapter` | 30 s | 5 min |
| Remote contact refresh | `RemoteAdapter` | 60 s | 5 min |
| Remote contact cache save | `RemoteAdapter` | 10 s after a change, at most 2 min | Unchanged |
| Remote position push | `RemoteAdapter::positionTick` | 30 s check | Unchanged (it is location reporting) |
| UI render, animation, header clock | `App::loop` | on change, 40 ms animating | Suspended while asleep |
| Key scan | keyboard drivers | 20 ms | T-Deck 50 ms; Cardputer suspended, G0 still read |
| Battery/diagnostic sample | `PowerStats` | 5 min | Unchanged |
| CPU clock | `CpuClock` | on screen state/setting change | 160 MHz |

GPS requests from the GPS screen need no code: the screen is only reachable with the screen on, when the receiver is read as before.
Telemetry location answers to other nodes use the last position the receiver gave; with the screen off and position sharing off that is the position from when the screen went dark.

Acceptance: fewer wakeups and less background work while locked, with no regression in enabled services or persistence.

## Phase 5: light sleep with BLE off

### 5a: Bluetooth off means controller off

- Make the `Bluetooth` off setting stop and deinitialize the NimBLE stack and controller at runtime, and turning it on reinitialize them, with no reboot. Verify heap is recovered (free and largest block) and that many repeated toggles, including while a phone is connected, stay stable. If runtime reinit proves unreliable, fall back to applying the change on restart.
- Remote mode always needs BLE and is excluded from light sleep.
- **Result (on the device, 2026-10-29): with the NimBLE stack stopped and started inside a running session, Android pairing broke, and an older pairing record then failed after reboots too.** A reboot is fine because RAM starts clean. Cause, read from the NimBLE-Arduino 1.4.3 source (`ble_hs_pvcy.c:27-28,220`, `ble_hs_startup.c:331,381`) and confirmed against the probes on the device: the host keeps our IRK in a static that `deinit()` does not clear. Every sync resets the controller, which empties its resolving list and turns address resolution off, then calls `ble_hs_pvcy_set_our_irk(NULL)`; that call does nothing when the IRK is unchanged, which after a restart it is. So the restarted stack never resolved the phone's private address to its bond (`peer_id_addr` stayed a random address, `isBonded()` false, `Bonds` still 1), and the phone pairs again over an unresolved link. NimBLE then overwrites the old record in RAM but its NVS writer only writes when the counts differ (`ble_store_nvs.c:537-566`), so the old keys stay in flash and a reboot loads them (`Last fail enc 0`). Upstream esp-nimble 1.5 removed the guard. The fix in `build()` after a restart: set a different IRK, then the default (clears the list, turns resolution on, adds our entry), then `ble_hs_misc_restore_irks()` to put the paired phones back; test on the device with the protocol below. Confirmed on the T-Deck (build 1.3.0+21.9520078, Android phone, 2026-10-29): after a Bluetooth off and on, and after reboots, the phone reconnects without a PIN prompt (4 connections, all `B+`), and the stored pairing record is unchanged across the toggle. Not yet tried on the Cardputer, which shares the code. The Bluetooth page and its probes stay until light sleep is done.

#### Bluetooth switch on the main menu

Light sleep depends on Bluetooth being off, so turning it off and on must be quick. Move the switch out of Settings to the main menu:

- Add a main-menu heading `GPS & BLUETOOTH` between `MESH` and `DIAGNOSTICS`, holding:
  - `GPS`: moved from `DIAGNOSTICS`; still opens the GPS page and shows off/searching/fix.
  - `Bluetooth`: Enter or Left/Right toggles it; value shows off/on/connected. Same preference and apply path as today. Disabled and skipped by the cursor on USB-only builds (`n/a`) and in remote node mode (`in use`, since the link is the node connection; switching mode stays in Settings > Mesh node).
- On the GPS page (`GpsScreen` in `src/ui/screens/diag.cpp`), let the receiver be switched on and off: Left/Right toggles it, using the same preference and `setGpsPower()` path as the Settings row. Enter (set clock from GPS) and Tab (Settings) keep their meanings. Add a `<>` `on/off` hint and check it still fits the T-Deck shortcut bar and the Cardputer hint line. Show the result at once in the page's status line (`Receiver off` / `Searching...` / `3D fix`).
- Remove `Bluetooth (phone app)` and `GPS receiver` from Settings. The Bluetooth PIN rows and the other GPS options (share position, clock at boot) stay in Settings; share position stays disabled while the receiver is off.
- Keep the Settings heading `MESH & BLUETOOTH`, since the PIN rows remain under it.
- Turning Bluetooth off while a phone is connected asks `Disconnect the phone?` first; otherwise one press, no prompt.
- Update `src/ui/screens/menu.cpp`, the README feature list, and the cursor counts in `test/scenarios/06-menu.sim`, `07-settings.sim`, and `tdeck-navbar.sim` (Diagnostics, Packet log, Settings, and Backup each move two rows down; GPS moves two rows up; Settings rows after ping bot shift up one, and those after the GPS heading up one more). Add scenario shots of the Bluetooth toggle and of the GPS page switching the receiver off and on.

This can ship ahead of the rest of Phase 5, since it is a UI change only.

### 5b: integrate MeshCore's `ESP32Board::sleep()`

- Reuse the vendored helper (light sleep with DIO1 high-level wake, optional timer wake, pending-IRQ check, `inhibit_sleep`). The vendored ESP32 companion loop does not call it, so MeshMash's board loops need explicit integration. Extend it through the existing dependency patch workflow rather than writing a parallel implementation.
- Extensions needed: GPIO0 unlock wake (low level, only when released), and a wake duration in milliseconds or a deadline rather than whole seconds.
- Check after wake that the DIO1 interrupt type the helper restores (`GPIO_INTR_POSEDGE`) matches what RadioLib expects.
- Sleep only when all hold: display off, BLE controller off, no USB connection (USB builds), no audio playing, no active GPS request, no pending mesh/adapter work. Report the first failing condition as the `Block:` reason.
- Keep SX1262 RX active while the CPU sleeps.
- T-Deck first, then Cardputer.

**5b status (2026-10-29, T-Deck only, not yet tried on the device):** `src/app/sleep_gate.h` decides, `boards/common/esp32/light_sleep.h` sleeps (same steps as `ESP32Board::sleep()` with the length in ms and a GPIO0 low-level wake, enabled only while the click is released; kept in our tree instead of a dependency patch). The T-Deck loop sleeps at most 1 s per pass when: screen off and locked (unlocked would need a key wake, not wired), Bluetooth off (and not remote mode), no USB host, no audio, no GPS wanted, no pending MeshCore work, and the click released. Why a pass did not sleep shows as `Block:` on the Power page, with `Sleep calls` and `Wakes`. `App::loop` now returns 1000 ms when the screen is off and nothing is due. The Cardputer is not wired yet (G0 is the same GPIO0 low wake; its key scan is already suspended while locked).

### 5c: deadlines

`hasPendingWork()` is not a deadline API. Add an explicit next-deadline query covering delayed sends, TX completion/timeouts, retries, advertisements, CAD/noise/AGC work, adapter timers from the Phase 4 inventory, and deferred writes. Until every source exposes a deadline, cap each sleep at 1 s (still about 100 times fewer wakes than 10 ms polling); raise the cap only for sources covered by the deadline query.

Replacing the 10 ms `delay()` with an event-driven wait gives little benefit without light sleep, so do it only where the deadline work requires it.

### Acceptance

- Sustained locked idle with BLE off spends most screen-off time in sleep calls.
- Incoming packets are processed without unlocking; the screen lights only if the notification policy says so.
- Unlock is responsive, including near-simultaneous G0 and packet arrival, G0 held across sleep entry, timer wake, and repeated lock/unlock cycles.
- No missed-wakeup hangs, immediate-wake loops, or delivery regressions. Record ambiguous GPIO wake causes as ambiguous rather than guessing the pin.
- Watchdogs, timekeeping (`millis()` across sleep), and peripheral state after wake are correct.

## Phase 6: BLE-on and remote mode

No light sleep while the BLE controller runs (see Framework constraints). Within that limit:

- Reduce remote-mode status polling and cosmetic sync while locked (Phase 4).
- Investigate lengthening NimBLE advertising interval and connection parameters while locked. Measure phone discovery, reconnect, and delivery latency in companion and remote-node modes. Ship only as an opt-in setting if any of those get slower; never by default.
- Do not disconnect BLE or rely on reconnect-and-sync to save power.
- Power Diagnostics shows `Block: BLE on` so users can see why sleep isn't happening.

## Validation

### Automated

Extend `test/scenarios/tdeck-lock.sim` and `test/scenarios/cardputer-sleep.sim`: notification wake versus unlock, notification timeout, "stay dark", preference migration, frequency-policy transitions, sleep-gate conditions, deadline selection, and suspended background work. Simulations validate policy, not hardware sleep or current.

### On device

For each board and build variant:

- Receive numbered messages while locked (BLE off and BLE on); compare delivery and retries with unchanged firmware.
- Outbound/retry work crossing the lock transition.
- Unlock responsiveness and pocket-input filtering.
- GPS, BLE toggling, remote mode, USB connected, and persistence across lock/unlock.
- No hangs, reconnect loops, resets/brownouts, or lost data.

### Comparison

Record firmware revision, device, settings, enabled services, and starting battery voltage. Charge fully (or to a recorded starting voltage), reset Power counters, lock for a fixed interval of at least several hours, unlock, and read the summary. Compare equivalent intervals before and after each change: quiet listening, controlled incoming messages, notification illumination, and BLE/GPS on. Counters are evidence of execution behaviour; the mV/h trend is the rough drain comparison, valid only between runs with similar starting voltage.

Record results in `docs/DEVICE-TESTING.md`.

## Delivery order

1. Minimal Power Diagnostics page; Bluetooth switch on the main menu.
2. Separate notification display wake from input unlock; Cardputer G0 lock.
3. UI CPU-speed preference; Cardputer screen-off clock.
4. Timer inventory and reduced screen-off work.
5. Bluetooth off stops the controller; light sleep with BLE off, T-Deck then Cardputer; deadline API.
6. BLE-on and remote-mode tuning within the no-sleep limit.
7. Expand Power Diagnostics detail pages if comparisons need them.
8. Convert the plans into documentation of what was built (see below).

Success requires preserved mesh and notification behaviour, reliable unlock, reduced unnecessary execution, and real sleep with BLE off. Do not infer battery-life numbers from software counters or convert the voltage trend into hours or current.

## Final step: replace the plans with as-implemented documentation

When implementation is complete, remove `docs/POWER-PLAN.md` and `docs/NOTIFY-PREVIEW-PLAN.md` and replace them with documentation of what was actually built, written like the other documents in `docs/` (`unicode.md`, `FLASHING.md`): present-tense descriptions of behaviour and how it works, with source locations, not phases, options, or rationale for paths not taken.

- `docs/power.md`: power states, locking and unlock on each board, notification wake, the CPU-speed setting, what runs while locked, when the device sleeps and what blocks it, the Bluetooth and GPS switches, and how to read Power Diagnostics (including the battery trend and its limits).
- Fold the locked notification preview into `docs/power.md`, or give it its own short document if it grows large.
- Describe the shipped behaviour, including any limits found on hardware (for example BLE-on configurations that never sleep); drop anything planned but not built.
- Move on-device findings and support matrices to `docs/DEVICE-TESTING.md`; update `README.md` features to match.
- The plans stay in git history; nothing links to them afterwards.

