#!/usr/bin/env python3
"""
Watch the ring's temperature, hot-flash detector and spot readings live
(firmware v0.8.0+, Wellness characteristic).

    python tools/wellness_watch.py                  # watch, build defaults
    python tools/wellness_watch.py --mode 2         # detect awake too
    python tools/wellness_watch.py --rise 6         # 0.6 C threshold
    python tools/wellness_watch.py --spot-now       # take a reading now
    python tools/wellness_watch.py --schedule 30 30 180

Prints a line whenever Wellness changes and once a minute otherwise, and
logs every line to wellness_<timestamp>.csv.

A cheap test of the hot-flash detector, before anyone wears the ring
through a real one: --mode 2, wear the ring 30 min (the baseline has to
settle first: the "settled" column), then wrap that hand round a warm
mug or a hand warmer (~40 C) for two minutes. Keep the ring dry: it is
not sealed. Expect "HOT FLASH" within ~3 min.

--mode and --rise go out as opcode 0x13 and --schedule as 0x12. Both
are RAM only: a reboot restores the build's defaults.

Pairs like the dashboard does (Wellness is encrypted), so it takes the
ring's single bond slot -- see tools/dashboard.py.
"""

import argparse
import asyncio
import csv
import datetime
import struct
import sys
import time

from bleak import BleakClient, BleakScanner


def ring_uuid(n):
    return "f0a1%04x-1e5c-4a2b-8d3f-9c7b6e5a4d21" % n


CONTROL = ring_uuid(4)
WELLNESS = ring_uuid(0xB)
FWREV = "00002a26-0000-1000-8000-00805f9b34fb"

WELL = struct.Struct("<hhhBBBBBBI")      # 16 bytes, APP_INTEGRATION.md 17
NONE = -32768
KINDS = {0: "-", 1: "night", 2: "day", 3: "hot flash", 4: "manual"}


def parse(d):
    t, base, skin, flags, hot, kind, hr, rmssd, beats, up = WELL.unpack(d[:WELL.size])
    c = lambda v: None if v == NONE else v / 100.0  # noqa: E731
    return {
        "temp": c(t), "baseline": c(base), "skin": c(skin),
        "settled": bool(flags & 1), "hot_flash": bool(flags & 2),
        "spot_running": bool(flags & 4), "spot_valid": bool(flags & 8),
        "hot_flashes": hot, "spot_kind": KINDS.get(kind, str(kind)),
        "spot_hr": hr, "spot_rmssd": rmssd, "spot_beats": beats,
        "spot_uptime_s": up,
    }


def fmt(v, spec="5.2f"):
    return "  -  " if v is None else format(v, spec)


async def find(address):
    if address:
        return address
    dev = await BleakScanner.find_device_by_name("Ring PPG", timeout=15)
    if not dev:
        sys.exit("no 'Ring PPG' advertising nearby")
    return dev.address


async def run(args):
    address = await find(args.address)
    name = "wellness_%s.csv" % datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    cols = ["time", "temp", "baseline", "skin", "settled", "hot_flash",
            "hot_flashes", "spot_running", "spot_kind", "spot_hr",
            "spot_rmssd", "spot_beats", "spot_valid", "spot_uptime_s"]
    last = {}

    with open(name, "w", newline="") as f:
        log = csv.writer(f)
        log.writerow(cols)

        def show(w, why=""):
            now = datetime.datetime.now().strftime("%H:%M:%S")
            rise = (w["temp"] - w["baseline"]
                    if w["temp"] is not None and w["baseline"] is not None else None)
            line = (f"{now}  ring {fmt(w['temp'])} C  base {fmt(w['baseline'])}"
                    f"  rise {fmt(rise, '+5.2f')}  skin {fmt(w['skin'])}"
                    f"  {'settled' if w['settled'] else 'settling'}"
                    f"  flashes {w['hot_flashes']}")
            if w["hot_flash"]:
                line += "  ** HOT FLASH **"
            if w["spot_running"]:
                line += "  [spot reading running]"
            if w["spot_kind"] != "-":
                line += (f"  last spot {w['spot_kind']}: HR {w['spot_hr']}"
                         f" RMSSD {w['spot_rmssd']} ({w['spot_beats']})"
                         f" {'ok' if w['spot_valid'] else 'rejected'}")
            print(line + (f"  <- {why}" if why else ""), flush=True)
            log.writerow([now] + [w[c] for c in cols[1:]])
            f.flush()

        def on_wellness(_s, d):
            w = parse(bytes(d))
            why = ""
            if last:
                if w["hot_flash"] and not last["hot_flash"]:
                    why = "hot flash started"
                elif last["hot_flash"] and not w["hot_flash"]:
                    why = "hot flash ended"
                elif w["spot_uptime_s"] != last["spot_uptime_s"]:
                    why = "spot reading finished"
            last.update(w)
            show(w, why)

        print(f"connecting to {address} ...")
        async with BleakClient(address, timeout=20) as c:
            try:
                await c.pair()
            except Exception as e:          # already bonded, or the OS does it
                print(f"pair: {e!r} (continuing)")

            fw = (await c.read_gatt_char(FWREV)).decode(errors="replace")
            print(f"firmware {fw}")
            if c.services.get_characteristic(WELLNESS) is None:
                sys.exit("this firmware has no Wellness characteristic (needs v0.8.0+)")

            if args.mode is not None or args.rise is not None:
                mode = 1 if args.mode is None else args.mode
                rise = 10 if args.rise is None else args.rise
                await c.write_gatt_char(CONTROL, bytes([0x13, mode, rise]), response=True)
                print(f"hot-flash detection: mode {mode}, rise {rise / 10:.1f} C")
            if args.schedule:
                night, day, window = args.schedule
                await c.write_gatt_char(CONTROL, bytes([0x12]) +
                                        struct.pack("<HHH", night, day, window),
                                        response=True)
                print(f"spot schedule: night {night} min, day {day} min, {window} s each")

            await c.start_notify(WELLNESS, on_wellness)
            on_wellness(None, await c.read_gatt_char(WELLNESS))

            if args.spot_now:
                await c.write_gatt_char(CONTROL, bytes([0x14]), response=True)
                print("spot reading requested: keep the hand still for ~3 min")

            t_end = time.monotonic() + args.minutes * 60 if args.minutes else None
            while c.is_connected and (t_end is None or time.monotonic() < t_end):
                await asyncio.sleep(1)

    print(f"wrote {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--address", help="ring address (default: scan for 'Ring PPG')")
    ap.add_argument("--mode", type=int, choices=[0, 1, 2],
                    help="hot-flash detection: 0 off, 1 asleep only, 2 always")
    ap.add_argument("--rise", type=int, help="onset threshold, 0.1 C (3-50)")
    ap.add_argument("--schedule", type=int, nargs=3, metavar=("NIGHT", "DAY", "WINDOW"),
                    help="spot schedule: night_min day_min window_s")
    ap.add_argument("--spot-now", action="store_true", help="take a spot reading now")
    ap.add_argument("--minutes", type=float, default=0, help="stop after this long (0 = until Ctrl-C)")
    args = ap.parse_args()
    if args.rise is not None and not 3 <= args.rise <= 50:
        ap.error("--rise must be 3-50 (0.3-5.0 C)")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
