# Installing MeshMash

Images for the M5Stack Cardputer ADV with the Cap LoRa-1262. Build them with
`pixi run package`. They are written to `dist/<version>/`:

| File | Use it with |
|---|---|
| `meshmash-<ver>-cardputer_adv-full.bin` | esptool, web flashers, M5Burner: one image, written at offset `0x0`; and Launcher from the SD card |
| `meshmash-<ver>-cardputer_adv.bin` | Launcher's WebUI install (`pixi run launcher-install`), or anything that installs an app at `0x10000` |
| `...-cardputer_adv_usb...` | The same pair for the USB-only build (no Bluetooth, about 0.23 MB smaller) |
| `SHA256SUMS` | Checksums of the above |

Use the `cardputer_adv` images unless you know you want the USB-only build.

## Before you flash: back up

Flashing replaces whatever is on the device. To be able to go back, save the
whole flash first. It is 8 MB and takes about a minute:

```sh
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 read_flash 0 0x800000 cardputer-backup.bin
```

Keep that file outside this repository. It holds your node's private key.

To restore it later:
```sh
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 write_flash 0x0 cardputer-backup.bin
```

## Download mode

If the port does not appear or the upload times out, put the Cardputer into
download mode:
1. Unplug it, or switch it off.
2. Hold **G0** (the button on top).
3. Plug in USB while still holding G0, then let go.

After flashing, press the reset button or power-cycle the device.

## esptool (command line)

```sh
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 write_flash 0x0 meshmash-<ver>-cardputer_adv-full.bin
```

From a source checkout, `pio run -e cardputer_adv -t upload` builds and flashes
in one step.

## Web flasher

Open Espressif's [esptool-js](https://espressif.github.io/esptool-js/) in
Chrome or Edge, then:
1. Connect to the Cardputer.
2. Add the `-full.bin` file at address `0x0`.
3. Program.

## M5Burner

M5Burner can write a local image to the device. Choose the `-full.bin` file.
If it asks for an address, use `0x0`.

## M5Launcher (bmorcelli's Launcher)

From the SD card:
1. Copy `meshmash-<ver>-cardputer_adv-full.bin` (or `-tdeck-full.bin`)
   to the SD card. Use the `-full` image: Launcher reads its partition table
   and makes the `spiffs` partition the firmware saves to. From the app-only
   image it makes none, and then nothing is saved.
2. In Launcher, choose SD, then the file, and install.

Over WiFi, from a source checkout: start Launcher's WebUI on the device, then
```sh
pixi run launcher-install <IP> --variant cardputer --replace --boot
```
`--variant` (`cardputer` or `tdeck`) is required: a device takes one build, so
only that board's BLE build is compiled and packaged, then installed (`--usb`
makes it the USB-only build, installed as `meshmash-usb`). Give an image file
instead to install it without building. `--replace` deletes an earlier
`meshmash` in Launcher's menu first; without it a second copy is added. `--upload [FOLDER]` only copies the `-full`
image to the SD card in the device (to `/` by default), to install from
Launcher's SD browser later. The WebUI login is Launcher's default unless
`LAUNCHER_USER` and `LAUNCHER_PASS` are set.

The install also asks Launcher for a small (448 KB) `spiffs` data partition,
where the firmware keeps its identity, contacts, settings and recent history
(older history goes to the SD card); Launcher's own
table has none. An existing `spiffs` is kept as it is. Without one nothing is
saved: every boot is a first boot, and importing an identity from the app
fails. The firmware then shows "No storage partition" at boot. To fix an
install like that, install again from the `-full` image or over WiFi, or add
a `spiffs` partition in Launcher's Partition Manager.

Launcher has its own partition layout, so settings stored on the internal
flash may not survive switching apps. Turn on Settings > Backup & restore >
Auto-backup to SD before switching. When the firmware starts with nothing on flash and finds a
backup on the card, it offers to restore it.

## Coming from stock MeshCore

MeshMash runs stock MeshCore underneath and keeps its files on the
SPIFFS partition in the stock format. If you flash only the app (Launcher, or
an app-only write to `0x10000`), an existing companion's identity, contacts
and channels carry over. A `-full.bin` write at `0x0` keeps them too, because
the image ends long before the data partition at `0x670000`. It does blank
the small NVS area, so a phone paired over Bluetooth has to pair again. Erasing the flash
(`esptool.py erase_flash`) removes everything. Use the MeshCore app's key
export, or this firmware's SD backup, first if you want to keep the node's
identity.

## First boot

The device shows a splash with the node name, then Home. On a fresh device:
- Pick your region's preset in Settings > Radio > Preset. The default is USA.
- Set your node name in Settings.
- If you will pair the MeshCore phone app, check Settings > Bluetooth PIN.
  Pairing shows the PIN on screen.

`DEVICE-TESTING.md` lists what to check on new hardware.
