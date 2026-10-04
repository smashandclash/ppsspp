#!/usr/bin/env python3
"""Turns the generated masters (audio/masters/<console>/*.mp3) into each console's sound:

  * the lobby and game themes become seamless loops: cut on the bar from the first downbeat,
    a whole number of bars long (lined up by cross-correlation), the seam crossfaded;
  * the jingles and sound effects are trimmed to the sound and levelled;
  * then encoded the way each console plays them:
      GBA  8-bit PCM at 13379 Hz for DirectSound (224 samples a frame, loops a whole number of frames)
      DS   IMA-ADPCM at 16384 Hz for the sound hardware (music and jingles), 8-bit PCM for effects
      PSP  16-bit PCM at 22050 Hz, mixed by the game

  python tools/audio/make_audio.py            (needs numpy and ffmpeg on the PATH)

Writes <console>/audio/ (the binaries, audio.s and audio.h, committed) and audio/processed/ (WAVs).
"""
import os
import struct
import subprocess
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SR = 44100
CONSOLES = ("gba", "ds", "psp")
MUSIC = ("lobby", "game")
JINGLES = ("win", "lose")
SFX = ("move", "select", "back", "place", "capture", "turn", "error", "start")
BARS = {"lobby": 24, "game": 24}  # the longest loop wanted, in bars
BPM_HINT = {("gba", "lobby"): 140, ("gba", "game"): 118, ("ds", "lobby"): 128, ("ds", "game"): 112, ("psp", "lobby"): 124, ("psp", "game"): 116}


def load(path, sr=SR):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def resample(x, sr_in, sr_out):
    """ffmpeg's resampler (with its anti-alias filter)."""
    p = subprocess.run(["ffmpeg", "-v", "error", "-f", "f32le", "-ar", str(sr_in), "-ac", "1", "-i", "-", "-af", "aresample=resampler=soxr:precision=28",
                        "-ar", str(sr_out), "-f", "f32le", "-"], input=x.astype(np.float32).tobytes(), capture_output=True, check=True).stdout
    return np.frombuffer(p, dtype=np.float32).astype(np.float64)


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def level(x, rms_db, peak_db=-1.0):
    """To a target loudness, then a soft limit so no peak passes peak_db."""
    x = x * (10 ** (rms_db / 20) / max(rms(x), 1e-9))
    ceiling = 10 ** (peak_db / 20)
    over = np.abs(x) > ceiling * 0.7
    if over.any():  # a gentle tanh knee above 70% of the ceiling
        k = ceiling * 0.7
        s = np.sign(x[over])
        a = np.abs(x[over]) - k
        x[over] = s * (k + (ceiling - k) * np.tanh(a / (ceiling - k)))
    return x


def onset_env(x, sr, hop=441):
    n = 2048
    frames = 1 + (len(x) - n) // hop
    win = np.hanning(n)
    idx = np.arange(n)[None, :] + hop * np.arange(frames)[:, None]
    spec = np.abs(np.fft.rfft(x[idx] * win, axis=1))
    flux = np.maximum(0, np.diff(np.log1p(spec * 10), axis=0)).sum(axis=1)
    flux = np.concatenate([[0], flux])
    return flux - flux.mean(), sr / hop


def beat_grid(x, sr, bpm_hint):
    env, fps = onset_env(x, sr)
    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    lags = np.arange(len(ac))
    lo, hi = fps * 60 / (bpm_hint * 1.06), fps * 60 / (bpm_hint * 0.94)
    sel = (lags >= lo) & (lags <= hi)
    lag = lags[sel][np.argmax(ac[sel])]
    # refine to a fraction of a frame with a parabola through the peak
    a, b, c = ac[lag - 1], ac[lag], ac[lag + 1]
    lag = lag + 0.5 * (a - c) / (a - 2 * b + c + 1e-12)
    beat = lag / fps  # seconds
    # the phase that puts the most onset energy on the grid, then the bar's downbeat
    best, phase = -1e18, 0.0
    for ph in np.linspace(0, beat, 48, endpoint=False):
        t = np.arange(ph, len(x) / sr - 0.1, beat)
        s = env[np.clip((t * fps).astype(int), 0, len(env) - 1)].sum()
        if s > best:
            best, phase = s, ph
    beats = np.arange(phase, len(x) / sr, beat)
    strength = [env[np.clip((beats[k::4] * fps).astype(int), 0, len(env) - 1)].sum() for k in range(4)]
    down = int(np.argmax(strength))
    return beat, beats[down::4]


def make_loop(x, sr, bpm_hint, max_bars):
    beat, downs = beat_grid(x, sr, bpm_hint)
    bar = beat * 4
    # where the music is still at full strength (a generated track may fade at its end)
    hop = int(0.25 * sr)
    env = np.array([rms(x[i:i + hop]) for i in range(0, len(x) - hop, hop)])
    full = np.median(env)
    alive = np.where(env > full * 0.35)[0]
    end_ok = (alive[-1] + 1) * hop / sr if len(alive) else len(x) / sr
    start = next((d for d in downs if d >= 0.25), downs[0])
    xf = 0.08  # the seam's crossfade
    bars = max_bars
    while bars > 4 and start + bars * bar + xf + 0.2 > end_ok:
        bars -= 1
    s0 = int(round(start * sr))
    L = int(round(bars * bar * sr))
    # line the loop up exactly: the length that best repeats the opening two seconds
    w = int(2.0 * sr)
    head = x[s0:s0 + w]
    best, bestL = -1e18, L
    for d in range(-int(0.03 * sr), int(0.03 * sr) + 1, 8):
        seg = x[s0 + L + d:s0 + L + d + w]
        if len(seg) < w:
            continue
        c = float(np.dot(head, seg))
        if c > best:
            best, bestL = c, L + d
    for d in range(bestL - 8, bestL + 9):  # then to the sample
        seg = x[s0 + d:s0 + d + w]
        if len(seg) == w:
            c = float(np.dot(head, seg))
            if c > best:
                best, bestL = c, d
    L = bestL
    n = int(xf * sr)
    seg = x[s0:s0 + L + n].copy()
    t = np.linspace(0, np.pi / 2, n)
    loop = seg[:L].copy()
    loop[:n] = seg[:n] * np.sin(t) + seg[L:L + n] * np.cos(t)  # the tail flows into the head
    return loop, 60 / beat, bars, start


def trim(x, sr, floor_db=-42, tail_db=-55, fade=0.03):
    peak = np.abs(x).max() + 1e-12
    on = np.where(np.abs(x) > peak * 10 ** (floor_db / 20))[0]
    if not len(on):
        return x
    tail = np.where(np.abs(x) > peak * 10 ** (tail_db / 20))[0]
    a, b = max(0, on[0] - int(0.002 * sr)), min(len(x), tail[-1] + 1)
    x = x[a:b].copy()
    n = min(len(x) // 4, int(fade * sr))
    if n:
        x[-n:] *= np.linspace(1, 0, n) ** 2
    m = min(len(x) // 8, int(0.002 * sr))
    if m:
        x[:m] *= np.linspace(0, 1, m)
    return x


def write_wav(path, x, sr):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())


def dither(x, bits):
    lsb = 2.0 / (1 << bits)
    return x + (np.random.random_sample(len(x)) - np.random.random_sample(len(x))) * lsb


def pcm8(x):
    return np.clip(np.round(dither(x, 8) * 127), -128, 127).astype(np.int8).tobytes()


def pcm16(x):
    return np.clip(np.round(dither(x, 16) * 32767), -32768, 32767).astype("<i2").tobytes()


IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
             209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
             2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
             22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_adpcm(x):
    """IMA-ADPCM the Nintendo DS way: a 4-byte header (the first sample, the step index, 0),
    then 4-bit codes, low nibble first. Returns bytes, a multiple of 4 long."""
    s = np.clip(np.round(dither(x, 16) * 32767), -32768, 32767).astype(int)
    pred, idx = int(s[0]), 0
    out = bytearray(struct.pack("<hBB", pred, idx, 0))
    codes = []
    for v in s[1:]:
        step = IMA_STEPS[idx]
        # the code whose result lands nearest (the standard bit test, but never worse)
        best, bcode, bpred = None, 0, pred
        for code in range(16):
            delta = step >> 3
            if code & 4: delta += step
            if code & 2: delta += step >> 1
            if code & 1: delta += step >> 2
            p = max(-32768, min(32767, pred - delta if code & 8 else pred + delta))
            e = abs(v - p)
            if best is None or e < best:
                best, bcode, bpred = e, code, p
        pred = bpred
        idx = max(0, min(88, idx + IMA_INDEX[bcode & 7]))
        codes.append(bcode)
    while (len(codes) % 8) != 0:  # whole words
        codes.append(0)
    for i in range(0, len(codes), 2):
        out.append(codes[i] | (codes[i + 1] << 4))
    return bytes(out)


def emit(con, entries):
    """<con>/audio/: the binaries, audio.s (.incbin) and audio.h (symbols and sizes)."""
    out = os.path.join(ROOT, con, "audio")
    os.makedirs(out, exist_ok=True)
    s = ["/* The sound, pulled in whole (generated by tools/audio/make_audio.py). */"]
    h = ["// Generated by tools/audio/make_audio.py: do not edit.", "#ifndef SNC_AUDIO_H", "#define SNC_AUDIO_H", "#include <stdint.h>", ""]
    for name, data, rate, samples, kind in entries:
        fname = f"{name}.bin"
        with open(os.path.join(out, fname), "wb") as f:
            f.write(data)
        sym = f"snd_{name}"
        s.append(f'    .section .rodata.{sym},"a"\n    .global {sym}\n    .balign 4\n{sym}:\n    .incbin "{con}_audio_dir/{fname}"')
        h.append(f"extern const uint8_t {sym}[];  // {kind}")
        h.append(f"#define {sym.upper()}_BYTES {len(data)}\n#define {sym.upper()}_RATE {rate}\n#define {sym.upper()}_SAMPLES {samples}")
    h.append("\n#endif")
    with open(os.path.join(out, "audio.h"), "w", newline="\n") as f:
        f.write("\n".join(h) + "\n")
    with open(os.path.join(out, "audio.s"), "w", newline="\n") as f:
        f.write("\n".join(s).replace(f"{con}_audio_dir", "audio") + "\n")
    print(f"{con}: {sum(len(e[1]) for e in entries) // 1024} KB in {len(entries)} sounds -> {out}")


def main():
    np.random.seed(7)
    for con in CONSOLES:
        src = os.path.join(ROOT, "audio", "masters", con)
        proc = os.path.join(ROOT, "audio", "processed", con)
        sounds = {}
        for name in MUSIC:
            x = load(os.path.join(src, f"{name}.mp3"))
            loop, bpm, bars, start = make_loop(x, SR, BPM_HINT[(con, name)], BARS[name])
            loop = level(loop, -19.0, -1.5)
            sounds[name] = loop
            print(f"  {con}/{name}: {bpm:.1f} BPM, {bars} bars from {start:.2f} s = {len(loop) / SR:.2f} s loop")
        for name in JINGLES:
            sounds[name] = level(trim(load(os.path.join(src, f"{name}.mp3")), SR, -40, -60, 0.25), -17.0, -1.0)
        for name in SFX:
            sounds["sfx_" + name] = level(trim(load(os.path.join(src, f"sfx-{name}.mp3")), SR), -15.0, -1.0)
        for k, v in sounds.items():
            write_wav(os.path.join(proc, f"{k}.wav"), v, SR)

        entries = []
        if con == "gba":
            rate = 13379
            for k, v in sounds.items():
                y = resample(v, SR, rate)
                if k in MUSIC:  # a whole number of frames (224 samples each), stretched by a hair to fit
                    n = int(round(len(y) / 224)) * 224
                    y = np.interp(np.linspace(0, len(y), n, endpoint=False), np.arange(len(y)), y)
                else:
                    y = np.concatenate([y, np.zeros((-len(y)) % 224)])
                entries.append((k, pcm8(y), rate, len(y), "8-bit PCM"))
        elif con == "ds":
            for k, v in sounds.items():
                if k.startswith("sfx_") or k in JINGLES:  # short: 8-bit PCM sounds cleaner than ADPCM
                    y = resample(v, SR, 16384)
                    y = np.concatenate([y, np.zeros((-len(y)) % 4)])
                    entries.append((k, pcm8(y), 16384, len(y), "8-bit PCM"))
                else:
                    y = resample(v, SR, 16384)
                    entries.append((k, ima_adpcm(y), 16384, len(y), "IMA-ADPCM"))
        else:  # the PSP has the memory for clean 16-bit sound
            for k, v in sounds.items():
                y = resample(v, SR, 22050)
                y = np.concatenate([y, np.zeros((-len(y)) % 2)])
                entries.append((k, pcm16(y), 22050, len(y), "16-bit PCM"))
        emit(con, entries)


if __name__ == "__main__":
    main()
