// Firmware entry point for the LilyGo T-Deck Plus. As the Cardputer's
// (boards/cardputer_adv/main.cpp), it mirrors the stock companion example's
// setup()/loop() (examples/companion_radio/main.cpp) so phone apps keep
// working, with our UI as the display task. When MeshCore changes that file,
// review this one too (scripts/check-upstream-main.sh warns).
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
#include "st7789_tdeck.h"
#include "tdeck_keyboard.h"
#include "tdeck_idle.h"
#include "gt911_touch.h"
#include "esp32_platform.h"
#include "i2s_sound.h"
#include <new>
#include <esp_heap_caps.h>

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
static app::MeshAdapter adapter(&board, &interfaces, platform);
static StdRNG fast_rng;
static SimpleMeshTables tables;
// The node (350 contacts) and the UI (a 320x240 frame) live in PSRAM: this
// core can't place static data there, so they are made at boot. MyMesh.h's
// extern the_mesh is not defined; this file reaches the node through the
// same name.
static MyMesh* g_mesh = nullptr;
#define the_mesh (*g_mesh)
static St7789TDeck display(loraSpi);
static TDeckKeyboard keyboard;
static TDeckLed led(keyboard);   // the key backlight, blinked for a new message
static Gt911Touch touch;
static TDeckIdle idle(keyboard, touch);   // slower while the screen is off
static I2sSound sound;
// The UI, built in setup() on the local or the remote model.
static void* ui_storage = nullptr;
static ui::App* ui_app = nullptr;
// SPIFFS mounted. Launcher can install an app without a partition for it,
// and then nothing (identity, contacts, settings) is saved.
static bool fs_ok = false;
static bool remoteMode = false;

// Settings the drivers follow.
static void applyBoardPrefs() {
  const model::Prefs& p = ui_app->m().prefs();
  keyboard.setBall(p.trackball);
}

static void halt(const char* why) {
  Serial.printf("halt: %s\n", why);
  for (;;) delay(1000);
}

static void* psram(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) halt("no PSRAM");
  return p;
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
  touch.begin();
  sound.begin();
  ui_app->setTouch(&touch);
  ui_app->setLed(&led);
  idle.begin();
  ui_app->setIdleHook(&idle);
  platform.lowClockSecondsFn = [] { return idle.lowSeconds(); };
  ui_app->begin(ui::screens::boot());
  board.onBootComplete();
  Serial.printf("meshmash %s (remote node mode): keyboard %s, touch %s, sd %s, heap %u (min %u)\n",
                MC_UI_VERSION, keyboard.present() ? (keyboard.rawMode() ? "ok" : "chars only") : "MISSING",
                touch.present() ? "ok" : "MISSING", platform.sd() ? "yes" : "no", (unsigned)platform.heapFree(),
                (unsigned)platform.heapMin());
}
#endif

void setup() {
  Serial.begin(115200);
  board.begin();
  ui_storage = psram(sizeof(ui::App));
  display.begin();
  fs_ok = SPIFFS.begin(true);
#if defined(BLE_PIN_CODE) && MC_UI_REMOTE
  remoteMode = app::readDeviceMode(SPIFFS) == model::Mode::Remote;
  if (remoteMode) {
    setupRemote();
    return;
  }
#endif

  if (!radio_init()) halt("radio init failed");
  g_mesh = new (psram(sizeof(MyMesh))) MyMesh(radio_driver, fast_rng, rtc_clock, tables, store, &adapter);
  fast_rng.begin(radio_driver.getRngSeed());

  bool fresh = !SPIFFS.exists("/identity/_main.id");
  store.begin();
  the_mesh.begin();
  the_mesh.setListener(&adapter);

  adapter.begin(the_mesh, store, rtc_clock, SPIFFS, kBleBuilt, fresh);
  ui_app = new (ui_storage) ui::App(adapter, display, keyboard, &sound);
  // Show the splash now: Bluetooth and sensors take a while to start.
  keyboard.begin();
  touch.begin();
  sound.begin();
  ui_app->setTouch(&touch);
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
  // One status line for bring-up (no keys, names or addresses).
  Serial.printf("meshmash %s: flash fs %s, keyboard %s, touch %s, sd %s, contacts %d, heap %u (min %u), psram %u\n",
                MC_UI_VERSION, fs_ok ? "ok" : "MISSING",
                keyboard.present() ? (keyboard.rawMode() ? "ok" : "chars only") : "MISSING",
                touch.present() ? "ok" : "MISSING", platform.sd() ? "yes" : "no", the_mesh.getNumContacts(),
                (unsigned)platform.heapFree(), (unsigned)platform.heapMin(), (unsigned)ESP.getFreePsram());
}

void loop() {
#if defined(BLE_PIN_CODE) && MC_UI_REMOTE
  if (remoteMode) {
    sensors.loop();
    remote->loop();
    applyBoardPrefs();
    uint32_t uiWait = ui_app->loop(millis());
    delay(uiWait < 10 ? uiWait : 10);
    return;
  }
#endif
  the_mesh.loop();
  interfaces.loop();
  sensors.loop();
  adapter.loop();
  applyBoardPrefs();
  uint32_t uiWait = ui_app->loop(millis());
  rtc_clock.tick();
  // Don't spin: sleeping lets the idle task halt the CPU until the next
  // interrupt. At most 10 ms, so the radio (one packet buffer, ~40 ms per
  // packet at SF7) is always read in time; not at all while MeshCore has
  // packets to send or retry.
  if (!the_mesh.hasPendingWork()) delay(uiWait < 10 ? uiWait : 10);
}
