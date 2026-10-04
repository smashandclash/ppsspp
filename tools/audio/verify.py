#!/usr/bin/env python3
"""Checks the encoded sound: IMA-ADPCM decodes back close to the source (SNR), and every
loop's seam is no louder a jolt than the music's own beats.   python tools/audio/verify.py"""
import os
import struct
import wave

import numpy as np

from make_audio import IMA_INDEX, IMA_STEPS, ROOT, onset_env, resample


def ima_decode(data, n):
    pred, idx = struct.unpack_from("<hB", data, 0)
    out = [pred]
    for byte in data[4:]:
        for code in (byte & 15, byte >> 4):
            if len(out) >= n:
                break
            step = IMA_STEPS[idx]
            delta = step >> 3
            if code & 4: delta += step
            if code & 2: delta += step >> 1
            if code & 1: delta += step >> 2
            pred = max(-32768, min(32767, pred - delta if code & 8 else pred + delta))
            idx = max(0, min(88, idx + IMA_INDEX[code & 7]))
            out.append(pred)
    return np.array(out, dtype=np.float64) / 32768


def wav(path):
    with wave.open(path) as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768, w.getframerate()


def seam(loop, sr):
    two = np.concatenate([loop, loop])
    env, fps = onset_env(two, sr, hop=int(sr / 100))
    k = int(len(loop) / sr * fps)
    around = env[max(0, k - 5):k + 6].max()
    return around / (np.percentile(env, 99) + 1e-9)


def main():
    for con in ("ds",):
        rate = 16384
        for name in ("lobby", "game"):
            src, sr = wav(os.path.join(ROOT, "audio/processed", con, f"{name}.wav"))
            ref = resample(src, sr, rate)
            data = open(os.path.join(ROOT, con, "audio", f"{name}.bin"), "rb").read()
            dec = ima_decode(data, len(ref))
            n = min(len(ref), len(dec))
            err = ref[:n] - dec[:n]
            snr = 10 * np.log10(np.sum(ref[:n] ** 2) / (np.sum(err ** 2) + 1e-12))
            print(f"{con}/{name}: ADPCM SNR {snr:.1f} dB")
    for con in ("gba", "ds", "psp"):
        for name in ("lobby", "game"):
            x, sr = wav(os.path.join(ROOT, "audio/processed", con, f"{name}.wav"))
            print(f"{con}/{name}: seam jolt {seam(x, sr):.2f}x the music's own 99th-percentile onset (under ~1.2 is clean)")


if __name__ == "__main__":
    main()
