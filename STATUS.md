# NERVA firmware — project status

Written 2026-09-17 at `v0.4.1-review-fixes`, revised 2026-09-19 for the
sleep wear check. This is the honest one-page view: what the firmware does,
what has actually been proven, what is known to be wrong, and what can hurt
you. Every other document is linked from here. If this file and another
disagree, this one is newer.

---

## 1. State in one paragraph

Zephyr 4.4 / nRF Connect SDK v3.4.0 firmware for a smart ring on a u-blox
ANNA-B402 (nRF52833): optical heart rate (MAXM86161), motion wake
(LSM6DSV16BX), charging (BQ25120A), GSR front end, and BLE with standard
Heart Rate and Battery services plus an encrypted custom service. It builds
clean in five configurations. **The last version that ran on a physical
board is `v0.1-bench`.** Everything since — survivability, BLE security,
recovery paths, roughly 3,000 lines — is compile-verified only, because no
working board has existed since late August. Treat the first flash of
anything newer as a bring-up, not an update.

---

## 2. Proven vs unproven

| Area | Status | Evidence |
|---|---|---|
| MAXM86161 enumerates, streams green PPG at 100 sps | **Proven** | RTT log, `v0.1-bench`, 2026-08-23 |
| Finger detection (DC 2,860 empty vs 53,000 finger) | **Proven** | measured on board |
| Beat detection produces plausible resting rates | **Proven plausible only** | 55–64 bpm on a finger; never compared to a reference |
| No false HR on an empty sensor | **Proven** | three attempts to fix, then held |
| IMU reads gravity, wake-on-motion fires | **Proven** | after reflowing U4; cold joint on INT1 |
| PMIC configures, charges at 20 mA | **Proven** | `STAT=01` observed |
| Battery voltage via VBMON | **Proven** | 3,696–3,780 mV observed |
| BLE advertises, visible on a phone | **Proven** | phone scan, HRS/BAS only |
| Green power-LED bench indicator | **Proven** | this is the image in `release/` |
| Watchdog, reboot-on-fatal, reset-cause report | **Compile only** | `v0.2` |
| Bench instruments compiled out by Kconfig | **Compile only** | 7 kB flash delta proves the mechanism |
| Honest health flags, PPG/IMU/PMIC recovery paths | **Compile only** | `v0.3`–`v0.4.1` |
| Custom Ring Service — status, PPG stream, IMU stream, control | **Never exercised by any phone** | no subscription has ever happened |
| Encryption, Just Works pairing, bond persistence in NVS | **Compile only** | partition never written |
| Advertising restart after disconnect | **Compile only** | `recycled()` traced in Zephyr source |
| GSR reading | **Unknown** | see §5 |
| Heart-rate accuracy | **Unvalidated** | never against a reference monitor |
| Step count, sleep sessions, calorie estimate | **Compile only, unvalidated** | new this revision; no board, no reference pedometer or sleep log to compare against |
| RMSSD arithmetic | **Verified against the definition** | `tests/test_hrv.c` compares the integer pipeline to the textbook formula in floating point |
| RMSSD against a real heart | **Unvalidated** | never compared to an ECG or a chest strap; rests on a beat detector that is itself unvalidated |
| SpO2 ratio-of-ratios arithmetic | **Verified against the definition** | `tests/test_spo2.c`, known AC/DC on each channel |
| SpO2 percentage | **UNCALIBRATED, and flagged as such on the wire** | the R-to-SpO2 curve is a literature default; fitting it needs desaturation against a reference oximeter |
| Red and IR LEDs ever lit | **Never** | only the green LED has run on hardware; the two-slot sequence is compile-verified only |
| Activity characteristic (steps/sleep/calories over BLE) | **Never exercised by any phone** | same as the rest of the Ring Service, see row above |
| Sleep wear check reaches the right verdict | **Simulated only** | `scratchpad/sim_sleep_wear.c`, 137 checks; compiles `sleep.c` for real, models `main.c` |
| The DC threshold the wear check rests on, for a ring worn at rest | **Never measured** | 53,000 was a finger pressed on a bench sensor, not a ring on a sleeping hand — see §5 |

---

## 3. Features

**Power model.** Three states. `IDLE`: optics off, 5 V boost off, IMU wake
armed at 80 mg, CPU asleep, accelerometer polled every 200 ms — or every
40 ms while recent motion says the wearer may be walking, which is what the
pedometer needs to see footfalls at all (§5). `MEASURING`: 15 s window every
60 s, abandoned after 6 s if no finger. `CHARGING`: all optics off, and no
accelerometer reads at all, so steps and sleep stop with it. The app can
change the duty cycle or force a window.

`STILL_TIMEOUT_MS` is **not** a stillness timeout, whatever it is called and
whatever this document used to say. Finding a finger during a window
refreshes `last_motion`, so a **worn** ring never goes stale however still
the wearer is — it keeps measuring 15 s in every 60 all night. What goes
stale is a ring that is **off a finger**: it stops finding one, stops
refreshing `last_motion`, and falls silent about three minutes later. That
correction came out of the simulation described in §5, and R13 is what it
costs.

**Sensing.** Green-LED PPG at ~15 mA, 100 sps, FIFO polled every 20 ms.
Integer-only HR: DC tracker → 4 Hz low-pass → adaptive threshold → median of
8 intervals with a 60 %-of-median refractory. Battery in millivolts from the
PMIC monitor. GSR through a transimpedance stage into the SAADC.

**Activity.** Steps, sleep and calories, all derived from the IMU's
accelerometer magnitude and none of them from any new sensor. Steps:
peak-detection pedometer (`steps.c`), same technique as the HR detector
applied to motion, needing roughly a 100 mg swing to fire. Sleep
(`sleep.c`): one-minute stillness buckets, 10 consecutive still minutes to
start a session, 3 consecutive active minutes to end one, and a session
cannot span a gap in accelerometer data such as a charge. Calories
(`calories.c`): a MET table keyed on steps *per minute* — a count over the
interval, not a sampled instantaneous cadence, which is what keeps a
minute from inheriting whatever the wearer was doing at the instant the
tick fired — times body weight (default 70 kg, settable over BLE). All
three stop dead in `CHARGING`, which reads no accelerometer: a ring on a
charger is not a ring being worn, and none of them pretend otherwise.

**All three are unvalidated, like heart rate** (§2, §5).

**Sleep wear check.** Sleep used to have no wear detection at all: a ring on
a nightstand is perfectly still and logged a full night. It now corroborates
a session against the PPG DC level — the only wear signal this board has,
and one of the few things actually measured on it (2,860 empty against
53,000 on skin). While a session is open, `IDLE` opens a 6 s PPG window
every 30 minutes purely to read DC, ignoring the stale-timeout that would
otherwise keep the optics off. One positive reading makes the session
`worn`; two negatives make it `not worn`; anything less, or a PPG that will
not answer, leaves it `unknown` — which is a real answer and not a synonym
for "not worn".

**The verdict is reported, not enforced.** A session that fails the check is
still reported as sleep, with its minutes still in `sleep_total_min`, and
the app gets the verdict plus the raw check counts to decide with. The
threshold behind the verdict was measured with a finger pressed against a
bench sensor, never with a ring worn loosely on a sleeping hand, so
suppressing a session in firmware would trade a false positive the app can
filter for a false negative nobody would ever see. Wire format:
[APP_INTEGRATION.md](APP_INTEGRATION.md) §7. Power cost and what it does not
cover: §5. Build options: `CONFIG_RING_SLEEP_WEAR_CHECK` and the two
settings under it.

**The sleep classifier is closer to binary than its name suggests**:
simulated over 8 hours, a wearer moving at least every 8 minutes logs no
sleep at all, one moving only every 16 minutes logs the entire stretch. The cliff is the ten consecutive still minutes that onset
requires; `tests/test_activity.c` pins both ends of it so tuning cannot
move it unnoticed.

**GSR stream.** Raw ADC counts at 20 Hz on its own characteristic, opcode
`0x08`, off by default. The 1 Hz `gsr_mv` in the status packet cannot
resolve a skin conductance response at all -- they peak about 1.4 s after
onset -- so this exists to find out whether the front end produces
anything real. Counts rather than millivolts, because the conversion
constants are board-specific and not yet trusted. It holds `GSR_PWR` on
for as long as it runs, so it suspends the analog duty cycling; forced off
on disconnect.

**HRV.** RMSSD over a rolling window of the last 64 successive
differences, on its own characteristic (`hrv.c`). Only intervals that pass
the beat detector's plausibility range *and* agree with the recent median
to within 20 % are used -- half the 40 % the heart rate itself tolerates,
because a metronome rhythm with a motion artifact invents 200 ms of HRV at
the looser bar, and a difference is never formed across a rejected beat — that
one rule is worth 144 ms of false HRV in simulation. The hardware sets the
ceiling: beats land on 100 sps samples, so a metronome heart measures
about 8.5 ms of HRV that is not there, which inflates low readings
proportionally more than high ones.

**SpO2.** Ratio of ratios on red and IR, opt-in via opcode `0x09`, on its
own characteristic (`spo2.c`). The driver gained a two-slot LED sequence
for it, refactored so the proven green path and the new red/IR path share
one init table rather than acquiring two that drift. **R is published and
is a real measurement; the percentage is not.** The R-to-SpO2 curve is the
published default and has never been fitted against this hardware, so
every reading carries an UNCALIBRATED flag that no build can clear, and
anything outside 70-100 % is withheld rather than shown. Heart rate pauses
while the mode is on, because every HR number here comes from the green
channel.

**BLE.** Standard HRS (0x180D) and BAS (0x180F), open by default so generic
apps work. Custom Ring Service (`f0a1…`) with a 20-byte status packet, a
20-byte activity packet, a 3-byte HRV packet, raw PPG and IMU streams, and
a control
characteristic — all requiring an encrypted link. Just Works pairing, one
bond, persisted. Device Information Service reports firmware `0.5.0`.
Control opcode `0x05` clears bonds; `0x06`/`0x07` set body weight and reset
the activity counters. Full wire contract:
[APP_INTEGRATION.md](APP_INTEGRATION.md).

**Survivability.** 10 s hardware watchdog fed only from the main loop;
fatal errors reboot; reset cause logged at boot. Health flags reflect
whether each device is answering *now*. PPG re-probes after 5 failed
reads, IMU re-inits after 10 with a 60 s backoff and gives up after 3
cycles, an unreachable PMIC stops moving the state machine and falls back
to idle after 30 s.

**Build.** One tree, two images: `.\build.ps1` (production) and
`.\build.ps1 -Bench` (instrumented, green LED held on). Bench instruments
are Kconfig, never hand-edited source.

---

## 4. Risk register

Ordered by how much it would hurt, not how likely it is.

| # | Risk | Likelihood | Symptom | Recovery |
|---|---|---|---|---|
| R1 | **Charging with no battery temperature limit.** TS ball C3 is unconnected; firmware clears `TS_EN` so charging works at all. Cell is worn against skin. | Certain (by design) | none until something goes wrong | Fit the TS divider on the next spin — `HARDWARE_NOTES.md` §9. Do not ship without it. |
| R2 | **Watchdog reboot-loop on a merely slow board.** Timeout was sized by inspection. A stalled I2C bus costs 500 ms per transaction. | Low–medium | `reset cause: WATCHDOG` every boot | chip-erase, flash `v0.1-bench`. Or build with `-DCONFIG_RING_WATCHDOG=n`. |
| R3 | **NVS partition at 0x7c000 has never been written.** A used board has stale bytes there. | Medium on a reused board | `settings_load failed`, no advertising | `pyocd erase --chip` before the first v0.4 flash. `CONFIG_NVS_INIT_BAD_MEMORY_REGION=y` is the safety net, untested. |
| R4 | **The first phone to pair owns the ring** until opcode `0x05` is sent over the bonded link. | Certain (by design) | owner's phone gets pairing failure | owner sends `0x05`; otherwise SWD erase |
| R5 | **Heart rate is unvalidated.** Numbers are plausible, not correct. | Certain | none — it looks fine | validate against a chest strap before any clinical or health claim |
| R5a | **Steps, sleep and calories are unvalidated,** and doubly so for a finger-worn ring, which does not move the way a wrist or waist does. | Certain | none — the numbers look plausible | compare against a reference pedometer/sleep log once hardware exists before showing these without a caveat |
| R5b | **The wear check can call a real night "not worn."** Its threshold (`FINGER_DC_MIN`) was measured with a finger pressed against a bench sensor. A ring that sits loosely, or rotates off the pad in sleep, may read empty. | Unknown — never measured | a worn night reports `wear_state = 2` | the firmware never acts on it: the session is still reported, and `wear_checks`/`wear_confirmed` show the app how thin the evidence is. Measure worn-at-rest DC on the first working board and move the threshold |
| R6 | **Resting heart rate leaks over open HRS** to anyone in range. | Certain (chosen) | none | `-DCONFIG_RING_HRS_OPEN=n` — costs generic-app compatibility |
| R7 | **No MITM protection.** Just Works is encrypted but unauthenticated. | Certain | none | needs a display/keypad or OOB; not fixable in firmware alone |
| R7a | **An app could show the uncalibrated SpO2 percentage as a reading.** The flag is set on every path and the docs are explicit, but nothing in the firmware can stop a phone ignoring it. | Certain (by design) | a plausible-looking number that means nothing clinically | show `ratio_x1000`, or label the percentage; calibrate before any health claim |
| R8 | **No OTA.** Field units can only be updated over SWD, which on this board needs series resistors and a fresh APPROTECT unlock. | Certain | — | MCUboot + DFU is the next big item |
| R9 | **PMIC I2C dies while charging** → ring parks with optics off, then after 30 s guesses "idle" and may run LEDs against a 20 mA charger. | Low | `PMIC unreachable for 30 s` | power question, not a hazard; PMIC health flag tells the app |
| R10 | **Custom BLE service has never been exercised.** Any of status/stream/control could be wrong on first contact. | Medium | app sees nothing / wrong bytes | it is all logged; fix from RTT |
| R11 | **Bench probe uses the bus-wedging bare read.** Only in `-Bench` builds. | Certain in bench | MAXM86161 stops answering until bus idles | production has no bare read; see `POSTMORTEM.md` |
| R12 | **Battery "percent" over BAS is a voltage bar,** not state of charge. | Certain | wrong mid-range | app should use `battery_mv` from the status packet |
| R13 | **A worn ring measures 15 s in every 60 indefinitely,** including all night. `last_motion` is refreshed by finding a finger, so `STILL_TIMEOUT_MS` never fires while the ring is on a hand. Eight hours is ~5,745 s of LED — order 39 mAh at the cell under §5's assumptions, which is more than a plausible ring battery holds. | Certain (by design, and nobody noticed) | battery flat by morning | not touched here — it is a duty-cycle decision that needs a characterised battery first. The app can already drop the night-time duty cycle with opcode `0x04`. Found by the simulation in §5 |

---

## 5. Known-bad and open

* **GSR does not produce a meaningful reading and nobody knows why**, and
  the new 20 Hz stream (opcode `0x08`) is the instrument meant to settle
  it rather than a feature built on top of it. Look at a trace off a real
  finger before trusting any of it. The
  ADC path had two firmware bugs; one (`zephyr,vref-mv` missing) is fixed
  and alone explains every 0 mV ever seen. The second (SAADC channel index)
  is a hypothesis; the move to channel 0 is made and harmless, and the
  bench A/B line decides it — most likely both channels read the same. Even
  with the ADC right, R5 = 91 kΩ against dry skin through 2 mm electrodes
  gives 1–10 % swing. `HARDWARE_NOTES.md` §12 sizes R5. U7 also runs at its
  1.8 V absolute minimum from a GPIO (§10).
* **`HANDOFF.md` and `BRINGUP_RESULTS.md` once said GSR was a dead U7.** It
  was not. The claim was written before the `vref-mv` bug was found and has
  been corrected in HANDOFF; BRINGUP_RESULTS still carries the original
  reasoning as a historical record.
* **`src/main.c.bench` is a stale duplicate** from before Kconfig replaced
  it. It does not build, has none of the survivability work, and exists
  only because nobody has said to delete it. `git checkout v0.1-bench` is
  the real copy.
* **SWD is hand-soldered and flaky.** `No ACK` at random; one flash took 56
  attempts. `scratchpad/flashloop.py` retries. Next spin needs the
  SM03B-SRSS-TB connector (`HARDWARE_NOTES.md` §1).
* **The IMU on the board is not the one in the datasheet folder.**
  `WHO_AM_I = 0x71` is an LSM6DSV16BX; the filed PDF is the LSM6DSV. Basic
  registers match; embedded functions do not.
* **Logging is INF over an 8 kB RTT buffer in production.** Fine for
  bring-up, wasteful for a shipped image.
* **No CI, and the only behavioural tests are for the activity modules.**
  Verification is still the five-configuration build sweep run by hand on
  one Windows machine with NCS installed at `C:\ncs`. `cd tests && make`
  now adds two things to that: it runs `steps.c`/`sleep.c`/`calories.c`
  against a simulated wearer, which is what caught the defects listed
  above, and it puts every file in `src/` through
  `-fsyntax-only -Wall -Wextra -Werror` against stub Zephyr headers, in
  both the production and bench configurations.
  The syntax pass is a filter, not a build — the stubs are ours, so it
  proves the code type-checks against our idea of the API and nothing
  more. The behavioural tests cover three files out of thirteen and feed
  them an assumed footfall rather than a recording off this board.
  Nothing here has ever run on hardware.
  The one other simulation is `scratchpad/sim_sleep_wear.c`, which
  exercises the sleep wear check; it is not part of `make`, below.
* **The pedometer needs a fast poll, and that changes the idle power
  model.** 5 Hz — the old idle poll rate — does not undercount gait, it
  misses it almost entirely: simulated against a 5 min walk it counted
  nothing below a 250 mg magnitude swing, against 0–1 % error at 25 Hz.
  So `RING_IDLE` now polls the accelerometer every 40 ms whenever recent
  motion suggests the wearer may be walking, and drops back to 200 ms once
  they are still (`STEP_POLL_MS` / `STEP_MOTION_MG` in `main.c`). A still
  ring — the whole of the night — is unchanged at 5 Hz.
  Do not read "only while moving" as "almost never", though: the 10 s
  hold after the last qualifying motion means an ordinary active day
  spends a lot of the waking hours on the fast poll. Simulated across
  three days of a plausible routine, 41 % of samples were at 40 ms and
  the average rate was 7.1 Hz against the old flat 5 Hz. That is still
  small beside the optical front end's 15 mA duty cycle, but it is a
  ~40 % increase in accelerometer traffic, not a rounding error. Every
  figure here is simulation: **nobody has measured what this costs on a
  real battery.**
* **The 100 mg detection floor is a guess.** It rejects typing and
  gesturing in simulation and it rejects gentle walking too. Where that
  line actually belongs can only be settled on a wrist — sorry, a finger —
  with a reference count.
* **Nothing in the activity feature is persisted.** Steps, sleep minutes,
  calories and the body weight the estimate depends on all live in RAM and
  start from zero on any reset — watchdog (R2), fatal error, flat battery,
  or the battery contact bouncing under flex (`HARDWARE_NOTES.md` §13).
  This is deliberate rather than an oversight: the settings partition has
  never been successfully written on this board (R3), and putting a step
  counter on a flash-write path before that is proven would put the bonds
  at risk alongside it. The app is the system of record and has to
  accumulate its own totals, watching `uptime_s` in the status packet to
  spot a reboot — `APP_INTEGRATION.md` §7 spells out how. Revisit once
  NVS has demonstrably worked on real hardware.
* **No RTC**, so sleep sessions are durations from `k_uptime_get()`, not
  clock times — see `APP_INTEGRATION.md` §7. A session spanning a reboot
  (watchdog reset, battery pull) is lost: `sleep.c` has no persistence and
  starts back in the awake state. The wear verdict goes with it.

### The sleep wear check: what it costs and what it does not prove

The nightstand false positive is fixed in the sense that the firmware now
takes a real reading and reports it. It is not fixed in the sense that
anybody has watched it work.

**How it was verified.** `scratchpad/sim_sleep_wear.c`, built with

```
gcc -std=c11 -Wall -Wextra -O1 -I src -I scratchpad/sim_stubs \
    scratchpad/sim_sleep_wear.c src/sleep.c -o /tmp/sim_sleep_wear
```

against a three-macro stub of `<zephyr/sys/util.h>` in
`scratchpad/sim_stubs/`. 137 checks, all passing. It compiles `sleep.c`
itself, so what it says about `sleep.c` holds for the shipped code. It does
**not** compile `main.c` — that cannot be done without Zephyr — so the
window scheduler in its part B is a hand model written from `main.c`'s
constants. It can show the policy is self-consistent and it caught a real
design bug; it cannot show that `main.c` implements it. Nobody has run any
of this on hardware, because there is no hardware.

**What it costs.** Simulated, at the defaults (6 s window, 30 min interval),
over an eight-hour session:

| Case | LED before | LED after | Delta |
|---|---|---|---|
| Ring worn | 5,745 s | 5,745 s | −9 s to +0 s across 38 duty-cycle phases |
| Ring on a nightstand | 12 s | 108 s | **+96 s**, 16 wear windows |

The worn case is free because of the R13 correction above: a worn ring is
already measuring every minute, and those windows answer the wear question
on their way out. At most one extra window opens per session, usually none.
(The delta goes slightly negative at some phases because a 6 s wear window
pushes the next scheduled window out by a full period like any other, so it
sometimes displaces a 15 s one. That is lost heart rate, not a saving.)

The nightstand case pays: 96 s at ~15.4 mA off the 5 V rail is ~0.41 mAh at
5 V, or **~0.65 mAh referred to a 3.7 V cell assuming 85 % boost
efficiency**, before the AFE's own draw. Both of those assumptions are
assumptions. **Nobody has characterised the battery**, so what fraction of a
night this is cannot be stated — on a 15 mAh cell it would be a few percent.
Measure it on the first working board and move
`CONFIG_RING_SLEEP_WEAR_CHECK_INTERVAL_MIN` against a real number, or set
it past a night's length for exactly one check per session.

**What was rejected, and why.** Requiring some minimum micro-movement per
session would have been free — a worn ring twitches, a table does not — and
it is exactly the kind of invented threshold R5/R5a exist to warn about.
Nothing here has ever been compared against a sleeping person, and the
failure mode is discarding the nights of whoever sleeps most still. Doing
nothing but adding a flag was the other option; it is honest, but the app
has no wear signal of its own either, so it amounts to relabelling the bug.
The choice made was to spend the LED current on a real reading and then
report it rather than act on it.

**What is still not covered.**

* A ring worn so loosely, or rotated so far, that the PPG never sees skin
  reads exactly like a nightstand. R5b.
* A dead or absent PPG leaves every session `unknown` forever. That is the
  correct answer, and it means the nightstand case is unfixed on a board
  whose sensor has failed. Same for a part that starts cleanly and then
  returns an empty FIFO: an empty read is not a failed one, so it never
  trips `PPG_FAIL_LIMIT`, and `hr_primed()` is what stops it being read as
  a confident "not worn".
* The check says the ring was on *a* hand, not that its owner was asleep.
  Sitting motionless at a desk with the ring on still reads as sleep, and
  now reads as *confirmed worn* sleep, which is arguably worse — it looks
  more trustworthy without being more correct.
* Nothing is persisted. A reboot mid-session loses the session and its
  verdict together.

---

## 6. Hardware changes pending

Thirteen items in [HARDWARE_NOTES.md](HARDWARE_NOTES.md). The ones that
block shipping: **§9 TS divider** (R1 above), **§1 debug connector**,
**§13 battery contact resets the MCU under flex**. The ones that decide
whether GSR ever works: **§12 R5 sizing**, **§10 U7 supply**, **§11
electrodes can bridge to a rail**.

---

## 7. First flash of anything newer than v0.1

1. `python -m pyocd erase -t nrf52833 -f 1000000 --chip` — clears
   APPROTECT and the stale storage region in one go.
2. Flash `release/ring-fw-v0.4-bench.hex` **first**. The green LED answers
   "is it alive?" without a debugger.
3. Watch the first 30 s of RTT for, in order: `reset cause:` (should not
   say WATCHDOG), `MAXM86161 found at 0x62`, `watchdog armed: 10000 ms`,
   `advertising as Ring PPG`, the `PMIC TS monitor disabled` warning.
4. Pair from a phone: `pairing requested`, `security … level 2`,
   `paired … bonded yes`.
5. Only then flash the production image.

If it reboot-loops: erase, flash `ring-fw-v0.1-bench-greenled.hex`, and
disconnect the **battery**, not just the charger — the watchdog survives a
soft reset.

### Validating the activity features

These have never touched hardware, and the numbers they produce are
plausible-looking whether or not they are right — which is the dangerous
kind of wrong. The `activity:` line over RTT is the only way to see them
without a working phone app; it prints whenever the step count or the
sleep state changes. Do these in order, because each one tells you
something the next assumes:

1. **Does it count at all?** Hold the board and walk 100 steps, counting
   out loud. Expect an `activity:` line with a step count in the
   neighbourhood and a plausible `spm`. Anything near zero means the
   detector is not seeing the motion at all: check `AMPLITUDE_FLOOR_MG`
   in `steps.c` against what a finger-worn ring actually swings, which is
   the single biggest unknown in this feature.
2. **Does it over-count?** Put the board on a desk for ten minutes, then
   type at a keyboard with it strapped on for another ten. Both should
   add zero steps. Tremor and typing counting as walking is the classic
   pedometer failure and is what the floor is defending against.
3. **Is the poll rate doing its job?** Walk 100 steps again, this time
   watching whether the count keeps up in real time rather than arriving
   late. `STEP_POLL_MS` exists because 200 ms polling counted essentially
   nothing in simulation (§5); if counting is erratic while walking,
   suspect the fast poll is not engaging — `STEP_MOTION_MG` decides that.
4. **Sleep.** Leave the board perfectly still for 15 minutes: expect an
   `asleep` line after about 10. Then pick it up and move it for 3
   minutes: expect `awake`. Note that a board on a bench passes this test
   perfectly while telling you nothing about a sleeping human — see the
   wear-detection caveat in §5.
5. **GSR, which is the point of the stream.** Enable it with control
   opcode `0x08` and record a trace with the electrodes on a finger. Take
   a deep breath, or have someone clap unexpectedly: a skin conductance
   response should appear as a rise peaking roughly 1.4 s later. If the
   trace is flat, or is pure noise with no response shape in it, that is
   the answer the project has been waiting for since August — write it
   into `BRINGUP_RESULTS.md` either way. Expect the first notification
   about a second after enabling; that is the 800 ms front-end settle.
6. **SpO2, and its LEDs.** Send opcode `0x09` and watch for the two
   `PPG slot` lines at window start — slot 1 IR, slot 2 red. **This is the
   first time the red and IR drivers will ever have been lit on this
   board**, so check they come up at all before trusting anything
   downstream. Then look for `SpO2 R x.xxx -> nn%` once a second with a
   finger on. Record R against a reference pulse oximeter on the same
   finger: those pairs are exactly what a calibration needs, and until
   somebody collects them the percentage is arithmetic, not a reading.
7. **Calories.** Send control opcode `0x06` with a real body weight
   before believing any number; the default is 70 kg. At rest the total
   should climb about 1.2 kcal per minute for a 70 kg wearer, which is a
   figure you can check against the clock.

Numbers worth recording in `BRINGUP_RESULTS.md` when you do: the step
count against a hand count, the magnitude swing a real walking finger
produces, and whether typing generates false steps. All three are
assumptions in `tests/sim.c` right now, and every accuracy claim in this
repo rests on them.

---

## 8. Version history

| Tag | What | Hardware |
|---|---|---|
| `v0.1-bench` | bring-up, green LED, all sensors, BLE advertising | **ran** |
| `v0.2-phase1` | Kconfig bench switch, watchdog, reboot-on-fatal, reset cause, fast boot, honest flags, I2C general-call fix | compile only |
| `v0.3-review-fixes` | 7 review findings incl. the charger-state bug and I2C-timeout-vs-watchdog | compile only |
| `v0.3.1-review-fixes` | 3 more: IMU flag lied for unconfigured part, PMIC stranding, audit | compile only |
| `v0.4-ble` | MTU NULL-deref fix, encryption, pairing, NVS bonds, DIS, flash partition | compile only |
| `v0.4-sensing` | SAADC channel 0, IMU mid-run re-init | compile only |
| `v0.4.1-review-fixes` | 8 more: advertising restart, settings_load fatal, NVS bad-region, opcode 0x05, backoff, torn-read lock, RING_HRS_OPEN | compile only |

Every batch since v0.2 was reviewed adversarially and every review found
real bugs in the previous batch, including two that were strictly worse
than what they replaced. Assume the next review will too.

---

## 9. The documents

| File | Read it when |
|---|---|
| [HANDOFF.md](HANDOFF.md) | starting a new session — traps, build, what is open |
| [APP_INTEGRATION.md](APP_INTEGRATION.md) | writing the phone app |
| [HARDWARE_NOTES.md](HARDWARE_NOTES.md) | respinning the PCB |
| [POSTMORTEM.md](POSTMORTEM.md) | before probing any I2C device — two days lost to a masked errno |
| [BRINGUP_RESULTS.md](BRINGUP_RESULTS.md) | what each subsystem did on the real board |
| [FLASHING.md](FLASHING.md) | the 600-character version for whoever holds the probe |
| [release/README.md](release/README.md) | which hex to flash and what to watch |
| [README.md](README.md) | build, netlist, SWD levels |
