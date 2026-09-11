#!/usr/bin/env python3
"""
Retry a pyOCD flash until the hand-soldered SWD link gives us a window.

The wires on this board are tacked to pads and drop out constantly -- see
HANDOFF.md section 3. A single flash attempt failing means nothing; the
record so far is 56 attempts over 104 s. This just keeps trying and reports
how hard it had to work, so a genuinely dead link is distinguishable from
the usual flakiness.

    python scratchpad/flashloop.py [hex] [-n attempts] [-f freq [freq ...]]

Frequencies are tried round-robin. A sustained flash write is much more
demanding than the single register read that `pyocd commander` does, so a
link that connects fine can still fail to program -- dropping the clock
often buys the margin needed.
"""

import argparse
import re
import subprocess
import sys
import time

DEFAULT_HEX = "build/ring-fw/zephyr/zephyr.hex"
ERR = re.compile(r"No ACK|Unexpected ACK|communication failure|Error|Failed",
                 re.I)


def attempt(hexfile, freq):
    p = subprocess.run(
        [sys.executable, "-m", "pyocd", "flash", "-t", "nrf52833",
         "-f", str(freq), hexfile],
        capture_output=True, text=True, timeout=300)
    out = (p.stdout or "") + (p.stderr or "")
    return (p.returncode == 0 and not ERR.search(out)), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hexfile", nargs="?", default=DEFAULT_HEX)
    ap.add_argument("-n", "--attempts", type=int, default=60)
    ap.add_argument("-f", "--freq", type=int, nargs="+",
                    default=[1000000, 500000, 250000])
    args = ap.parse_args()

    start = time.time()
    tally = {}

    for i in range(1, args.attempts + 1):
        freq = args.freq[(i - 1) % len(args.freq)]
        try:
            ok, out = attempt(args.hexfile, freq)
        except subprocess.TimeoutExpired:
            ok, out = False, "timeout"

        if ok:
            print("\nflashed on attempt %d at %d Hz after %.1f s"
                  % (i, freq, time.time() - start))
            print(out.strip()[-400:])
            return 0

        why = ERR.search(out)
        why = why.group(0) if why else "unknown"
        tally[why] = tally.get(why, 0) + 1
        print("  %3d  %7d Hz  %s" % (i, freq, why), flush=True)

    print("\ngave up after %d attempts, %.1f s" % (args.attempts,
                                                   time.time() - start))
    for k, v in sorted(tally.items(), key=lambda kv: -kv[1]):
        print("  %4d x %s" % (v, k))
    print("\nAll attempts failing the same way at every frequency points at\n"
          "the link, not the clock. See HANDOFF.md section 3.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
