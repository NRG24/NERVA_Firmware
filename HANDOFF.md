# Handoff — read this first

Written 2026-08-23 for a session starting cold. Everything here is either
verified on hardware or explicitly flagged as unverified.

---

## 1. What this is

Firmware for a smart ring: optical heart rate (MAXM86161), IMU
(LSM6DSV16BX), PMIC (BQ25120A), 5 V boost (TPS61240), on a u-blox
ANNA-B402 (nRF52833) module. Built on nRF Connect SDK v3.4.0 / Zephyr 4.4.

Read the other docs in this order if you have time:

| Doc | Why |
|---|---|
| `POSTMORTEM.md` | Two days lost to a masked error code. The rules at the end matter. |
| `BRINGUP_RESULTS.md` | What each subsystem actually did, plus five hardware discoveries |
| `HARDWARE_NOTES.md` | Nine changes for the next PCB spin |
| `APP_INTEGRATION.md` | The phone-side BLE contract |

---

## 2. Build and flash

```bash
.\build.ps1
```

Under the hood, and the parts that are non-obvious:

```bash
west build -b ring_anna/nrf52833 -d build . -- -DBOARD_ROOT=<app dir>
```

* west runs with its **cwd inside `C:\ncs\v3.4.0`** because the app lives
  outside the workspace
* **`-DBOARD_ROOT` is mandatory.** In NCS 3.x sysbuild is the top-level
  CMake source, so the app directory is not auto-added as a board root and
  the custom `ring_anna` board fails to resolve
* Toolchain launches via
  `nrfutil sdk-manager toolchain launch --ncs-version v3.4.0`

Flash and log with pyOCD (Raspberry Pi Debug Probe, CMSIS-DAP):

```bash
python -m pyocd flash -t nrf52833 -f 1000000 build/ring-fw/zephyr/zephyr.hex
```

```bash
python -m pyocd rtt -t nrf52833 -f 1000000
```

There is **no UART on this board**. RTT over SWD is the only console.

---

## 3. Traps that will cost you hours

These are all confirmed on hardware. Every one of them looked like a
different problem than it was.

**Never probe the MAXM86161 with a bare address read.** `i2c_read()` with
no register pointer returns `-EIO` and leaves the part unable to answer
register reads until the bus idles. A 112-address bus scan built from bare
reads broke the sensor at every boot and cost two days and two boards.
Pointer-first, always. Full story in `POSTMORTEM.md`.

**Never return a synthesized errno during bring-up.** The driver returned
its own `-ENODEV` for two days, hiding the real error. One boot with
per-transaction logging solved what two days of theorising could not.

**The PMIC kills its own I2C on battery.** BQ25120A CD (P1.09) has a 900 kΩ
internal pull-down. CD low with no charger = High-Z = I2C off, while SYS
keeps running so the board looks perfectly healthy. Firmware must drive CD
high. But CD high with a charger attached **disables charging**, so
`pmic_service()` re-parks it every 2 s.

**Charging is blocked by an unconnected TS pin.** BQ25120A ball C3 floats,
reads as an out-of-range thermistor, and suspends charging with
`STAT = FAULT` while the *fault* register reads `0x00`. Firmware clears
`TS_EN` to work around it. **This disables battery over-temperature
protection** — a bench workaround, not shippable.

**ANNA-B402 ships with APPROTECT enabled.** Until
`pyocd erase -t nrf52833 --chip` is run, everything fails and looks exactly
like a wiring fault. Once per module. Unlock and flash in the same session:
a reset re-locks it if UICR has been erased.

**The SWD wires are hand-soldered to pads and fail constantly.** Expect
`No ACK` / `ACK '0'` at random. `scratchpad/flashloop.py` retries until a
window opens — one flash took 56 attempts over 104 s. The durable fix is
the SM03B-SRSS-TB connector in `HARDWARE_NOTES.md` item 1.

**The IMU is not the part in the datasheet folder.** `WHO_AM_I = 0x71` is
an LSM6DSV16BX; the filed datasheet is the LSM6DSV (0x70). Basic
accelerometer and interrupt registers match; embedded functions do not.

---

## 4. Firmware architecture

```
src/main.c        state machine, duty cycling, orchestration
src/maxm86161.c   PPG driver (probe, config, FIFO, LED indicator)
src/hr.c          heart rate: DC tracker, low-pass, peak detect, median
src/imu.c         LSM6DSV16BX: accel readout, wake-on-motion
src/selftest.c    PMIC, battery, GSR, pin checks; pmic_service()
src/ble.c         HRS + BAS + custom Ring Service
```

Three states in `main.c`:

| State | Behaviour |
|---|---|
| `RING_IDLE` | PPG off, boost disabled, IMU wake armed. CPU sleeps. |
| `RING_MEASURING` | 25 s window every 60 s (~42% duty; 15 s before v0.6.2). Gives up after 6 s if no finger. |
| `RING_CHARGING` | All optics off. Red LED flashes 3 s when charger detected. |

Duty cycle is runtime-adjustable from the app (control opcode `0x04`).

---

## 5. Verified vs unverified

**Verified on hardware:**

* MAXM86161 at 0x62, PART_ID 0x36, green PPG streaming at 100 sps
* Finger detection — DC 2,860 empty vs 53,000+ with a finger, gate at 15,000
* Beat detection produces plausible rates (55–64 bpm) with a finger on
* No false positives on an empty sensor (this took three attempts to fix)
* IMU responds, reads real gravity
* PMIC status, faults, 20 mA charge current, charging confirmed (`STAT = 01`)
* Battery voltage via BQ25120A monitor (3696–3780 mV observed)
* BLE advertising, confirmed visible on a phone
* State machine transitions, CPU sleeping while idle

**NOT verified — do not assume these work:**

* **IMU wake-on-motion.** This is the open item, see section 6.
* **Heart rate accuracy.** Never checked against a reference monitor.
  Plausible numbers are not validated numbers.
* **GSR: the analog front end is fine. The 0 mV was firmware, twice over.**
  The entry that used to sit here blamed U7 and told you not to bother
  bridging the electrodes. That was wrong, and it was written at 23:30 on
  2026-08-23 -- before either cause was found. Corrected 2026-09-10.

  Cause 1, fixed: the ADC channel in `ring_anna_nrf52833.dts` had no
  `zephyr,vref-mv`. `ADC_DT_SPEC_GET` defaults it to 0 and
  `adc_raw_to_millivolts_dt()` computes `raw * vref_mv`, so **every
  conversion returned 0 mV no matter how good the sample was.** Every
  "GSR reads a hard 0" observation in this project came through that
  multiply. The property is now set to 450 (VDD/4 with VDD = 1.8 V).

  Cause 2, a hypothesis with a harmless change already applied, and
  probably not a cause at all: a hand-rolled SAADC config on **channel 0**
  read ~500 mV from AIN1 (P0.03) correctly, while the devicetree path --
  byte-identical except that it used **channel 1** -- returned 0.

  Two things undercut it. The devicetree path was missing `zephyr,vref-mv`
  at the time, and that alone forces every conversion to 0 mV however good
  the sample is, which is Cause 1 and is already fixed. And the driver does
  not care: `adc_nrfx_saadc.c` rejects a `channel_id` only when it is
  `>= SAADC_CH_NUM`, and accepts channel 1 with any input exactly as it
  accepts channel 0. Nothing requires index 0.

  So the honest position is that the channel index was the last untested
  difference, not a demonstrated fault, and the **most likely outcome is
  that ch0 and ch1 read the same** -- meaning the index was never the
  problem and Cause 1 was the whole of it.

  **The move has been made and is not being reverted** -- it costs nothing
  and removes the variable. As of 2026-09-17 the devicetree node in
  `ring_anna_nrf52833.dts` is `channel@0` with `reg = <0>`, and
  `zephyr,user` has `io-channels = <&adc 0>`. Every other property is
  unchanged, including `zephyr,input-positive = <NRF_SAADC_AIN1>` -- the
  channel index is a SAADC channel slot and says nothing about which pin is
  sampled. Nothing in the firmware hard-codes the index: `gsr_adc` is
  `ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0)` and every read goes
  through `adc_*_dt()`, so the whole GSR path followed the node.

  The A/B diagnostic in `gsr_ain_scan()` (`selftest.c`, bench builds only)
  is deliberately kept -- it is the confirmation, not scaffolding. The line
  to look for on RTT is:

  ```
  GSR   channel A/B, input fixed at AIN1: ch0=<n>mV ch1=<n>mV ch2=... ch3=...
  ```

  **The A/B log decides this, nothing else.** `ch0` and `ch1` both near
  500 mV -- the expected result -- means the channel index was never the
  problem, Cause 2 is closed as a non-cause, and `zephyr,vref-mv` was the
  whole fix. `ch0` near 500 mV with `ch1` at 0 mV would be the surprise,
  and would make the move the fix after all. Cross-check with the two
  lines that follow: `GSR   path A/B: dt raw=... || hand raw=...` should
  show both raw counts non-zero and within a few LSB of each other, and
  `GSR   dt spec: ch_id=0 input_p=1 ...` should report ch_id=0.

  The meter measurements were correct all along: V_OUT_GSR sits at V_REF
  (~0.5 V) with the electrodes open, exactly as the topology predicts.

  Still open once readings work: **R5 is probably sized wrong.** Output is
  `V_REF x (1 + R5/R_skin)`. With R5 = 91 k and dry skin at 1-10 M through
  2 mm electrodes, touching the electrodes moves the output by 1-10% --
  technically present, easily lost in noise. R5 nearer 1 M would give
  usable swing. That is a sensitivity problem, not the cause of the zeros.
* **The custom BLE service.** Compiles and the stack starts, but no phone
  has ever subscribed to the Ring Service characteristics or written a
  control opcode. Assume nothing here works until tested.
* **Charging after the TS fix persists across a power cycle.**

---

## 6. RESOLVED: IMU wake-on-motion was a cold joint on U4

**Fixed 2026-08-23 by reflowing the IMU.** Wake-on-motion works.

The symptom was that shaking produced no `motion wake` lines. It was never a
firmware bug -- ACC_INT was held low by a bad INT1 joint on U4. Reflowing U4
flipped all three tests at once:

| Test | Before | After |
|---|---|---|
| INT1 drive (DRDY_XL @120 Hz, 40 ms) | `0/400 high` | `400/400 high` |
| INT1 release (open-drain + pull-up) | `pin=0` | `pin=1` |
| Wake diag while shaking | `pin=0` | `pin=1` with `WU_IA` |
| ISR flag | never set | `isr_flag=1` |

**One real firmware bug was found and fixed along the way:** `WAKE_UP_SRC`
was at `0x1b`, the LSM6DSO/DSL address. On the LSM6DSV family it is `0x45`
(DS13476 Rev 2 section 9.43). `0x1b` lands in the OIS/reserved block and
reads back without error, so the register that proves the IMU is detecting
motion read `0x00` forever. Fixing it is what made the hardware fault
visible -- before that, a dead joint and a mis-set register were
indistinguishable.

The rest of the wake chain was audited byte-for-byte against the datasheet
and confirmed on silicon: `BANK=0x00` (main register bank, not the
embedded-function page), `IF_CFG=0x00` (push-pull, active high),
`CTRL1=0x06`, `TAP_CFG0=0x01`, `INACT_DUR=0x00`, `WAKE_THS=0x0a`,
`WAKE_DUR=0x00`, `MD1_CFG=0x20`, `FUNC_EN=0x80`.

### What actually solved it

Three instruments in `imu.c`, gated behind `CONFIG_RING_IMU_WAKE_DIAG`, each answering
one question the previous one raised:

1. **`imu_wake_diag()`** -- samples the INT1 pin *before* reading the
   latched source, separating "the IMU never fired" from "the IMU fired and
   the pin did not move". This is what proved detection was fine.
2. **ACC_INT net test** -- pull-up vs pull-down. Distinguishes an open net
   from a driven one.
3. **INT1 drive test** -- routes data-ready to the pin at 120 Hz. Proves
   whether the output stage can drive high *at all*, independent of wake
   routing. `0/400` is what localised the fault to the joint.

These are off unless you ask for them. They now live behind
`CONFIG_RING_IMU_WAKE_DIAG` (under `RING_BENCH`) rather than a hand-edited
`#define` in `imu.c` -- build with `.uild.ps1 -Bench` to get them. They
read latched registers and pulse INT1, so they perturb what they measure.

### Still to confirm

End-to-end behaviour has not been watched yet: with the charger attached the
ring parks in `RING_CHARGING`, where `imu_take_wake_event()` is never
called. Unplug the charger, shake, and a green LED within ~1 s is the
confirmation -- no debugger needed.

**Also seen along the way:** a full I2C bus stall after SWD rework, with all
three devices timing out at `-116`. `main.c` now measures SDA/SCL idle
levels straight from `P0.IN` at boot and reports which line is held, plus
whether `i2c_recover_bus()` actually released it. That turned a dead bus
from a mystery into one log line. Reflowing U4 cleared this too, so the
stuck SDA was the same bad joint.

## 7. What is left

* Verify IMU wake (section 6)
* Validate HR against a reference monitor
* Test GSR with the electrodes bridged
* Test the custom BLE service from a phone — nothing has exercised it,
  and as of v0.4 that includes pairing: the Ring Service now refuses an
  unencrypted link, so an app that does not pair sees nothing at all
* **BLE encryption/bonding: done in v0.4, unverified.** The Ring Service
  characteristics and CCCs require an encrypted link, pairing is Just
  Works (no display, no keypad), and bonds persist in a 16 kB settings
  partition at 0x7c000. Compile-verified only. Still missing: a way to
  clear a bond without an SWD erase, and MITM protection, which this
  hardware cannot provide. HRS and BAS are deliberately still open.
* Respin the PCB with the nine changes in `HARDWARE_NOTES.md`, especially
  the debug connector and the TS divider
* **v0.6 on-ring history (2026-10-07): working on ring 2.**
  `src/history.c` keeps a 16-byte record every 5 min in a new 176 kB
  partition at `0x50000` (the image shrank to 320 kB; bonds untouched at
  `0x7c000`), and the phone syncs it with opcodes `0x0D`/`0x0E` --
  `APP_INTEGRATION.md` §16. First boot formats the partition (~4 s). To
  verify at the jig: flash `release/ring-fw-v0.6.1-daily.hex`, watch RTT for
  `history: ... records, next seq ...`, leave it 15 min, then
  `python tools/history_sync.py` should show 3 contiguous records with
  good CRCs. `tests/test_history.c` covers the ring buffer against a fake
  flash but has not been run (no host compiler on the PC).
  **Verified on ring 2, 2026-10-07:** 58 records synced in 0.1 s, no bad
  CRCs, no gaps; dating of the current boot's records correct. The first
  flash also exposed an inherited-watchdog reset during the format, fixed
  in `wdt.c` (`kick_inherited()`).
* **Heart rate is not usable yet (2026-10-07, ring 2 vs a WHOOP).** The
  ring read about half, then about double, a steady 67-78 bpm. Raw PPG
  shows ~10 % single-sample glitches on a 70 ms rhythm (electrical) and a
  pulse barely above drift: pulse SNR -9 to -16 dB. v0.6.1 adds opcodes
  `0x10` (LED current / ADC range / averaging) and `0x11` (TX power) so
  `tools/ppg_sweep.py` can tune the front end and test whether the
  glitches follow the radio, one flash instead of one per setting.
* **v0.6.2 (2026-10-08): new heart-rate detector, built, not yet flashed.**
  The sweep showed the signal carried the true rate (50-100 % of 10 s
  windows with a still hand) while the old detector was 25-57 bpm off, so
  `src/hr.c` was rewritten: median-of-5 despike, band-pass, a 10 s
  Goertzel spectrum once a second, confidence gates, median of recent
  estimates; beats for HRV are trusted only against the spectral rate.
  Replayed against the WHOOP recordings (`tools/hr_eval.py`, which builds
  the real hr.c with Clang): 6.8 bpm mean error / 60 % within 5 bpm,
  against 46 bpm / 0 % for the old one. Defaults moved with it: 31 mA,
  4x on-chip averaging (glitches ~22 % -> ~3 %), 25 s windows. Host tests
  now run on this PC: `pip install ziglang`, then
  `python tests/run_tests.py` -- all 7 suites pass, including
  `test_history` for the first time. Opcode `0x11` (TX power) is still
  suspected of one reboot; untested since.
  **Flashed on ring 2 (2026-10-08). Live against a WHOOP: 4.3 bpm mean
  error, 68 % within 5 bpm, reading 71 % of the time.**
* **Test firmware frozen, 2026-10-12:** `../ring-fw-test-v0.6.2/` (see its
  FROZEN.md), with the exact hex on ring 2 hash-verified. This tree is
  production from here on.
* **v0.7.0 production (2026-10-12): built, not flashed.**
  `.\build.ps1` now uses `prod.conf`, which sets +8 dBm, a 25 s window every
  300 s, the charge LED, and no GSR status sampling or diagnostic opcodes.
  The new options default to the old behaviour, so the test builds resolve
  to the v0.6.2 configuration exactly (verified by diffing the resolved
  .config). Two source changes are shared by every build:
  * **Steps are rhythm-gated.** The user reported the step count as
    "garbage". In simulation, 30 min of hand movement read as 545 steps.
    `steps.c` now holds candidates until 8 arrive in rhythm, then credits
    them. The same simulation now reads 0, and simulated walks stay within
    1-3 %. Pinned by `test_no_steps_from_hand_movement`. Real-walk
    validation still to do, with `tools/steps_record.py` and
    `tools/steps_eval.py` (which also replays the frozen counter).
  * Heart-rate bias checked: there is no consistent offset (-3.4 to +2.9
    bpm per clean recording). A differenced spectrum was tried and
    rejected.
* **2026-10-15, more v0.7.0 work (host-tested, still not flashed).
  `ROADMAP.md` is the game plan; read it next.**
  * `test_hr` found two detector bugs, both fixed. Spectral averaging cut
    false readings on noise from 50 % to 5 %.
  * Steps credit a missed footfall.
  * Power: advertising slows after 30 s; the connection interval is
    managed; the accelerometer runs in low-power mode at 60 Hz (CTRL1 0x55,
    from ST's 16BX driver, **unverified on hardware**); the PPG runs 15 mA
    with 2x averaging (provisional). Estimates are in `POWER.md`.
  * New docs: `ANTENNA_TUNING.md` (there is no pi network; the knob is the
    ANT_PCB strip), `POWER.md`, `ROADMAP.md`. `HARDWARE_NOTES` gains items
    14 and 15, and §12's "do not change R5" is superseded.
  * New tools: `ppg_sweep.py --ab`, `prod_check.py`, `rssi_ab.py`,
    `gerber_render.py`. `tests/run_tests.py` now runs the syntax pass too.
* **2026-10-24: every MAXM86161 on hand is dead.** Board 3 got v0.7.0
  flashed and its green/red LED test lit once, but the modules tried since
  give no light. The boards are fine (5 V boost present). Replacements are
  ordered; nothing that needs a heart rate or a wear verdict can be tested
  until they arrive.
* **v0.8.0 production (2026-10-25): built, not flashed.**
  `release/ring-fw-v0.8.0-prod.hex`. For the app's burnout, stress and
  night hot-flash features; the app-side design is
  `../mac-handoff/WELLNESS_ANALYTICS.md`.
  * **History v2:** 32-byte records (HR min/max, temperatures, motion,
    spot readings, hot flashes), packet `0x03`, 19 days. **The first boot
    reformats a v1 log: sync before flashing.** Offsets pinned by
    `_Static_assert`s in `test_history.c`; `history_sync.py` parses both.
  * `src/thermo.c`: hot flashes from the nRF die temperature
    (`mpsl_temperature_get()`, every 10 s), gated on 30 min worn, the
    charger, workouts, and by default sleep. `test_thermo` (59 checks).
  * `src/spot.c`: 3-minute green windows for resting HR and RMSSD, asleep
    hourly, awake-and-still hourly, on demand (`0x14`), after a hot flash.
    `test_spot` (57 checks).
  * MAXM86161 die temperature (regs 0x40-0x42) read once per window as
    `skin_c100`.
  * Wellness characteristic `f0a1000b` (GATT table now 33 attributes);
    opcodes `0x12`, `0x13`, `0x14`. `tools/wellness_watch.py` watches it.
  * Kconfig `RING_SPOT_*`, `RING_HOT_FLASH_*` default off; `prod.conf`
    turns them on. A history sync below MTU 37 is refused with ATT `0x11`.
  * Wear verdicts for both come from PPG windows, so **with a dead MAXM
    neither feature ever runs** (the temperature is still logged).

---

## 8. Working style that paid off

Instrument before theorising. Every hard problem in this project was solved
by making the firmware report what it actually saw, and every long detour
came from reasoning about hardware while the software was hiding the
evidence. When something looks like a hardware fault, first make sure you
are seeing real error codes rather than a placeholder.
