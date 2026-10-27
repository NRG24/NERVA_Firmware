#!/usr/bin/env python3
"""
Replay recorded PPG through two revisions of src/hr.c and compare them:
heart rate against the reference, and HRV (RMSSD) side by side.

    python tools/replay_ab.py --base b3763f5 recordings/*_ppg.csv
    python tools/replay_ab.py --base HEAD~3 --head HEAD rec_ppg.csv
    python tools/replay_ab.py --synthetic out/            # make test recordings

Any algorithm change to hr.c should be run through this on real
recordings before it is flashed: it answers "did heart rate get worse"
and "what did HRV do" in a minute, against the same data, without a ring.

Revisions are git refs; WORKTREE (the default --head) means the files on
disk, uncommitted changes included. Each side is compiled from that
revision's src/hr.c and src/hr.h with tests/hr_replay.c (Clang from
`pip install ziglang`, as tests/run_tests.py uses).

Recordings are the formats tools/hr_eval.py reads: hr_compare.py's
*_ppg.csv with *_hr.csv beside it (reference HR per second), or
ppg_sweep.py's. A *_rr.csv beside a recording (written by --synthetic)
gives the true beat intervals, and then RMSSD is scored as well.

HRV here is the textbook figure the ring's spot readings report: RMSSD
over each 60 s of trusted, successive intervals (10 differences or more),
summarised as the median across windows.
"""

import argparse
import csv
import math
import os
import random
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hr_eval  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FS = 100
WIN_S = 60


# --- building one revision -----------------------------------------------------

def source(rev, path):
    if rev == "WORKTREE":
        return open(os.path.join(REPO, path), encoding="utf-8").read()
    return subprocess.run(["git", "show", "%s:%s" % (rev, path)], cwd=REPO,
                          capture_output=True, text=True, check=True).stdout


def build(rev):
    d = tempfile.mkdtemp(prefix="replay_%s_" % rev.replace("~", "_").replace("/", "_"))
    for f in ("src/hr.c", "src/hr.h"):
        open(os.path.join(d, os.path.basename(f)), "w", encoding="utf-8").write(source(rev, f))
    exe = os.path.join(d, "hr_replay.exe")
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O2", "-w",
                    "-I" + d, "-I" + os.path.join(REPO, "tests", "stubs"),
                    "-o", exe, os.path.join(REPO, "tests", "hr_replay.c"),
                    os.path.join(d, "hr.c")], check=True)
    return exe


def replay(exe, values):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(str(int(x)) for x in values))
        path = f.name
    out = subprocess.run([exe, path], capture_output=True, text=True, check=True).stdout
    os.unlink(path)
    per_sec, beats = [], []
    for line in out.splitlines():
        p = line.split()
        if p[0] == "S":
            per_sec.append(int(p[2]) / 10.0)
        elif p[0] == "B":
            # sample, ibi ms, trusted, successive
            beats.append((int(p[1]), int(p[2]), p[3] == "1", p[4] == "1"))
    return np.array(per_sec), beats


# --- HRV ----------------------------------------------------------------------------

def windowed_rmssd(beats, n_samples):
    """RMSSD per 60 s window from replay beats; None where under 10 diffs."""
    out = []
    for w0 in range(0, n_samples - WIN_S * FS + 1, WIN_S * FS):
        prev = None
        diffs = []
        for sample, ibi, trusted, successive in beats:
            if not w0 <= sample < w0 + WIN_S * FS:
                continue
            if not trusted:
                prev = None
                continue
            if successive and prev is not None:
                diffs.append(ibi - prev)
            prev = ibi
        out.append(math.sqrt(sum(d * d for d in diffs) / len(diffs))
                   if len(diffs) >= 10 else None)
    return out


def true_rmssd(rr_ms, n_samples):
    """The same windows over the true intervals, by beat start time."""
    t = np.cumsum([0.0] + list(rr_ms))
    out = []
    for w0 in range(0, n_samples - WIN_S * FS + 1, WIN_S * FS):
        lo, hi = w0 * 10.0, (w0 + WIN_S * FS) * 10.0
        rr = [r for r, s in zip(rr_ms, t[:-1]) if lo <= s < hi]
        d = np.diff(rr)
        out.append(float(np.sqrt(np.mean(d * d))) if len(d) >= 10 else None)
    return out


def med(xs):
    xs = [x for x in xs if x is not None]
    return float(np.median(xs)) if xs else None


# --- synthetic recordings --------------------------------------------------------

def synthesise(outdir, seed=7):
    """
    Three 5-minute recordings in hr_compare.py's format, from a heart with
    known intervals: steady with breathing (RSA), low HRV, and a noisy one.
    Drift and white noise on top, DC and swing as the board measured.
    """
    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(seed)
    cases = [("rsa", 62.0, 25.0, 0.4), ("lowhrv", 70.0, 4.0, 0.4),
             ("noisy", 58.0, 30.0, 1.5)]
    paths = []
    for name, bpm, rsa_ms, noise_pct in cases:
        mean_rr = 60000.0 / bpm
        rr = []
        t = 0.0
        while t < 300000.0:
            v = mean_rr + rsa_ms * math.sin(2 * math.pi * t / 4000.0) + rng.gauss(0, rsa_ms / 5)
            rr.append(v)
            t += v
        beat_t = np.cumsum([0.0] + rr)
        n = int(300 * FS)
        vals = []
        k = 0
        for i in range(n):
            ts = i * 10.0
            while k + 1 < len(beat_t) - 1 and beat_t[k + 1] <= ts:
                k += 1
            ph = 2 * math.pi * (ts - beat_t[k]) / (beat_t[k + 1] - beat_t[k])
            pulse = 0.8 * math.sin(ph) + 0.25 * math.sin(2 * ph + 0.9)
            drift = 2500 * math.sin(2 * math.pi * ts / 23000.0)
            vals.append(int(53000 + 1500 * pulse + drift +
                            rng.gauss(0, 1500 * noise_pct / 100)))
        base = os.path.join(outdir, "synthetic_%s" % name)
        with open(base + "_ppg.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_rx_s", "value"])
            for i, v in enumerate(vals):
                w.writerow(["%.2f" % (i / FS), v])
        with open(base + "_hr.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "ref_bpm"])
            for s in range(300):
                inst = [r for r, st in zip(rr, beat_t[:-1]) if s * 1000 <= st < (s + 1) * 1000]
                w.writerow([s, "%.1f" % (60000.0 / np.mean(inst)) if inst else ""])
        with open(base + "_rr.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["rr_ms"])
            for r in rr:
                w.writerow(["%.2f" % r])
        paths.append(base + "_ppg.csv")
    return paths


# --- main ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("recordings", nargs="*")
    ap.add_argument("--base", default="HEAD", help="git ref (default HEAD)")
    ap.add_argument("--head", default="WORKTREE", help="git ref, or WORKTREE (default)")
    ap.add_argument("--synthetic", metavar="DIR",
                    help="write synthetic recordings to DIR and compare on them")
    args = ap.parse_args()

    paths = list(args.recordings)
    if args.synthetic:
        paths += synthesise(args.synthetic)
    if not paths:
        ap.error("give recordings, or --synthetic DIR")

    exes = {"base": build(args.base), "head": build(args.head)}
    print("base = %s, head = %s; HR as err bpm / coverage %% / within 5 bpm %%; "
          "HRV as median 60 s RMSSD, ms" % (args.base, args.head))
    print("%-30s %5s | %-17s | %-17s | %6s %6s %6s" % (
        "recording", "secs", "base HR", "head HR", "true", "base", "head"))
    for p in paths:
        v, ref = hr_eval.load(p)
        if len(v) < WIN_S * FS:
            continue
        rr_path = p.replace("_ppg.csv", "_rr.csv")
        truth = None
        if os.path.exists(rr_path):
            rr = [float(r["rr_ms"]) for r in csv.DictReader(open(rr_path))]
            truth = med(true_rmssd(rr, len(v)))
        cells = {}
        for side, exe in exes.items():
            per_sec, beats = replay(exe, v)
            hr = hr_eval.score(per_sec, ref, 1) if ref else (None, 0, 0)
            cells[side] = ("%5.1f %4.0f %4.0f" % hr if hr[0] is not None else "   no reading",
                           med(windowed_rmssd(beats, len(v))))
        f = lambda x: "%6.1f" % x if x is not None else "     -"
        print("%-30s %5d | %-17s | %-17s | %s %s %s" % (
            os.path.basename(p)[:30], len(v) // FS, cells["base"][0], cells["head"][0],
            f(truth), f(cells["base"][1]), f(cells["head"][1])))


if __name__ == "__main__":
    main()
