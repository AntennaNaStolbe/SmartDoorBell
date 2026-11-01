#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

namespace {

constexpr uint8_t kLedPin = 6;
constexpr uint8_t kButtonPin = 7;
constexpr uint16_t kLedCount = 8;
constexpr uint8_t kBrightness = 102;
constexpr uint32_t kDebounceMs = 40;
constexpr uint32_t kStepMs = 100;
constexpr uint32_t kHoldMs = 500;

Adafruit_NeoPixel strip(kLedCount, kLedPin, NEO_GRB + NEO_KHZ800);
const uint32_t kWhite = strip.Color(255, 255, 255);
const uint32_t kBlue = strip.Color(0, 0, 255);

enum class State : uint8_t { Idle, Fill, Hold };

State state = State::Idle;
uint8_t litCount = 0;
uint32_t lastStepMs = 0;
uint32_t holdStartMs = 0;

int lastButtonReading = HIGH;
int stableButtonReading = HIGH;
uint32_t lastButtonChangeMs = 0;
bool buttonPressed = false;

void fillAll(uint32_t color) {
  for (uint16_t i = 0; i < kLedCount; ++i) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

}  // namespace

void setup() {
  strip.begin();
  strip.setBrightness(kBrightness);
  fillAll(kWhite);
  pinMode(kButtonPin, INPUT_PULLUP);
}

void loop() {
  const uint32_t now = millis();

  const int reading = digitalRead(kButtonPin);
  if (reading != lastButtonReading) {
    lastButtonReading = reading;
    lastButtonChangeMs = now;
  }
  if (now - lastButtonChangeMs >= kDebounceMs && reading != stableButtonReading) {
    stableButtonReading = reading;
    if (stableButtonReading == LOW) {
      buttonPressed = true;
    }
  }

  if (buttonPressed) {
    buttonPressed = false;
    if (state == State::Idle) {
      state = State::Fill;
      litCount = 0;
      fillAll(kWhite);
      lastStepMs = now - kStepMs;
    }
  }

  switch (state) {
    case State::Fill:
      if (now - lastStepMs >= kStepMs) {
        lastStepMs = now;
        if (litCount < kLedCount) {
          strip.setPixelColor(litCount, kBlue);
          ++litCount;
          strip.show();
        }
        if (litCount >= kLedCount) {
          state = State::Hold;
          holdStartMs = now;
        }
      }
      break;

    case State::Hold:
      if (now - holdStartMs >= kHoldMs) {
        fillAll(kWhite);
        state = State::Idle;
      }
      break;

    case State::Idle:
      break;
  }

  delay(1);
}
