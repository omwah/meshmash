# Device bring-up checklist (Cardputer ADV + Cap LoRa-1262; T-Deck Plus in section 8)

Everything below was built and tested in the simulators. These items depend on
hardware facts that the simulators can't check. Verify them in this order on
first flash. Keep serial logs and photos of real devices **out of git**: they
can show MAC addresses, node keys and names.

## Flash

```sh
pixi run build                                  # both firmware images
pio run -e cardputer_adv -t upload              # USB-C; hold G0 while plugging in if needed
pio device monitor -e cardputer_adv             # 115200 baud
```

## 1. Boot, display, keyboard

Serial bring-up commands (Bluetooth build): `stat`, `bl <0-255>|gpio`,
`blf <hz> <duty>`, `fill <rrggbb>`, `init`, `shot` (dumps the UI frame) and
`key <row> <col>`. See `boards/cardputer_adv/diag.h`.

- [x] **Splash.** It shows "MeshCore keyboard edition" right way up, with correct colours. Check that Midnight's header is dark blue, not yellow. If colours are inverted, toggle INVON (0x21) in `st7789_display.cpp`. If the picture is mirrored or offset, check MADCTL 0x60 and the gap (40, 53). Colours look right on hardware.
- [x] **Backlight.** It needs a low PWM rate: at 5 kHz the driver stays dark, while 256 Hz works (found on hardware).
- [x] **Brightness and timeout.** Brightness follows Settings > Brightness, and the backlight turns off after the screen timeout. Any key wakes it without doing anything else. Works on hardware.
- [x] **Keyboard map.** Every key types its printed legend in a message. `; , . /` move in menus. Opt opens the menu. Holding `` ` `` goes Home. Works on hardware.
- [x] **No missed or doubled presses.** With Settings > Key filter at 60 ms, fast Up/Down presses must neither drop nor double. If they do, try other values (10–500 ms). Works on hardware at 60 ms.

## 2. Radio (the riskiest assumptions)

- [x] **Radio starts.** Serial shows no `halt: radio init failed`. Serial command `radio` shows the live settings and counters.
- [x] **RF switch enabled.** The Cap's antenna switch is powered by PI4IOE5V6408 (I2C 0x43) P0 high: registers 0x05 (output), 0x03 (direction) and 0x07 (high-Z) are set by `cardputer_board.cpp`. Without it, transmit works only at very short range. Check this with a second node a room away. Works on hardware: messages went both direct and through a one-hop repeater.
- [x] **TCXO voltage.** It is set to 3.3 V (`SX126X_DIO3_TCXO_VOLTAGE`). If the radio fails to start or is deaf, try 1.8. 3.3 V works on hardware.
- [ ] **TX current limit.** It is 140 mA. The other open Cardputer port uses 240 mA for this Cap; only raise it after measuring.
- [x] **Receive (SoCal preset).** Packets and adverts from a stock node are received.
- [x] **Transmit.** Another node hears our adverts and DMs (serial `radio`: tx count). Works on hardware.
- [x] **Interop.** Exchange adverts and a DM with a stock MeshCore node or the phone app on the same preset (default "USA", 910.525 MHz / 62.5 kHz / SF7 / CR5). Works on hardware, direct and through a one-hop repeater.

- [x] **Ping bot.** Turn on Settings > Answer "ping" DMs, then send `ping` from a phone app on another node through a repeater. The reply names the repeater, or shows its hash if it isn't a contact. Send `ping` again straight away: no second reply. Works on hardware.

## 3. SD card (shares the LoRa SPI bus)

- [x] **Backup works.** "Back up now" succeeded on hardware.
- [x] **Card detection.** Insert a FAT32 card. Backup & restore shows "SD card ready". Works on hardware (backups are written).
- [x] **No radio disruption.** "Back up now" while messages arrive must not disturb the radio. RadioLib and SD each drive their own CS (5 and 12). Works on hardware: messages arriving during backups all showed delivered.
- [ ] **History archive.** With a card in, after more than 512 messages in all, scrolling up past the oldest flash message loads older ones from `/meshmash/history/`. "Clear history" removes that thread's file. (Covered in simnet with a 40-record ring.)
- [x] **Fresh-install restore.** Erase flash (`pio run -t erase`), reflash, and accept the restore offer. Check that the same node key, contacts and channels come back. Works on hardware (a fresh Launcher install).

## 4. GPS (ATGM336H)

- [x] **Fix.** Turn GPS on. Within a few minutes outdoors the GPS screen shows satellites and a fix. The module's TX goes to GPIO15 and its RX to GPIO13, at 115200 baud. If nothing arrives, swap `PIN_GPS_TX/RX`. Works on hardware.
- [x] **Boot clock sync.** "Set clock from GPS at boot" sets the clock and then turns the receiver off again, when GPS itself is off. Works on hardware, and the GPS menu shows "off" while GPS is off.

## 5. Bluetooth (default build)

The items below passed with stock's Bluedroid stack. Bluetooth now runs on
NimBLE (`boards/common/esp32/nimble_serial.*`), so they need checking again:

- [x] **NimBLE: pairing.** On the phone, forget the old pairing first. The app finds the device, the PIN shows on screen while pairing, and pairing with it succeeds. A wrong PIN is refused and the device advertises again. Works on hardware.
- [x] **NimBLE: reconnect.** After a reboot of either side, the phone reconnects without asking for the PIN again. Works on hardware.
- [x] **NimBLE: phone app.** Contacts and channels sync, and messages go both ways, including long ones (the app's whole frame in one notify). Works on hardware.
- [x] **NimBLE: toggle.** Settings > Bluetooth off disconnects the phone and stops advertising; on, it advertises again. Works on hardware.
- [x] **NimBLE: heap.** Diagnostics > System shows more free heap than the Bluedroid build, with a phone connected. Works on hardware: 143 KB free with a phone connected.

Earlier, with Bluedroid:
- [x] **Pairing and key import.** Pairing works, and so does the app's private key import (after enabling ENABLE_PRIVATE_KEY_IMPORT/EXPORT as stock does).
- [x] **Phone app.** The MeshCore phone app pairs using the PIN from Settings. Messages sent from the phone appear in the device's history too, and the reverse. Works on hardware.
- [x] **Bluetooth toggle.** Settings > Bluetooth off stops advertising. Works on hardware.

## 5b. Remote node mode (Bluetooth client)

Needs a second MeshCore node with stock **companion** firmware over BLE (a
Heltec V3, T-Echo or similar), on the same radio preset as the mesh.

- [x] **Switch.** Settings > Mesh node > remote: the device restarts on the Remote node screen (no node chosen yet). The LoRa Cap may be attached or not. Works on hardware.
- [x] **Find.** Find nodes nearby lists the companion by its Bluetooth name (MeshCore-<name>) with a signal level. Works on hardware.
- [x] **Pair.** Choosing it asks for the PIN (123456 prefilled); the right PIN connects ("Connected", node name, battery), a wrong one says so and stops. Works on hardware (after two fixes: the scan list was cleared under the PIN prompt, and the address was used byte-reversed).
- [x] **Sync.** Home fills with the node's contacts and channels; messages that waited on the node arrive. Works on hardware.
- [x] **Messages.** A DM out shows delivered; a channel post shows "heard N repeats" when a repeater is near; incoming DMs and channel posts appear and chime. Works on hardware.
- [x] **Reconnect.** Restart the node (Remote node > Restart the node), then the Cardputer: both times the link comes back by itself without the PIN. Works on hardware.
- [x] **Admin.** Log in to a repeater or room through the node; status and neighbours come back. Works on hardware.
- [x] **Settings.** Rename the node, change a radio setting: the node keeps them (check with the phone app or after a node restart). Works on hardware.
- [x] **Backup.** Back up now with an SD card in; Restore puts it back (try after changing a channel). Works on hardware.
- [x] **Battery, heap.** Header shows the Cardputer's battery; Diagnostics shows the node's battery, and free heap stays healthy. Works on hardware.
- [x] **Back to local.** Settings > Mesh node > this device: the own node, its contacts and history are as before. Works on hardware.

## 6. Sound

- [x] **Chime.** Works on hardware. Serial `beep` (or a new message with Settings > Sound on) plays two rising notes. Is the volume and tone OK? The codec is ES8311 at 0x18; I2S pins 41/43/42, as in M5Unified.
- [x] **No chime when muted.** No chime for messages in muted channels, or with Sound off. Works on hardware for both.

- [x] **New-message LED.** Settings > New message: Enter plays the colour burst (red to violet, about 2 s). With "LED only", let the screen time out and send a DM from another node: the LED flashes. Also try "screen on", "stay dark" and "screen + LED". Works on hardware. Findings: the LED (WS2812, GPIO21) runs on the backlight's power, which the backlight PWM chops, and the LED forgets its colour at each drop. So the burst holds that power fully on: the screen glows during a flash with the screen off, and jumps to full brightness for it with the screen on. Showing black on the panel instead of sleeping glowed more. Brightness 128/255 (32 was too dim).
- [x] **Chat preview.** On the Chats tab, the selected chat's long last message scrolls through to its end (Settings > Chat preview: whole message). Works on hardware.
- [x] **Show in Chats.** Hiding a DM, then a message from that contact brings it back with an alert; a hidden muted channel stays off while its dimmed count rises on the Channels tab. Works on hardware.

## 7. Power and memory

- [x] **Battery drain.** With the idle sleep in the main loop: note the voltage on Diagnostics > System, run an hour on battery with the screen timing out, and note it again. Compare with an older build if possible. On hardware: 94% to 84% in an hour with Bluetooth connected, so roughly 9 to 10 hours per charge. Percentages only; no voltage or older-build comparison yet.
- [ ] **Idle power (T-Deck).** The backlight dims for the last 8 s before the screen times out (any key or touch brings it back); with the screen off the CPU drops to 160 MHz and the keys and touch panel are polled every 50 / 80 ms (`-D MC_IDLE_CPU_MHZ=0` keeps the clock). Check that a key or touch still wakes the screen at once, that DMs and channel messages keep arriving with the screen off and Bluetooth connected, and compare the hourly drain with an older build. Not measured.
- [ ] **Lower CPU clock.** Build with `-D ESP32_CPU_FREQ=80` (or 160): check that Bluetooth with the phone, radio receive and the UI still work, then repeat the drain test.

- [x] **Battery reading.** GPIO10 through a 2:1 divider, measured as esp-vim and M5Unified do. On USB with the power switch off it reads 4.14 V, the charger's output.
- [x] **Charging.** Pure hardware; the firmware has nothing to enable. A computer's USB port charged it little or not at all (3.5 V after hours); a standalone USB charger works. Use a wall charger. The power switch only disconnects the battery: with USB in, the firmware runs with the switch off too, and the battery still charges, slowly.
- [x] **Heap.** After an hour with Bluetooth connected, the heap minimum on Diagnostics > System stays above about 30 KB. On hardware: free heap went from 148 KB to 140 KB over the hour (the minimum wasn't read).
- [x] **Launcher.** If you use bmorcelli's Launcher: the app runs from it (installed over its WebUI), and on first start the restore offer appears and brings back the identity, contacts and settings. Works on hardware.

## 8. LilyGo T-Deck Plus

Install with `pixi run launcher-install IP --variant tdeck --replace` (or `pio run -e tdeck -t
upload`; if a flash won't take, hold the trackball down while pressing reset).
The serial status line at boot reports the keyboard, touch, SD and PSRAM.

- [ ] **Boots.** Splash, then Home; the status line says keyboard ok, touch ok and shows free PSRAM. GPIO10 powers the keyboard, touch, display and radio, and is raised 200 ms before anything is probed.
- [ ] **Display.** Colours right (inversion on), not mirrored, whole 320x240 used. Brightness in Settings steps visibly (16 pulse-counted steps on GPIO42), and the screen timeout turns it off and back on.
- [ ] **Keyboard.** Letters, Shift, Sym digits and symbols type as printed; Alt is Esc (hold for Home), Sym+Enter the menu, Sym+Space Tab, Mic Ctrl. If the status line says "chars only", the C3 has old firmware (before June 2025): flash LilyGo's T-Keyboard_Keyboard_ESP32C3_250620.bin to it.
- [ ] **Key light.** It comes on while typing and goes off about 10 s after.
- [ ] **New-message light.** Settings > New message: "LED only" with the screen timed out: a message blinks the key backlight (7 blinks, 280 ms). Settings > LED brightness changes how bright and flashes it as a preview (the brightness setting works on hardware; the blink with the screen timed out is not yet confirmed); "stay dark" does nothing.
- [x] **Hold to lock.** Hold the trackball click for about 0.7 s while unlocked: the device locks and the screen goes dark, with no Enter sent (try on a chat row). A short click still opens it. Then a click unlocks. Works on hardware.
- [x] **Bluetooth PIN.** Settings > Bluetooth PIN (while "each boot"): confirm, enter 6 digits; the row shows the new PIN with "(restart)". Restart, forget the old pairing on the phone, pair with the new PIN. Works on hardware.
- [ ] **Vim keys.** On by default: j/k/gg/G, Ctrl-d/u, /, n, gt, ]u, ZZ work as in the README.
- [ ] **Trackball.** Rolls move one row per notch-ish (2 toggles per step), the click is Enter. Settings > Trackball off: rolls ignored, the click still works.
- [ ] **Touch alignment.** Tap each corner's control (header back arrow top left, Menu bottom right, a row near each edge): the right thing responds. If taps land mirrored, note the GT911's Module_Switch1 (panels differ between batches; the driver follows it from 0xB5).
- [ ] **Touch gestures.** Shortcut bar from deep screens; tap to open, hold for details; drag a long list and a conversation (older messages load at the top); sideways swipe on Home; ‹ value › taps in Settings; Confirm's Yes/No.
- [ ] **Shared SPI.** Scroll and drag continuously while another node sends DMs and channel posts: every message arrives, nothing glitches on screen. Then the same with an SD card in (back up now during traffic).
- [ ] **Radio.** Adverts go out and are heard; DMs show delivered (SX1262, TCXO 1.8 V, no DIO2 RF switch, as MeshCore's own T-Deck build).
- [ ] **GPS (Plus).** Fix, clock sync, position in adverts. If nothing arrives at 38400 (MeshCore's rate), the module may run at 9600: change GPS_BAUD_RATE and note which.
- [ ] **Bluetooth.** Phone app pairs and syncs; remote node mode connects to another node.
- [ ] **Sound.** A new message chimes through the speaker (I2S 7/5/6); not with Sound off.
- [ ] **Battery.** Diagnostics shows a sensible voltage (GPIO4, 2:1). An hour on battery with the screen timing out: note the drop.
- [ ] **Memory.** Diagnostics > System free heap after an hour with Bluetooth connected (internal RAM; the node and the UI frame are in PSRAM).
- [ ] **Launcher.** The app fits Launcher's partition on the T-Deck and keeps its files (SPIFFS) across restarts.
