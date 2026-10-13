"""Finds note onsets in a rendered audio file. Uses ffmpeg to decode and the standard library
to measure, so it needs no extra packages."""

import io
import math
import struct
import subprocess
import wave

WINDOW_SECONDS = 0.005
ON_DB = -40.0  # an onset needs the window to rise above this many dB under the file's peak
OFF_DB = -50.0  # and the level has to fall under this before the next onset can count


def decode(path, rate=44100):
    """Returns (samples as floats in -1..1, rate) for mono 16-bit audio decoded by ffmpeg."""
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-ac", "1", "-ar", str(rate), "-f", "wav", "-"],
        capture_output=True, check=True)
    with wave.open(io.BytesIO(result.stdout)) as handle:
        raw = handle.readframes(handle.getnframes())
    count = len(raw) // 2
    values = struct.unpack("<%dh" % count, raw[:count * 2])
    return [v / 32768.0 for v in values], rate


def onsets(samples, rate):
    window = max(1, int(rate * WINDOW_SECONDS))
    levels = []
    for start in range(0, len(samples) - window + 1, window):
        chunk = samples[start:start + window]
        levels.append(math.sqrt(sum(x * x for x in chunk) / window))
    peak = max(levels, default=0.0)
    if peak <= 1e-6:
        return {"peak_db": None, "silent": True, "onsets": []}
    on, off = peak * 10 ** (ON_DB / 20), peak * 10 ** (OFF_DB / 20)
    found, armed = [], True
    for i, level in enumerate(levels):
        if armed and level >= on:
            found.append({"time": round(i * window / rate, 4), "level_db": round(20 * math.log10(level / peak), 1)})
            armed = False
        elif not armed and level <= off:
            armed = True
    return {"peak_db": round(20 * math.log10(max(abs(x) for x in samples)), 1), "silent": False, "onsets": found}


def analyze(path):
    samples, rate = decode(path)
    out = onsets(samples, rate)
    out["seconds"] = round(len(samples) / rate, 3)
    out["path"] = str(path)
    return out
