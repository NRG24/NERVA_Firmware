#!/usr/bin/env python3
"""
Ring heart rate against a reference (WHOOP / chest strap), with the raw PPG.

    python tools/hr_compare.py --ref WHOOP --seconds 300

Connects to the ring (paired: it needs the encrypted Ring Service), turns the
green PPG stream on (opcode 0x01) so the ring measures continuously whatever
build it runs, and reads the reference's standard Heart Rate broadcast. Every
second it prints and logs both rates; every raw PPG sample is logged too, so
the beat detector can be judged offline against the same signal.

Writes hrcmp_<stamp>_hr.csv (1 Hz) and hrcmp_<stamp>_ppg.csv (100 Hz).
Turns the PPG stream off again on exit.

WHOOP: switch on "Heart Rate Broadcast" in the WHOOP app first.
"""

import argparse
import asyncio
import csv
import datetime
import struct
import time

from bleak import BleakClient, BleakScanner


def ring_uuid(n):
    return "f0a1%04x-1e5c-4a2b-8d3f-9c7b6e5a4d21" % n


HRM = "00002a37-0000-1000-8000-00805f9b34fb"
STATUS, PPG, CONTROL = ring_uuid(1), ring_uuid(2), ring_uuid(4)
STATUS_FMT = struct.Struct("<BBHHhIHHI")


def hrm_bpm(d):
    d = bytes(d)
    if not d:
        return None
    return d[1] | (d[2] << 8) if d[0] & 1 else d[1]


class State:
    def __init__(self):
        self.ring_bpm = self.ref_bpm = None
        self.ring_at = self.ref_at = 0.0
        self.status = None
        self.ppg_rows = []
        self.ppg_seq_last = None
        self.ppg_lost = 0
        self.t0 = time.time()


async def find(sub, timeout=30):
    sub = sub.lower()
    return await BleakScanner.find_device_by_filter(
        lambda dev, adv: sub in (dev.name or adv.local_name or "").lower(),
        timeout=timeout)


async def run(args):
    st = State()
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    stop = time.time() + args.seconds

    print("looking for the ring and %r ..." % args.ref, flush=True)
    ring_dev = await find(args.ring)
    ref_dev = await find(args.ref)
    if not ring_dev:
        raise SystemExit("ring %r not advertising" % args.ring)
    if not ref_dev:
        raise SystemExit("reference %r not advertising -- is HR broadcast on?"
                         % args.ref)

    def on_ring_hr(_s, d):
        st.ring_bpm, st.ring_at = hrm_bpm(d), time.time()

    def on_ref_hr(_s, d):
        st.ref_bpm, st.ref_at = hrm_bpm(d), time.time()

    def on_status(_s, d):
        if len(d) >= STATUS_FMT.size:
            st.status = STATUS_FMT.unpack(bytes(d[:STATUS_FMT.size]))

    def on_ppg(_s, d):
        d = bytes(d)
        if len(d) < 5:
            return
        seq, n = struct.unpack_from("<IB", d)
        if st.ppg_seq_last is not None and 0 < seq - st.ppg_seq_last < 1000:
            st.ppg_lost += seq - st.ppg_seq_last - 1
        st.ppg_seq_last = seq
        t = time.time() - st.t0
        for i in range(n):
            (w,) = struct.unpack_from("<I", d, 5 + 4 * i)
            st.ppg_rows.append((round(t, 3), seq, i, w & 0x7FFFF, w >> 19))

    async with BleakClient(ring_dev, timeout=20) as ring, \
            BleakClient(ref_dev, timeout=20) as ref:
        try:
            await ring.pair()
        except Exception as e:
            print("ring pair: %r (continuing)" % e)
        print("ring %s  MTU %d | ref %s %s" % (ring.address, ring.mtu_size,
                                              ref_dev.name, ref.address))
        await ref.start_notify(HRM, on_ref_hr)
        await ring.start_notify(HRM, on_ring_hr)
        await ring.start_notify(STATUS, on_status)
        await ring.start_notify(PPG, on_ppg)
        await ring.write_gatt_char(CONTROL, b"\x01\x01", response=True)
        print("PPG on. Keep the ring on, hand still, for the first minute.\n")
        print("   t   ring  ref   diff   dc      PI%   lost")

        hr_path = "hrcmp_%s_hr.csv" % stamp
        with open(hr_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "ring_bpm", "ref_bpm", "ring_hr_x10_status",
                        "ppg_dc", "perfusion_x10", "finger", "ppg_lost"])
            try:
                while time.time() < stop and ring.is_connected:
                    await asyncio.sleep(1.0)
                    now = time.time()
                    rb = st.ring_bpm if now - st.ring_at < 3 else None
                    fb = st.ref_bpm if now - st.ref_at < 3 else None
                    s = st.status
                    dc = s[5] if s else None
                    pi = s[7] if s else None
                    finger = (s[1] & 1) if s else None
                    hr10 = s[2] if s else None
                    w.writerow([round(now - st.t0, 1), rb, fb, hr10, dc, pi,
                                finger, st.ppg_lost])
                    diff = (rb - fb) if (rb and fb) else None
                    print("%5.0f  %4s  %4s  %5s  %6s  %4s  %4d" % (
                        now - st.t0, rb or "--", fb or "--",
                        "%+d" % diff if diff is not None else "",
                        dc if dc is not None else "-",
                        "%.1f" % (pi / 10) if pi is not None else "-",
                        st.ppg_lost), flush=True)
            finally:
                if ring.is_connected:
                    try:
                        await ring.write_gatt_char(CONTROL, b"\x01\x00",
                                                   response=True)
                    except Exception:
                        pass

    ppg_path = "hrcmp_%s_ppg.csv" % stamp
    with open(ppg_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_rx_s", "seq", "idx", "value", "tag"])
        w.writerows(st.ppg_rows)
    print("\nwrote %s and %s (%d PPG samples, %d packets lost)" % (
        hr_path, ppg_path, len(st.ppg_rows), st.ppg_lost))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ring", default="Ring PPG")
    ap.add_argument("--ref", default="WHOOP")
    ap.add_argument("--seconds", type=int, default=300)
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
