#include "audio_player.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>

#include "config.h"

namespace {

I2SClass i2s;
std::atomic<bool> busy{false};
std::atomic<bool> stopRequested{false};

struct WavInfo {
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
};

uint16_t readU16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool readWavInfo(File &file, WavInfo &info) {
  uint8_t header[12];
  if (file.read(header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  if (std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool haveFormat = false;
  while (file.available() >= 8) {
    uint8_t chunkHeader[8];
    if (file.read(chunkHeader, sizeof(chunkHeader)) != sizeof(chunkHeader)) {
      return false;
    }
    const uint32_t chunkSize = readU32(chunkHeader + 4);

    if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
      if (chunkSize < 16) {
        return false;
      }
      uint8_t fmt[16];
      if (file.read(fmt, sizeof(fmt)) != sizeof(fmt)) {
        return false;
      }
      const uint16_t audioFormat = readU16(fmt + 0);
      info.channels = readU16(fmt + 2);
      info.sampleRate = readU32(fmt + 4);
      info.bitsPerSample = readU16(fmt + 14);

      if (audioFormat != 1 || (info.channels != 1 && info.channels != 2) ||
          info.bitsPerSample != 16 || info.sampleRate == 0) {
        return false;
      }

      const uint32_t remaining = chunkSize - 16;
      if (remaining > 0 && !file.seek(file.position() + remaining)) {
        return false;
      }
      if ((chunkSize & 1U) != 0 && !file.seek(file.position() + 1)) {
        return false;
      }
      haveFormat = true;
    } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
      if (!haveFormat) {
        return false;
      }
      info.dataOffset = file.position();
      info.dataSize = chunkSize;
      return true;
    } else {
      const uint32_t skip = chunkSize + (chunkSize & 1U);
      if (!file.seek(file.position() + skip)) {
        return false;
      }
    }
  }

  return false;
}

void writeSilence(uint32_t sampleRate) {
  constexpr uint8_t kSilence[256] = {};
  const uint32_t bytes = (sampleRate / 50U) * 4U;
  uint32_t written = 0;
  while (written < bytes) {
    const size_t chunk = (bytes - written) < sizeof(kSilence) ? (bytes - written) : sizeof(kSilence);
    if (i2s.write(kSilence, chunk) != chunk) {
      break;
    }
    written += chunk;
  }
}

void audioTask(void *) {
  File file = LittleFS.open(config::kDoorbellPath, "r");
  if (!file) {
    Serial.printf("Audio: file %s not found\n", config::kDoorbellPath);
    busy.store(false);
    vTaskDelete(nullptr);
    return;
  }

  WavInfo info;
  if (!readWavInfo(file, info)) {
    Serial.println("Audio: unsupported WAV format");
    file.close();
    busy.store(false);
    vTaskDelete(nullptr);
    return;
  }

  Serial.printf("Audio: %lu Hz, %u ch, %u bit, %lu bytes\n",
                static_cast<unsigned long>(info.sampleRate), info.channels, info.bitsPerSample,
                static_cast<unsigned long>(info.dataSize));

  i2s.setPins(config::kI2sBclkPin, config::kI2sLrcPin, config::kI2sDinPin);
  if (!i2s.begin(I2S_MODE_STD, info.sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Audio: I2S init failed");
    file.close();
    busy.store(false);
    vTaskDelete(nullptr);
    return;
  }

  if (!file.seek(info.dataOffset)) {
    Serial.println("Audio: seek failed");
    i2s.end();
    file.close();
    busy.store(false);
    vTaskDelete(nullptr);
    return;
  }

  constexpr size_t kSamplesPerChunk = 256;
  uint8_t input[kSamplesPerChunk * 2];
  uint8_t output[kSamplesPerChunk * 4];
  uint32_t remaining = info.dataSize;
  bool writeOk = true;

  while (remaining > 0 && !stopRequested.load()) {
    size_t request = remaining < sizeof(input) ? remaining : sizeof(input);
    request &= ~static_cast<size_t>(1);
    if (request == 0) {
      break;
    }

    const size_t got = file.read(input, request);
    if (got == 0) {
      break;
    }
    remaining -= got;

    if (info.channels == 1) {
      const size_t samples = got / 2;
      for (size_t i = 0; i < samples; ++i) {
        output[i * 4 + 0] = input[i * 2 + 0];
        output[i * 4 + 1] = input[i * 2 + 1];
        output[i * 4 + 2] = input[i * 2 + 0];
        output[i * 4 + 3] = input[i * 2 + 1];
      }
      if (i2s.write(output, samples * 4) != samples * 4) {
        writeOk = false;
        break;
      }
    } else {
      if (i2s.write(input, got) != got) {
        writeOk = false;
        break;
      }
    }
  }

  if (writeOk && !stopRequested.load()) {
    writeSilence(info.sampleRate);
    delay(50);
  }

  i2s.end();
  file.close();
  stopRequested.store(false);
  busy.store(false);
  vTaskDelete(nullptr);
}

}  // namespace

namespace audio {

void begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("Audio: LittleFS mount failed");
    return;
  }
  Serial.printf("Audio: LittleFS ready, %s %s\n", config::kDoorbellPath,
                LittleFS.exists(config::kDoorbellPath) ? "found" : "missing");
}

bool playDoorbell(bool restart) {
  if (busy.load()) {
    if (!restart) {
      return false;
    }

    stopRequested.store(true);
    const uint32_t start = millis();
    while (busy.load() && millis() - start < config::kAudioRestartTimeoutMs) {
      delay(1);
    }
    if (busy.load()) {
      stopRequested.store(false);
      Serial.println("Audio: restart timeout");
      return false;
    }
  }

  if (!LittleFS.exists(config::kDoorbellPath)) {
    Serial.printf("Audio: file %s not found\n", config::kDoorbellPath);
    return false;
  }

  bool expected = false;
  if (!busy.compare_exchange_strong(expected, true)) {
    return false;
  }

  stopRequested.store(false);
  if (xTaskCreate(audioTask, "doorbell", 8192, nullptr, 2, nullptr) != pdPASS) {
    Serial.println("Audio: task creation failed");
    busy.store(false);
    return false;
  }
  return true;
}

bool isBusy() { return busy.load(); }

bool isAvailable() { return LittleFS.exists(config::kDoorbellPath); }

void stop() { stopRequested.store(true); }

}  // namespace audio
