#!/usr/bin/env python3
"""
Score step counters against recordings from tools/steps_record.py.

    python tools/steps_eval.py steps_*.csv

For each recording, replays the accelerometer magnitude through two builds
of the step counter -- the current src/steps.c and the frozen test-firmware
copy in ../ring-fw-test-v0.6.2 -- and prints both counts against the truth
you typed in (0 for desk recordings), plus what the ring itself counted
live.

Needs Clang: pip install ziglang.
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FROZEN = os.path.join(os.path.dirname(REPO), "ring-fw-test-v0.6.2")


def build(src_dir, tag):
    exe = os.path.join(tempfile.gettempdir(), "steps_replay_%s.exe" % tag)
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O2",
                    "-I" + os.path.join(REPO, "tests", "stubs"), "-I" + os.path.join(src_dir, "src"),
                    "-o", exe, os.path.join(REPO, "tests", "steps_replay.c"),
                    os.path.join(src_dir, "src", "steps.c")], check=True)
    return exe


def load(path):
    truth, samples, ring = 0, [], []
    for line in open(path):
        if line.startswith("# truth="):
            truth = int(line.split("=")[1])
            continue
        parts = line.strip().split(",")
        if len(parts) < 6 or not parts[0].isdigit():
            continue
        samples.append((int(parts[0]), int(parts[4])))
        if parts[5]:
            ring.append(int(parts[5]))
    live = (ring[-1] - ring[0]) if len(ring) > 1 else None
    return truth, samples, live


def replay(exe, samples):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("".join("%d %d%s" % (t, mg, os.linesep) for t, mg in samples))
        path = f.name
    out = subprocess.run([exe, path], capture_output=True, text=True, check=True).stdout
    os.unlink(path)
    return int(out.split()[-1])


def main(paths):
    exes = {"new": build(REPO, "new")}
    if os.path.isdir(FROZEN):
        exes["frozen"] = build(FROZEN, "frozen")
    print("%-34s %6s %6s %8s %6s" % ("recording", "truth", "new", "frozen", "live"))
    for p in paths:
        truth, samples, live = load(p)
        counts = {k: replay(e, samples) for k, e in exes.items()}
        print("%-34s %6d %6d %8s %6s" % (os.path.basename(p)[:34], truth, counts["new"],
                                         counts.get("frozen", "-"), live if live is not None else "-"))


if __name__ == "__main__":
    main(sys.argv[1:])
