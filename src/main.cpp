#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>

#include "audio_player.h"
#include "config.h"
#include "doorbell_zigbee.h"
#include "light_settings.h"

namespace {

Adafruit_NeoPixel strip(config::kLedCount, config::kLedPin, NEO_GRB + NEO_KHZ800);
const uint32_t kRed = strip.Color(255, 0, 0);
const uint32_t kBlack = strip.Color(0, 0, 0);

constexpr uint8_t kCenterOutOrder[config::kLedCount] = {4, 3, 5, 2, 6, 1, 7, 0, 8};

enum class AnimState : uint8_t { Idle, Fill, Hold };
enum class RingSource : uint8_t { Touch, Zigbee, Repeat };

AnimState animState = AnimState::Idle;
uint8_t litCount = 0;
uint32_t lastStepMs = 0;
bool cycleActive = false;
bool cycleAudioOk = false;

int lastTouchReading = LOW;
int stableTouchReading = LOW;
uint32_t lastTouchChangeMs = 0;
bool touchHeld = false;

bool factoryResetHeld = false;
bool factoryResetDone = false;
uint32_t factoryResetSinceMs = 0;

Preferences preferences;
LightSettings idleSettings{};
bool settingsDirty = false;
uint32_t settingsDirtySinceMs = 0;

void fillAll(uint32_t color) {
  for (uint16_t i = 0; i < config::kLedCount; ++i) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

uint8_t scaleLevel(uint8_t level) {
  if (level > 254) {
    level = 254;
  }
  return static_cast<uint8_t>((static_cast<uint16_t>(level) * 255U + 127U) / 254U);
}

void applyIdle() {
  if (!idleSettings.on || idleSettings.level == 0) {
    strip.setBrightness(0);
    fillAll(kBlack);
    return;
  }

  strip.setBrightness(scaleLevel(idleSettings.level));
  fillAll(strip.Color(idleSettings.red, idleSettings.green, idleSettings.blue));
}

void loadSettings() {
  idleSettings = LightSettings{};
  idleSettings.level = config::kDefaultIdleLevel;
  idleSettings.red = config::kDefaultIdleRed;
  idleSettings.green = config::kDefaultIdleGreen;
  idleSettings.blue = config::kDefaultIdleBlue;

  if (!preferences.begin(config::kSettingsNamespace, false)) {
    Serial.println("Settings: NVS open failed");
    return;
  }

  idleSettings.on = preferences.getBool("on", idleSettings.on);
  idleSettings.level = preferences.getUChar("level", idleSettings.level);
  idleSettings.red = preferences.getUChar("red", idleSettings.red);
  idleSettings.green = preferences.getUChar("green", idleSettings.green);
  idleSettings.blue = preferences.getUChar("blue", idleSettings.blue);
  if (idleSettings.level > 254) {
    idleSettings.level = 254;
  }
}

void saveSettings() {
  preferences.putBool("on", idleSettings.on);
  preferences.putUChar("level", idleSettings.level);
  preferences.putUChar("red", idleSettings.red);
  preferences.putUChar("green", idleSettings.green);
  preferences.putUChar("blue", idleSettings.blue);
}

void resetSettings() {
  preferences.clear();
  idleSettings = LightSettings{};
  idleSettings.level = config::kDefaultIdleLevel;
  idleSettings.red = config::kDefaultIdleRed;
  idleSettings.green = config::kDefaultIdleGreen;
  idleSettings.blue = config::kDefaultIdleBlue;
  saveSettings();
  if (animState == AnimState::Idle) {
    applyIdle();
  }
}

void markSettingsDirty(uint32_t now) {
  settingsDirty = true;
  settingsDirtySinceMs = now;
}

void saveSettingsIfNeeded(uint32_t now) {
  if (!settingsDirty || now - settingsDirtySinceMs < config::kSettingsSaveDelayMs) {
    return;
  }

  saveSettings();
  settingsDirty = false;
}

void startCycle(uint32_t now, RingSource source) {
  Serial.println(source == RingSource::Touch ? "Touch: starting doorbell"
                                             : (source == RingSource::Zigbee
                                                    ? "Zigbee: starting doorbell"
                                                    : "Doorbell: repeating cycle"));

  animState = AnimState::Fill;
  litCount = 0;
  cycleActive = true;
  lastStepMs = now - config::kFillStepMs;

  strip.setBrightness(config::kMaxBrightness);
  if (idleSettings.on && idleSettings.level > 0) {
    fillAll(strip.Color(idleSettings.red, idleSettings.green, idleSettings.blue));
  } else {
    fillAll(kBlack);
  }

  cycleAudioOk = audio::playDoorbell(true);
  if (!cycleAudioOk) {
    Serial.println("Doorbell: audio unavailable");
  }

  if (source == RingSource::Touch) {
    doorbell_zigbee::notifyPress(doorbell_zigbee::unixNow());
  } else if (source == RingSource::Zigbee) {
    doorbell_zigbee::reportPressTime(doorbell_zigbee::unixNow());
  }
}

bool takeLightChange(uint32_t now) {
  LightSettings changed;
  if (!doorbell_zigbee::takeLightChange(changed)) {
    return false;
  }

  idleSettings = changed;
  if (animState == AnimState::Idle) {
    applyIdle();
  }
  markSettingsDirty(now);
  return true;
}

void checkFactoryReset(uint32_t now) {
  if (digitalRead(config::kFactoryResetPin) == LOW) {
    if (!factoryResetHeld) {
      factoryResetHeld = true;
      factoryResetSinceMs = now;
    } else if (!factoryResetDone &&
               now - factoryResetSinceMs >= config::kFactoryResetHoldMs) {
      factoryResetDone = true;
      Serial.println("Zigbee: factory reset");
      resetSettings();
      doorbell_zigbee::factoryReset();
    }
  } else {
    factoryResetHeld = false;
    factoryResetDone = false;
  }
}

void checkTouch(uint32_t now) {
  const int reading = digitalRead(config::kTouchPin);
  if (reading != lastTouchReading) {
    lastTouchReading = reading;
    lastTouchChangeMs = now;
  }

  if (now - lastTouchChangeMs < config::kTouchDebounceMs ||
      reading == stableTouchReading) {
    return;
  }

  stableTouchReading = reading;
  if (stableTouchReading == config::kTouchActiveLevel) {
    touchHeld = true;
    startCycle(now, RingSource::Touch);
  } else {
    touchHeld = false;
  }
}

void updateAnimation(uint32_t now) {
  switch (animState) {
    case AnimState::Fill:
      if (now - lastStepMs >= config::kFillStepMs) {
        lastStepMs = now;
        const uint8_t count = (litCount == 0) ? 1 : 2;
        for (uint8_t i = 0; i < count && litCount < config::kLedCount; ++i) {
          strip.setPixelColor(kCenterOutOrder[litCount], kRed);
          ++litCount;
        }
        strip.show();
        if (litCount >= config::kLedCount) {
          animState = AnimState::Hold;
        }
      }
      break;

    case AnimState::Hold:
      if (!cycleActive || audio::isBusy()) {
        break;
      }
      if (touchHeld && cycleAudioOk) {
        startCycle(now, RingSource::Repeat);
      } else {
        cycleActive = false;
        animState = AnimState::Idle;
        applyIdle();
      }
      break;

    case AnimState::Idle:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(150);

  loadSettings();

  strip.begin();
  applyIdle();

  pinMode(config::kTouchPin,
          config::kTouchActiveLevel == HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  lastTouchReading = digitalRead(config::kTouchPin);
  stableTouchReading = lastTouchReading;
  lastTouchChangeMs = millis();

  pinMode(config::kFactoryResetPin, INPUT_PULLUP);

  audio::begin();
  doorbell_zigbee::begin(idleSettings);
  Serial.println("SmartDoorBell ready");
}

void loop() {
  const uint32_t now = millis();

  if (doorbell_zigbee::takeRingRequest()) {
    startCycle(now, RingSource::Zigbee);
  }

  checkFactoryReset(now);
  checkTouch(now);
  takeLightChange(now);
  saveSettingsIfNeeded(now);
  updateAnimation(now);

  delay(1);
}
