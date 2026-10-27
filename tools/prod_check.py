#!/usr/bin/env python3
"""
Acceptance check for a freshly flashed production ring (v0.8.0+).

    python tools/prod_check.py

Run it with the ring next to the PC, nothing else connected to it (close
the dashboard and the phone app), and at least ~40 s after the ring booted
or last disconnected, so advertising has had time to slow down.

Checks, in order, everything that can be checked over BLE:

  1. advertising has slowed: ~1 packet a second, not ~20 (prod.conf
     CONFIG_RING_ADV_SLOW_AFTER_S)
  2. firmware revision reads 0.8.x
  3. a diagnostic opcode (0x10) is refused with ATT 0x13 (CONFIG_RING_DIAG_
     OPCODES off)
  4. Status reads, gsr_mv is -1 (CONFIG_RING_GSR_STATUS off), the PPG, IMU
     and PMIC health bits are set
  5. setting the clock (0x0D) and a history sync (0x0E) both work, and the
     records are contiguous with good CRCs
  6. Wellness reads a plausible ring temperature; the spot schedule (0x12)
     and hot-flash settings (0x13) take the prod defaults and refuse a bad
     value

It prints PASS/FAIL per check. Things it cannot see over BLE -- the
connection interval the ring asked for, wake-on-motion and step counting
in low-power accelerometer mode -- are listed at the end as manual checks.
"""

import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import history_sync as hs  # noqa: E402


def ring_uuid(n):
    return "f0a1%04x-1e5c-4a2b-8d3f-9c7b6e5a4d21" % n


STATUS, CONTROL, HISTORY, WELLNESS = (ring_uuid(1), ring_uuid(4), ring_uuid(0xA),
                                      ring_uuid(0xB))
FWREV = "00002a26-0000-1000-8000-00805f9b34fb"
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + detail) if detail else ""),
          flush=True)


async def main(address=None):
    # 1. Advertising rate.
    times = []

    def cb(dev, adv):
        name_ok = (dev.name or adv.local_name or "").startswith("Ring PPG")
        addr_ok = address is None or dev.address.upper() == address.upper()
        if name_ok and addr_ok:
            times.append(time.time())

    print("listening to advertising for 15 s ...", flush=True)
    async with BleakScanner(cb):
        await asyncio.sleep(15)
    rate = len(times) / 15.0
    check("advertising slowed (~1/s)", 0 < rate < 3,
          "%.1f packets/s%s" % (rate, " -- not advertising?" if not times else ""))

    dev = (await BleakScanner.find_device_by_address(address, timeout=30) if address
           else await BleakScanner.find_device_by_name("Ring PPG", timeout=30))
    if not dev:
        check("ring found", False, "not advertising; close the dashboard/phone app")
        return

    async with BleakClient(dev, timeout=20) as c:
        try:
            await c.pair()
        except Exception:
            pass

        # 2. Firmware revision.
        fw = (await c.read_gatt_char(FWREV)).decode(errors="replace")
        check("firmware is 0.8.x", fw.startswith("0.8"), fw)

        # 3. Diagnostic opcode refused.
        try:
            await c.write_gatt_char(CONTROL, bytes([0x10, 0x80, 0, 2, 0]), response=True)
            check("diagnostic opcode 0x10 refused", False, "it was accepted -- a test build?")
        except BleakError as e:
            check("diagnostic opcode 0x10 refused", True, str(e)[:60])

        # 4. Status.
        s = await c.read_gatt_char(STATUS)
        state, flags, hr10, mv, gsr = struct.unpack_from("<BBHHh", s)
        uptime = struct.unpack_from("<I", s, 16)[0]
        check("gsr_mv is -1 (GSR sampling off)", gsr == -1, "gsr_mv %d" % gsr)
        check("PPG/IMU/PMIC healthy", (flags & 0x1C) == 0x1C,
              "flags 0x%02x, battery %d mV, uptime %d s" % (flags, mv, uptime))

        # 5. Clock and history.
        records, end, done = [], {}, asyncio.Event()

        def on_history(_s, d):
            d = bytes(d)
            if hs.parse_packet(d, records):
                pass
            elif d[0] == 0x02:
                _, o, n, up, ux, boot, ep = hs.END.unpack(d[:hs.END.size])
                end.update(oldest=o, newest=n, unix=ux, boot=boot, epoch=ep)
                done.set()

        await c.start_notify(HISTORY, on_history)
        await c.write_gatt_char(CONTROL, bytes([0x0D]) + struct.pack("<I", int(time.time())),
                                response=True)
        await c.write_gatt_char(CONTROL, bytes([0x0E]) + struct.pack("<I", 0), response=True)
        try:
            await asyncio.wait_for(done.wait(), 120)
        except asyncio.TimeoutError:
            pass
        bad = sum(1 for r in records if not r["crc_ok"])
        gaps = sum(1 for a, b in zip(records, records[1:]) if b["seq"] != a["seq"] + 1)
        check("history sync completes", bool(end), "%d records" % len(records))
        check("history records clean", bad == 0 and gaps == 0,
              "%d bad CRC, %d gaps" % (bad, gaps))
        check("clock set", bool(end) and end.get("unix", 0) > 1.7e9,
              "ring unix %s" % end.get("unix"))
        check("epoch is 300 s", end.get("epoch") == 300, "epoch %s" % end.get("epoch"))
        check("records are v2 (32 bytes)", bool(records) and "temp_c" in records[-1],
              "newest has %s" % (sorted(records[-1])[:4] if records else "nothing"))

        # 6. Wellness and its settings.
        w = await c.read_gatt_char(WELLNESS)
        temp, base, skin, wflags, hot = struct.unpack_from("<hhhBB", w)
        check("ring temperature plausible", temp != -32768 and 1500 < temp < 4500,
              "%.2f C, baseline %s, skin %s, flags 0x%02x, hot flashes %d" % (
                  temp / 100, "none" if base == -32768 else "%.2f" % (base / 100),
                  "none" if skin == -32768 else "%.2f" % (skin / 100), wflags, hot))
        try:
            await c.write_gatt_char(CONTROL, bytes([0x12]) + struct.pack("<HHH", 60, 60, 180),
                                    response=True)
            await c.write_gatt_char(CONTROL, bytes([0x13, 1, 10]), response=True)
            check("spot and hot-flash settings accepted", True)
        except BleakError as e:
            check("spot and hot-flash settings accepted", False, str(e)[:60])
        try:
            await c.write_gatt_char(CONTROL, bytes([0x12]) + struct.pack("<HHH", 5, 60, 180),
                                    response=True)
            check("bad spot interval refused", False, "5 min was accepted")
        except BleakError as e:
            check("bad spot interval refused", True, str(e)[:60])

    print("\n%d/%d checks passed" % (sum(ok for _, ok in results), len(results)))
    print("""
Manual checks (not visible over BLE):
  * RTT log after connecting: 'asked for idle connection parameters', then
    'connection interval ~75-105 ms, latency 4' -- the central accepted.
  * Wake-on-motion in low-power accelerometer mode: leave the ring still
    for 5 min, pick it up; RTT shows 'motion wake' within ~1 s.
  * Temperature: RTT shows the ring temperature in every 'hot flash' log
    line, and a synced record's temp_c should sit a few degrees under skin
    temperature once worn for 30 min.
  * Spot readings: worn and still for 3 min, RTT shows 'state -> MEASURING
    (spot reading)', then about 3 min later 'spot day: HR .., RMSSD ..'.
  * Steps: walk 50 counted steps; the Activity step count rises by ~50
    (in a jump of 8 first). tools/steps_record.py walk does this properly.
""")


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--address", help="this ring only, when more than one is advertising")
    asyncio.run(main(ap.parse_args().address))
