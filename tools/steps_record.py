#!/usr/bin/env python3
"""
Record the ring's accelerometer while you do a known activity, for scoring
step counters offline (tools/steps_eval.py).

    python tools/steps_record.py walk     # walk; type the count you took
    python tools/steps_record.py desk     # hand movement, no walking (truth 0)

Pairs like the dashboard does (the IMU stream is in the encrypted Ring
Service), turns the IMU stream on (opcode 0x02, ~50 samples a second), and
logs every sample with a PC timestamp. The ring's own step count from the
Activity characteristic is logged too, so the algorithm it is running can
be compared with what the replay predicts. Press Enter to stop; for a walk
you are then asked how many steps you counted.

The link is weak (antenna ~17 dB down on a WHOOP), so stay within a couple
of metres of the PC: walk laps of the room, or walk on the spot. Count
every footfall -- both feet.

Writes steps_<kind>_<stamp>.csv: a header line "# truth=<n>" then
"t_ms,x,y,z,mg,ring_steps".
"""

import argparse
import asyncio
import datetime
import math
import struct
import sys
import time

from bleak import BleakClient, BleakScanner


def ring_uuid(n):
    return "f0a1%04x-1e5c-4a2b-8d3f-9c7b6e5a4d21" % n


IMU, CONTROL, ACTIVITY = ring_uuid(3), ring_uuid(4), ring_uuid(5)


async def run(args):
    dev = await BleakScanner.find_device_by_name("Ring PPG", timeout=30)
    if not dev:
        sys.exit("ring not advertising -- close the dashboard and the phone app")
    rows = []
    ring_steps = [None]
    t0 = time.time()

    def on_imu(_s, d):
        x, y, z = struct.unpack("<hhh", bytes(d)[:6])
        # raw counts at +/-2 g -> mg
        xm, ym, zm = (v * 2000 / 32768 for v in (x, y, z))
        mg = int(round(math.sqrt(xm * xm + ym * ym + zm * zm)))
        rows.append((int((time.time() - t0) * 1000), x, y, z, mg, ring_steps[0]))

    def on_activity(_s, d):
        ring_steps[0] = struct.unpack_from("<I", bytes(d))[0]

    async with BleakClient(dev, timeout=20) as c:
        try:
            await c.pair()
        except Exception:
            pass
        await c.start_notify(ACTIVITY, on_activity)
        ring_steps[0] = struct.unpack_from("<I", await c.read_gatt_char(ACTIVITY))[0]
        await c.start_notify(IMU, on_imu)
        await c.write_gatt_char(CONTROL, b"\x02\x01", response=True)
        start_steps = ring_steps[0]
        print("Recording (%s). Ring step count now %d. Go -- press Enter to stop."
              % (args.kind, start_steps), flush=True)
        await asyncio.get_running_loop().run_in_executor(None, sys.stdin.readline)
        try:
            await c.write_gatt_char(CONTROL, b"\x02\x00", response=True)
        except Exception:
            pass
        end_steps = ring_steps[0]

    truth = 0
    if args.kind == "walk":
        truth = int(input("How many steps did you count? ").strip())
    secs = rows[-1][0] / 1000 if rows else 0
    name = "steps_%s_%s.csv" % (args.kind, datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    with open(name, "w") as f:
        f.write("# truth=%d\n" % truth)
        f.write("t_ms,x,y,z,mg,ring_steps\n")
        for r in rows:
            f.write("%d,%d,%d,%d,%d,%s\n" % (r[0], r[1], r[2], r[3], r[4],
                                              "" if r[5] is None else r[5]))
    print("%d samples over %.0f s (%.0f/s). Ring counted %s steps, truth %d. Wrote %s"
          % (len(rows), secs, len(rows) / max(secs, 1),
             (end_steps - start_steps) if None not in (start_steps, end_steps) else "?",
             truth, name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("kind", choices=["walk", "desk"])
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
