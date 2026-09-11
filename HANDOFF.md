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
* **GSR is broken, not merely untested.** Localised 2026-08-23 to the U7
  front end: the output should idle at V_REF (~900 mV) with the electrodes
  open, and reads a hard 0 mV from 1 ms to 2 s. The ADC net is proven clean
  (forcing P0.03 high succeeds through R6), so the op-amp output really is
  at 0 V. **Do not bother bridging the electrodes** -- with V_REF at 0 the
  output is 0 regardless of skin resistance, so that test cannot say
  anything. See `BRINGUP_RESULTS.md` Discovery 4 for the meter sequence.
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

Three instruments in `imu.c`, gated behind `IMU_WAKE_DIAG`, each answering
one question the previous one raised:

1. **`imu_wake_diag()`** -- samples the INT1 pin *before* reading the
   latched source, separating "the IMU never fired" from "the IMU fired and
   the pin did not move". This is what proved detection was fine.
2. **ACC_INT net test** -- pull-up vs pull-down. Distinguishes an open net
   from a driven one.
3. **INT1 drive test** -- routes data-ready to the pin at 120 Hz. Proves
   whether the output stage can drive high *at all*, independent of wake
   routing. `0/400` is what localised the fault to the joint.

Set `IMU_WAKE_DIAG` to 0 in `imu.c` once you are done; it reads latched
registers and pulses INT1, so it perturbs what it measures.

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
* Test the custom BLE service from a phone — nothing has exercised it
* **Add BLE encryption/bonding.** Currently anyone in range can connect and
  read biometric data. Must be fixed before shipping.
* Respin the PCB with the nine changes in `HARDWARE_NOTES.md`, especially
  the debug connector and the TS divider

---

## 8. Working style that paid off

Instrument before theorising. Every hard problem in this project was solved
by making the firmware report what it actually saw, and every long detour
came from reasoning about hardware while the software was hiding the
evidence. When something looks like a hardware fault, first make sure you
are seeing real error codes rather than a placeholder.
