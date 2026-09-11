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

## v0.2-phase1 -- NOT YET RUN ON HARDWARE

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
