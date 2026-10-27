#!/usr/bin/env python3
"""
Live ring dashboard: heart rate, PPG waveform, GSR and IMU in a browser.

    python tools/dashboard.py            # then open http://localhost:8765

    # two rings side by side, each locked to its own address:
    python tools/dashboard.py --address CC:C3:B4:E6:B1:8E --label "Ring 1"
    python tools/dashboard.py --address E7:8D:FC:57:23:07 --label "Ring 2" --port 8766

Connects to "Ring PPG", pairs (Just Works, the same as a phone), subscribes
to every stream and turns the IMU and GSR streams on (opcodes 0x02, 0x08).
The green PPG starts OFF and follows the "Green PPG" button (opcode 0x01);
on a demo build (.\\build.ps1 -Demo) that button is the only thing that
lights the LED. A small built-in web server pushes what arrives to the page
about 20 times a second over Server-Sent Events. Standard library plus
bleak, nothing else.

Pairing takes the ring's single bond slot (CONFIG_BT_MAX_PAIRED=1). To hand
the ring to a phone afterwards, press "Unpair ring" on the page (opcode
0x05), or remove "Ring PPG" in Windows Bluetooth settings and reflash.

Wire formats are APP_INTEGRATION.md sections 3-9. Unit conversions:
  IMU  raw counts at +/-2 g     -> mg = raw * 2000 / 32768
  GSR  12-bit, 1.8 V full scale -> mV = raw * 1800 / 4096   (see the DTS)
  PPG  FIFO word                -> value = word & 0x7FFFF, tag = word >> 19
"""

import argparse
import asyncio
import json
import os
import struct
import time

from bleak import BleakClient, BleakScanner

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "dashboard.html")


def ring_uuid(n):
    return "f0a1%04x-1e5c-4a2b-8d3f-9c7b6e5a4d21" % n


HRM = "00002a37-0000-1000-8000-00805f9b34fb"
BATTERY = "00002a19-0000-1000-8000-00805f9b34fb"
FWREV = "00002a26-0000-1000-8000-00805f9b34fb"
STATUS, PPG, IMU, CONTROL = ring_uuid(1), ring_uuid(2), ring_uuid(3), ring_uuid(4)
ACTIVITY, HRV, GSR = ring_uuid(5), ring_uuid(6), ring_uuid(7)

STATES = {0: "idle", 1: "measuring", 2: "charging"}


class Hub:
    """Everything the page shows. BLE callbacks write, the ticker reads."""

    def __init__(self):
        self.ppg, self.imu, self.gsr = [], [], []
        self.scalars = {"link": "searching", "fw": None}
        self.clients = set()
        self.seq = {"ppg": None, "gsr": None}
        self.lost = {"ppg": 0, "gsr": 0}
        self.counts = {"ppg": 0, "imu": 0, "gsr": 0}
        self.rates = {"ppg": 0, "imu": 0, "gsr": 0}
        self.last_rate = time.time()
        self.client = None
        # Set by "Unpair ring": stay off the ring so a phone can have it.
        self.paused = False
        # The Green PPG button. Off until someone presses it, and re-sent
        # on every reconnect because the ring drops streams on disconnect.
        self.ppg_wanted = False
        self.label = "Ring"

    def note_seq(self, name, seq):
        prev = self.seq[name]
        # A ring that rebooted starts again at 0, which modulo 2^32 reads
        # as four billion lost packets. Only a small forward gap is loss.
        if prev is not None and prev < seq <= prev + 1000:
            self.lost[name] += seq - prev - 1
        self.seq[name] = seq

    # --- notification handlers -------------------------------------------

    def on_hrm(self, _s, d):
        if len(d) < 2:
            return
        bpm = d[1] | (d[2] << 8) if d[0] & 1 and len(d) >= 3 else d[1]
        self.scalars["bpm"] = bpm
        self.scalars["bpm_at"] = time.time()

    def on_battery(self, _s, d):
        if d:
            self.scalars["battery_pct"] = d[0]

    def on_status(self, _s, d):
        if len(d) < 20:
            return
        st, fl, hr, bat, gsr, dc, ac, pi, up = struct.unpack("<BBHHhIHHI",
                                                             d[:20])
        self.scalars.update({
            "state": STATES.get(st, str(st)), "finger": bool(fl & 1),
            "charger": bool(fl & 2), "ppg_ok": bool(fl & 4),
            "imu_ok": bool(fl & 8), "pmic_ok": bool(fl & 16),
            "hr_status": hr / 10, "battery_mv": bat, "gsr_status_mv": gsr,
            "ppg_dc": dc, "ppg_ac": ac, "pi": pi / 10, "uptime": up,
        })

    def on_ppg(self, _s, d):
        if len(d) < 5:
            return
        seq, n = struct.unpack_from("<IB", d)
        self.note_seq("ppg", seq)
        for i in range(n):
            if 5 + 4 * i + 4 > len(d):
                break
            w = struct.unpack_from("<I", d, 5 + 4 * i)[0]
            if (w >> 19) & 0x1F == 1:       # green slot only
                self.ppg.append(w & 0x7FFFF)
                self.counts["ppg"] += 1

    def on_imu(self, _s, d):
        if len(d) < 6:
            return
        x, y, z = struct.unpack("<hhh", d[:6])
        k = 2000 / 32768
        self.imu.append((round(x * k), round(y * k), round(z * k)))
        self.counts["imu"] += 1

    def on_activity(self, _s, d):
        if len(d) < 20:
            return
        steps, kcal, cad = struct.unpack_from("<IIH", d)
        self.scalars.update({"steps": steps, "kcal": kcal / 1000,
                             "cadence": cad})

    def on_hrv(self, _s, d):
        if len(d) >= 3:
            rmssd, beats = struct.unpack("<HB", d[:3])
            self.scalars.update({"rmssd": rmssd / 10, "rmssd_beats": beats})

    def on_gsr(self, _s, d):
        if len(d) < 5:
            return
        seq, n = struct.unpack_from("<IB", d)
        self.note_seq("gsr", seq)
        for i in range(n):
            if 5 + 2 * i + 2 > len(d):
                break
            raw = struct.unpack_from("<h", d, 5 + 2 * i)[0]
            self.gsr.append(round(raw * 1800 / 4096, 1))
            self.counts["gsr"] += 1

    # --- page side ----------------------------------------------------------

    def frame(self):
        now = time.time()
        if now - self.last_rate >= 1.0:
            dt = now - self.last_rate
            self.rates = {k: round(v / dt, 1) for k, v in self.counts.items()}
            self.counts = {k: 0 for k in self.counts}
            self.last_rate = now
        s = dict(self.scalars)
        if "bpm_at" in s and now - s["bpm_at"] > 4:
            s["bpm"] = None             # stale: the detector lost lock
        s.pop("bpm_at", None)
        s["ppg_wanted"] = self.ppg_wanted
        s["label"] = self.label
        f = {"ppg": self.ppg, "imu": self.imu, "gsr": self.gsr, "s": s,
             "rates": self.rates, "lost": self.lost}
        self.ppg, self.imu, self.gsr = [], [], []
        return json.dumps(f, separators=(",", ":")).encode()


async def ticker(hub):
    while True:
        await asyncio.sleep(0.05)
        data = b"data: " + hub.frame() + b"\n\n"
        for q in list(hub.clients):
            if q.qsize() < 100:
                q.put_nowait(data)


ADDRESS_FILE = os.path.join(HERE, ".ring_address")


def remember_address(addr):
    try:
        with open(ADDRESS_FILE, "w") as f:
            f.write(addr)
    except OSError:
        pass


def load_address():
    try:
        with open(ADDRESS_FILE) as f:
            return f.read().strip() or None
    except OSError:
        return None


async def hr_only_session(hub, c):
    """Heart rate and battery over the open standard services, no pairing.

    For a computer the ring is not bonded to: the Ring Service is
    encrypted and the ring has one bond slot (CONFIG_BT_MAX_PAIRED=1), but
    Heart Rate (CONFIG_RING_HRS_OPEN), Battery and Device Information are
    readable by anyone. On an HR test build that is live BPM; on a demo
    build nothing ever asks for a measurement, so it stays blank.
    """
    hub.scalars["fw"] = (await c.read_gatt_char(FWREV)).decode(
        errors="replace")
    await c.start_notify(HRM, hub.on_hrm)
    hub.on_battery(None, await c.read_gatt_char(BATTERY))
    try:
        await c.start_notify(BATTERY, hub.on_battery)
    except Exception:
        pass            # notify is optional on BAS; the read above stands


async def ble_task(hub, name, address=None, hr_only=False):
    """Find, pair, subscribe, switch the streams on; reconnect forever."""
    failures = 0
    while True:
        if hub.paused:
            hub.scalars["link"] = "unpaired - restart the dashboard to pair"
            await asyncio.sleep(1)
            continue
        hub.scalars["link"] = "searching"
        if address:
            # Locked to one ring: every ring advertises the same name, so
            # with two on the desk a name scan takes whichever answers
            # first.
            dev = await BleakScanner.find_device_by_address(address,
                                                            timeout=10)
            # Not advertising usually means Windows already holds the
            # link (CONFIG_BT_MAX_CONN=1); it can still open it by address.
            target = dev or address
        else:
            dev = await BleakScanner.find_device_by_name(name, timeout=10)
            if dev is not None:
                target = dev
                remember_address(dev.address)
            else:
                # Same reason as above: a paired ring Windows is holding
                # stops advertising, so fall back to the last address.
                target = load_address()
                if target is None:
                    continue
        hub.scalars["link"] = "connecting"
        # Which step a failure happened in is the whole diagnosis -- pairing,
        # first encrypted access, or a stream switch -- so name each one.
        step = "connect" if hr_only else "connect + pair"
        try:
            async with BleakClient(target, pair=not hr_only,
                                   timeout=30) as c:
                hub.client = c
                if hr_only:
                    step = "heart rate / battery (open services)"
                    await hr_only_session(hub, c)
                    hub.scalars["link"] = "connected (heart rate only)"
                    print("connected to %s, fw %s, heart rate only" % (
                        c.address, hub.scalars["fw"]), flush=True)
                else:
                    # Before any stream can start, so the first packets of
                    # this connection are not compared with the last one's.
                    hub.seq = {"ppg": None, "gsr": None}
                    step = "read firmware revision (unencrypted)"
                    hub.scalars["fw"] = (await c.read_gatt_char(
                        FWREV)).decode(errors="replace")
                    for label, uuid, cb in (
                            ("heart rate", HRM, hub.on_hrm),
                            ("status", STATUS, hub.on_status),
                            ("ppg", PPG, hub.on_ppg),
                            ("imu", IMU, hub.on_imu),
                            ("activity", ACTIVITY, hub.on_activity),
                            ("hrv", HRV, hub.on_hrv),
                            ("gsr", GSR, hub.on_gsr)):
                        step = "subscribe " + label
                        await c.start_notify(uuid, cb)
                    step = "read status"
                    hub.on_status(None, await c.read_gatt_char(STATUS))
                    # Streams are forced off on every disconnect, so always
                    # turn them back on here. PPG follows the button:
                    # turning its stream on is what switches the green LED
                    # on, and on a demo build (CONFIG_RING_PPG_ON_DEMAND)
                    # nothing else does.
                    ppg = b"\x01\x01" if hub.ppg_wanted else b"\x01\x00"
                    for op in (ppg, b"\x02\x01", b"\x08\x01"):
                        step = "control opcode 0x%02x" % op[0]
                        await c.write_gatt_char(CONTROL, op, response=True)
                    hub.scalars["link"] = "connected"
                    print("connected to %s, fw %s, MTU %d" % (
                        c.address, hub.scalars["fw"], c.mtu_size),
                        flush=True)
                hub.scalars["address"] = c.address
                failures = 0
                while c.is_connected:
                    await asyncio.sleep(0.5)
        except Exception as e:
            failures += 1
            print("link: failed at '%s': %s: %s" % (step, type(e).__name__,
                                                     e), flush=True)
            # Never forget the PC-side keys automatically. The ring keeps
            # its own copy and, with CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE
            # off, refuses a fresh Just Works pairing from a peer it is
            # already bonded to -- so dropping only this side's keys locks
            # the PC out until the ring's bond is cleared over SWD. Say so
            # instead, and leave the decision to a person.
            if failures == 3:
                print("link: three failures in a row. If this says "
                      "'pair' or 'canceled', the PC and the ring disagree "
                      "about the bond: clear it on the ring first "
                      "(Unpair button, or an SWD erase of the storage "
                      "partition), then remove 'Ring PPG' from Windows "
                      "Bluetooth settings.", flush=True)
        hub.client = None
        hub.scalars["link"] = "disconnected"
        print("link: disconnected, retrying", flush=True)
        await asyncio.sleep(1)


async def http(hub, reader, writer):
    try:
        line = await reader.readline()
        while (await reader.readline()) not in (b"\r\n", b"\n", b""):
            pass
        parts = line.decode(errors="replace").split()
        method, path = (parts + ["", ""])[:2]

        if path == "/events":
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream"
                         b"\r\nCache-Control: no-cache\r\n"
                         b"Connection: keep-alive\r\n\r\n")
            q = asyncio.Queue()
            hub.clients.add(q)
            try:
                while True:
                    writer.write(await q.get())
                    await writer.drain()
            finally:
                hub.clients.discard(q)

        elif method == "POST" and path.startswith("/ppg"):
            hub.ppg_wanted = path.endswith("on=1")
            c = hub.client
            sent = False
            if c is not None and c.is_connected:
                try:
                    await c.write_gatt_char(
                        CONTROL, b"\x01\x01" if hub.ppg_wanted else b"\x01\x00",
                        response=True)
                    sent = True
                except Exception as e:
                    print("ppg: %s: %s" % (type(e).__name__, e), flush=True)
            body = json.dumps({"on": hub.ppg_wanted, "sent": sent}).encode()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json"
                         b"\r\nContent-Length: %d\r\n\r\n" % len(body) + body)

        elif method == "POST" and path == "/ship":
            # Opcode 0x0C: PMIC ship mode. The ring is off until a charger
            # is plugged in. ble_task keeps retrying meanwhile, so the page
            # reconnects by itself as soon as the ring boots again.
            c = hub.client
            ok = False
            if c is not None and c.is_connected:
                try:
                    await c.write_gatt_char(CONTROL, b"\x0c", response=True)
                    ok = True
                except Exception as e:
                    # On battery the ring can power off before the write
                    # response arrives; that is success, not failure.
                    print("ship: %s: %s" % (type(e).__name__, e), flush=True)
                    ok = True
            body = json.dumps({"ok": ok}).encode()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json"
                         b"\r\nContent-Length: %d\r\n\r\n" % len(body) + body)

        elif method == "POST" and path == "/unpair":
            c = hub.client
            ok = False
            if c is not None and c.is_connected:
                hub.paused = True
                try:
                    await c.write_gatt_char(CONTROL, b"\x05", response=True)
                except Exception:
                    pass        # the ring drops the link as it clears
                try:
                    await c.unpair()
                except Exception:
                    pass
                ok = True
            body = json.dumps({"ok": ok}).encode()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json"
                         b"\r\nContent-Length: %d\r\n\r\n" % len(body) + body)

        else:
            with open(PAGE, "rb") as f:
                body = f.read()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; "
                         b"charset=utf-8\r\nCache-Control: no-cache\r\n"
                         b"Content-Length: %d\r\n\r\n" % len(body) + body)
        await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def main():
    ap = argparse.ArgumentParser(description="Live ring dashboard.")
    ap.add_argument("--name", default="Ring PPG")
    ap.add_argument("--address", default=None,
                    help="connect to this address if the ring is not "
                         "advertising (remembered after the first find)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--label", default="Ring",
                    help="name shown on the page, e.g. 'Ring 1'")
    ap.add_argument("--ppg-on", action="store_true",
                    help="start with the green PPG on instead of off")
    ap.add_argument("--hr-only", action="store_true",
                    help="no pairing: heart rate and battery only, over the "
                         "open standard services. For a computer the ring "
                         "is not paired with")
    args = ap.parse_args()

    hub = Hub()
    hub.label = args.label
    hub.ppg_wanted = args.ppg_on
    hub.scalars["hr_only"] = args.hr_only
    server = await asyncio.start_server(
        lambda r, w: http(hub, r, w), "127.0.0.1", args.port)
    print("dashboard on http://localhost:%d -- Ctrl+C to stop" % args.port,
          flush=True)
    await asyncio.gather(server.serve_forever(), ticker(hub),
                         ble_task(hub, args.name, args.address, args.hr_only))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nstopped")
