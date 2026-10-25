# MeshMash

**Stop tapping, start mashing.**

MeshMash is a keyboard-native user interface for
[MeshCore](https://github.com/meshcore-dev/MeshCore) LoRa mesh radios: you
mash keys, not a phone screen. It runs on the M5Stack Cardputer ADV with the
Cap LoRa-1262 module, and on the LilyGo T-Deck Plus. The UI is kept separate from the
hardware, so other devices can be added as new board directories.

- **Stock MeshCore inside.** MeshCore is a git submodule pinned to a release.
  Changes we need live as small patches in `patches/meshcore/`, so moving to a
  new MeshCore release means bumping the submodule and refreshing the patches.
- **Small.** The UI draws into a 16 KB 4-bit framebuffer with Spleen bitmap
  fonts (about 15 KB of flash). Only rows that changed are sent to the panel.
  Nothing on the UI side uses the heap.
- **Unicode and emoji.** Received text beyond Latin, Cyrillic and Greek names
  falls back to Noto Sans Mono, and emoji are drawn in colour from Noto
  Color Emoji. What a build carries is chosen at build time: see
  [docs/unicode.md](docs/unicode.md).
- **Mentions.** MeshCore writes a mention as `@[Name]`; here it shows as @Name on a
  highlight (the selection colour, in the theme's accent), brackets hidden, also when
  the name wraps over lines.
- **Colour themes.** Every colour is a *role* (header, selection, unread,
  channel, repeater...). A theme is a 16-entry palette, so switching is
  instant. Themes: Midnight, Amber, Phosphor, Contrast, Daylight, Ocean.
- **Vim keys.** Move with `j` `k` `h` `l`, jump with `gg` `G` and counts,
  page with Ctrl-d / Ctrl-u, find with `/` and `n`, switch tabs with `gt`,
  write with `i` and send with `ZZ`. No arrow keys, trackball or touch needed:
  on by default on the T-Deck, a setting on the Cardputer. See
  [Vim keys](#vim-keys).

## Screenshots

Captured from the scripted mock-mesh simulator at 2× scale. Names and messages
are example data, not real contacts. The device display is 240 × 135 pixels.

| Chats | Direct messages |
|---|---|
| ![Chats tab with unread conversations](docs/screenshots/home-chats.png) | ![Direct-message conversation with delivery and signal details](docs/screenshots/chat-dm.png) |
| **Inline reply** | **Channel conversation** |
| ![Composing an inline reply](docs/screenshots/chat-compose.png) | ![Public channel with incoming messages](docs/screenshots/channel.png) |
| **Contact details** | **Radio diagnostics** |
| ![Contact details and actions](docs/screenshots/contact-detail.png) | ![Radio diagnostics](docs/screenshots/diagnostics.png) |
| **Amber theme** | |
| ![Settings in the Amber theme](docs/screenshots/amber-theme.png) | |

On the T-Deck Plus (320 × 240, touch):

| Chats, with the shortcut bar | A conversation |
|---|---|
| ![Chats tab on the T-Deck with the touch shortcut bar](docs/screenshots/tdeck-home.png) | ![Direct-message conversation on the T-Deck](docs/screenshots/tdeck-chat.png) |
| **Contact details (hold a row)** | **Vim keys: / and n** |
| ![Contact details with touch value arrows](docs/screenshots/tdeck-details.png) | ![Search results stepped through with n](docs/screenshots/tdeck-vim-find.png) |

Generate the full screenshot gallery locally with `pixi run shots` (`shots/cardputer/index.html`),
and the T-Deck's with `python -I scripts/screenshots.py --board tdeck` (`shots/tdeck/index.html`).

## Remote node mode

The Cardputer can also be the keyboard and screen of another MeshCore node
that runs stock **companion** firmware (a Heltec, T-Echo, RAK...), over
Bluetooth, the way the phone app is. Settings > Mesh node > remote switches
to it (the device restarts); its own LoRa radio then sleeps and need not
even be attached. Remote node (in the main menu) finds nodes nearby and
connects with the node's Bluetooth PIN (stock default 123456); the device
reconnects by itself after that.

Everything works against the remote node: chats, contacts, channels,
heard adverts, repeater and room admin, trace and ping, radio settings,
diagnostics and the packet log (from the node's raw receive log). History is
kept on the Cardputer, per node. Backup & restore saves the node's settings,
channels, contacts and identity (if the node allows export) to the SD card
and puts them back on the node. The Cardputer's GPS can set the node's clock
and advert position. Remote node also restarts or erases the node.

The Bluetooth build only: the USB-only build has no remote mode.

## Keys (Cardputer)

| Key | In lists and menus | While typing |
|---|---|---|
| `;` `.` | up / down | type `;` `.` (Fn + key moves) |
| `,` `/` | previous / next tab, change value | type `,` `/` (Fn + key moves cursor) |
| Enter | open / select | send |
| `` ` `` (Esc) | back (on Home: clear search) | stop typing |
| hold Esc | go Home | go Home |
| Backspace | back (on Home: delete from the search) | delete |
| Opt | main menu | main menu |
| Tab | details / actions for the selection | |
| letters | find (on Home), start writing (in a chat) | type |

## Keys and touch (T-Deck Plus)

The T-Deck has no arrow keys, and its trackball wears with age, so nothing
depends on it: vim keys are on by default, and everything can be done by touch.

| Key | Does |
|---|---|
| letters | vim keys (below): `j` `k` move, `h` `l` change tab or value, `/` finds |
| Enter, trackball click | open / select / send |
| Alt | Esc: back, stop typing; hold for Home |
| Backspace | back, outside text (on Home: edits the search) |
| Sym + Enter | main menu |
| Sym + Space | Tab: details / actions for the selection |
| Mic | Ctrl (for Ctrl-d, Ctrl-u, Ctrl-o) |
| Sym + Shift + h j k l | arrows, also while typing (move the cursor) |
| Sym, Sym + Shift | the second legends printed on the keys, and symbols that aren't printed (`boards/tdeck/keymap/tdeck_keymap.h` lists them) |
| trackball | arrows (Settings → Trackball off ignores a worn one's rolls) |

Pocket lock (Settings → Lock when screen off, on by default): when the screen
times out the T-Deck locks, and keys, trackball rolls and touches do nothing
(they don't even wake it) until the trackball is clicked. A message can still
light the screen; it stays locked, with a padlock in the header, and a key
pressed then says so. The click that unlocks does nothing else. Off, any key
wakes the screen as before. Holding the click for about a second locks it by hand (the screen goes dark too), with the timeout lock on or off. The Cardputer, which has no such button, never locks.

Touch:

- **Shortcut bar** at the bottom: Chats, Contacts, Nodes, Channels and Menu,
  from anywhere, with unread counts. It hides while typing.
- **Rows:** tap to open, hold for details. A heard advert is added on a
  second tap.
- **Drag** to move through lists and scroll conversations, the same way for both.
  Settings → Touch Scrolling Direction: *same* (default) moves the selection or view the way the
  finger goes, as j/k do (drag up for older messages, which load at the top);
  *opposite* has the content follow the finger instead (drag down for older).
  The header names the tab; there's no tab strip, the bar is the tabs. **Swipe sideways** on Home to change tabs.
- **‹** in the header goes back; hold it for Home.
- A setting's value shows `‹ value ›`: tap either side to change it.

## Vim keys

Settings → Vim keys (on by default where the keyboard has no arrows). Letters
become commands in lists, menus and the message view; writing is unchanged.
On the Cardputer, `;` `,` `.` `/` then always type, and Fn + them still moves.

| Keys | Does |
|---|---|
| `j` `k` / `h` `l` | down / up; previous / next tab, change value |
| `gg` `G`, `{n}G` | first / last, row n |
| Ctrl-d Ctrl-u, Ctrl-f Ctrl-b | half page, page down / up |
| `5j` | a count repeats a motion |
| `/` then `n` `N` | find on Home (Enter keeps the matches), next / previous match |
| `gt` `gT`, `{n}gt` | next / previous Home tab, tab n |
| `i` `a` `o` | write a message (`o` on Home: open the selection and write) |
| `ZZ` `ZQ` | send / discard the draft left after Esc |
| `q` | back |
| Ctrl-o | the conversation before this one |
| `]u` `[u` | next / previous conversation with unread messages |

A command being typed (`2g`) shows at the right of the footer.

## Screens

- **Home** has four tabs:
  - Chats: conversations, unread first by recency. The selected chat's last message scrolls by in full (Settings > Chat preview: "short" scrolls only its first 44 characters).
  - Contacts.
  - Nodes: repeaters, rooms and sensors.
  - Channels.
- **Conversation:**
  - Each message shows its delivery state: sending, sent, delivered or failed, with retries and repeat counts.
  - Incoming messages show hops and SNR.
  - A "New" line marks where the messages that arrived since you last opened the chat begin, and the chat opens scrolled to it.
  - You can compose replies inline, in a word-wrapped box.
  - History: the newest 512 messages stay on flash. With an SD card, older
    ones move to `/meshmash/history/` on the card instead of being
    lost. Up at the top of a conversation loads older messages, and Down at
    the bottom comes back.
  - "Clear history" on a contact or channel deletes its messages, including
    the card's copy.
- **Contact details:**
  - Path, hops, last heard, SNR/RSSI and position.
  - Actions: favourite, reset path, trace route, ping, show in Chats, remove.
- **Repeater / room admin:** login, status, neighbours, telemetry, clock sync, advert, CLI command.
- **Channels:** join a `#hashtag` channel, join or create a private one, mute, show the key, leave. Muted channels don't alert; their unread count shows, dimmed, on the Channels tab.
- **Show in Chats:** a DM or channel can be taken off the Chats list without leaving it or losing its history. A new message brings a DM or unmuted channel back; a muted channel stays off until shown again.
- **Radio:**
  - Community presets, including USA and USA - Southern California.
  - Custom frequency, bandwidth, SF, CR and TX power.
  - Repeat and RX boost.
- **Diagnostics:**
  - Radio page: noise floor, RSSI/SNR, duty cycle, packet counters, airtime, queues.
  - System page: heap, battery, uptime, capacity.
- **Packet log:** every received packet with its type, route, hops, SNR, RSSI, length and duplicate flag.
- **Heard adverts** and **Discover repeaters**.
- **GPS:**
  - The receiver is off by default.
  - Sharing your position in adverts and setting the clock at boot are separate options, also off by default.
- **Settings:** node name, theme, brightness, screen timeout, lock when the screen is off and touch scroll direction (T-Deck), text size, chat preview, key filter, sound, new-message alert and LED brightness, auto-add, ping bot, Bluetooth and its PIN (a new random one at every boot by default; Settings → Bluetooth PIN offers to switch to a fixed one and asks for it; a new PIN applies after a restart, and the phone has to forget the old pairing), factory reset.
- **New-message alert:** on the T-Deck, which has no LED, the key backlight blinks instead (at the LED brightness). On the Cardputer, a 2-second burst of colours on the RGB LED with the screen left off (the default, easiest on the battery), the screen turning on, both, or neither. The chime follows the Sound setting either way. Enter on the setting previews the colours. The LED runs on the backlight's power, so the screen glows briefly while it flashes.
- **Ping bot** (local node mode, off by default):
  - A direct message that is just `ping` or `test` gets an automatic reply with the path it took, for example `pong: 2 hops a1>Hilltop, SNR 6.5`.
  - Relays are named when their hash matches exactly one repeater or room contact.
  - A DM that came by a direct route has no path left when it arrives, so the reply shows our stored route to the sender, reversed and marked `(stored route)`.
  - Each contact gets at most one answer every 10 seconds.
- **Backup & restore to SD:**
  - Automatic backup can be turned on or off.
  - Manual backup and restore, plus restore of the previous backup.
  - On a fresh install, the device offers to restore.

## Building

The host tools are managed by [pixi](https://pixi.sh):

```sh
git submodule update --init
pixi install
scripts/install-hooks.sh     # pre-commit check that keeps personal data out of git

pixi run test                # host unit tests
pixi run shots               # run the simulator scenarios -> shots/cardputer/index.html
pixi run build               # firmware: Cardputer ADV and T-Deck Plus, BLE and USB-only builds
pixi run simnet-test         # UI on real MeshCore nodes in a simulated mesh
pixi run ci                  # everything, including size budgets
```

Flash with `pio run -e cardputer_adv -t upload` (or `-e tdeck`), or install
over bmorcelli's Launcher with `pixi run launcher-install IP --variant cardputer|tdeck --replace`, which builds only that board. `pixi run package` writes
release images for esptool, web flashers, M5Burner and M5Launcher to `dist/`.
See `docs/FLASHING.md` for how to install them, and `docs/DEVICE-TESTING.md`
for the first-flash checklist.

| Build | What | Flash | Static RAM |
|---|---|---|---|
| `cardputer_adv` | UI + Bluetooth phone companion + remote node mode (default) | ~0.94 MB | ~113 KB |
| `cardputer_adv_usb` | UI + USB companion, no Bluetooth | ~0.69 MB | ~101 KB |
| `tdeck` | T-Deck Plus: as `cardputer_adv`, with touch, 350 contacts, 16 channels | ~0.95 MB | ~61 KB + PSRAM |
| `tdeck_usb` | T-Deck Plus, USB companion only | ~0.71 MB | ~49 KB + PSRAM |

On the T-Deck the node (its contacts) and the UI's frame live in PSRAM, so
internal RAM stays free for Bluetooth.

The stock MeshCore companion builds are about 1.25 MB with Bluetooth and
0.64 MB without. The keyboard UI and its diagnostics add about 45 KB. Our
Bluetooth build uses NimBLE instead of stock's Bluedroid, which makes it
about 0.3 MB smaller than stock even with the UI and remote mode. The phone apps see the same
service; a phone that was paired with a Bluedroid build has to forget the
device and pair again once.

**Messages and the phone app (local mode).** What the node receives reaches both the phone
and this device. A message sent from the phone also appears here, as yours, with its
delivery status (a DM shows pending, then delivered when the ACK comes; the phone does
its own retries). The other direction is a limit of MeshCore's phone protocol, not of this
firmware: it has no frame for "a message I sent", only received ones, so a message sent
on this device cannot appear in the phone app's history.

### Simulators

There are two simulators. Both run the real UI on a virtual clock, and key
presses go through the same Cardputer keymap code as the device.

- **`sim`** uses a scripted mock mesh. It gives deterministic screenshots of
  every screen and theme. Scenarios are in `test/scenarios/*.sim`.
  **`sim_tdeck`** is the same at the T-Deck's 320x240
  (`scripts/screenshots.py --board tdeck`).
- **`simnet`** runs stock MeshCore: our node, scripted companion nodes, and
  unmodified MeshCore repeaters, on a simulated LoRa medium with real airtime
  and per-link SNR. It covers routing, ACKs, retries, admin, trace, channels,
  backup and restore, and reboots. Scenarios with expectations are in
  `test/simnet/*.sim`. With `--remote`, the UI drives our node as a remote
  companion over a simulated Bluetooth link (it paces and drops frames as
  stock's BLE interface does); scenarios marked `# both-modes` run both ways.

The script commands are listed in `boards/sim/main.cpp`,
`boards/simnet/main.cpp` and `boards/sim/runner.cpp`.

## Layout

```
src/ui/        UI: canvas, fonts, themes, widgets, screens (no Arduino/MeshCore)
src/model/     MeshModel: the UI's only view of the mesh
src/app/       MeshCore adapter (local node), radio presets, backup
src/app/remote/ remote node: companion protocol client and its MeshModel
src/hal/       display / keyboard interfaces
boards/<name>/ device drivers and MeshCore target for a board
boards/sim/    mock-data simulator and the shared script runner
boards/simnet/ MeshCore on the host: Arduino shim, LoRa medium, nodes, repeaters
external/      MeshCore submodule (never edited; see patches/)
```

## Credits

- The UI design is inspired by
  [anton-vinogradov/meshtastic-adv](https://github.com/anton-vinogradov/meshtastic-adv).
- The keyboard mappings, the T-Deck keyboard, trackball and touch handling,
  and the font tooling are adapted from esp-vim (Apache-2.0).
- The fonts are [Spleen](https://github.com/fcambus/spleen) by Frederic Cambus (BSD-2-Clause),
  with Noto Sans Mono and Noto Color Emoji (SIL OFL 1.1) as fallbacks; see `NOTICE`.

See `NOTICE` for details.

## License

MIT. See `LICENSE`.
