#pragma once

#include <Arduino.h>

namespace config {

constexpr uint8_t kLedPin = 6;
constexpr uint16_t kLedCount = 9;
constexpr uint8_t kMaxBrightness = 255;
constexpr uint8_t kDefaultIdleLevel = 102;
constexpr uint8_t kDefaultIdleRed = 255;
constexpr uint8_t kDefaultIdleGreen = 255;
constexpr uint8_t kDefaultIdleBlue = 255;

constexpr uint8_t kTouchPin = 7;
constexpr uint8_t kTouchActiveLevel = HIGH;
constexpr uint32_t kTouchDebounceMs = 40;

constexpr uint8_t kFactoryResetPin = 9;
constexpr uint32_t kFactoryResetHoldMs = 5000;

constexpr uint8_t kZigbeeEndpoint = 1;
constexpr uint32_t kTimeSyncRetryMs = 10000;
constexpr uint32_t kTimeResyncMs = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kOtaQueryIntervalMs = 6UL * 60UL * 60UL * 1000UL;

constexpr uint32_t kFillStepMs = 100;
constexpr uint32_t kAudioRestartTimeoutMs = 300;
constexpr uint32_t kSettingsSaveDelayMs = 500;

constexpr const char *kSettingsNamespace = "doorbell";

constexpr uint32_t kFirmwareVersion = 0x00010002;
constexpr uint16_t kOtaManufacturer = 0x1A1A;
constexpr uint16_t kOtaImageType = 0x0001;
constexpr uint16_t kOtaHardwareVersion = 0x0001;
constexpr uint8_t kOtaMaxDataSize = 223;

constexpr int8_t kI2sBclkPin = 1;
constexpr int8_t kI2sLrcPin = 2;
constexpr int8_t kI2sDinPin = 0;

constexpr const char *kDoorbellPath = "/doorbell.wav";

}  // namespace config
