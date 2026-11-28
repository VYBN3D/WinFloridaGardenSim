#pragma once
#include <Arduino.h>

class TwoWire {
public:
  void begin(int sda, int scl) { (void)sda; (void)scl; }
  void beginTransmission(uint8_t addr);
  bool endTransmission(bool stop = true);
  size_t write(uint8_t v);
  uint8_t requestFrom(uint8_t addr, uint8_t n);
  int available();
  int read();
};
extern TwoWire Wire;
