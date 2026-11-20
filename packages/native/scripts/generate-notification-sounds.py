"""Write the five short reference Web Audio presets as mono PCM."""
import math
from pathlib import Path
import struct
import sys
import wave

SAMPLE_RATE = 24_000
# Frequency (Hz), initial gain, duration (s), start offset (s), waveform.
PRESETS = {
    "default": [(800, .1, .3, 0, "sine")],
    "subtle": [(660, .07, .22, 0, "sine")],
    "digital": [(880, .09, .12, 0, "triangle"), (1320, .08, .12, .09, "triangle")],
    "glass": [(1480, .075, .18, 0, "sine")],
    "pulse": [(640, .09, .1, 0, "square"), (760, .085, .1, .14, "square")],
}


def write_sound(path, tones):
    end = max(start + duration for _, _, duration, start, _ in tones)
    samples = [0.0] * round(end * SAMPLE_RATE)
    for frequency, gain, duration, start, waveform in tones:
        offset = round(start * SAMPLE_RATE)
        for i in range(round(duration * SAMPLE_RATE)):
            t = i / SAMPLE_RATE
            sample = math.sin(2 * math.pi * frequency * t)
            if waveform == "triangle":
                sample = 2 * math.asin(sample) / math.pi
            elif waveform == "square":
                sample = 1 if sample >= 0 else -1
            samples[offset + i] += sample * gain * (.001 / gain) ** (t / duration)
    with wave.open(str(path), "wb") as out:
        out.setparams((1, 2, SAMPLE_RATE, 0, "NONE", ""))
        out.writeframes(b"".join(struct.pack("<h", round(value * 32767)) for value in samples))


if __name__ == "__main__":
    target = Path(sys.argv[1])
    target.mkdir(parents=True, exist_ok=True)
    for name, tones in PRESETS.items():
        write_sound(target / (name + ".wav"), tones)
