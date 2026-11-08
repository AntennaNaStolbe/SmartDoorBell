#pragma once

#include <Arduino.h>

struct LightSettings {
  bool on = true;
  uint8_t level = 102;
  uint8_t red = 255;
  uint8_t green = 255;
  uint8_t blue = 255;
};
