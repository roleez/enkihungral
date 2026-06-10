#pragma once
#include <Arduino.h>

class FilteredSensorASM {
private:
  const uint8_t PIN;
  uint32_t samples[7];

  // RISC-V (ESP32-C3) kompatibilis rendezőhálózat
  // Az eredeti Xtensa 'min/max' utasítások itt nem léteznek,
  // de a fordító ezt inline-ra és branchless kódra optimalizálja (-O2).
  static inline void cas(uint32_t &a, uint32_t &b) {
    uint32_t t = a < b ? a : b;
    b = a < b ? b : a;
    a = t;
  }

public:
  FilteredSensorASM(uint8_t pin) : PIN(pin) {
    memset(samples, 0, sizeof(samples));
  }

  void begin() { pinMode(PIN, INPUT); }

  int readRaw() {
    for (int i = 0; i < 7; i++) {
      //samples[i] = analogRead(PIN);
      samples[i] = analogReadMilliVolts(PIN);
      vTaskDelay(10);
    }

    // Ugyanaz a 7-elemű rendezőhálózat, csak C++-ban
    uint32_t *s = samples;
    cas(s[0], s[1]);
    cas(s[2], s[3]);
    cas(s[4], s[5]);
    cas(s[0], s[2]);
    cas(s[1], s[3]);
    cas(s[4], s[6]); // s[6] itt jelenik meg először
    cas(s[0], s[4]);
    cas(s[1], s[5]);
    cas(s[2], s[6]); // s[6] itt is
    cas(s[0], s[1]);
    cas(s[2], s[3]);
    cas(s[4], s[5]);
    cas(s[1], s[2]);
    cas(s[3], s[4]);
    cas(s[5], s[6]); // s[6] lezárása
    cas(s[1], s[3]);
    cas(s[2], s[4]);
    cas(s[2], s[3]);

    return (int)s[3];
  }
};