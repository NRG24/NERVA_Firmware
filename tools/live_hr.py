#!/usr/bin/env python3
"""
Live heart rate from the ring over the standard Heart Rate Service, with an
optional reference monitor read side by side.

    python tools/live_hr.py                 # ring only
    python tools/live_hr.py --ref WHOOP     # ring vs a WHOOP / chest strap

Prints one line a second and logs every line to a CSV next to where it was
started, so a session can be compared afterwards.

The ring only notifies once its beat detector has a rate, so expect "--"
for the first ~5-10 s after a finger goes on, and whenever it loses lock.
Production firmware measures 15 s in every 60 and stops when still; flash
the HR test build (.\\build.ps1 -HrTest) for a continuous reading.

The reference must broadcast standard HRS. A WHOOP does once "Heart Rate
Broadcast" is switched on in its app; chest straps (Polar, Garmin HRM) do
by default. Nothing here pairs or bonds, so the ring's single bond slot is
left for the phone.
"""

import argparse
import asyncio
import csv
import datetime
import sys
import time

from bleak import BleakClient, BleakScanner

HRM = "00002a37-0000-1000-8000-00805f9b34fb"
STALE_S = 3.0


class Feed:
    def __init__(self, label, name):
        self.label = label
        self.name = name
        self.bpm = None
        self.contact = None
        self.at = 0.0
        self.connected = False

    def fresh(self):
        return self.bpm is not None and time.time() - self.at < STALE_S

    def handle(self, _sender, data):
        if not data:
            return
        flags = data[0]
        if flags & 0x01 and len(data) >= 3:
            bpm = data[1] | (data[2] << 8)
        elif len(data) >= 2:
            bpm = data[1]
        else:
            return
        # Bits 1-2: 3 = contact detected, 2 = supported but not detected.
        sc = (flags >> 1) & 0x03
        self.contact = {3: True, 2: False}.get(sc)
        self.bpm = bpm
        self.at = time.time()


def matches(name_sub):
    def f(dev, adv):
        name = adv.local_name or dev.name or ""
        return name_sub.lower() in name.lower()
    return f


async def follow(feed, stop):
    """Find, connect, subscribe; reconnect for as long as we run."""
    while not stop.is_set():
        dev = await BleakScanner.find_device_by_filter(matches(feed.name),
                                                       timeout=10)
        if dev is None:
            print("[%s] '%s' not found, still looking" % (feed.label,
                                                         feed.name))
            continue
        try:
            async with BleakClient(dev) as client:
                await client.start_notify(HRM, feed.handle)
                feed.connected = True
                print("[%s] connected to %s (%s)" % (feed.label, dev.name,
                                                     dev.address))
                while client.is_connected and not stop.is_set():
                    await asyncio.sleep(0.5)
        except Exception as e:
            print("[%s] %s: %s" % (feed.label, type(e).__name__, e))
        if feed.connected:
            print("[%s] disconnected, reconnecting" % feed.label)
        feed.connected = False
        await asyncio.sleep(1)


def show(feed):
    if not feed.connected:
        return "  (not connected)"
    if not feed.fresh():
        return "     --  "
    tag = {True: "", False: " no-contact", None: ""}[feed.contact]
    return "%5d bpm%s" % (feed.bpm, tag)


async def report(ring, ref, stop, path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["time", "ring_bpm", "ref_bpm", "diff"])
        while not stop.is_set():
            await asyncio.sleep(1)
            r = ring.bpm if ring.fresh() else None
            line = "%s  ring %s" % (time.strftime("%H:%M:%S"), show(ring))
            row = [time.strftime("%H:%M:%S"), r, "", ""]
            if ref is not None:
                q = ref.bpm if ref.fresh() else None
                line += "   %s %s" % (ref.label, show(ref))
                row[2] = q
                if r is not None and q is not None:
                    line += "   diff %+d" % (r - q)
                    row[3] = r - q
            print(line, flush=True)
            w.writerow(row)
            f.flush()


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ring", default="Ring PPG",
                    help="ring's advertised name (default 'Ring PPG')")
    ap.add_argument("--ref", default=None, metavar="NAME",
                    help="also read a reference HRS device whose name "
                         "contains NAME, e.g. WHOOP or Polar")
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    path = "hr_log_%s.csv" % stamp
    print("logging to %s -- Ctrl+C to stop\n" % path)

    stop = asyncio.Event()
    ring = Feed("ring", args.ring)
    ref = Feed(args.ref, args.ref) if args.ref else None

    tasks = [follow(ring, stop), report(ring, ref, stop, path)]
    if ref is not None:
        tasks.append(follow(ref, stop))
    try:
        await asyncio.gather(*tasks)
    finally:
        stop.set()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nstopped")
        sys.exit(0)
