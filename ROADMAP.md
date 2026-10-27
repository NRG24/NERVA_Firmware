# Ring roadmap

The game plan, as of 2026-10-25. It is ordered by what unblocks the most.
**You** means hands-on work (jig, wearing the ring, buying parts). **PC
Claude** is the firmware session on this PC. **Mac Claude** is the app
agent working from `mac-handoff/`.

---

## Where things stand

| Area | State |
|---|---|
| Test firmware | **Frozen**: `../ring-fw-test-v0.6.2/`. It is on ring 2. History works; heart rate measured 4.3 bpm mean error, 68 % within 5 bpm against a WHOOP |
| Production firmware | **v0.9.0 built, not flashed**: `release/ring-fw-v0.9.0-prod.hex` (v0.8.0 plus MCUboot and over-the-air updates, `OTA.md`). v0.8.0 added temperature, hot flashes, spot readings, history v2 |
| Host tests | Syntax pass plus 11 suites, all passing on the PC (`python tests/run_tests.py`) |
| Phone app | Flutter, built by Mac Claude from `mac-handoff.zip` |
| PPG modules | **All MAXM86161s on hand are dead** (2026-10-24); replacements ordered. The boards themselves work (boost gives 5 V, green LED test lit). Nothing below that needs a heart rate can happen until they arrive |
| Hardware | Rev 2026-09-23. Next spin list in `HARDWARE_NOTES.md` (items 13, 1, 11, 10, 6, 4, 2, 14, 15) |
| Commits | v0.8.0 committed as `fdda8a1` (2026-10-27); later work on top of it |

## What v0.8.0 adds (host-tested and compile-verified, not on a ring)

For the app's burnout, stress and night hot-flash features. Design for the
app side: `../mac-handoff/WELLNESS_ANALYTICS.md`.

* **History record v2, 32 bytes:** heart-rate min/max, ring and sensor
  temperature, motion share, spot-reading HR/RMSSD, hot flashes. Capacity
  drops from 38 to 19 days. **Flashing v0.8.0 erases the v1 log: sync
  first.** Packet type `0x03`; a sync below MTU 37 is refused.
* **Spot readings** (`src/spot.c`): 3-minute green windows for a clean
  resting HR and RMSSD: hourly asleep, at most hourly awake and still,
  one on demand (opcode `0x14`), one after a hot flash. Schedule: opcode
  `0x12`.
* **Hot-flash detection** (`src/thermo.c`) from the nRF die temperature:
  quick rise over a settled baseline, night only by default. Opcode
  `0x13`. **Thresholds unvalidated.**
* **Wellness characteristic** `f0a1000b`: live temperature, baseline,
  hot-flash state, last spot reading.
* Cost: ~+0.6-1 mAh/day (`POWER.md`). Test builds keep both features off
  unless the app turns them on; temperature is logged in every build.

## What v0.7.0 changed (all tested on the host, none on the ring yet)

* **Heart rate:**
  * Two bugs found by the new `test_hr` are fixed: a strong harmonic gave
    no reading, and pure noise gave a reading half the time.
  * Spectral averaging cuts false readings on noise from 50 % to 5 % and
    improves mean error from 7.2 to 5.6 bpm on the recordings.
* **Steps:**
  * Rhythm-gated: 30 min of hand movement went from 545 steps to 0.
  * A missed footfall now counts instead of breaking the walk.
* **Power** (estimated ~12 -> ~3 mAh/day, see `POWER.md`):
  * advertising slows after 30 s
  * the ring manages its connection interval
  * the accelerometer runs in low-power mode
  * the PPG front end uses a quarter of the LED energy (provisional)
  * a heart-rate window every 5 min
* **Prod hygiene:** GSR status sampling off; diagnostic opcodes off.

---

## Next: one jig session (You + PC Claude, about 1 h)

1. **Commit first.** Two weeks of work is sitting uncommitted in one folder.
   Say the word and PC Claude commits it on a branch.
2. **Front-end A/B on the current test firmware, before flashing prod**
   (*You*: wear the ring and the WHOOP, hand still near the PC, ~20 min).
   It decides the prod LED setting, which is the biggest single power
   number.

   ```bash
   python tools/ppg_sweep.py --ab
   ```

   PC Claude then sets `CONFIG_RING_PPG_LED_PA`/`AVG_LOG2` in `prod.conf`
   to the cheapest setting that scores as well as the best.
3. **Real step data** (*You*, ~10 min). Walk 100-200 counted steps in laps
   near the PC, then do a minute of desk activity:

   ```bash
   python tools/steps_record.py walk
   ```

   ```bash
   python tools/steps_record.py desk
   ```

   ```bash
   python tools/steps_eval.py steps_*.csv
   ```

   This scores the new counter against the frozen one, on your hand. It
   settles the arm-swing question that `test_arm_swing` can only guess at.
4. **Sync ring 2's history first** (`python tools/history_sync.py`): v0.8.0+ reformats a v1 log.
   Then **flash prod** on ring 2. v0.9.0 brings the bootloader, so this
   is the last flash that has to go over SWD:

   ```bash
   python tools/flash_on_contact.py release/ring-fw-v0.9.0-prod.hex --no-verify --rtt 40 -f 1000000 500000 250000
   ```

   Then run the first-update checklist at the end of `OTA.md` while it is
   still on the jig: one update over the air, with RTT watching.

5. **Accept it** (*You* + PC Claude):

   ```bash
   python tools/prod_check.py
   ```

   Then do the manual checks it prints: the connection interval in the
   log, wake-on-motion, and steps in low-power accelerometer mode.
   **If wake-on-motion fails, set `CONFIG_RING_IMU_LOW_POWER=n` and
   reflash.** It is the riskiest change.
6. **Hand ring 2 to the phone:** dashboard, **Unpair ring**, then pair from
   the app.

## Then: live with it (You, a few days)

* **Battery rundown.** Charge full, wear it, don't connect, and sync the
  history afterwards. Battery millivolts are in every 5-minute record, so
  that gives the real discharge curve and the real capacity, which
  `POWER.md` can only guess.
* **Wear it with the WHOOP** and sync both. Daily heart-rate traces side by
  side are the best validation the ring can get without a lab.

## Antenna (You, whenever)

`ANTENNA_TUNING.md` has the full procedure.

1. Measure stock:

   ```bash
   python tools/rssi_ab.py measure "stock"
   ```

2. **Direction test:** solder ~2 mm of fine wire onto the strip's open
   end and measure again. Worse means the antenna is too long, so shorten
   it; better means too short, so lengthen it.
3. Trim or extend in small steps, measuring each time.

Expect several dB, not 17. Ground plane and finger absorption need the
respin.

## The phone app (Mac Claude)

* **M1/M2:** live data and pairing. They need ring 2 unpaired from the PC
  (jig session, step 6).
* **M3:** history sync and day charts. Works now on ring 2's v0.6.2.
* Things to design for in v0.7.0 (handoff §3):
  * steps arrive in jumps of 8
  * heart rate is often silent
  * a heart-rate window every 5 min, plus `0x03` on the HR screen

## Things only data can decide

| Question | How it gets answered |
|---|---|
| Is 15 mA 2x good enough for heart rate? | `ppg_sweep.py --ab` |
| Does the step counter halve on a real arm swing? | `steps_record.py` walks, then `steps_eval.py` |
| What is the battery capacity, and the real day's draw? | Rundown test; a PPK2 (POWER.md) |
| Do the PPG glitches follow the radio? | Scope VLED (respin test point), or retry opcode 0x11 on a bench build |
| Was opcode 0x11 the 2026-10-08 reboot? | A bench build plus RTT while sending it alone |
| Is the antenna tuned low? | The `rssi_ab.py` direction test |
| Does the ring temperature see a fast finger warm-up at all? | `tools/wellness_watch.py --mode 2`, then wrap the ringed hand round a warm mug or hand warmer (~40 C, ring kept dry) for 2 min: a hot flash should log within ~3 min. A cheap stand-in for a real one |
| How far does the die read under skin? | `wellness_watch.py`: `temp` against `skin` once worn 30 min |
| Do night spot readings agree with the WHOOP? | A night with both, then compare the synced `rest_hr_bpm`/`rest_rmssd_ms` with WHOOP's resting HR and HRV |
| Is 1.0 C the right hot-flash threshold? | Only real events, confirmed or dismissed in the app |

## Next board spin (when ready)

`HARDWARE_NOTES.md` has the list and the reasons. In short:

* **P0:** stiffener and anchored battery connector, debug connector,
  electrode clearance, ACC_INT test point.
* **P1:**
  * BOOST_EN pull-up (DNP)
  * RESET_N
  * 3.0 V rail (optional)
  * **antenna pads, trimmable strip, solid ground (14)**
  * **quiet PPG supply plus VLED test point (15)**
* **GSR:** R5 470k-1M, quiet ADC reference, bigger electrodes, or drop
  GSR until all three are possible.
