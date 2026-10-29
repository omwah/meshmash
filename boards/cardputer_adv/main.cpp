// Firmware entry point for the Cardputer ADV. Mirrors the stock companion
// example's setup()/loop() (examples/companion_radio/main.cpp) so phone apps
// keep working, with our UI as the display task. When MeshCore changes that
// file, review this one (scripts/check-upstream-main.sh warns).
#include <Arduino.h>
#include "gen/mc_version.h"
#include <SPIFFS.h>
#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/MultiSerialInterface.h>
#include "MyMesh.h"
#include "app/mesh_adapter.h"
#include "ui/app.h"
#include "ui/screens/screens.h"
#include "st7789_display.h"
#include "tca8418_keyboard.h"
#include "esp32_platform.h"
#include "cardputer_idle.h"
#include "es8311_sound.h"
#include "ws2812_led.h"
#include <new>
#if !defined(ENABLE_USB_INTERFACE)
  #include "diag.h"
#endif

#if defined(BLE_PIN_CODE)
  // MeshCore's Bluetooth companion over NimBLE; it also tells the UI when a
  // phone is pairing, so it can show the PIN.
  #include "nimble_serial.h"
  static NimbleSerialInterface ble_interface;
  static constexpr bool kBleBuilt = true;
#else
  static constexpr bool kBleBuilt = false;
#endif
#if defined(BLE_PIN_CODE) && MC_UI_REMOTE
  // Remote node mode: this device as a companion node's screen, over BLE.
  #include "nimble_central.h"
  #include "app/remote/remote_adapter.h"
  static NimbleCentral* central = nullptr;
  static app::RemoteAdapter* remote = nullptr;
#endif
#if defined(ENABLE_USB_INTERFACE)
  #include <helpers/ArduinoSerialInterface.h>
  static ArduinoSerialInterface usb_interface;
#endif

static MultiSerialInterface interfaces;
static DataStore store(SPIFFS, rtc_clock);
static Esp32Platform platform;
static CardputerIdle idle;   // the clock follows the screen and Settings > CPU speed
static app::MeshAdapter adapter(&board, &interfaces, platform);
static StdRNG fast_rng;
static SimpleMeshTables tables;
// MyMesh.h declares this extern.
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store, &adapter);
static St7789Display display;
static Tca8418Keyboard keyboard;
static Es8311Sound sound;
static Ws2812Led led(display);
// The UI, built in setup() on the local or the remote model.
alignas(ui::App) static uint8_t ui_storage[sizeof(ui::App)];
static ui::App* ui_app = nullptr;
// SPIFFS mounted. Launcher can install an app without a partition for it,
// and then nothing (identity, contacts, settings) is saved.
static bool fs_ok = false;
static bool remoteMode = false;

static void halt(const char* why) {
  Serial.printf("halt: %s\n", why);
  for (;;) delay(1000);
}

#if defined(BLE_PIN_CODE) && MC_UI_REMOTE
// Remote node mode: no mesh node of our own. The radio sleeps (it need not
// even be attached); the UI drives a companion node over Bluetooth.
static void setupRemote() {
  if (radio_init()) radio.sleep();
  sensors.begin();   // this device's GPS
  central = new NimbleCentral();
  central->begin("MeshCore-UI");
  remote = new app::RemoteAdapter(*central, platform);
  remote->begin(SPIFFS);
  ui_app = new (ui_storage) ui::App(*remote, display, keyboard, &sound);
  keyboard.begin();
  sound.begin();
  led.begin();
  ui_app->setLed(&led);
  idle.begin();
  ui_app->setIdleHook(&idle);
  platform.lowClockSecondsFn = [] { return idle.lowSeconds(); };
  ui_app->begin(ui::screens::boot());
  board.onBootComplete();
  Serial.printf("meshmash %s (remote node mode): keyboard %s, speaker %s, sd %s, heap %u (min %u)\n",
                MC_UI_VERSION, keyboard.present() ? "ok" : "MISSING", sound.present() ? "ok" : "MISSING",
                platform.sd() ? "yes" : "no", (unsigned)platform.heapFree(), (unsigned)platform.heapMin());
}
#endif

void setup() {
  Serial.begin(115200);
  board.begin();
  display.begin();
  fs_ok = SPIFFS.begin(true);
#if defined(BLE_PIN_CODE) && MC_UI_REMOTE
  remoteMode = app::readDeviceMode(SPIFFS) == model::Mode::Remote;
  if (remoteMode) {
    setupRemote();
    return;
  }
#endif

  if (!radio_init()) halt("radio init failed (is the Cap LoRa-1262 attached?)");
  fast_rng.begin(radio_driver.getRngSeed());

  bool fresh = !SPIFFS.exists("/identity/_main.id");
  store.begin();
  the_mesh.begin();
  the_mesh.setListener(&adapter);

  adapter.begin(the_mesh, store, rtc_clock, SPIFFS, kBleBuilt, fresh);
  ui_app = new (ui_storage) ui::App(adapter, display, keyboard, &sound);
  // Show the splash now: Bluetooth and sensors take a while to start.
  keyboard.begin();
  sound.begin();
  led.begin();
  ui_app->setLed(&led);
  idle.begin();
  ui_app->setIdleHook(&idle);
  platform.lowClockSecondsFn = [] { return idle.lowSeconds(); };
  ui_app->begin(ui::screens::boot());
  if (!fs_ok) ui_app->toast("No storage partition: nothing is saved", ui::C_ERR);
  ui_app->loop(millis());
#if defined(BLE_PIN_CODE)
  platform.blePairingFn = [] { return ble_interface.pairing(); };
  ble_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, adapter.choosePinForBoot());
  interfaces.addInterface(InterfaceType::Bluetooth, &ble_interface);
#endif
#if defined(ENABLE_USB_INTERFACE)
  usb_interface.begin(Serial);
  interfaces.addInterface(InterfaceType::USB, &usb_interface);
#endif
  the_mesh.startInterface(interfaces);
  sensors.begin();
#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

  if (!adapter.prefs().bleEnabled) adapter.disableBluetooth();
  board.onBootComplete();
#if !defined(ENABLE_USB_INTERFACE)
  g_diagBeep = [] {
    static const hal::Tone t[] = {{1760, 70}, {0, 40}, {2349, 110}};
    sound.play(t, 3);
  };
#endif
  // One status line for bring-up (no keys, names or addresses).
  Serial.printf("meshmash %s: flash fs %s, lora-cap %s, keyboard %s, speaker %s, sd %s, contacts %d, heap %u (min %u)\n",
                MC_UI_VERSION, fs_ok ? "ok" : "MISSING", board.loraCapPresent() ? "ok" : "MISSING", keyboard.present() ? "ok" : "MISSING",
                sound.present() ? "ok" : "MISSING",
                platform.sd() ? "yes" : "no", the_mesh.getNumContacts(), (unsigned)platform.heapFree(),
                (unsigned)platform.heapMin());
}

void loop() {
#if defined(BLE_PIN_CODE) && MC_UI_REMOTE
  if (remoteMode) {
    if (remote->gpsWanted()) sensors.loop();
    remote->loop();
    uint32_t uiWait = ui_app->loop(millis());
    platform.powerTick(ui_app->asleep(), 0, sound.busy());
    diagPoll(*ui_app, display, keyboard, platform);
    delay(uiWait < 10 ? uiWait : 10);
    return;
  }
#endif
  the_mesh.loop();
  interfaces.loop();
  if (adapter.gpsWanted()) sensors.loop();   // screen off: only for the clock and position sharing
  adapter.loop();
#if defined(BLE_PIN_CODE)
  idle.setHold(ble_interface.pairing());   // before the UI, which wakes the screen for a pairing
#endif
  uint32_t uiWait = ui_app->loop(millis());
  platform.powerTick(ui_app->asleep(), radio_driver.getPacketsSent(), sound.busy());
  rtc_clock.tick();
#if !defined(ENABLE_USB_INTERFACE)
  diagPoll(*ui_app, display, keyboard, platform);
#endif
  // Don't spin: sleeping lets the idle task halt the CPU until the next
  // interrupt. At most 10 ms, so the radio (one packet buffer, ~40 ms per
  // packet at SF7) is always read in time; not at all while MeshCore has
  // packets to send or retry.
  if (!the_mesh.hasPendingWork()) delay(uiWait < 10 ? uiWait : 10);
}
