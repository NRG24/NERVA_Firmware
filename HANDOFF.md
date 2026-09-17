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
| `RING_MEASURING` | 15 s window every 60 s (25% duty). Gives up after 6 s if no finger. |
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

  Cause 2, fix applied but still unconfirmed on hardware: with vref-mv
  fixed, a hand-rolled SAADC config on **channel 0** reads ~500 mV from
  AIN1 (P0.03) correctly, while the devicetree path -- byte-identical
  except that it used **channel 1** -- still returned 0.

  **The move has been made.** As of 2026-09-17 the devicetree node in
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

  `ch0` near 500 mV with `ch1` at 0 mV confirms the channel index was the
  fault and that the move fixes it. Cross-check with the two lines that
  follow it: `GSR   path A/B: dt raw=... || hand raw=...` should now show
  both raw counts non-zero and within a few LSB of each other, and
  `GSR   dt spec: ch_id=0 input_p=1 ...` should report ch_id=0. If instead
  `ch0` and `ch1` both read ~500 mV, the channel index was never the
  problem and Cause 2 needs reopening.

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

---

## 8. Working style that paid off

Instrument before theorising. Every hard problem in this project was solved
by making the firmware report what it actually saw, and every long detour
came from reasoning about hardware while the software was hiding the
evidence. When something looks like a hardware fault, first make sure you
are seeing real error codes rather than a placeholder.
