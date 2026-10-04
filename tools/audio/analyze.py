#!/usr/bin/env python3
"""Measures the generated audio: length, loudness, tempo, and how the start and end sound
(a loop must not fade in or out).   python tools/audio/analyze.py [audio/masters]"""
import glob
import os
import subprocess
import sys

import numpy as np

SR = 22050


def load(path, sr=SR):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def db(x):
    return 20 * np.log10(max(1e-9, float(np.sqrt(np.mean(x * x))))) if len(x) else -180


def onset_env(x, hop=512):
    n = 2048
    frames = 1 + (len(x) - n) // hop
    if frames < 4:
        return np.zeros(1), hop
    win = np.hanning(n).astype(np.float32)
    spec = np.abs(np.fft.rfft(np.stack([x[i * hop:i * hop + n] * win for i in range(frames)]), axis=1))
    flux = np.maximum(0, np.diff(np.log1p(spec * 10), axis=0)).sum(axis=1)
    return flux - flux.mean(), hop


def tempo(x):
    env, hop = onset_env(x)
    if len(env) < 64:
        return 0
    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    fps = SR / hop
    lo, hi = int(fps * 60 / 180), int(fps * 60 / 70)
    lag = lo + int(np.argmax(ac[lo:hi]))
    return 60 * fps / lag


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "audio/masters"
    for path in sorted(glob.glob(os.path.join(root, "*", "*.mp3"))):
        x = load(path)
        dur = len(x) / SR
        s1, e1 = x[:SR], x[-SR:]
        line = f"{os.path.relpath(path, root):22s} {dur:6.2f}s  rms {db(x):6.1f} dB  peak {20*np.log10(np.abs(x).max()+1e-9):5.1f}"
        if "sfx" not in path:
            line += f"  first1s {db(s1):6.1f}  last1s {db(e1):6.1f}  bpm {tempo(x):6.1f}"
        print(line)


if __name__ == "__main__":
    main()
