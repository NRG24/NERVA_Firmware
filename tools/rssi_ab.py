#!/usr/bin/env python3
"""
A/B the ring's antenna without a VNA: received signal strength against a
reference worn on the same body.

    python tools/rssi_ab.py measure "stock"          # 3 positions x 30 s
    python tools/rssi_ab.py measure "stub trimmed"   # after a change
    python tools/rssi_ab.py table                    # compare everything

The PC scans advertising packets from the ring and from a reference (a
WHOOP broadcasting heart rate, by default) at the same time. The number
that matters is DELTA = ring RSSI - reference RSSI, per position: the
reference moves with you, so the delta cancels most of where you sit, how
your body is turned, and what the room is doing. Absolute RSSI does not.

For each label it asks you to hold three positions in turn (or whatever
--positions says) and records --secs seconds at each, then prints the
median ring, reference and delta, and appends a row per position to
antenna_sweep.csv.

Rules of thumb:
  * Wear the ring the same way every time; the finger is part of the
    antenna. Keep the PC where it is.
  * The ring must be advertising: close the dashboard and the phone app.
    v0.7.0 production slows to ~1 advertisement a second after 30 s, which
    still gives ~30 samples; the test firmware advertises fast.
  * A change of 2 dB or more in the mean delta is real; under ~1 dB is
    noise. Repeat a measurement when two labels are close.

See ANTENNA_TUNING.md for what to change between measurements.
"""

import argparse
import asyncio
import csv
import datetime
import os
import statistics
import sys

from bleak import BleakScanner

CSV = "antenna_sweep.csv"


async def scan(secs, ring_name, ref_name):
    ring, ref = [], []

    def cb(dev, adv):
        name = (dev.name or adv.local_name or "")
        if name.startswith(ring_name):
            ring.append(adv.rssi)
        elif ref_name.lower() in name.lower():
            ref.append(adv.rssi)

    async with BleakScanner(cb):
        await asyncio.sleep(secs)
    return ring, ref


def med(v):
    return statistics.median(v) if v else None


async def measure(args):
    rows = []
    for i, pos in enumerate(args.positions.split(",")):
        input("[%d/%d] Position '%s': settle, then press Enter (%d s)..."
              % (i + 1, len(args.positions.split(",")), pos.strip(), args.secs))
        ring, ref = await scan(args.secs, args.ring, args.ref)
        r, f = med(ring), med(ref)
        d = (r - f) if (r is not None and f is not None) else None
        print("    ring %s dBm (n=%d)   reference %s dBm (n=%d)   delta %s dB" % (
            r, len(ring), f, len(ref), "%+.1f" % d if d is not None else "--"))
        if not ring:
            print("    !! no ring packets -- is something connected to it?")
        rows.append((pos.strip(), r, len(ring), f, len(ref), d))

    deltas = [row[5] for row in rows if row[5] is not None]
    if deltas:
        print("\n'%s': mean delta %+.1f dB over %d positions"
              % (args.label, sum(deltas) / len(deltas), len(deltas)))
    new = not os.path.exists(CSV)
    with open(CSV, "a", newline="") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(["time", "label", "position", "ring_dbm", "ring_n",
                        "ref_dbm", "ref_n", "delta_db"])
        stamp = datetime.datetime.now().isoformat(" ", "seconds")
        for row in rows:
            w.writerow([stamp, args.label, *row])
    print("appended to %s" % CSV)


def table():
    if not os.path.exists(CSV):
        sys.exit("no %s yet -- run measure first" % CSV)
    by = {}
    for r in csv.DictReader(open(CSV)):
        if r["delta_db"]:
            by.setdefault(r["label"], []).append(float(r["delta_db"]))
    print("%-30s %6s %10s %8s" % ("label", "n", "mean delta", "spread"))
    for label, ds in sorted(by.items(), key=lambda kv: -sum(kv[1]) / len(kv[1])):
        print("%-30s %6d %+9.1f %7.1f" % (label, len(ds), sum(ds) / len(ds),
                                          max(ds) - min(ds)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("measure")
    m.add_argument("label", help="what this measurement is, e.g. 'stub trimmed'")
    m.add_argument("--secs", type=int, default=30)
    m.add_argument("--positions", default="hand on desk,hand on lap,arm raised")
    m.add_argument("--ring", default="Ring PPG")
    m.add_argument("--ref", default="WHOOP")
    sub.add_parser("table")
    args = ap.parse_args()
    if args.cmd == "measure":
        asyncio.run(measure(args))
    else:
        table()


if __name__ == "__main__":
    main()
