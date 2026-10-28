// MeshCore's Bluetooth companion link (the phone apps' Nordic UART service)
// over NimBLE instead of stock's Bluedroid (helpers/esp32/SerialBLEInterface):
// the same service, characteristics, static-PIN bonding and frame pacing, in
// far less flash and heap. Phones bonded to a Bluedroid build pair again once.
#pragma once

#include <helpers/BaseSerialInterface.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class NimbleSerialInterface : public BaseSerialInterface,
                              NimBLEServerCallbacks,
                              NimBLECharacteristicCallbacks {
public:
  NimbleSerialInterface();

  // As SerialBLEInterface::begin: name "@@MAC" is replaced by the MAC (IN/OUT).
  void begin(const char* prefix, char* name, uint32_t pin_code);

  // A phone is connected but has not finished pairing (the UI shows the PIN).
  bool pairing() const { return _linked && !_authed; }

  // BaseSerialInterface
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _enabled; }
  bool isConnected() const override { return _authed; }
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;

private:
  // NimBLEServerCallbacks
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override;
  void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* desc) override;
  uint32_t onPassKeyRequest() override { return _pin; }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override;
  // NimBLECharacteristicCallbacks
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override;

  void clearBuffers();
  void build();
  void teardown();

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };
  static constexpr int QUEUE = 4;

  NimBLEServer* _server = nullptr;
  NimBLEService* _service = nullptr;
  NimBLECharacteristic* _tx = nullptr;
  uint32_t _pin = 0;
  bool _enabled = false;
  bool _stacked = false;   // the NimBLE host and controller are running
  char _devName[48] = {};
  // Written from NimBLE's host task, read from loop().
  volatile bool _linked = false, _authed = false;
  volatile uint16_t _conn = 0xFFFF;
  bool _wasAuthed = false;
  unsigned long _lastWrite = 0, _advRestartAt = 0;

  StaticQueue_t _rxState;
  uint8_t _rxStorage[QUEUE * sizeof(Frame)];
  QueueHandle_t _rx;
  int _txLen = 0;
  Frame _txq[QUEUE];
};
