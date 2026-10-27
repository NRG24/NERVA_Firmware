#!/usr/bin/env python3
"""
Score heart-rate algorithms offline against recorded PPG with a reference.

    python tools/hr_eval.py <recordings...>

Each recording is a raw-PPG CSV from hr_compare.py (*_ppg.csv, with its
*_hr.csv beside it giving the reference per second) or from ppg_sweep.py
(one "value" column; the reference is the mean in *_summary.csv).

Algorithms:
  old   a line-for-line model of src/hr.c as of v0.6.1 (integer EMAs,
        threshold crossing, median of 8 intervals). Its errors should match
        what the ring itself reported, which is how this harness is checked.
  new   the real src/hr.c, compiled with Clang (pip install ziglang) via
        tests/hr_replay.c. Pass -DHR_PMR_MIN=.. -DHR_UNIQ_PCT=.. to sweep its
        gates; --tables regenerates the constant tables in hr.c.

Reports, per recording: mean |error| in bpm over the seconds where the
algorithm gave a reading, coverage (share of seconds with a reading), and the
share of readings within 5 bpm of the reference.
"""

import csv
import glob
import os
import sys

import numpy as np

FS = 100


# --- the current firmware (src/hr.c), modelled in integers -----------------

def old_hr(raw):
    """Per-sample bpm_x10 from a model of hr.c's hr_update()."""
    BASE, SMOOTH = 6, 2
    FINGER, FLOOR, MIN_IV, SPREAD = 15000, 40, 4, 40
    out = []
    primed = False
    baseline = smooth = amplitude = last_min = last_max = 0
    above = False
    ticks = last_beat = 0
    ibi = []

    def med():
        return sorted(ibi)[len(ibi) // 2] if ibi else 0

    def bpm():
        m = med()
        if len(ibi) < MIN_IV or m == 0:
            return 0
        agree = sum(1 for v in ibi if abs(v - m) * 100 <= m * SPREAD)
        return (600 * FS) // m if agree >= MIN_IV else 0

    for x in raw:
        x = int(x)
        ticks += 1
        if not primed:
            baseline, smooth, last_min, last_max, primed = x, 0, 0, 0, True
            out.append(0)
            continue
        baseline += (x - baseline) >> BASE
        if baseline < FINGER:
            ibi, last_beat, above = [], 0, False
            out.append(0)
            continue
        smooth += ((x - baseline) - smooth) >> SMOOTH
        last_max, last_min = max(last_max, smooth), min(last_min, smooth)
        if ticks % FS == 0:
            amplitude += ((last_max - last_min) - amplitude) >> 1
            last_max = last_min = smooth
        thr = (amplitude * 2) // 20
        refr = (FS * 60) // 220
        if len(ibi) >= 2:
            refr = max(refr, (med() * 6) // 10)
        if not above and smooth > thr and amplitude > FLOOR:
            above = True
            since = ticks - last_beat
            if last_beat != 0 and since >= refr:
                b = (60 * FS) // since
                if 30 <= b <= 220:
                    ibi.append(since)
                    ibi = ibi[-8:]
            last_beat = ticks
        elif above and smooth < thr // 2:
            above = False
        if amplitude <= FLOOR:
            ibi = []
        out.append(bpm())
    return np.array(out) / 10.0


# --- the replacement --------------------------------------------------------

def median5(x):
    p = np.pad(x, 2, mode="edge")
    return np.median(np.lib.stride_tricks.sliding_window_view(p, 5), axis=1)


def acf_hr(raw, win_s=8, hop_s=1, conf_min=0.35, keep=5):
    """Per-second bpm (0 = no reading) and confidence."""
    x = median5(np.asarray(raw, float))
    # Decimate 100 -> 50 Hz by pair averaging.
    n = len(x) // 2 * 2
    y = x[:n].reshape(-1, 2).mean(axis=1)
    fs = FS / 2
    # Two cascaded one-pole high-passes at ~0.5 Hz kill drift; a one-pole
    # low-pass at ~4 Hz kills what the median left.
    a_hp = 1 / 16.0
    b1 = b2 = y[0]
    lp = 0.0
    bp = np.empty_like(y)
    for i, v in enumerate(y):
        b1 += (v - b1) * a_hp
        h1 = v - b1
        b2 += (h1 - b2) * a_hp
        h2 = h1 - b2
        lp += (h2 - lp) * 0.4
        bp[i] = lp
    W, H = int(win_s * fs), int(hop_s * fs)
    lag_min, lag_max = int(fs * 60 / 200), int(fs * 60 / 40)
    est, conf, recent = [], [], []
    for end in range(W, len(bp) + 1, H):
        s = bp[end - W:end]
        s = s - s.mean()
        e = np.dot(s, s)
        if e <= 0:
            est.append(0), conf.append(0)
            continue
        r = np.array([np.dot(s[:-L], s[L:]) for L in range(lag_min, lag_max + 2)]) / e
        lags = np.arange(lag_min, lag_max + 2)
        peaks = [i for i in range(1, len(r) - 1) if r[i] >= r[i - 1] and r[i] > r[i + 1]]
        if not peaks:
            est.append(0), conf.append(0)
            continue
        best = max(peaks, key=lambda i: r[i])
        # Harmonic check: the shortest-lag peak that is nearly as strong is
        # the true period; a longer winner is usually 2x it (half rate).
        for i in peaks:
            if r[i] >= 0.8 * r[best]:
                best = i
                break
        c = r[best]
        # Parabolic refinement of the lag.
        den = r[best - 1] - 2 * r[best] + r[best + 1]
        d = 0.5 * (r[best - 1] - r[best + 1]) / den if den != 0 else 0
        lag = lags[best] + d
        b = 60 * fs / lag
        recent.append(b if c >= conf_min else None)
        recent = recent[-keep:]
        good = [v for v in recent if v]
        est.append(float(np.median(good)) if len(good) >= 3 else 0)
        conf.append(c)
    return np.array(est), np.array(conf)


# --- recordings and scoring -------------------------------------------------

def load(path):
    rows = list(csv.DictReader(open(path)))
    v = np.array([float(r["value"]) for r in rows])
    ref = None
    hr_path = path.replace("_ppg.csv", "_hr.csv")
    if path.endswith("_ppg.csv") and os.path.exists(hr_path):
        t0 = float(rows[0]["t_rx_s"])
        hr = list(csv.DictReader(open(hr_path)))
        pts = [(float(r["t_s"]) - t0, float(r["ref_bpm"])) for r in hr if r["ref_bpm"]]
        ref = lambda t: min(pts, key=lambda p: abs(p[0] - t))[1]
    else:
        stamp = os.path.basename(path).split("_")[1:3]
        summ = glob.glob(os.path.join(os.path.dirname(path) or ".",
                                      "ppgsweep_%s_%s_summary.csv" % tuple(stamp)))
        label = os.path.basename(path)[len("ppgsweep_%s_%s_" % tuple(stamp)):-4]
        if summ:
            for r in csv.DictReader(open(summ[0])):
                if r["label"].replace(" ", "_").replace(",", "") == label and r["ref_bpm"]:
                    val = float(r["ref_bpm"])
                    ref = lambda t, val=val: val
    return v, ref


def score(per_sec, ref, t0=0.0):
    errs, n = [], 0
    for k, b in enumerate(per_sec):
        n += 1
        if b:
            errs.append(abs(b - ref(t0 + k)))
    if not errs:
        return None, 0.0, 0.0
    e = np.array(errs)
    return float(e.mean()), 100 * len(e) / max(n, 1), 100 * float(np.mean(e <= 5))


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build_replay(defs=()):
    """Compile tests/hr_replay.c against src/hr.c; returns the exe path."""
    import subprocess
    import tempfile
    exe = os.path.join(tempfile.gettempdir(), "hr_replay_%d.exe" % (abs(hash(defs)) % 10**8))
    cmd = [sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O2",
           "-I" + os.path.join(REPO, "tests", "stubs"), "-I" + os.path.join(REPO, "src"),
           *defs, "-o", exe, os.path.join(REPO, "tests", "hr_replay.c"),
           os.path.join(REPO, "src", "hr.c")]
    subprocess.run(cmd, check=True)
    return exe


def c_hr(v, exe):
    """Per-second bpm from the real C detector; element k is second k+1."""
    import subprocess
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(str(int(x)) for x in v))
        path = f.name
    out = subprocess.run([exe, path], capture_output=True, text=True, check=True).stdout
    os.unlink(path)
    return np.array([int(l.split()[2]) / 10.0 for l in out.splitlines() if l.startswith("S ")])


def main(paths, defs=()):
    exe = build_replay(tuple(defs))
    print("%-42s %6s | %-20s | %-20s" % ("recording", "secs", "old: err cov% ok%", "new C: err cov% ok%"))
    tot = {"old": [], "new": []}
    for p in paths:
        v, ref = load(p)
        if ref is None or len(v) < 1200:
            continue
        old = old_hr(v)[FS - 1::FS]             # one reading a second
        new = c_hr(v, exe)
        o = score(old, ref, 1)
        a = score(new, ref, 1)
        for k, sc in (("old", o), ("new", a)):
            if sc[0] is not None:
                tot[k].append(sc + (len(v) // FS,))
        fmt = lambda sc: "%5.1f %4.0f %4.0f" % sc if sc[0] is not None else "   -- no reading  "
        print("%-42s %6d | %-20s | %-20s" % (os.path.basename(p)[:42], len(v) // FS, fmt(o), fmt(a)))
    for k in tot:
        if tot[k]:
            arr = np.array(tot[k])
            w = arr[:, 3]
            print("%-3s time-weighted: err %.1f bpm, coverage %.0f%%, within 5 bpm %.0f%%" % (
                k, np.average(arr[:, 0], weights=w), np.average(arr[:, 1], weights=w),
                np.average(arr[:, 2], weights=w)))


def tables():
    """The Goertzel and Hann tables in src/hr.c, regenerated."""
    import math
    coef = [round(2 * math.cos(2 * math.pi * (0.70 + 0.025 * k) / 25) * 16384) for k in range(105)]
    hann = [round(32767 * (0.5 - 0.5 * math.cos(2 * math.pi * n / 249))) for n in range(125)]
    print("goertzel_coef:", coef)
    print("hann_half:", hann)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["--tables"]:
        tables()
    else:
        defs = [a for a in args if a.startswith("-D")]
        main([a for a in args if not a.startswith("-D")], defs)
