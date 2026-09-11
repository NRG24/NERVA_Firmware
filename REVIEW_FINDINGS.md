# Phase 1 code review — seven findings to fix

Written 2026-09-10 after reviewing `v0.1-bench..v0.2-phase1`. All seven are
confirmed against the code or a real build. Two were introduced by Phase 1
itself.

**Tree state:** clean at tag `v0.2-phase1` (commit `0f661bc`), branch `prod`.
Nothing from this list is applied yet.

**No hardware is available.** Compiling is the only verification this project
has, and it has already caught two bugs. Every fix below must end with the
four-configuration build sweep at the bottom of this file.

---

## 1. Unreachable PMIC drops the ring out of RING_CHARGING

`src/main.c:589` — **fix this first, it has a safety cost**

`charging` is initialised to `false` at the top of the block. When
`pmic_service()` returns an error that initialiser survives, and the
`else if (!charging && state == RING_CHARGING)` branch reads it as "charger
removed": the ring leaves `RING_CHARGING`, sets `next_window = now`, and
starts the PPG and the 5 V boost while still sitting on a 20 mA charger —
the exact thing that state exists to prevent. One transient `-116` on this
board's I2C bus is enough.

`pmic_rc` is computed two lines above and used only for the health flag.

**Fix:** guard the state transitions on `pmic_rc == 0`. On error, clear
`RING_FLAG_PMIC_OK` and change nothing else — do not move the state machine
on data you could not read. The transition bodies need re-indenting one tab
under the success branch.

## 2. Runtime re-probe can exceed the 10 s watchdog

`src/main.c:295`

`CONFIG_I2C_NRFX_TRANSFER_TIMEOUT` is 500 ms. `ppg_probe(2, 50)` is
2 attempts x 2 candidate addresses x `try_addr()`, and `try_addr()` issues up
to 4 transactions (bare, pointer write, split read, restart) — so up to
16 x 500 ms = 8 s in a single main-loop pass. Add the same pass's
`pmic_service` (~0.5 s), `slow_sense` (~1.3 s incl. the 800 ms GSR settle)
and `imu_magnitude_mg` (0.5 s) and one pass reaches ~10.4 s. The watchdog
fires on a loop that was never hung, only slow, and the board reboot-loops —
on exactly the wedged-bus fault the watchdog was added for.

**Fix:**
* `ppg_probe()` calls `ring_wdt_feed()` at the top of each attempt. This is a
  bounded loop making definite progress, not a hang; feeding it is correct.
* Runtime re-probe drops to **one** attempt (`ppg_probe(1, 0)`). A second try
  50 ms after eight consecutive timeouts tells you nothing the next window
  will not.
* Add a `ring_wdt_feed()` after the charger block (it contains the 3 s
  `maxm86161_indicate`) and after `slow_sense()`.

## 3. The BLOCKING AUDIT comment is wrong

`src/main.c:535`

It counts only `k_msleep` delays and concludes "~3.1 s against a 10 s budget,
roughly 3x margin". It bills `pmic_service` at ~55 ms (true only when the bus
answers; 505 ms when it times out), ignores `selftest_battery_mv`'s failing
reads inside `slow_sense`, and does not mention `ppg_on()`'s re-probe at all —
the longest blocking path in the loop.

This comment is what future changes get sized against, so an understated one
is load-bearing.

**Fix:** rewrite it with the I2C timeout paths included, and state where the
feeds are (loop top, per probe attempt, after the charger block, after
`slow_sense`) so the longest un-fed span is what actually matters.

## 4. A sensor that wedges after boot never re-probes

`src/main.c:674`

`ppg_present` is only cleared inside `ppg_probe()`, which only runs when
`ppg_present` is already false. If the boot probe succeeds and the part
wedges later — the failure mode this project actually observed, see
`POSTMORTEM.md` — `maxm86161_fifo_read` returns a negative errno every pass,
line 674 logs it, and nothing else happens. `RING_FLAG_PPG_OK` stays set, so
the app is told the PPG is healthy. Only a reset recovers.

The comment at line 290 claims this path "recovers a part that was wedged",
which is true only for a part already dead at boot.

**Fix:** add a consecutive-failure counter (`ppg_fail_count`, limit ~5). Reset
it on any successful read. On reaching the limit: log a warning, clear
`ppg_present` and `RING_FLAG_PPG_OK`, call `ppg_off()`, drop to `RING_IDLE`
and set `next_window = now + measure_period_ms` so the next window re-probes.

## 5. The probe's bare read is now on the runtime path

`src/maxm86161.c:65`

`try_addr()` still begins with a register-pointer-less `i2c_read()` — the
exact pattern `POSTMORTEM.md` identifies as wedging the MAXM86161 until the
bus idles. That was acceptable as one-time boot instrumentation. Phase 1
moved probing onto the runtime path, so with the sensor missing it now runs
4 times every measurement window (default every 60 s), and can hold the part
in the state it is trying to recover from — or defeat the `restart` read
later in the same `try_addr()` call.

**Fix:** guard the bare read behind `CONFIG_RING_BENCH` so production probes
pointer-first only, and adjust the log line so it reads correctly when the
bare read was not attempted.

## 6. Valid Kconfig combination builds with a warning

`src/selftest.c:517`

`gsr_conductance_us_x10()` sits inside the `CONFIG_RING_BENCH` block, but its
only caller (line ~945) is inside the narrower `CONFIG_RING_GSR_MONITOR`
block. Verified:

```
-DCONFIG_RING_BENCH=y -DCONFIG_RING_GSR_MONITOR=n
src/selftest.c:517:12: warning: 'gsr_conductance_us_x10' defined but not used
```

Both are user-selectable, and `RING_GSR_MONITOR` exists precisely so it can
be turned off independently.

**Fix:** guard that one function with `CONFIG_RING_GSR_MONITOR`, matching its
caller.

## 7. RING_FLAG_IMU_OK still latches from boot

`src/main.c:493`

Phase 1 made `RING_FLAG_PMIC_OK` and `RING_FLAG_PPG_OK` reflect current
reachability and left this one set once at boot, so three health bits in the
same status byte now mean different things to the app. A cold joint on U4 is
documented in `HANDOFF.md` section 6, and a stuck SDA has taken down all
three devices at once.

**Fix:** in `RING_IDLE`, set the flag when `imu_magnitude_mg()` returns a
valid reading and clear it when it returns negative.

---

## Verification — all four must build clean

```powershell
.\build.ps1                                    # production
.\build.ps1 -Bench                             # bench
```

and the two combinations that are not covered by those, run with explicit
`-D` flags into their own build directories:

* `-DCONFIG_RING_WATCHDOG=n` — caught the `wdt.c` redefinition in Phase 1
* `-DCONFIG_RING_BENCH=y -DCONFIG_RING_GSR_MONITOR=n` — finding 6 above

Expected before the fixes: production 180,504 B, bench 187,540 B.

`-Wunused-function` warnings count as failures here. The whole value of this
sweep is that it is the only check available.

## Commit

One commit on `prod`, tagged `v0.3-review-fixes`, then refresh
`release/*.hex` and `release/SHA256SUMS.txt` and update
`release/README.md`. End the commit message with:

```
Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

Delete this file as part of that commit — it is a work list, not
documentation.
