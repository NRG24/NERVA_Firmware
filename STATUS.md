# NERVA firmware — project status

Written 2026-09-17 at `v0.4.1-review-fixes`. This is the honest one-page
view: what the firmware does, what has actually been proven, what is known
to be wrong, and what can hurt you. Every other document is linked from
here. If this file and another disagree, this one is newer.

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
| Activity characteristic (steps/sleep/calories over BLE) | **Never exercised by any phone** | same as the rest of the Ring Service, see row above |

---

## 3. Features

**Power model.** Three states. `IDLE`: optics off, 5 V boost off, IMU wake
armed at 80 mg, CPU asleep, accelerometer polled every 200 ms — or every
40 ms while recent motion says the wearer may be walking, which is what the
pedometer needs to see footfalls at all (§5). `MEASURING`: 15 s window every
60 s, abandoned after 6 s if no finger. `CHARGING`: all optics off, and no
accelerometer reads at all, so steps and sleep stop with it. After 3 min
without motion the ring stops probing on a timer and waits for the
interrupt. The app can change the duty cycle or force a window.

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
tick fired — times body weight (default 70 kg, settable over BLE).

**All three are new and, like heart rate, unvalidated** (§2, §5). The one
to keep in front of you: **sleep has no wear detection.** A ring on a
nightstand is perfectly still and logs a full night. That is confirmed
behaviour, and it is not fixable in firmware while the power model stops
opening PPG windows — the only wear signal — after three minutes of
stillness.

**BLE.** Standard HRS (0x180D) and BAS (0x180F), open by default so generic
apps work. Custom Ring Service (`f0a1…`) with a 20-byte status packet, a
17-byte activity packet, raw PPG and IMU streams, and a control
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
| R6 | **Resting heart rate leaks over open HRS** to anyone in range. | Certain (chosen) | none | `-DCONFIG_RING_HRS_OPEN=n` — costs generic-app compatibility |
| R7 | **No MITM protection.** Just Works is encrypted but unauthenticated. | Certain | none | needs a display/keypad or OOB; not fixable in firmware alone |
| R8 | **No OTA.** Field units can only be updated over SWD, which on this board needs series resistors and a fresh APPROTECT unlock. | Certain | — | MCUboot + DFU is the next big item |
| R9 | **PMIC I2C dies while charging** → ring parks with optics off, then after 30 s guesses "idle" and may run LEDs against a 20 mA charger. | Low | `PMIC unreachable for 30 s` | power question, not a hazard; PMIC health flag tells the app |
| R10 | **Custom BLE service has never been exercised.** Any of status/stream/control could be wrong on first contact. | Medium | app sees nothing / wrong bytes | it is all logged; fix from RTT |
| R11 | **Bench probe uses the bus-wedging bare read.** Only in `-Bench` builds. | Certain in bench | MAXM86161 stops answering until bus idles | production has no bare read; see `POSTMORTEM.md` |
| R12 | **Battery "percent" over BAS is a voltage bar,** not state of charge. | Certain | wrong mid-range | app should use `battery_mv` from the status packet |

---

## 5. Known-bad and open

* **GSR does not produce a meaningful reading and nobody knows why.** The
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
* **No unit tests, no CI.** Verification is the five-configuration build
  sweep run by hand on one Windows machine with NCS installed at `C:\ncs`.
* **The pedometer needs a fast poll, and that changes the idle power
  model.** 5 Hz — the old idle poll rate — does not undercount gait, it
  misses it almost entirely: simulated against a 5 min walk it counted
  nothing below a 250 mg magnitude swing, against 0–1 % error at 25 Hz.
  So `RING_IDLE` now polls the accelerometer every 40 ms whenever recent
  motion suggests the wearer may be walking, and drops back to 200 ms once
  they are still (`STEP_POLL_MS` / `STEP_MOTION_MG` in `main.c`). A still
  ring — the whole of the night — is unchanged at 5 Hz, and the extra
  ~20 I2C reads a second while moving are small beside the optical front
  end's 15 mA duty cycle. Both numbers are simulation, not bench
  measurement: **nobody has measured what this costs on a real battery.**
* **The 100 mg detection floor is a guess.** It rejects typing and
  gesturing in simulation and it rejects gentle walking too. Where that
  line actually belongs can only be settled on a wrist — sorry, a finger —
  with a reference count.
* **Body weight for the calorie estimate is not persisted.** It lives in
  RAM only (`calories.c`), defaults to 70 kg, and resets to that default on
  every reboot. The app has to resend control opcode `0x06` after every
  power cycle if the wearer is not 70 kg.
* **No RTC**, so sleep sessions are durations from `k_uptime_get()`, not
  clock times — see `APP_INTEGRATION.md` §7. A session spanning a reboot
  (watchdog reset, battery pull) is lost: `sleep.c` has no persistence and
  starts back in the awake state.

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
