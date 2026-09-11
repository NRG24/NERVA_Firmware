# Archived binaries

Both built from the tagged `v0.1-bench` source tree with NCS v3.4.0 /
Zephyr 4.4.0, `west build -b ring_anna/nrf52833`. Flash 35.3% (184,968 B),
RAM 36.1% (47,286 B).

| File | `BENCH_POWER_LED` | What it does |
|---|---|---|
| `ring-fw-v0.1-bench-greenled.hex` | 1 | **The one that lit the green LED.** Holds LED1 solid as a power indicator and never sleeps the 5 V boost. Use this to prove a board is alive. |
| `ring-fw-v0.1-bench-noled.hex` | 0 | Same tree, indicator off, normal duty cycling. |

Both still contain the bench instruments (`IMU_WAKE_DIAG = 1`,
`selftest_gsr_monitor()` in the main loop). Neither is a production image.

Flash with the series resistors fitted -- see `../FLASHING.md`:

```
python -m pyocd flash -t nrf52833 -f 1000000 release/ring-fw-v0.1-bench-greenled.hex
```

To rebuild either from source:

```
git checkout v0.1-bench
```

then set `BENCH_POWER_LED` in `src/main.c` and run `.\build.ps1`.
