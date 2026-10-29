// Light sleep between loop passes, as MeshCore's ESP32Board::sleep() does it (radio DIO1 high-level
// wake, pending-packet check, DIO1 back to a rising edge afterwards) with two additions it lacks: the
// length is in milliseconds, and the unlock button (GPIO low-level) wakes it, enabled only while the
// button is released so a held button cannot wake it in a loop.
#pragma once
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

// Returns the time spent asleep in ms; 0 when it did not sleep (a packet was waiting, or the unlock
// button was down).
inline uint32_t lightSleepMs(int dio1Pin, int unlockPin, uint32_t ms) {
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  const gpio_num_t dio1 = (gpio_num_t)dio1Pin, unlock = (gpio_num_t)unlockPin;
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  portENTER_CRITICAL(&mux);
  if (gpio_get_level(dio1) == 1 || (unlockPin >= 0 && gpio_get_level(unlock) == 0)) {
    portEXIT_CRITICAL(&mux);
    return 0;
  }
  esp_sleep_enable_gpio_wakeup();
  gpio_wakeup_enable(dio1, GPIO_INTR_HIGH_LEVEL);   // a packet arrived
  if (unlockPin >= 0) gpio_wakeup_enable(unlock, GPIO_INTR_LOW_LEVEL);
  uint32_t t0 = millis();
  esp_light_sleep_start();
  uint32_t slept = millis() - t0;
  // A level interrupt left armed would flood on the way out.
  gpio_wakeup_disable(dio1);
  if (unlockPin >= 0) gpio_wakeup_disable(unlock);
  gpio_set_intr_type(dio1, GPIO_INTR_POSEDGE);   // what RadioLib expects of DIO1
  portEXIT_CRITICAL(&mux);
  return slept;
}
