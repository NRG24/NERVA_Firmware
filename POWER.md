# Power budget

Written 2026-10-15. **Every number here is an estimate from datasheets.
Nothing has been measured on this board.** The battery's capacity is not
known either: the BOM says "customer supplied, confirm capacity". The
purpose of this page is to show **where the power goes** and **which
settings matter**, so the first real measurement has something to be
checked against. Section "Measure it" says how to get that measurement.

## Assumptions

* The cell is 3.7 V. The 1.8 V rail comes from the BQ25120A's buck at an
  assumed 90 %, so 1 mA on the rail is about **0.54 mA from the cell**.
* VLED (5 V, TPS61240 boost) is at an assumed 85 %, so 1 mA of LED supply
  is about **1.6 mA from the cell**.
* Radio currents come from the ANNA-B402 datasheet at 3 V (+8 dBm TX 15.5
  mA, RX 6 mA), treated as rail current. That is pessimistic at 1.8 V.
* Typical day: worn, **phone connected 2 h** (syncing, glancing),
  **disconnected 22 h**, walking 1 h.

## Where it goes

Cell current, averaged over the day:

| Consumer | Datasheet basis | v0.6.2 test (daily) | v0.7.0 prod |
|---|---|---|---|
| **PPG LED**, heart-rate windows | MAXM86161: ~600 uA VLED at 100 sps, PA 0xFF, 1 pulse/sample | 31 mA, 4x, 25 s/600 s: **~160 uA** | 15 mA, 2x, 25 s/300 s: **~80 uA** |
| **Accelerometer**, always on | LSM6DSV: 190 uA high-perf., 20 uA low-power (60 Hz, 4-avg) | HP 120 Hz: **~100 uA** | LP 60 Hz: **~11 uA** |
| **Advertising** while disconnected | 3 channels x ~0.35 ms TX at +8 dBm per event | every ~45 ms: **~240 uA** x 22 h | fast 30 s, then ~1.1 s: **~10 uA** x 22 h |
| **Connection** while connected | an RX+TX event per interval | central's ~30 ms: **~90 uA** x 2 h | ~100 ms, latency 4: **~5 uA** x 2 h |
| MCU during windows | 3.3 mA CPU, 20 ms FIFO poll, spectrum 1/s | ~8 uA | ~15 uA |
| MCU idle poll (IMU every 200 ms) | ~1 ms awake per read | ~9 uA | ~9 uA |
| Sleep floor | nRF52833 System ON 2.6 uA, PMIC <1 uA, MAXM86161 1.6 uA, boost off 1.5 uA | ~5 uA | ~5 uA |
| GSR status sampling | 800 ms settle every 30 s | small | off |

## Totals

| | mAh / day | Days on 15 mAh | Days on 25 mAh |
|---|---|---|---|
| v0.6.2 test (daily build) | **~12** | ~1.2 | ~2 |
| v0.7.0 prod (provisional front end) | **~3** | ~5 | ~8 |
| v0.8.0 prod: v0.7.0 plus spot readings (balanced schedule) | **~3.7** (up to ~4 on a long still day) | ~4 | ~6.5 |
| v0.7.0 prod with the test front end (31 mA, 4x) | ~9 | ~1.7 | ~2.8 |
| What a 25 s window **every 60 s** would cost, at 31 mA, 4x | ~40+ | <0.5 | <1 |

The last row is the default period with today's 25 s window. It is why
`prod.conf` sets 300 s. On a ring battery, the heart-rate duty cycle and
the LED energy per sample are the two numbers that decide battery life.
The always-on accelerometer and the always-fast advertiser were the two
silent ones. Both are fixed in prod.

### What v0.8.0 adds

Spot readings (`src/spot.h`) are 3-minute green windows at the prod LED
setting. A spot reading replaces the ordinary 25 s window it overlaps, so
each costs ~2.5 extra minutes. At the balanced schedule (`prod.conf`:
hourly asleep, at most hourly awake and still) that is ~20 min a night
and typically ~10-40 min by day: 2-4 % extra LED duty, **~+25-40 uA, about
+0.6-1 mAh/day**. The LED+boost draw while a prod window runs is ~1 mA
(80 uA / 8.3 % duty, from the table above).

Ring temperature every 10 s is ~50 us of the TEMP peripheral: nothing.
The sensor-face temperature is one conversion per window: nothing.

## What each prod change is worth (estimated)

| Change in v0.7.0 | Saves about | Risk |
|---|---|---|
| Advertising slows after 30 s | 5 mAh/day disconnected | Low: a phone finds the ring within ~1 s instead of instantly |
| Accelerometer low-power, 60 Hz | 2 mAh/day | **Unverified:** wake-on-motion and step counting in this mode must be checked on the ring |
| PPG 15 mA 2x instead of 31 mA 4x | ~6 mAh/day at the prod duty | **Unproven for HR accuracy:** run `tools/ppg_sweep.py --ab` |
| Connection interval management | ~0.2 mAh/day at 2 h connected; more if the phone stays connected | Low: the central may refuse; it is logged |
| GSR status sampling off | small | None: the reading was meaningless |

## Measure it

The estimates above could easily be out by 2x either way. To replace them:

1. **Nordic Power Profiler Kit II (PPK2)**, about $100. Put it in series
   with the battery (source-meter mode replaces the cell). It logs current
   at 100 kHz, so advertising events, measurement windows and the sleep
   floor all show as separate features. This is the standard tool for
   nRF52 work and it will pay for itself in the first hour.
2. Measure in this order. Each step isolates one row of the table:
   * **Ship mode** (opcode `0x0C`): should be ~150 nA (PMIC datasheet).
     This proves the measurement setup.
   * **Idle, not connected, ring still**: the floor plus advertising plus
     the accelerometer.
   * **One measurement window**: the LED cost per window, at each
     front-end setting.
   * **Connected and idle**, before and after the interval request
     (`connection interval ...` in the log).
3. Put the real numbers into this table and pick the measurement period
   that gives the battery life you want.

Until then, the cheapest way to find the battery's real capacity is a
**rundown test**: charge full, flash prod, leave the ring worn and
disconnected, and sync the history afterwards. Battery millivolts are in
every 5-minute record, so the history *is* the discharge curve.
