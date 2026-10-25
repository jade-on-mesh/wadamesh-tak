#!/usr/bin/env python3
"""Reference for VoxBrighten (src/ui-touch/VoxVoice.cpp): the "Brighter voice"
bandwidth extension for Codec2 played on a standalone.

Codec2 stops at 4 kHz. This makes a 4-8 kHz band from the decoded voice itself:
the 2-3.9 kHz band, full-wave rectified (|x| puts energy at sums of its
frequencies, 4-7.8 kHz), high-passed at 4 kHz and split into four sub-bands.
Every 10 ms each sub-band gets a gain so its level lands where a rule says real
speech would be:

    target_k = m + a_k + b_k * (m - l) + c_k * (l + 30)        [dBFS]

m = level of the 2-3.9 kHz band, l = level of 300-1000 Hz (so m - l is the
tilt: fricatives are bright, vowels dark). The 12 numbers are fitted on wideband
speech run through Codec2 (`fit`). A tiny neural net can later replace the
rule; it sees the same features and gives the same four targets.

All filters are RBJ biquads at the output rate, so the same code runs at
16 kHz (T-Deck, Pager) or 44.1/48 kHz. One frame (10 ms) of latency: a frame's
gains are known when it ends, and it is played ramping from the previous
frame's gains to its own.

Usage:
  brighten_ref.py fit CLIP...              fit the rule (leave-one-out report)
  brighten_ref.py run IN.wav OUT.wav       Codec2 1200 round trip, then brighten
  brighten_ref.py golden CLIP DIR          the host test's input and output
(C2ROUND names the c2round binary: scripts/voice/c2round.c.)
"""
import math
import os
import subprocess
import sys

import numpy as np
from scipy.signal import lfilter, resample_poly

RATE = 16000
SILENCE_DB = -60.0
MAX_GAIN_DB = 40.0
BANDS = ((4000, 5000), (5000, 6000), (6000, 7000), (7000, 7900))

# Fitted by `fit` (ENERGY_MATCH 0.5) on LibriSpeech 198-209-0000, 3436-172162-0000, 5703-47212-0000
# (CC-BY-4.0) through Codec2 1200. Rows: a, b, c per sub-band.
COEF = np.array([
    [-7.835, -0.244, -0.636],
    [-15.876, -0.229, -0.845],
    [-21.726, -0.396, -0.946],
    [-33.911, -0.483, -0.981],
])


# ------------------------------------------------------------------ filters

def rbj(kind, f, rate, q=0.70710678):
    w0 = 2 * math.pi * f / rate
    c, s = math.cos(w0), math.sin(w0)
    al = s / (2 * q)
    if kind == "lp":
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
    elif kind == "hp":
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    else:  # band-pass, 0 dB peak
        b = [al, 0.0, -al]
    a = [1 + al, -2 * c, 1 - al]
    return np.array(b) / a[0], np.array(a) / a[0]


def band(lo, hi, rate):
    fc = math.sqrt(lo * hi)
    return rbj("bp", fc, rate, q=fc / (hi - lo))


def run(chain, x):
    for b, a in chain:
        x = lfilter(b, a, x)
    return x


def chains(rate):
    hp = lambda f: rbj("hp", f, rate)
    lp = lambda f: rbj("lp", f, rate)
    return {
        "src": [hp(2000), hp(2000), lp(3900), lp(3900)],
        "lo": [hp(300), lp(1000)],
        "dc": [hp(100)],
        "hb": [hp(4000), hp(4000)],
        "bands": [[band(lo, hi, rate)] * 2 for lo, hi in BANDS],
    }


def frames_db(x, n):
    k = x.size // n
    e = (x[: k * n].reshape(k, n) ** 2).mean(1)
    return 10 * np.log10(e / 32768.0 ** 2 + 1e-12)


def analyse(x, rate=RATE):
    """x: PCM16-scale floats at `rate`. Returns per-frame m, l, the excitation
    sub-band signals and their per-frame levels."""
    ch = chains(rate)
    n = rate // 100
    s = run(ch["src"], x)
    l = run(ch["lo"], x)
    e = run(ch["hb"], run(ch["dc"], np.abs(s)))
    bands = [run(c, e) for c in ch["bands"]]
    return frames_db(s, n), frames_db(l, n), bands, [frames_db(b, n) for b in bands]


def targets(m, l, coef):
    return np.stack([m + a + b * (m - l) + c * (l + 30) for a, b, c in coef], 1)


def brighten(x, coef=None, rate=RATE):
    """The whole effect, without the frame of latency (out[i] here is the
    firmware's out[i + rate/100])."""
    coef = COEF if coef is None else coef
    n = rate // 100
    m, l, bands, lev = analyse(x, rate)
    t = targets(m, l, coef)
    g_db = np.minimum(t - np.stack(lev, 1), MAX_GAIN_DB)
    g = 10 ** (g_db / 20)
    g[m < SILENCE_DB] = 0
    k = m.size
    prev = np.vstack([np.zeros((1, 4)), g[:-1]])
    ramp = (np.arange(1, n + 1) / n)[None, :, None]
    gs = (prev[:, None, :] + (g - prev)[:, None, :] * ramp).reshape(k * n, 4)
    y = x[: k * n].copy()
    for j in range(4):
        y += gs[:, j] * bands[j][: k * n]
    return soft_limit(y)


def soft_limit(v, knee=24000.0, ceil=32000.0):
    m = np.abs(v)
    e = np.maximum(m - knee, 0)
    r = np.where(m > knee, knee + (ceil - knee) * e / (e + (ceil - knee)), m)
    return np.sign(v) * r


# ------------------------------------------------- the playback chain before it

def play_out(pcm8k, rate=RATE):
    """VoxPlayOut: gain (voxPlayNormGain), linear interpolation 8 kHz -> rate,
    RBJ low-pass at 3.6 kHz, soft limit."""
    pk = np.abs(pcm8k[: 3 * 8000]).max() if pcm8k.size else 0
    gain = 1.0 if pk <= 0 else float(np.clip(23000.0 / pk, 1, 6))
    cur = pcm8k.astype(np.float64) * gain
    step = 8000 / rate
    t = np.arange(int(cur.size / step)) * step
    i = np.floor(t).astype(int)
    fr = t - i
    prev = np.concatenate([[0.0], cur])
    y = prev[i] + (prev[i + 1] - prev[i]) * fr
    b, a = rbj("lp", 3600, rate)
    return soft_limit(lfilter(b, a, y))


def codec2(x8, mode=5, tool=None):
    """Codec2 encode+decode of 8 kHz floats (-1..1) with a c2round binary."""
    tool = tool or os.environ.get("C2ROUND", "c2round")
    pcm = (np.clip(x8, -1, 1) * 32767).astype("<i2").tobytes()
    r = subprocess.run([tool, str(mode), "-", "-"], input=pcm, capture_output=True, check=True)
    return np.frombuffer(r.stdout, "<i2").astype(np.int16)


def load16(path):
    import soundfile as sf
    x, sr = sf.read(path, dtype="float64")
    if x.ndim > 1:
        x = x.mean(1)
    x = resample_poly(x, RATE, sr)
    return x / (np.abs(x).max() + 1e-9) * 0.9


def pair(path):
    """(codec2 played out at 16 kHz, the wideband original at the same gain)."""
    w = load16(path)
    c8 = codec2(resample_poly(w, 1, 2))
    y = play_out(c8)
    pk = np.abs(c8[: 3 * 8000]).max()
    gain = float(np.clip(23000.0 / pk, 1, 6)) if pk > 0 else 1.0
    # Codec2 keeps the level of what it's given; the original is scaled the
    # way the decoded clip is, then delayed like it (best lag by correlation).
    ref = w * 32767 * gain
    n = min(y.size, ref.size) - 800
    lp = rbj("lp", 1500, RATE)
    yl, rl = lfilter(*lp, y[:n]), lfilter(*lp, ref[: n + 800])
    lag = int(np.argmax([np.dot(yl[800:], rl[800 - d: n - d]) for d in range(0, 800)]))
    if os.environ.get("BRIGHTEN_DEBUG"):
        print(f"  {os.path.basename(path)}: codec lag {lag} samples")
    ref = np.concatenate([np.zeros(lag), ref])[: n]   # delayed by the codec's lag
    return y[:n], ref


def fit_rows(paths, rate=RATE):
    X, Y = [], []
    ch = chains(rate)
    for p in paths:
        y, ref = pair(p)
        m, l, _, _ = analyse(y, rate)
        true = np.stack([frames_db(run(c, ref), rate // 100) for c in ch["bands"]], 1)
        k = min(m.size, true.shape[0])
        keep = m[:k] > -50
        X.append(np.c_[np.ones(keep.sum()), (m - l)[:k][keep], (l + 30)[:k][keep]])
        Y.append((true[:k] - m[:k, None])[keep])
    return np.vstack(X), np.vstack(Y)


# A least-squares fit in dB lands on the typical frame, which is ~10 dB under
# real speech's average treble energy (frames vary by ~10 dB, and the loud
# fricatives carry most of it). Each band's offset is raised by this share of
# that gap: 0.5 puts the treble about where the first prototype had it (the
# "file 3" clips, which were picked over the AI model's), 1 would match the
# average energy.
ENERGY_MATCH = 0.5


def fit(paths):
    X, Y = fit_rows(paths)
    coef = np.linalg.lstsq(X, Y, rcond=None)[0].T   # [4, 3]
    pred = X @ coef.T
    gap = 10 * np.log10((10 ** (Y / 10)).mean(0)) - 10 * np.log10((10 ** (pred / 10)).mean(0))
    coef[:, 0] += ENERGY_MATCH * gap
    rms = np.sqrt(((X @ coef.T - Y) ** 2).mean(0))
    return coef, rms


def band_error(paths, coef):
    """RMS dB error of the sub-band levels the output gets, on active frames."""
    errs = []
    ch = chains(RATE)
    for p in paths:
        y, ref = pair(p)
        out = brighten(y, coef)
        n = RATE // 100
        m = frames_db(run(ch["src"], y), n)
        got = np.stack([frames_db(run(c, out), n) for c in ch["bands"]], 1)
        want = np.stack([frames_db(run(c, ref[: out.size]), n) for c in ch["bands"]], 1)
        none = np.stack([frames_db(run(c, y[: out.size]), n) for c in ch["bands"]], 1)
        k = min(got.shape[0], want.shape[0], m.size)
        act = m[:k] > -50
        errs.append((np.sqrt(((got - want)[:k][act] ** 2).mean()),
                     np.sqrt(((none - want)[:k][act] ** 2).mean())))
    return errs


if __name__ == "__main__":
    if sys.argv[1] == "fit":
        paths = sys.argv[2:]
        for i, p in enumerate(paths):
            coef, _ = fit([q for j, q in enumerate(paths) if j != i])
            (e, e0), = band_error([p], coef)
            print(f"held out {os.path.basename(p)}: sub-band level error {e:.1f} dB (as now: {e0:.1f} dB)")
        coef, rms = fit(paths)
        print("COEF (all clips):")
        for row in coef:
            print("  {%.3ff, %.3ff, %.3ff}," % tuple(row))
        print("fit rms per band (dB):", np.round(rms, 1))
    elif sys.argv[1] == "golden":
        # test/data/vox/brighten_{in,out}.raw: 1.5 s of CLIP with the most
        # treble, through Codec2 1200 and VoxPlayOut, and this module's output.
        from scipy.signal import lfilter as _lf
        y, ref = pair(sys.argv[2])
        e = _lf(*rbj("hp", 4000, RATE), ref) ** 2
        n = 24000
        best = max(range(0, y.size - n, 1600), key=lambda i: e[i:i + n].sum())
        x = np.round(y[best:best + n]).astype("<i2")
        np.round(brighten(x.astype(np.float64))).astype("<i2").tofile(os.path.join(sys.argv[3], "brighten_out.raw"))
        x.tofile(os.path.join(sys.argv[3], "brighten_in.raw"))
    else:
        import soundfile as sf
        coef = COEF
        y, _ = pair(sys.argv[2])
        sf.write(sys.argv[3], brighten(y, coef) / 32768, RATE)
