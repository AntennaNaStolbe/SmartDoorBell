#pragma once

namespace audio {

void begin();
bool playDoorbell(bool restart = false);
bool isBusy();
bool isAvailable();
void stop();

}  // namespace audio
