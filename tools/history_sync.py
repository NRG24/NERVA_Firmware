#!/usr/bin/env python3
"""
Download the ring's on-ring history (firmware v0.6+) and check it.

    python tools/history_sync.py                 # everything the ring has
    python tools/history_sync.py --from-seq 120  # only seq >= 120
    python tools/history_sync.py --address CC:C3:B4:E6:B1:8E

Sets the ring's clock (opcode 0x0D), subscribes to the History
characteristic, asks for a sync (opcode 0x0E) and prints every record,
verifying each CRC and that seq is contiguous. Writes them to
history_<timestamp>.csv beside the current directory.

This is the reference for the phone app's sync: same opcodes, same packet
parsing, same time reconstruction. Protocol: APP_INTEGRATION.md section 16.

Pairs like the dashboard does (the History characteristic is encrypted), so
it takes the ring's single bond slot -- see tools/dashboard.py.
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
HISTORY = ring_uuid(0xA)
FWREV = "00002a26-0000-1000-8000-00805f9b34fb"

REC = struct.Struct("<IIBBHBBBB")   # v1, 16 bytes, packet 0x01 (v0.6-v0.7)
REC2 = struct.Struct("<IIHhhBBBBBBBBBBBBBB3xB")  # v2, 32 bytes, packet 0x03 (v0.8+)
END = struct.Struct("<BIIIIBH")     # 20 bytes
FLAG_NAMES = ["TIME", "WORN", "CHG", "SLEEP", "WORKOUT", "HOTFLASH",
              "SPOT_NIGHT", "SPOT_DAY"]
TEMP_NONE = -32768
assert REC.size == 16 and REC2.size == 32


def crc8(data):
    """CRC-8/SMBUS: poly 0x07, init 0, no reflection, no final XOR."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


assert crc8(b"123456789") == 0xF4


def parse_record(raw):
    """A v1 (16-byte) record, from packet 0x01."""
    seq, t, hr, rmssd, steps, batt, flags, boot, crc = REC.unpack(raw)
    return {
        "seq": seq, "time_s": t, "hr_bpm": hr, "rmssd_ms": rmssd,
        "steps": steps, "battery_mv": batt * 20, "flags": flags,
        "boot_id": boot, "crc_ok": crc8(raw[:15]) == crc,
    }


def _temp(c100):
    return None if c100 == TEMP_NONE else c100 / 100.0


def parse_record_v2(raw):
    """A v2 (32-byte) record, from packet 0x03. 0 means "none" for every
    heart-rate field, None for the temperatures, motion_pct None when the
    accelerometer was not read (255 on the wire)."""
    (seq, t, steps, temp, skin, hr, hr_min, hr_max, rmssd, rest_hr,
     rest_rmssd, rest_beats, rise, motion, batt, flags, hot, boot,
     flags2, crc) = REC2.unpack(raw)
    return {
        "seq": seq, "time_s": t, "steps": steps,
        "temp_c": _temp(temp), "skin_c": _temp(skin),
        "hr_bpm": hr, "hr_min_bpm": hr_min, "hr_max_bpm": hr_max,
        "rmssd_ms": rmssd, "rest_hr_bpm": rest_hr,
        "rest_rmssd_ms": rest_rmssd, "rest_beats": rest_beats,
        "temp_rise_c": rise / 10.0,
        "motion_pct": None if motion == 255 else motion,
        "battery_mv": batt * 20, "flags": flags, "hot_flashes": hot,
        "spot_manual": bool(flags2 & 1),
        "boot_id": boot, "crc_ok": crc8(raw[:31]) == crc,
    }


def parse_packet(d, records):
    """Append the records in one history notification; True if it was a
    record packet."""
    if d[0] == 0x01:
        for i in range(d[1]):
            records.append(parse_record(d[2 + 16 * i: 18 + 16 * i]))
        return True
    if d[0] == 0x03:
        for i in range(d[1]):
            records.append(parse_record_v2(d[2 + 32 * i: 34 + 32 * i]))
        return True
    return False


def wall_time(rec, end):
    """Unix time for a record, or None -- the rule in section 16."""
    if rec["flags"] & 1:
        return rec["time_s"]
    if rec["boot_id"] == end["boot_id"] and end["unix_s"]:
        return end["unix_s"] - (end["uptime_s"] - rec["time_s"])
    return None


async def find(address):
    if address:
        return address
    dev = await BleakScanner.find_device_by_name("Ring PPG", timeout=15)
    if not dev:
        sys.exit("no 'Ring PPG' advertising nearby")
    return dev.address


async def run(args):
    address = await find(args.address)
    records, end = [], {}
    done = asyncio.Event()

    def on_history(_s, d):
        d = bytes(d)
        if parse_packet(d, records):
            pass
        elif d[0] == 0x02 and len(d) >= END.size:
            _, oldest, newest, up, unix_s, boot, epoch = END.unpack(d[:END.size])
            end.update(oldest_seq=oldest, newest_seq=newest, uptime_s=up,
                       unix_s=unix_s, boot_id=boot, epoch_s=epoch)
            done.set()

    print(f"connecting to {address} ...")
    async with BleakClient(address, timeout=20) as c:
        try:
            await c.pair()
        except Exception as e:          # already bonded, or the OS does it
            print(f"pair: {e!r} (continuing)")

        fw = (await c.read_gatt_char(FWREV)).decode(errors="replace")
        print(f"firmware {fw}, MTU {c.mtu_size}")
        if c.services.get_characteristic(HISTORY) is None:
            sys.exit("this firmware has no History characteristic (needs v0.6+)")

        await c.start_notify(HISTORY, on_history)
        now = int(time.time())
        await c.write_gatt_char(CONTROL, bytes([0x0D]) + struct.pack("<I", now),
                                response=True)
        t0 = time.monotonic()
        await c.write_gatt_char(CONTROL, bytes([0x0E]) + struct.pack("<I", args.from_seq),
                                response=True)
        try:
            await asyncio.wait_for(done.wait(), timeout=args.timeout)
        except asyncio.TimeoutError:
            print(f"!! no end packet after {args.timeout} s -- partial sync")
        took = time.monotonic() - t0

    bad = [r for r in records if not r["crc_ok"]]
    gaps = [(a["seq"], b["seq"]) for a, b in zip(records, records[1:])
            if b["seq"] != a["seq"] + 1]
    print(f"\n{len(records)} records in {took:.1f} s, {len(bad)} bad CRC, "
          f"{len(gaps)} gaps {gaps[:5]}")
    if end:
        print("end:", end)

    if records:
        name = "history_%s.csv" % datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        cols = ["seq", "wall_time", "hr_bpm", "hr_min_bpm", "hr_max_bpm",
                "rmssd_ms", "rest_hr_bpm", "rest_rmssd_ms", "rest_beats",
                "steps", "temp_c", "skin_c", "temp_rise_c", "motion_pct",
                "hot_flashes", "battery_mv", "flags", "boot_id", "crc_ok"]
        with open(name, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(cols)
            for r in records:
                t = wall_time(r, end) if end else None
                row = dict(r)
                row["wall_time"] = (datetime.datetime.fromtimestamp(t)
                                    .isoformat(" ", "seconds") if t else "")
                row["flags"] = "|".join(n for i, n in enumerate(FLAG_NAMES)
                                        if r["flags"] & (1 << i))
                # v1 records have no v2 fields: blank, not zero.
                w.writerow(["" if row.get(c) is None else row[c] for c in cols])
        print(f"wrote {name}")
        for r in records[-args.show:]:
            t = wall_time(r, end) if end else None
            ts = datetime.datetime.fromtimestamp(t).strftime("%m-%d %H:%M") if t else "  (undated)"
            temp = r.get("temp_c")
            extra = (f"  {temp:5.2f} C" if temp is not None else "") + (
                f"  rest {r['rest_hr_bpm']}/{r['rest_rmssd_ms']} ms"
                if r.get("rest_hr_bpm") else "") + (
                f"  HOT FLASH x{r['hot_flashes']}" if r.get("hot_flashes") else "")
            print(f"  seq {r['seq']:6d}  {ts}  HR {r['hr_bpm']:3d}  steps {r['steps']:5d}"
                  f"  {r['battery_mv']} mV  flags 0x{r['flags']:02x}  boot {r['boot_id']}"
                  f"{extra}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--address", help="ring address (default: scan for 'Ring PPG')")
    ap.add_argument("--from-seq", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=180)
    ap.add_argument("--show", type=int, default=12, help="print the last N records")
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
