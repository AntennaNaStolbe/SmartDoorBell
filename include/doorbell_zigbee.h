#pragma once

#include <Arduino.h>

#include "light_settings.h"

namespace doorbell_zigbee {

bool begin(const LightSettings &initialSettings);
bool connected();
bool syncTime();
void requestOtaUpdate();
uint32_t unixNow();
void notifyPress(uint32_t unixTime);
void reportPressTime(uint32_t unixTime);
bool takeRingRequest();
bool takeLightChange(LightSettings &settings);
void factoryReset();

}  // namespace doorbell_zigbee
