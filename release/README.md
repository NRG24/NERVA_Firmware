# Archived binaries

Built with NCS v3.4.0 / Zephyr 4.4.0, `west build -b ring_anna/nrf52833`.

Flash with the series resistors fitted -- see `../FLASHING.md`:

```
python -m pyocd flash -t nrf52833 -f 1000000 release/<file>.hex
```

---

## v0.1-bench -- proven on hardware

Built from the tagged `v0.1-bench` tree. These are the images that actually
ran: MAXM86161 at 0x62, IMU wake working after the U4 reflow, PMIC charging
at 20 mA, BLE advertising.

| File | What it does |
|---|---|
| `ring-fw-v0.1-bench-greenled.hex` | **The one that lit the green LED.** Holds LED1 solid as a power indicator and never sleeps the 5 V boost. Use this to prove a board is alive. |
| `ring-fw-v0.1-bench-noled.hex` | Same tree, indicator off, normal duty cycling. |

Both carry the bench instruments in the main loop. Neither is a production
image. Rebuild either with `git checkout v0.1-bench`, then set
`BENCH_POWER_LED` in `src/main.c` and run `.\build.ps1`.

---

## v0.4-ble -- NOT YET RUN ON HARDWARE

BLE security. The Ring Service no longer talks to an unpaired phone, bonds
survive a power cycle, and a null-pointer crash in the PPG stream is gone.
**Compile-verified only**, across the same four configurations. Supersedes
v0.3-review-fixes -- flash these, not those.

| File | Build |
|---|---|
| `ring-fw-v0.4-production.hex` | `.\build.ps1` -- no bench instruments |
| `ring-fw-v0.4-bench.hex` | `.\build.ps1 -Bench` -- instrumented |

What changed since v0.3:

* `ble_publish_ppg()` called `bt_gatt_get_mtu(NULL)`. NULL reaches
  `att_get()`, which dereferences `conn->state` -- a hard fault the moment
  anything asked for the MTU. The connection is now kept, referenced, from
  the connect callback, and the function returns early when nobody is there.
* every Ring Service characteristic and CCC requires an encrypted link. The
  control characteristic mattered most: anyone in range could previously
  write opcodes and force measurement windows on someone else's ring.
* Just Works pairing, because the ring has no display and no keypad.
  Encrypted and bonded, but no MITM protection.
* bonds persist. Settings over NVS in a new 16 kB `storage` partition at
  0x7c000, loaded with `settings_load()` after `bt_enable()`.
* Device Information Service, firmware revision `0.4.0`, also logged over
  RTT at boot so a log line and a GATT read always agree.
* Heart Rate and Battery are deliberately still open, so off-the-shelf HR
  apps keep working.
* v0.4-sensing: the GSR ADC node moved from `channel@1` to `channel@0`
  (`io-channels = <&adc 0>`, input still `NRF_SAADC_AIN1`) -- HANDOFF.md
  section 5 "Cause 2". Confirm on RTT with the bench line
  `GSR   channel A/B, input fixed at AIN1:` reading `ch0` near 500 mV and
  `ch1` at 0 mV.
* v0.4-sensing: an IMU that stops answering mid-run is now re-initialised.
  Ten consecutive failed reads while idle (~2 s) clear `imu_ready` and
  trigger `imu_init()` on the next pass, so wake-on-motion comes back
  without a reset instead of staying dead for the session.

Production flash grew from 180,944 B to 198,196 B (+17.3 kB) and RAM from
47,350 B to 48,438 B (+1.1 kB). That is the settings subsystem, NVS, SMP
bond storage and DIS. The image region is also capped at 496 kB now by
`zephyr,code-partition`, so an image that grew into the bonds would fail to
link rather than quietly eat them.

All four configurations build with no warnings:

| Build | Flash | RAM |
|---|---|---|
| production | 198,412 B (39.1% of 496 kB) | 48,438 B (37.0%) |
| bench | 205,528 B (40.5%) | 48,502 B (37.0%) |
| `-DCONFIG_RING_WATCHDOG=n` | 197,248 B (38.8%) | 48,374 B (36.9%) |
| bench, `-DCONFIG_RING_GSR_MONITOR=n` | 205,060 B (40.4%) | 48,438 B (37.0%) |

These are the v0.4-sensing figures, and they are what the hex files in this
directory contain. The two sensing fixes cost 216 B of flash over v0.4-ble
and no RAM.

### First flash needs a full chip erase

The partition layout changed. A board carrying a v0.3 image has whatever the
old image left at 0x7c000, and while NVS should treat an unrecognised sector
as empty, that path has never been exercised here. Erase, then flash:

```
python -m pyocd erase -t nrf52833 -f 1000000 --chip
python -m pyocd flash -t nrf52833 -f 1000000 release/ring-fw-v0.4-production.hex
```

### What to watch on RTT

`ble_start()` now prints the firmware revision, the service list and a line
saying the Ring Service needs encryption. A pairing attempt should produce
`pairing requested by ...`, then `security with ... is level 2`, then
`paired with ..., bonded yes`. Anything else is the thing to report.

A phone app written against v0.3 **will stop working** -- it must pair
before it can touch the Ring Service. See `../APP_INTEGRATION.md` section 2.

---

## v0.3-review-fixes -- NOT YET RUN ON HARDWARE

Seven code-review fixes on top of Phase 1, two of them for bugs Phase 1
introduced. **Compile-verified only**, across four configurations. Supersedes
v0.2-phase1 -- flash these, not those.

| File | Build |
|---|---|
| `ring-fw-v0.3-production.hex` | `.\build.ps1` -- no bench instruments |
| `ring-fw-v0.3-bench.hex` | `.\build.ps1 -Bench` -- instrumented |

The one that matters if you leave a board on a charger: an unreachable PMIC
used to read as "charger removed", so a single transient I2C error pulled the
ring out of `RING_CHARGING` and started the PPG and the 5 V boost while still
on a 20 mA charger. **Do not leave a v0.2-phase1 image running unattended on
a charger.**

Also fixed: the watchdog could reset a board that was merely slow (a stalled
I2C bus costs 500 ms per transaction, and a re-probe issues up to sixteen);
a PPG that wedged mid-run never recovered and kept reporting healthy; the
probe's known bus-wedging bare read is now bench-only; and the IMU health
flag tracks reachability like the other two.

A second review pass on those fixes found three more, all folded in here:

* the IMU flag could be **set** for a device that failed initialisation --
  a successful read only proves the bus works, and an unconfigured part in
  power-down answers reads with zeros. A failed `imu_init()` is now retried
  once a minute and the wake interrupt re-armed, instead of leaving the IMU
  dead for the session.
* refusing to act on an unreachable PMIC (correctly) left no way out of
  `RING_CHARGING`, so a PMIC that never answered again stranded the ring
  with the optics off forever. After 30 s of failures it now falls back to
  idle.
* the watchdog blocking audit, rewritten to be accurate, still did not
  account for the new recovery paths.

---

## v0.2-phase1 -- SUPERSEDED, has the charger bug above

Phase 1 survivability work. **Every one of these changes is compile-verified
only.** No board was available when they were written, so treat the first
flash as a bring-up, not an update.

| File | Build |
|---|---|
| `ring-fw-v0.2-phase1-production.hex` | `.\build.ps1` -- no bench instruments |
| `ring-fw-v0.2-phase1-bench.hex` | `.\build.ps1 -Bench` -- instrumented |

What changed since v0.1:

* bench instruments moved behind Kconfig and compiled out by default
* hardware watchdog, 10 s, resets the SoC if the main loop stops feeding it
* fatal errors reboot instead of spinning forever in `k_fatal_halt`
* reset cause reported at boot, so a watchdog reset is visible
* boot no longer blocks up to 30 s on the PPG probe; BLE starts first
* `RING_FLAG_PMIC_OK` reflects whether the PMIC actually answered
* MAXM86161 driver refuses to write to I2C address 0 (general call)

### Flash the bench image first

`ring-fw-v0.2-phase1-bench.hex` lights the green LED exactly as v0.1 did, so
it answers "did this brick the board?" by eye, without a debugger.

If the board reboot-loops, the watchdog is the first suspect. Recover with:

```
python -m pyocd erase -t nrf52833 -f 1000000 --chip
```

then flash `ring-fw-v0.1-bench-greenled.hex` to get back to known-good. The
nRF52833 watchdog cannot be stopped once started, so a power cycle is needed
to clear it -- disconnect the battery, not just the charger.

To build a Phase 1 image with the watchdog off:

```
west build -b ring_anna/nrf52833 -p always -d build-nowdt . -- -DBOARD_ROOT=<app> -DCONFIG_RING_WATCHDOG=n
```
