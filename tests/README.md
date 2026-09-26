# Host tests for the pure-logic modules

Builds steps, sleep, calories, HRV and SpO2 with a host compiler and runs
them against simulated signals. No Zephyr, no board, no debugger.

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

### `test_hrv` — RMSSD

Compares the integer pipeline against the textbook definition computed in
floating point, checks the quantisation floor the hardware imposes, and
builds `hr.c` so the gate deciding *which* intervals reach RMSSD is
covered too — without that, nothing would notice if the detector stopped
marking intervals trusted and RMSSD silently read 0 forever.

### `test_spo2` — ratio of ratios

The percentage is uncalibrated and cannot be tested against truth. What is
tested: R comes out right for known AC/DC on each channel (R is physics,
not a fit), the UNCALIBRATED flag is set on every path including the ones
reporting nothing, implausible inputs report nothing rather than a number,
and the curve points the right way round — a sign error there would read
high when it should read low and nothing else would catch it.

### `test_driver` — MAXM86161 register writes

Stubs the I2C layer, records every register write, and compares against
the values the driver produced *before* the two-slot SpO2 refactor. The
green channel at 100 sps is the only configuration this project has ever
seen work on hardware, so a refactor quietly changing one value there
would cost the next bring-up days and nothing else would notice. The
expected table is transcribed from the pre-refactor driver rather than
generated from the current code, which is what makes it a check rather
than a tautology.

### `test_activity` — workout calories

Checks the integer Keytel implementation against the published equations
in floating point, for both sexes and the unspecified mean, over 1,188
combinations of heart rate, weight and age: the worst disagreement is
1/1000 kcal. Then the three minute paths (heart rate, rest, fallback), the
floor at the step price and the 20 kcal/min cap, that heart rate is
ignored outside a workout, and start/stop/restart. It shows rowing at
140 bpm coming out near 410 kcal for half an hour where steps alone
say 42. It proves the arithmetic, not that the ring's heart rate is
right during exercise.

### `test_profile` — body weight in flash

Fakes the settings backend with a one-key "flash" that counts writes and
can be made to fail, and replays it through `profile.c`'s handler the way
`settings_load()` would. Tested: the weight survives a simulated reboot;
a wrong-size record, an unknown key or a failed read is skipped rather
than returned as an error (an error there would fail `settings_load()`,
which takes BLE down with it); resending an unchanged weight writes
nothing; a burst of changes writes at most once a minute and the last
value is the one that lands; a failing backend retries once per interval
rather than on every 20 ms pass; and the clamped weight, not the request,
is what gets stored. It proves nothing about NVS on the real part.

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
| `test_waking_up_is_not_restlessness` | `restless_min` counted the very minutes that ended the session, so a still night finishing with the wearer getting up reported three restless minutes — and carried them all the next day. |
| `test_a_gap_does_not_bank_provisional_minutes` | The gap path kept minutes credited provisionally while waiting on a wake confirmation, which the wake-confirm path in the same function backed out. |
| `test_reset_does_not_poison_the_next_minute` | `sleep_reset()` kept a step snapshot from before the counter was zeroed; the unsigned delta wrapped to ~4 billion. |
| `test_calories_do_not_depend_on_tick_phase` | A sampled instantaneous cadence made the same activity worth 42.0 or 12.3 kcal depending only on when the tick fired. |
| `test_charging_does_not_accrue_calories` | The calorie tick ran above the state switch and billed ~147 kcal for a two-hour charge. |
| `test_slow_polling_is_known_bad` | Documents the 200 ms cliff itself, so nobody quietly raises the poll interval back. |
| `check-constants` | main.c and the test's copy of the polling constants drifting apart. |
| `test_hrv` non-successive guard | Pairing intervals across a rejected beat — worth 144 ms of false HRV on a steady pulse train. |
| `test_hrv` detector plumbing | hr.c never marking an interval trusted, which would make RMSSD read 0 forever with no other symptom. |
| `test_spo2` uncalibrated flag | The flag going missing on a good reading, which is precisely the case where an app would show the number. |
| `test_spo2` channel order | Red and IR swapped in the ratio, which inverts R with no error anywhere. |
| `test_driver` green path | The two-slot refactor changing a register on the one configuration proven on hardware. |
| `test_driver` slot order | IR and red swapped in the LED sequence, which inverts R via the FIFO tags instead. |
| `test_activity` Keytel vs paper | A mistyped coefficient in either equation. |
| `test_activity` workout paths | The 90 bpm threshold, the step-price floor, or heart rate leaking into non-workout minutes. |
| `test_profile` body record | Age and sex written under the weight's key, overwriting it. |
| `test_profile` rate limit | Writing flash on every pass while a value is pending — every weight change, and every retry after a failure. |
| `test_profile` dedupe | Rewriting an unchanged weight on every connection, which the app is told to send. |
| `test_profile` junk in flash | A wrong-size record failing `settings_load()`, which would cost the ring its radio. |
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

### Buckets are wall-clock minutes, so stir durations matter

A test that stirs the sleeper for 70 seconds is not testing "one restless
minute". Sleep buckets close on wall-clock minute boundaries, so a 70 s
stir straddles three of them — two during the walk and one covering its
tail — which is three active minutes and *correctly* ends the session.
An early version of `test_a_genuine_stir_is_still_counted` asserted the
opposite and failed against correct code.

Measured: a stir of 10–55 s produces one active minute, and 70 s produces
three. If a sleep test's premise depends on the count, keep the stir well
under a minute, or instrument the transitions and check rather than
assuming.

---

## Adding a test

Keep `steps.c`, `sleep.c` and `calories.c` free of Zephyr dependencies
beyond `sys/util.h` — that is what makes them testable here, and
`stubs/zephyr/sys/util.h` is the entire stub surface. Anything needing a
device, a timer or the BLE stack belongs in `main.c` instead, where the
policy tests can emulate it.
