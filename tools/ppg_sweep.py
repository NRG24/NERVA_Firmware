#!/usr/bin/env python3
"""
Sweep the ring's PPG front end against a reference heart rate and score it.

    python tools/ppg_sweep.py --ref WHOOP

Needs firmware 0.6.1+ (control opcodes 0x10 PPG tune and 0x11 TX power),
the ring paired to this PC, and the WHOOP broadcasting heart rate. Wear both,
sit still with the ring hand resting, and leave it alone: about 6 minutes.

For each setting it waits for the front end and the ring's beat detector to
settle, records the raw PPG, then scores it:

  dc%      DC level as a share of the 19-bit ADC range; >80 % risks clipping
  glitch%  samples that jump away from their neighbours (the 70 ms spikes
           seen on 2026-10-07): electrical, not physiological
  snr_dB   power at the reference heart rate (and its 2nd harmonic) against
           the rest of the 0.5-4 Hz band, glitches removed first. The number
           that matters: the pulse itself, before any algorithm
  spec%    10 s windows whose dominant rhythm is within 5 bpm of the
           reference -- what a good algorithm could get from this signal
  ring_err the ring's own heart rate against the reference, mean |error|

The TX power rows hold the optics at the baseline and change only the
radio. If glitch% falls with TX power, the radio is the glitch source.

Writes ppgsweep_<stamp>_summary.csv and one raw CSV per setting. Puts the
ring back to its defaults (and +8 dBm) at the end, and on Ctrl+C.
"""

import argparse
import asyncio
import csv
import datetime
import struct
import time

import numpy as np
from bleak import BleakClient, BleakScanner


def ring_uuid(n):
    return "f0a1%04x-1e5c-4a2b-8d3f-9c7b6e5a4d21" % n


HRM = "00002a37-0000-1000-8000-00805f9b34fb"
FWREV = "00002a26-0000-1000-8000-00805f9b34fb"
PPG, CONTROL = ring_uuid(2), ring_uuid(4)
FS = 100.0
ADC_FULL = (1 << 19) - 1

# label, LED PA, LED range (0 = 31 mA, 1 = 62 mA), ADC range (0-3 = 4/8/16/32 uA),
# average log2 (0-2 = 1/2/4), TX dBm. Row 1 is what the ring shipped with.
SETTINGS = [
    ("baseline 15mA",        0x80, 0, 2, 0, 8),
    ("baseline, TX 0 dBm",   0x80, 0, 2, 0, 0),
    ("baseline, TX -8 dBm",  0x80, 0, 2, 0, -8),
    ("15mA avg4",            0x80, 0, 2, 2, 8),
    ("31mA",                 0xFF, 0, 2, 0, 8),
    ("31mA avg4",            0xFF, 0, 2, 2, 8),
    ("31mA avg4 ADC8uA",     0xFF, 0, 1, 2, 8),
    ("46mA avg4",            0xC0, 1, 2, 2, 8),
    ("62mA avg4",            0xFF, 1, 2, 2, 8),
]
# What the firmware boots with (v0.6.2+): put back at the end of a sweep.
DEFAULT = ("defaults", 0xFF, 0, 2, 2, 8)


def hrm_bpm(d):
    d = bytes(d)
    return (d[1] | (d[2] << 8)) if d[0] & 1 else d[1]


def medfilt(x, k=5):
    p = k // 2
    xp = np.pad(x, p, mode="edge")
    return np.median(np.lib.stride_tricks.sliding_window_view(xp, k), axis=1)


def psd(x, n=1024):
    acc, cnt = None, 0
    for k in range(0, len(x) - n + 1, n // 2):
        s = x[k:k + n]
        s = s - np.polyval(np.polyfit(np.arange(n), s, 1), np.arange(n))
        p = np.abs(np.fft.rfft(s * np.hanning(n), 4 * n)) ** 2
        acc = p if acc is None else acc + p
        cnt += 1
    return np.fft.rfftfreq(4 * n, 1 / FS), acc / cnt


def score(v, ref_bpm, ring_rows):
    out = {"n": len(v)}
    if len(v) < 1500 or not ref_bpm:
        return out
    dc = float(np.mean(v))
    out["dc"] = dc
    out["dc_pct"] = 100 * dc / ADC_FULL
    clean = medfilt(v, 5)
    dev = np.abs(v - clean)
    out["glitch_pct"] = 100 * float(np.mean(dev > 0.005 * dc))
    f0 = ref_bpm / 60.0
    f, p = psd(clean)
    band = (f >= 0.5) & (f <= 4.0)
    sig = band & ((np.abs(f - f0) < 0.08) | (np.abs(f - 2 * f0) < 0.08))
    out["snr_db"] = 10 * np.log10(p[sig].sum() / max(p[band & ~sig].sum(), 1e-9))
    hits = tot = 0
    for k in range(0, len(clean) - 1000, 250):
        s = clean[k:k + 1000]
        s = s - np.polyval(np.polyfit(np.arange(1000), s, 2), np.arange(1000))
        F = np.abs(np.fft.rfft(s * np.hanning(1000), 8192))
        ff = np.fft.rfftfreq(8192, 1 / FS)
        m = (ff > 0.7) & (ff < 3.3)
        tot += 1
        hits += abs(ff[m][np.argmax(F[m])] * 60 - ref_bpm) <= 5
    out["spec_pct"] = 100 * hits / max(tot, 1)
    errs = [abs(r - w) for r, w in ring_rows if r and w]
    out["ring_err"] = float(np.mean(errs)) if errs else None
    out["ring_cov"] = 100 * len(errs) / max(len(ring_rows), 1)
    return out


async def find(sub, timeout=30):
    sub = sub.lower()
    return await BleakScanner.find_device_by_filter(
        lambda dev, adv: sub in (dev.name or adv.local_name or "").lower(),
        timeout=timeout)


async def apply(ring, s, tx):
    _, pa, led_rge, adc_rge, avg, txdbm = s
    await ring.write_gatt_char(CONTROL, bytes([0x10, pa, led_rge, adc_rge, avg]),
                               response=True)
    if tx:
        await ring.write_gatt_char(CONTROL, bytes([0x11]) + struct.pack("<b", txdbm),
                                   response=True)


class RingLink:
    """The ring connection, re-established whenever it drops.

    The ring's antenna is ~17 dB down on a WHOOP's, so a link that holds
    for minutes at one hand position drops in seconds at another. A sweep
    that died on the first drop measured nothing; this reconnects instead.
    """

    def __init__(self, name, on_hr, on_ppg):
        self.name, self.on_hr, self.on_ppg = name, on_hr, on_ppg
        self.c = None
        self.drops = 0
        self.fw = None

    @property
    def up(self):
        return self.c is not None and self.c.is_connected

    async def ensure(self):
        if self.up:
            return self.c
        for attempt in range(1, 7):
            dev = await find(self.name, 30)
            if not dev:
                print("   (ring not advertising, attempt %d)" % attempt, flush=True)
                continue
            c = BleakClient(dev, timeout=20)
            try:
                await c.connect()
                try:
                    await c.pair()
                except Exception:
                    pass
                if self.fw is None:
                    self.fw = (await c.read_gatt_char(FWREV)).decode(errors="replace")
                    print("ring fw %s, MTU %d" % (self.fw, c.mtu_size), flush=True)
                await c.start_notify(HRM, self.on_hr)
                await c.start_notify(PPG, self.on_ppg)
                await c.write_gatt_char(CONTROL, b"\x01\x01", response=True)
                self.c = c
                return c
            except Exception as e:
                print("   (connect attempt %d failed: %r)" % (attempt, e), flush=True)
                try:
                    await c.disconnect()
                except Exception:
                    pass
        raise SystemExit("ring lost and not coming back -- move it closer to the PC")


async def run(args):
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    ref_dev = await find(args.ref)
    if not ref_dev:
        raise SystemExit("reference %r not advertising -- is HR broadcast on?" % args.ref)

    st = {"ring": None, "ring_at": 0, "ref": None, "ref_at": 0, "ppg": []}

    def on_ring(_s, d):
        st["ring"], st["ring_at"] = hrm_bpm(d), time.time()

    def on_ref(_s, d):
        st["ref"], st["ref_at"] = hrm_bpm(d), time.time()

    def on_ppg(_s, d):
        d = bytes(d)
        if len(d) < 5:
            return
        n = d[4]
        for i in range(n):
            (w,) = struct.unpack_from("<I", d, 5 + 4 * i)
            st["ppg"].append(w & 0x7FFFF)

    settings = [s for s in SETTINGS if args.tx or "TX" not in s[0]]
    link = RingLink(args.ring, on_ring, on_ppg)
    results = []

    async with BleakClient(ref_dev, timeout=20) as ref:
        print("reference %s" % ref_dev.name, flush=True)
        await ref.start_notify(HRM, on_ref)
        try:
            for s in settings:
                label = s[0]
                for attempt in range(1, 4):
                    ring = await link.ensure()
                    try:
                        await apply(ring, s, args.tx)
                        print("\n== %s: settling %d s" % (label, args.settle), flush=True)
                        await asyncio.sleep(args.settle)
                        st["ppg"] = []
                        ring_rows, refs = [], []
                        for _ in range(args.record):
                            await asyncio.sleep(1)
                            if not link.up:
                                raise ConnectionError("link dropped")
                            now = time.time()
                            r = st["ring"] if now - st["ring_at"] < 3 else None
                            w = st["ref"] if now - st["ref_at"] < 3 else None
                            ring_rows.append((r, w))
                            if w:
                                refs.append(w)
                        break
                    except Exception as e:
                        link.drops += 1
                        print("   ring link dropped during %s (%r), redoing it (try %d/3)"
                              % (label, e, attempt), flush=True)
                else:
                    print("   giving up on %s" % label, flush=True)
                    continue

                v = np.array(st["ppg"], dtype=float)
                ref_bpm = float(np.mean(refs)) if refs else None
                sc = score(v, ref_bpm, ring_rows)
                sc.update(label=label, pa=s[1], led_rge=s[2], adc_rge=s[3],
                          avg_log2=s[4], tx_dbm=s[5] if args.tx else None,
                          ref_bpm=ref_bpm)
                results.append(sc)
                safe = label.replace(" ", "_").replace(",", "")
                with open("ppgsweep_%s_%s.csv" % (stamp, safe), "w", newline="") as f:
                    csv.writer(f).writerows([["value"]] + [[int(x)] for x in v])
                print("   dc %5.1f%%  glitch %5.1f%%  snr %+5.1f dB  spec %3.0f%%  "
                      "ring_err %s  (ref %.0f bpm, %d samples)" % (
                          sc.get("dc_pct", 0), sc.get("glitch_pct", 0),
                          sc.get("snr_db", float("nan")), sc.get("spec_pct", 0),
                          "%.1f" % sc["ring_err"] if sc.get("ring_err") is not None else "--",
                          ref_bpm or 0, len(v)), flush=True)
        finally:
            try:
                ring = await link.ensure()
                await apply(ring, DEFAULT, args.tx)
                await ring.write_gatt_char(CONTROL, b"\x01\x00", response=True)
                await ring.disconnect()
                print("\nring back to defaults, PPG stream off (%d link drops)" % link.drops)
            except BaseException as e:
                print("!! could not restore defaults (%r) -- a reboot does" % e)

    keys = ["label", "pa", "led_rge", "adc_rge", "avg_log2", "tx_dbm", "ref_bpm",
            "n", "dc_pct", "glitch_pct", "snr_db", "spec_pct", "ring_err", "ring_cov"]
    path = "ppgsweep_%s_summary.csv" % stamp
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, keys, extrasaction="ignore")
        w.writeheader()
        w.writerows(results)

    print("\n%-22s %6s %7s %7s %6s %8s" % ("setting", "dc%", "glitch%", "snr_dB",
                                          "spec%", "ring_err"))
    for r in results:
        print("%-22s %6.1f %7.1f %+7.1f %6.0f %8s" % (
            r["label"], r.get("dc_pct", 0), r.get("glitch_pct", 0),
            r.get("snr_db", float("nan")), r.get("spec_pct", 0),
            "%.1f" % r["ring_err"] if r.get("ring_err") is not None else "--"))
    usable = [r for r in results if "snr_db" in r and r.get("dc_pct", 100) < 80]
    if usable:
        best = max(usable, key=lambda r: r["snr_db"])
        print("\nbest pulse SNR without clipping: %s" % best["label"])
    print("wrote %s" % path)


# --- A/B mode ----------------------------------------------------------------

# label, LED PA, LED range, ADC range, average log2. The candidates for the
# production front end: LED energy per sample spans 8x across them.
AB_SETTINGS = [
    ("15mA 1x", 0x80, 0, 2, 0),
    ("15mA 2x", 0x80, 0, 2, 1),
    ("31mA 1x", 0xFF, 0, 2, 0),
    ("31mA 4x", 0xFF, 0, 2, 2),
]

# MAXM86161 datasheet: ~600 uA average from VLED at 100 sps, PA 0xFF, one
# pulse a sample. Scales with PA and the pulse count; referred to the cell
# through the 5 V boost at an ASSUMED 85 % efficiency and 3.7 V.
def ab_mah_per_day(pa, avg_log2, window_s=25, period_s=300):
    vled_ma = 0.6 * (pa / 255.0) * (1 << avg_log2)
    cell_ma = vled_ma * 5.0 / (3.7 * 0.85)
    return cell_ma * window_s / period_s * 24


async def run_ab(args):
    import os
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import hr_eval

    exe = hr_eval.build_replay(())
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    ref_dev = await find(args.ref)
    if not ref_dev:
        raise SystemExit("reference %r not advertising -- is HR broadcast on?" % args.ref)

    st = {"ref": None, "ref_at": 0, "ppg": []}

    def on_ring(_s, d):
        pass

    def on_ref(_s, d):
        st["ref"], st["ref_at"] = hrm_bpm(d), time.time()

    def on_ppg(_s, d):
        d = bytes(d)
        if len(d) < 5:
            return
        for i in range(d[4]):
            (w,) = struct.unpack_from("<I", d, 5 + 4 * i)
            st["ppg"].append(w & 0x7FFFF)

    link = RingLink(args.ring, on_ring, on_ppg)
    per = {s[0]: [] for s in AB_SETTINGS}
    log = []

    async with BleakClient(ref_dev, timeout=20) as ref:
        await ref.start_notify(HRM, on_ref)
        try:
            for rnd in range(args.rounds):
                # Rotate the order each round so slow drift (heart rate,
                # how the ring sits) does not favour whichever goes first.
                order = AB_SETTINGS[rnd % len(AB_SETTINGS):] + AB_SETTINGS[:rnd % len(AB_SETTINGS)]
                for s in order:
                    label = s[0]
                    for attempt in range(1, 4):
                        ring = await link.ensure()
                        try:
                            await apply(ring, s + (8,), False)
                            print("\n== round %d/%d  %s: %d s" % (rnd + 1, args.rounds, label,
                                                                 args.ab_secs), flush=True)
                            st["ppg"] = []
                            refs = []
                            t0 = time.time()
                            while time.time() - t0 < args.ab_secs:
                                await asyncio.sleep(1)
                                if not link.up:
                                    raise ConnectionError("link dropped")
                                now = time.time()
                                refs.append(st["ref"] if now - st["ref_at"] < 3 else None)
                            break
                        except Exception as e:
                            link.drops += 1
                            print("   link dropped (%r), redoing (try %d/3)" % (e, attempt),
                                  flush=True)
                    else:
                        continue

                    v = np.array(st["ppg"], dtype=float)
                    est = hr_eval.c_hr(v, exe) if len(v) > 1500 else np.array([])
                    # Skip the detector's warm-up: 10 s window + agreement.
                    pairs = [(est[k], refs[k]) for k in range(13, min(len(est), len(refs)))
                             if refs[k]]
                    got = [(e, r) for e, r in pairs if e]
                    err = [abs(e - r) for e, r in got]
                    seg = {"n": len(pairs), "got": len(got),
                           "err": float(np.mean(err)) if err else None,
                           "ok5": sum(1 for x in err if x <= 5)}
                    per[label].append(seg)
                    log.append((rnd + 1, label, seg))
                    with open("ppgab_%s_%s_r%d.csv" % (stamp, label.replace(" ", "_"), rnd + 1),
                              "w", newline="") as f:
                        csv.writer(f).writerows([["value"]] + [[int(x)] for x in v])
                    print("   coverage %d/%d s  mean err %s  within 5 bpm %d" % (
                        seg["got"], seg["n"],
                        "%.1f" % seg["err"] if seg["err"] is not None else "--",
                        seg["ok5"]), flush=True)
        finally:
            try:
                ring = await link.ensure()
                await apply(ring, DEFAULT, False)
                await ring.write_gatt_char(CONTROL, b"\x01\x00", response=True)
                await ring.disconnect()
                print("\nring back to defaults, PPG stream off (%d link drops)" % link.drops)
            except BaseException as e:
                print("!! could not restore defaults (%r) -- a reboot does" % e)

    print("\n%-10s %8s %10s %10s %12s" % ("setting", "coverage", "mean err", "within 5", "mAh/day*"))
    rows = []
    for s in AB_SETTINGS:
        segs = per[s[0]]
        n = sum(x["n"] for x in segs)
        got = sum(x["got"] for x in segs)
        ok = sum(x["ok5"] for x in segs)
        errs = [x["err"] * x["got"] for x in segs if x["err"] is not None]
        mean = sum(errs) / got if got else None
        mah = ab_mah_per_day(s[1], s[4])
        rows.append((s[0], n, got, mean, ok, mah))
        print("%-10s %7.0f%% %10s %9.0f%% %12.1f" % (
            s[0], 100 * got / n if n else 0, "%.1f bpm" % mean if mean is not None else "--",
            100 * ok / n if n else 0, mah))
    print("* LED energy only, at 25 s every 300 s, assumed 85 % boost efficiency")
    with open("ppgab_%s_summary.csv" % stamp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["setting", "seconds", "with_reading", "mean_err_bpm", "within5", "mah_per_day"])
        w.writerows(rows)
        w.writerow([])
        w.writerow(["round", "setting", "seconds", "with_reading", "mean_err_bpm", "within5"])
        for rnd, label, seg in log:
            w.writerow([rnd, label, seg["n"], seg["got"], seg["err"], seg["ok5"]])
    print("wrote ppgab_%s_summary.csv" % stamp)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ab", action="store_true",
                    help="A/B the production candidates (AB_SETTINGS), scored by the "
                         "real detector against the reference")
    ap.add_argument("--rounds", type=int, default=3, help="--ab: rounds through every setting")
    ap.add_argument("--ab-secs", type=int, default=60, help="--ab: seconds per setting per round")
    ap.add_argument("--ring", default="Ring PPG")
    ap.add_argument("--ref", default="WHOOP")
    ap.add_argument("--settle", type=int, default=10, help="seconds per setting before recording")
    ap.add_argument("--record", type=int, default=30, help="seconds recorded per setting")
    ap.add_argument("--tx", action="store_true",
                    help="also sweep radio TX power (opcode 0x11; suspected of a reboot)")
    args = ap.parse_args()
    asyncio.run(run_ab(args) if args.ab else run(args))


if __name__ == "__main__":
    main()
