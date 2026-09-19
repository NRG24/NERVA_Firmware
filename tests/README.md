# Host tests for the activity modules

Builds `steps.c`, `sleep.c` and `calories.c` with a host compiler and runs
them against a simulated wearer. No Zephyr, no board, no debugger.

```sh
cd tests
make            # constants, syntax pass, then both suites
make syntax     # just the syntax pass
make run        # just the behavioural suites
```

Exit code is non-zero if anything fails. Runs on Linux, macOS, WSL, or
MinGW on the same Windows box that builds the firmware.

---

## What this proves, and what it does not

**It proves** the algorithms behave as their headers claim when fed a
plausible signal, and that eight specific defects stay fixed.

**It does not prove the numbers are right.** Nobody has recorded an
accelerometer trace off an actual NERVA ring, so the wearer in `sim.c` —
a fundamental plus a second harmonic, with noise — is an *assumption*
about what a footfall looks like on a finger. Every accuracy figure here
is accuracy against that assumption. If the real signal is shaped
differently, these tests will keep passing and the step count will still
be wrong.

That is the same caveat `STATUS.md` puts on heart rate, and it is not
weaker for having a test suite behind it. What the suite buys is that a
*change* cannot silently make things worse than they are today.

**It does not build the firmware.** `test_mainloop.c` re-implements
main.c's polling policy rather than including it. `make check-constants`
greps `../src/main.c` and fails if the two copies of the polling constants
drift apart, which covers the likeliest way that emulation goes stale, but
it cannot catch a change to the surrounding logic.

---

## The syntax pass

`make syntax` runs every file in `src/` through the compiler with
`-fsyntax-only -Wall -Wextra -Werror`, against hand-written stub headers
in `stubs/zephyr/`. Both configurations, production and `RING_BENCH`,
because the bench instruments are `#ifdef`'d in and would otherwise never
be parsed.

It exists because the real build needs NCS on a Windows machine, so
`main.c` and `ble.c` could otherwise go through a dozen edits without a
compiler ever looking at them. It catches typos, undeclared identifiers,
unbalanced braces, and mismatched log format specifiers — `LOG_*` route to
a `__attribute__((format(printf)))` function here for exactly that reason.

**It is not a build, and the stubs are not Zephyr.** They are ours, so a
stub with a wrong signature would hide the very mismatch you wanted
caught. A clean syntax pass means the code parses and type-checks against
*our idea* of the API; only `build.ps1` against real NCS proves it
compiles. Treat this as a fast first filter, never as permission to skip
the real build.

`test_gatt_layout.c` is the one part of the syntax pass that checks
behaviour rather than grammar. `ble.c` indexes the GATT table by raw
number (`ATTR_ACTIVITY` is `&ring_svc.attrs[12]`), which is only right for
one ordering of the `BT_GATT_*` macros — and a wrong index does not fail
to build, it notifies the wrong attribute on a service no phone has ever
exercised. The stub macros expand to the same number of array entries as
the real ones, so the file can assert the total and stop the build if
somebody inserts a characteristic in the middle.

---

## The suites

### `test_activity` — the modules

Step counting accuracy and cadence, false-positive rejection when still,
sleep onset/wake hysteresis, calorie arithmetic against hand-computed
values, and the published wire format (offsets are asserted against the
table in `APP_INTEGRATION.md` section 7, so the two cannot drift).

### `test_mainloop` — the polling policy

Emulates main.c's `RING_IDLE` case: sample, update `last_step_motion`,
feed the detectors, run the per-minute calorie tick, pick the next sleep
interval. This exists because **steps.c can be entirely correct and the
firmware can still count nothing** — which is precisely what happened.
It also asserts the other half of that trade: a still ring stays on the
slow poll, so the night-time power model survives.

---

## Regression guards

Tests marked `BUG:` in the source each pin a defect that shipped and was
found by running that exact scenario. Each one has been mutation-tested —
the fix was reverted and the suite confirmed to fail:

| Guard | The defect |
|---|---|
| `test_walking_is_counted_end_to_end` | 200 ms idle polling sat barely above Nyquist for gait; **0 of 1100 steps counted**. Steps were only ever counted during PPG windows. |
| `test_a_dead_imu_does_not_pin_the_fast_poll` | An unconfigured accelerometer answers reads with zeros, and zeros read as 1 g of motion — pinning the ring to the 40 ms poll forever and stopping sleep from ever being detected. |
| `test_charging_gap_ends_the_session` | An hour on the charger arrived as one stale bucket and the sleep session ran straight through it. |
| `test_short_gaps_are_caught_too` | The gap was measured from the start of the bucket, so the shortest outage it could see was a whole bucket long and a one-minute charge slipped through as ordinary stillness. |
| `test_a_slow_pass_is_not_a_gap` | The other side of that: a few seconds of I2C stall must stay an ordinary minute, or a marginal bus shreds every session. |
| `test_reset_does_not_poison_the_next_minute` | `sleep_reset()` kept a step snapshot from before the counter was zeroed; the unsigned delta wrapped to ~4 billion. |
| `test_calories_do_not_depend_on_tick_phase` | A sampled instantaneous cadence made the same activity worth 42.0 or 12.3 kcal depending only on when the tick fired. |
| `test_charging_does_not_accrue_calories` | The calorie tick ran above the state switch and billed ~147 kcal for a two-hour charge. |
| `test_slow_polling_is_known_bad` | Documents the 200 ms cliff itself, so nobody quietly raises the poll interval back. |
| `check-constants` | main.c and the test's copy of the polling constants drifting apart. |
| `test_gatt_layout` | A characteristic inserted into the Ring Service shifting the raw `ATTR_*` indices in ble.c onto the wrong attribute. |

`test_no_false_steps_across_a_gap` is deliberately *not* in that list: it
is a property check, and deleting the gap handling in `steps.c` does not
make it fail. The comment above it says so.

### Mutation-test your own tests

`test_charging_does_not_accrue_calories` passed the first time it was
written — and it was worthless. The charging loop in the harness had its
own copy of the per-minute tick, so it skipped the calories no matter what
the firmware did; reverting the fix did not make it fail. The harness now
runs one `ring_tick_common()` from both the idle and charging paths, which
is why the mutation is visible.

The lesson generalises: in a harness that re-implements part of the
firmware, a test can pass because the *harness* has the behaviour, not the
code under test. Reverting the fix and watching the test fail is the only
thing that tells those apart. Do it for every guard you add.

---

## Adding a test

Keep `steps.c`, `sleep.c` and `calories.c` free of Zephyr dependencies
beyond `sys/util.h` — that is what makes them testable here, and
`stubs/zephyr/sys/util.h` is the entire stub surface. Anything needing a
device, a timer or the BLE stack belongs in `main.c` instead, where the
policy tests can emulate it.
