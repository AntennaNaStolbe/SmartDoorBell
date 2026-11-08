#!/usr/bin/env python3
"""Generate a short two-tone doorbell chime as a 16-bit mono WAV file."""

from pathlib import Path
import sys
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required: python3 -m pip install numpy")

SAMPLE_RATE = 16000
OUTPUT = Path(__file__).resolve().parent.parent / "data" / "doorbell.wav"


def note(frequency, start, duration, amplitude, decay):
    count = int(SAMPLE_RATE * duration)
    t = np.arange(count, dtype=np.float64) / SAMPLE_RATE

    attack = 0.006
    envelope = np.minimum(t / attack, 1.0) * np.exp(-t / decay)

    release = int(SAMPLE_RATE * 0.05)
    if release < count:
        envelope[-release:] *= np.linspace(1.0, 0.0, release)

    signal = (
        np.sin(2.0 * np.pi * frequency * t)
        + 0.35 * np.sin(2.0 * np.pi * frequency * 2.01 * t)
        + 0.16 * np.sin(2.0 * np.pi * frequency * 3.00 * t)
    )
    return start, signal * envelope * amplitude


def build_chime():
    duration = 2.6
    total = int(SAMPLE_RATE * duration)
    mix = np.zeros(total, dtype=np.float64)

    notes = [
        note(659.25, 0.00, 1.00, 0.75, 0.42),
        note(523.25, 0.55, 1.60, 0.80, 0.62),
    ]

    for start, signal in notes:
        offset = int(SAMPLE_RATE * start)
        end = min(offset + len(signal), total)
        mix[offset:end] += signal[: end - offset]

    peak = np.max(np.abs(mix))
    if peak > 0:
        mix = mix / peak * 0.70

    return np.round(mix * 32767.0).astype("<i2")


def main():
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    samples = build_chime()

    with wave.open(str(OUTPUT), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(samples.tobytes())

    print(f"Wrote {OUTPUT} ({len(samples) / SAMPLE_RATE:.2f} s, {OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
