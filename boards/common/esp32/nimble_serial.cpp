// Bluetooth build only (the USB build has no NimBLE).
#if defined(BLE_PIN_CODE)
#include "nimble_serial.h"
#include <Arduino.h>
#include <esp_mac.h>

// The phone apps' service: Nordic UART, as stock MeshCore.
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static constexpr unsigned long ADVERT_RESTART_DELAY = 1000;   // ms, as stock
static constexpr unsigned long WRITE_MIN_INTERVAL = 60;       // ms between notifies, as stock

NimbleSerialInterface::NimbleSerialInterface() {
  _rx = xQueueCreateStatic(QUEUE, sizeof(Frame), _rxStorage, &_rxState);
}

void NimbleSerialInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
  _pin = pin_code;
  if (strcmp(name, "@@MAC") == 0) {
    uint8_t a[8] = {};
    esp_efuse_mac_get_default(a);
    sprintf(name, "%02X%02X%02X%02X%02X%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
  }
  snprintf(_devName, sizeof _devName, "%s%s", prefix, name);
  build();
}

// Starts the NimBLE host and controller and publishes the service: at boot, and again each time
// Bluetooth is switched back on.
void NimbleSerialInterface::build() {
  NimBLEDevice::init(_devName);
  // Room for a whole frame in one notify (the ATT header takes 3 bytes).
  NimBLEDevice::setMTU(MAX_FRAME_SIZE + 3);
  // Static PIN shown on our screen, typed on the phone; bonded, MITM, LE Secure.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(_pin);

  _server = NimBLEDevice::createServer();
  _server->setCallbacks(this, false);
  _server->advertiseOnDisconnect(false);   // restarted from loop, after a delay
  _service = _server->createService(SERVICE_UUID);
  _tx = _service->createCharacteristic(
      TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC |
                   NIMBLE_PROPERTY::READ_AUTHEN);
  NimBLECharacteristic* rx = _service->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(this);
  _service->start();
  NimBLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  _stacked = true;
}

// Stops the controller and frees the host's memory, so that nothing of Bluetooth runs (or can
// keep the chip from sleeping) while it is off.
void NimbleSerialInterface::teardown() {
  if (!_stacked) return;
  NimBLEDevice::getAdvertising()->stop();
  for (uint16_t h : _server->getPeerDevices()) _server->disconnect(h);
  // The disconnect completes on the host task; give it a moment.
  for (int i = 0; i < 50 && _server->getConnectedCount(); i++) delay(10);
  NimBLEDevice::deinit(true);
  _server = nullptr;
  _service = nullptr;
  _tx = nullptr;
  _stacked = false;
}

// ---- NimBLE callbacks (host task)

BleLastPeer bleLastPeer;

void NimbleSerialInterface::onConnect(NimBLEServer*, ble_gap_conn_desc* desc) {
  bleLastPeer.connects++;
  bleLastPeer.otaType = desc->peer_ota_addr.type;
  bleLastPeer.idType = desc->peer_id_addr.type;
  bleLastPeer.bonded = NimBLEDevice::isBonded(NimBLEAddress(desc->peer_id_addr));
  _conn = desc->conn_handle;
  _linked = true;
  _authed = false;
}

void NimbleSerialInterface::onDisconnect(NimBLEServer*, ble_gap_conn_desc*) {
  _linked = _authed = false;
  _conn = 0xFFFF;
}

void NimbleSerialInterface::onAuthenticationComplete(ble_gap_conn_desc* desc) {
  if (desc->sec_state.encrypted && desc->sec_state.authenticated) {
    bleLastPeer.authOk++;
    _authed = true;
  } else {
    bleLastPeer.authFail++;
    _linked = false;
    _server->disconnect(desc->conn_handle);
  }
}

void NimbleSerialInterface::onWrite(NimBLECharacteristic* c, ble_gap_conn_desc*) {
  NimBLEAttValue v = c->getValue();
  if (v.length() > MAX_FRAME_SIZE) return;
  Frame f;
  f.len = v.length();
  memcpy(f.buf, v.data(), f.len);
  xQueueSend(_rx, &f, 0);   // dropped if full, as stock
}

// ---- BaseSerialInterface (loop task)

void NimbleSerialInterface::clearBuffers() {
  xQueueReset(_rx);
  _txLen = 0;
}

void NimbleSerialInterface::enable() {
  if (_enabled) return;
  _enabled = true;
  clearBuffers();
  if (!_stacked) build();
  NimBLEDevice::getAdvertising()->start();
  _advRestartAt = 0;
}

void NimbleSerialInterface::disable() {
  _enabled = false;
  teardown();
  _linked = _authed = _wasAuthed = false;
  _conn = 0xFFFF;
  _advRestartAt = 0;
}

size_t NimbleSerialInterface::writeFrame(const uint8_t src[], size_t len) {
  if (!_stacked || len == 0 || len > MAX_FRAME_SIZE || !_authed || _txLen >= QUEUE) return 0;
  _txq[_txLen].len = len;
  memcpy(_txq[_txLen].buf, src, len);
  _txLen++;
  return len;
}

bool NimbleSerialInterface::isWriteBusy() const {
  return millis() < _lastWrite + WRITE_MIN_INTERVAL;
}

size_t NimbleSerialInterface::checkRecvFrame(uint8_t dest[]) {
  if (!_stacked) return 0;
  unsigned long now = millis();
  if (_txLen > 0 && now >= _lastWrite + WRITE_MIN_INTERVAL) {
    _lastWrite = now;
    _tx->setValue(_txq[0].buf, _txq[0].len);
    _tx->notify();
    _txLen--;
    memmove(_txq, _txq + 1, _txLen * sizeof(Frame));
  }

  Frame f;
  if (xQueueReceive(_rx, &f, 0) == pdTRUE) {
    memcpy(dest, f.buf, f.len);
    return f.len;
  }

  bool authed = _authed;
  if (authed != _wasAuthed) {
    _wasAuthed = authed;
    if (authed) {
      NimBLEDevice::getAdvertising()->stop();
      _advRestartAt = 0;
    } else {
      clearBuffers();
    }
  }
  // Advertise again a second after a phone leaves or fails to pair.
  if (_enabled && !_linked && !_advRestartAt && !NimBLEDevice::getAdvertising()->isAdvertising())
    _advRestartAt = now + ADVERT_RESTART_DELAY;
  if (_advRestartAt && now >= _advRestartAt) {
    _advRestartAt = 0;
    if (_enabled && _server->getConnectedCount() == 0) NimBLEDevice::getAdvertising()->start();
  }
  return 0;
}

#endif
