# App integration guide

Everything a phone app needs to talk to the ring. Firmware as of v0.4.1
(2026-09-17).

---

## 1. Discovery

The ring advertises as a connectable peripheral:

| Field | Value |
|---|---|
| Device name | `Ring PPG` (`CONFIG_BT_DEVICE_NAME`) |
| Advertised service UUIDs | `0x180D` (Heart Rate), `0x180F` (Battery) |
| Appearance | 833 |
| Advertising interval | `BT_LE_ADV_CONN_FAST_1` (~30 ms, fast connect) |

Scan-filtering on `0x180D` is enough to find it. The name lives in the
scan response, not the advertising packet, so filter on the service UUID
rather than the name if you want a reliable match.

Advertising runs continuously, including while charging.

---

## 2. Services

Three standard services plus one custom.

### Heart Rate Service — `0x180D`

Standard. Any generic HR app will work with no special support. The ring
notifies the Heart Rate Measurement characteristic (`0x2A37`) once per
second **while a valid reading exists**. Format is the SIG standard: flags
byte then an 8-bit BPM.

Notifications stop when no finger is present. **Absence of a notification
means "no reading", not "zero BPM".**

### Battery Service — `0x180F`

Standard, 0-100 percent, updated every 30 s.

> **Treat this number with suspicion.** It is a plain linear map of
> 3.30-4.20 V, not a state of charge. A LiPo discharge curve is strongly
> non-linear and terminal voltage sags under load, so this reads badly
> wrong in the middle of the range. Use `battery_mv` from the Ring Service
> status packet and apply your own curve if you need accuracy. See
> section 6.

### Device Information Service — `0x180A`

Standard, read-only, and readable without pairing. Read it to find out
which firmware you are talking to before you decide what to trust.

| Characteristic | UUID | Value |
|---|---|---|
| Firmware Revision | `0x2A26` | `0.5.0` |
| Manufacturer Name | `0x2A29` | `Ring project` |
| Model Number | `0x2A24` | `Ring ANNA-B402` |

The same revision string is logged over RTT at boot, so a log line and a
GATT read always agree about which image is running.

### Ring Service — `f0a10000-1e5c-4a2b-8d3f-9c7b6e5a4d21`

Everything the standard services cannot express. **Every attribute in
this service now requires an encrypted link** — see Pairing, below.

| Characteristic | UUID | Properties | Permission |
|---|---|---|---|
| Status | `f0a10001-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Read, Notify | Encrypted read |
| PPG stream | `f0a10002-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Notify | Encrypted CCC |
| IMU stream | `f0a10003-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Notify | Encrypted CCC |
| Control | `f0a10004-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Write, Write w/o response | Encrypted write |
| Activity | `f0a10005-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Read, Notify | Encrypted read |
| HRV | `f0a10006-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Read, Notify | Encrypted read |
| GSR stream | `f0a10007-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Notify | Encrypted CCC |
| SpO2 | `f0a10008-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Read, Notify | Encrypted read |
| Workout | `f0a10009-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Read, Notify | Encrypted read |

### Pairing

**New in v0.4, and it will break an app written against v0.3.** Reading
Status, subscribing to any Ring Service notification, or writing a control
opcode over an unencrypted link is now rejected with ATT error `0x0F`,
Insufficient Encryption.

The ring has no display and no keypad, so the association model is **Just
Works**: no passkey, nothing for the user to confirm on the ring, and the
firmware accepts every pairing request that arrives. What that buys is an
encrypted link and a stored bond. It is not authentication.

The flow from the app side:

1. Connect as usual. Heart Rate, Battery and Device Information all work
   immediately, unencrypted.
2. Raise security. Either ask your stack to do it explicitly
   (`createBond()` on Android) or simply touch an encrypted
   characteristic, which is enough to start pairing on iOS.
3. Pairing completes with no prompt on the ring. The phone may show its
   own system dialog; that is the phone, not us.
4. The bond is written to flash. On every later connection the phone
   re-encrypts from the stored keys and step 2 costs nothing.

Practical notes:

* **Raise security first, then subscribe.** Firing four subscriptions at
  an unencrypted link gets you four errors, and some stacks give up
  rather than retry after pairing.
* **Clearing a bond is control opcode `0x05`.** New in v0.4.1. It erases
  every bond on the ring and then drops the connection, so the next phone
  to connect pairs from scratch. Send it when the user taps "forget this
  ring", *before* you unpair on the phone side — the control
  characteristic needs an encrypted link, so once the phone has forgotten
  its keys the opcode is no longer reachable and only an SWD erase gets
  the ring back. Expect the disconnect; it is not an error.
* Bonds survive a reboot and a battery pull. They live in a 16 kB
  settings partition at the top of flash.
* Just Works means no MITM protection. Passive eavesdropping after
  pairing is prevented; an active attacker present at the moment of
  pairing is not.

---

## 3. Status characteristic

20 bytes, **little-endian**, sized to fit a default 23-byte ATT MTU so it
never fragments. Readable at any time; notified once per second while
measuring and on every state change.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u8 | `state` | 0 idle, 1 measuring, 2 charging |
| 1 | u8 | `flags` | see below |
| 2 | u16 | `hr_x10` | BPM x10. **0 means no valid reading** |
| 4 | u16 | `battery_mv` | millivolts, the trustworthy battery number |
| 6 | i16 | `gsr_mv` | millivolts, **-1 = not measured** |
| 8 | u32 | `ppg_dc` | PPG DC level, the wear-detection signal |
| 12 | u16 | `ppg_ac` | PPG AC amplitude |
| 14 | u16 | `perfusion_x10` | perfusion index in tenths of a percent |
| 16 | u32 | `uptime_s` | seconds since boot |

`flags` bits:

| Bit | Meaning |
|---|---|
| 0 | finger present (`ppg_dc` above the wear threshold) |
| 1 | charger attached |
| 2 | PPG sensor responding |
| 3 | IMU responding |
| 4 | PMIC responding |

Bits 2-4 are **live**, not latched at boot. Each one tracks whether that
subsystem is answering right now:

| Bit | Cleared when |
|---|---|
| 2 PPG | five consecutive FIFO reads fail, or a re-probe finds nothing |
| 3 IMU | an accelerometer read fails while idle; after 10 consecutive failures (~2 s) the driver is also marked uninitialised and `imu_init()` is retried **60 s later**, so the bit comes back by itself if the part recovers. After three such re-inits with no good read in between, the ring stops retrying until it is reset and the bit stays clear — a part that initialises cleanly and never produces a sample is not going to be fixed by a fourth attempt, and the retries cost real I2C traffic |
| 4 PMIC | a PMIC transaction fails |

Each is set again as soon as the device answers, so a bit that flickers is
reporting a real intermittent fault — a cold joint or a marginal bus — not
noise in the firmware. Surface them in a diagnostics screen rather than
silently showing zeros.

> This changed in v0.3. These bits used to be set once at boot and never
> re-evaluated, so a device that died mid-session still reported healthy.
> `RING_FLAG_PMIC_OK` was worse than latched: it was set unconditionally
> every poll, including when the PMIC could not be reached at all. If your
> app assumed "set once at boot", it needs no change to keep working, but it
> can now trust these to mean something.

### Signal quality

`perfusion_x10` is your signal-quality indicator, and it behaves
counter-intuitively:

| Condition | `ppg_dc` | `perfusion_x10` |
|---|---|---|
| Nothing on the sensor | ~2,800 | ~40 (meaningless, it is noise) |
| Finger, good pressure | ~53,000 | ~30 |
| Finger, pressed too hard | ~79,000 | ~10 |

Use `ppg_dc` to decide whether anything is on the sensor — it moves by a
factor of nineteen and is unambiguous. Use `perfusion_x10` **only once
`ppg_dc` says a finger is present**, where a falling PI means the user is
pressing too hard and occluding their own capillaries. "Loosen your grip"
is a genuinely useful prompt at `ppg_dc > 70000` with `perfusion_x10 < 15`.

---

## 4. PPG stream

Raw samples, off by default because it is expensive. Enable with control
opcode `0x01`.

```
offset 0   u32  seq          increments per notification, gaps = dropped packets
offset 4   u8   count        number of samples that follow
offset 5   u32[count]        raw FIFO words: tag in bits 23:19, ADC value in bits 18:0
```

Each word is the **raw FIFO entry**, not a bare ADC value: mask with
`0x7FFFF` for the reading and shift right by 19 for the 5-bit tag that says
which measurement slot produced it. In the normal single-LED mode every
word carries tag `0x01` (green).

**In SpO2 mode this stream changes shape.** Two LEDs means two slots, so
words arrive interleaved at 200 a second — tag `0x01` for IR and tag `0x02`
for red — rather than 100 single-channel samples. Demultiplex on the tag
rather than assuming alternation, and do not feed the mixed stream into a
heart-rate algorithm expecting one channel.

Sample rate is **100 Hz** per channel. Packet size adapts to the negotiated MTU:

| MTU | Samples per notification |
|---|---|
| 23 (default) | 4 |
| 247 (requested) | up to 40 |

**Request a 247-byte MTU.** At the default you get ~25 notifications per
second, which most phones will struggle to sustain and which will drop
packets. Watch `seq` for gaps.

Enabling the PPG stream forces the ring into a measurement window and
**keeps it there** — normal duty cycling is suspended for as long as you
are streaming. Turn it off when you are done or the battery will suffer.

---

## 5. IMU stream

Off by default. Enable with control opcode `0x02`.

```
offset 0   i16  x
offset 2   i16  y
offset 4   i16  z
```

Raw counts at +/-2 g full scale, so **milli-g = raw * 2000 / 32768**.
Sampled at the main loop rate, roughly 50 Hz while measuring and 50 Hz
while idle with streaming on.

---

## 6. Control characteristic

Write (with or without response). First byte is the opcode.

| Opcode | Payload | Effect |
|---|---|---|
| `0x01` | `u8 on` | Raw PPG streaming on/off. Turning it on forces a measurement window. |
| `0x02` | `u8 on` | IMU streaming on/off |
| `0x03` | none | Start a measurement window immediately, skipping the duty cycle |
| `0x04` | `u16 window_s`, `u16 period_s` | Set duty cycle, little-endian |
| `0x05` | none | Clear every bond, then disconnect |
| `0x06` | `u16 weight_kg_x10` | Set body weight for the calorie estimate, little-endian, kilograms x10 (e.g. `700` = 70.0 kg) |
| `0x07` | none | Reset steps, sleep and calorie counters to zero |
| `0x08` | `u8 on` | Raw GSR streaming on/off. Powers the analog front end for as long as it runs |
| `0x09` | `u8 on` | SpO2 mode on/off. Runs red+IR instead of green; **heart rate pauses** while on |
| `0x0A` | `u8 age`, `u8 sex` | Age in years (0 = not given, else clamped to 10-100) and sex (0 not given, 1 female, 2 male) for workout calories. Saved to flash like weight |
| `0x0B` | `u8 type` | Start a workout: 1 running, 2 rowing, 3 cycling, 4 other. `0` stops it. See section 11 |

Duty cycle values are clamped in firmware: window 5-300 s, period
window-3600 s. Defaults are a **15 s window every 60 s**, i.e. 25% duty.

Weight is clamped to 20.0-250.0 kg and defaults to 70.0 kg until set —
without it the calorie estimate in the Activity characteristic (section 7)
is only right for someone who happens to weigh 70 kg.

**The weight is saved to flash** and restored at boot, in the
same settings partition as the bonds. The value in use changes the
moment the write arrives; flash follows within a minute at most, so a
burst of writes (a slider) costs one or two flash writes, not dozens. A
write equal to what is already saved costs nothing.

Age and sex (opcode `0x0A`) are saved and restored the same way.

**Still send it on every connection.** Nothing on the wire reports which
weight the ring is using, the save path has not yet run on real hardware
(see STATUS.md, R3), and a resend of an unchanged value is free. Treat
persistence as the ring covering for a reboot you missed, not as a reason
to stop sending.

`0x05` is the unpair path. `CONFIG_BT_MAX_PAIRED` is 1 and
`CONFIG_BT_KEYS_OVERWRITE_OLDEST` is deliberately off, so without it the
first phone to pair owns the ring until somebody erases its flash over SWD.
The control characteristic is `BT_GATT_PERM_WRITE_ENCRYPT`, so the write
can only arrive over an encrypted link — on a ring holding exactly one
bond, that means the bonded owner and nobody else. The ring drops the
connection immediately afterwards, because the keys protecting that link
have just been deleted.

Both streams are forced off on disconnect, so a phone that walks away
cannot leave the ring burning battery.

Example — request a reading right now:

```
write [0x03]
```

Example — 10 s window every 5 minutes:

```
write [0x04, 0x0A, 0x00, 0x2C, 0x01]
              window=10        period=300
```

Example — hand the ring to somebody else:

```
write [0x05]        then expect a disconnect
```

Example — set body weight to 68.5 kg:

```
write [0x06, 0xAD, 0x02]
              weight_kg_x10=685 (68.5 kg, 0x02AD little-endian)
```

Example — start a new day (zero steps/sleep/calories):

```
write [0x07]
```

---

## 7. Activity characteristic

20 bytes, little-endian. Steps, sleep and calories come from the
accelerometer alone; the three wear fields come from the PPG. Readable at
any time; notified alongside Status, so the two never disagree about which
measurement period they describe.

> **This grew from 17 bytes to 20 in v0.5.** The first 17 bytes are
> unchanged, so an app that reads the old layout keeps working and simply
> ignores the new fields. It is still one unfragmented notification at the
> default 23-byte ATT MTU.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32 | `steps` | Total since boot or the last reset (opcode `0x07`) |
| 4 | u32 | `kcal_x1000` | Kilocalories x1000 since boot or the last reset |
| 8 | u16 | `cadence_spm` | Current steps per minute, 0 within a few seconds of stopping |
| 10 | u16 | `sleep_session_min` | Minutes into the current sleep session, 0 while awake |
| 12 | u16 | `sleep_total_min` | Minutes asleep since boot or the last reset |
| 14 | u16 | `restless_min` | Minutes of motion during the current/last session that did not end it |
| 16 | u8 | `sleep_state` | 0 awake, 1 asleep |
| 17 | u8 | `wear_state` | 0 unknown, 1 worn, 2 not worn — see below |
| 18 | u8 | `wear_checks` | Wear checks taken during this session, saturating at 255 |
| 19 | u8 | `wear_confirmed` | How many of those found a hand, saturating at 255 |

### Wear corroboration

Stillness cannot tell a sleeping hand from a nightstand, so while a sleep
session is open the ring briefly powers the PPG — a 6 s window every 30
minutes by default — purely to read the DC level and answer "is this on a
hand?". That is the same signal `ppg_dc` and the finger flag in section 3
are built on, and it is one of the few things ever actually measured on
this board: ~2,800 with nothing on the sensor against ~53,000 on skin.

| `wear_state` | Means | Reached when |
|---|---|---|
| 0 unknown | No usable evidence either way | Fewer than two checks have landed, or the PPG did not answer. **Not a synonym for "not worn"** |
| 1 worn | At least one check saw skin | Any single positive. One is enough — there is a factor of five between an empty sensor and the threshold |
| 2 not worn | Nothing on the sensor, twice running | Two or more checks, none positive |

`wear_checks` and `wear_confirmed` are there so you can weigh the verdict
instead of taking it. "Worn on 1 of 14" and "worn on 14 of 14" both report
`worn`, and they do not mean the same thing — the first looks like a ring
taken off shortly after going to bed.

**The ring does not act on this, and neither should it.** A session that
fails the check is still reported as sleep and its minutes are still in
`sleep_total_min`. The threshold was measured with a finger pressed against
a bench sensor, never with a ring worn loosely on a sleeping hand, so a
genuinely worn night *can* read as `not worn` — a loose ring, or one that
rotated off the pad. Suppressing sessions in firmware would hide that where
nobody could ever see it. Filtering is yours to do, with the counts in hand.

Both lifetimes are the same as `restless_min`: cleared when a new session
starts, not when one ends, so you can still read the verdict for the night
after the wearer gets up.

Turning it off (`CONFIG_RING_SLEEP_WEAR_CHECK=n`) makes every session report
`unknown`, except where a window opened for some other reason happened to
land inside one.

**None of this has been validated on hardware.** Steps come from
peak-detecting the IMU's acceleration magnitude — the same "plausible,
never checked against a reference" caveat that applies to heart rate in
section 14 applies here, and more so: a finger-worn ring does not move the
way a wrist or waist does, so even the detector's assumptions about what a
footstep looks like are unproven on this board. Calories are steps run
through a standard MET table, which only has a step count from the same
unvalidated detector to work from, plus whatever weight the app sent
(opcode `0x06`) or the 70 kg default. Exercise that steps cannot see,
such as rowing or cycling, needs a workout (section 11). Treat every field
here as a rough, uncalibrated trend line, not a number to show without a
caveat.

Three specific behaviours are worth designing around, because they are
confirmed in simulation rather than hypothetical:

* **"Asleep" really means "has not moved much for a while".** The
  classifier has one input — whether any minute saw a peak deviation of
  100 mg — and its behaviour is close to binary. Measured in simulation
  over an 8-hour stretch:

  | Wearer moves… | Logged as sleep |
  |---|---|
  | at least every 8 minutes | 0 of 8 hours |
  | only every 16 minutes | 8 of 8 hours |

  The transition sits at the 10 consecutive still minutes sleep onset
  requires. So anyone genuinely motionless for quarter-hours at a time —
  a long film, a flight, a nap on a sofa — is reported as asleep, and a
  restless sleeper who shifts every few minutes may never register a
  session at all.

* **A ring that is not being worn still logs sleep — but now it says so.**
  A ring on a nightstand is perfectly still, and stillness is the only
  signal the sleep *tracker* has. `wear_state` above is the corroboration:
  a nightstand session settles on `not worn` about half an hour after
  onset. Gate on it rather than on stillness heuristics of your own.
  Where `wear_state` is `unknown` — a dead PPG, or a session too short for
  two checks — the old advice still applies: an eight-hour "session" with
  `restless_min == 0` and no steps on either side of it is far more likely
  to be a bedside table than a night's sleep.
* **Gentle motion is not counted.** The detector needs roughly a ±100 mg
  swing in acceleration magnitude. Slow, smooth walking that never reaches
  that — and any stepping done with the hand in a pocket or resting on a
  pram handle — registers as nothing. Under-counting is the deliberate
  choice here: the threshold that would catch those also counts typing and
  gesturing as walking.
* **Everything stops while charging.** The ring reads no accelerometer at
  all in that state, so steps stop, a sleep session in progress ends
  rather than spanning the charge (`sleep_total_min` keeps whatever was
  credited before it), and `kcal_x1000` stops advancing — the ring has no
  reason to believe it is on a finger. Expect a flat spot, not a gap in
  the counters, and do not interpolate across it.

### Counters are since-boot, and a reboot zeroes them

**None of this is stored in flash.** Every field in this characteristic
lives in RAM and starts again from zero on any reset — a watchdog reboot,
a fatal error, a flat battery, or the battery contact bouncing under flex
(`HARDWARE_NOTES.md` §13, and it is a known issue on this revision). The
ring is not the system of record for anything here. **Your app is.**

Persistence was deliberately not added for these counters: the settings
partition has never been successfully written on this board, and putting
a step counter on a flash-write path before that is proven would risk the
bonds alongside it. (Body weight is saved, because it is written only when
the app changes it — see section 6.)

So accumulate on the phone side, and detect the resets:

1. Subscribe to **Status** as well as Activity, and watch `uptime_s`.
2. **If `uptime_s` goes backwards, the ring rebooted.** Everything in the
   Activity packet restarted from zero at that moment.
3. Keep your own running totals. On each read, add the *delta* since your
   last sample, and after a reboot treat the new value as the delta
   directly rather than subtracting your previous total from it — which
   would otherwise go negative and, unsigned, wrap.

Polling only the Activity characteristic cannot detect this: it carries
no uptime of its own, and a reboot mid-walk looks exactly like a step
count that quietly stopped climbing.

Body weight (opcode `0x06`) is the one exception: it is saved to flash
and survives a reboot (section 6). Resend it on each connection anyway —
see there for why.

There is also no RTC on this board (see README), so `sleep_session_min`
and `sleep_total_min` are durations, not clock times. If you want to show
"fell asleep at 11:42 PM," subtract `sleep_session_min` minutes from the
phone's own clock at the moment you read the characteristic — the ring has
no notion of wall-clock time to hand you instead.

---

## 8. HRV characteristic

3 bytes, little-endian. Notified alongside Status, so a reading always
pairs with the `hr_x10` and signal-quality numbers from the same moment.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u16 | `rmssd_x10` | RMSSD in milliseconds x10. **0 means not enough clean beats** |
| 2 | u8 | `rmssd_beats` | Successive differences behind the value |

`rmssd_beats` is the count of successive differences, so N differences
come from N+1 consecutive accepted beats. It is published so you can
apply your own bar: the firmware reports from 10 differences upward,
which is well under the 30-60 seconds of beats the HRV literature asks
for. Treat a value with 10-15 behind it as indicative, and wait for 30+
before showing a number you expect someone to act on.

### What gets counted

Only intervals that passed both of the beat detector's gates:

* the 30-220 bpm plausibility range, which every interval must pass to
  reach the heart-rate estimate at all, and
* agreement with the median of the recent intervals to within **20 %**.

That second bar is deliberately far tighter than the 40 % the heart rate
itself uses. A heart rate takes a median and shrugs off one odd interval;
RMSSD squares the error and lands it in two successive differences.
Measured: a metronome pulse train with a motion artifact partway through
each beat gets the artifact accepted as a real beat, and at 40 % the
resulting 600/400 ms alternation invents 200 ms of HRV from a rhythm that
has none. 20 % is the conventional artifact-rejection bound in the HRV
literature, and ordinary beat-to-beat variation sits far inside it — an
RMSSD of 40 ms on 1000 ms intervals is a 4 % swing.

A difference is only formed between two intervals that were genuinely
adjacent. A beat that was detected and then rejected still moves the beat
clock, so the interval following it is measured from a suspect beat — that
interval starts a new run rather than pairing across the gap. This matters
more than it sounds: a difference spanning a missed beat is roughly a
whole interval wide, and it enters the sum squared. Simulated, disabling
that one rule took a steady pulse train from 0 ms to 144 ms of apparent
HRV.

### The 10 ms floor, which you should surface

Beats are located to the nearest PPG sample and the PPG runs at 100 sps,
so **every interval is a multiple of 10 ms**. That quantisation alone puts
a floor under RMSSD: a metronome-steady heart with no variability at all
measures about **8.5 ms**, which is measured in `tests/test_hrv.c` rather
than estimated.

It adds in quadrature, so the error is not uniform:

| True RMSSD | Reads about |
|---|---|
| 40 ms | 41 ms |
| 30 ms | 31 ms |
| 20 ms | 22 ms |
| 10 ms | 13 ms |

A relaxed subject is barely affected. Low-HRV readings — stress,
exertion, illness, exactly the states a user would most want to trust —
are inflated the most and proportionally the most. Do not present small
differences between low readings as meaningful.

### It is not a 60-second RMSSD

The power model runs the PPG for 15 s in every 60, which is not enough
beats for a conventional window, so the ring keeps a rolling window of the
last 64 successive differences instead. At the default duty cycle that
window can span several minutes of wall time. No difference is ever
manufactured across a gap, but the reading is an average over a longer and
more ragged span than the literature's.

**If you want a reading closer to the textbook definition, buy yourself a
longer window**: control opcode `0x04` with a 60 s window and a 60 s
period runs the PPG continuously, and opcode `0x03` forces one immediately.
Both cost battery — the LEDs are the dominant draw — so do it while the
user is looking at an HRV screen, not in the background.

---

## 9. GSR stream

Raw skin conductance, off by default. Enable with control opcode `0x08`.

```
offset 0   u32  seq          increments per notification, gaps = dropped packets
offset 4   u8   count        number of samples that follow
offset 5   i16[count]        raw ADC counts, signed
```

**Sampled at 20 Hz.** The 1 Hz `gsr_mv` in the status packet exists for a
diagnostics view and is useless for event detection: a skin conductance
response peaks roughly 1.4 s after onset, so at 1 Hz you get about one
sample on the rise. 20 Hz is used rather than 10 so that ordinary jitter
in the firmware's main loop cannot drop the effective rate below the 10 Hz
floor where the shape stops being resolvable.

Batches are sized to the **default 23-byte MTU**: seven samples per
notification, about three notifications a second, always one unfragmented
packet. Requesting a larger MTU makes the packets no bigger — unlike the
PPG stream, this one is slow enough not to need it.

**Counts, not millivolts, and deliberately so.** Converting to volts and
from there to conductance needs `V_REF` and `R5`, which are board-specific
and — on this revision — not yet trusted. Sending counts keeps that
calibration on the phone, where you can change it without a firmware
flash. If you want the firmware's current opinion of the conversion, read
`gsr_mv` from Status and compare.

### This costs real battery

Enabling the stream holds `GSR_PWR` on continuously, because the front end
takes **800 ms to settle** and paying that per sample would defeat the
point. That suspends the analog side's duty cycling entirely for as long
as the stream runs. Turn it off when the user leaves the screen. It is
forced off on disconnect, so a phone that walks away cannot leave it
powered.

The first notification arrives roughly a second after you enable it: 800 ms
of settle, then seven samples at 20 Hz. That delay is the settle, not a
fault — and sampling before it completes would return the tail of a
power-on transient that looks exactly like a large response at the start
of every recording.

Order does not matter: you may subscribe before or after sending `0x08`.
Samples taken before you subscribe are simply not sent.

### It is unvalidated, and that is why it exists

**Nobody has confirmed the GSR front end produces a meaningful reading.**
`STATUS.md` has carried it as "unknown" for the life of the project: the
analog path had two firmware bugs, one is fixed, the second is a
hypothesis, and even with the ADC correct, R5 = 91 kΩ against dry skin
through 2 mm electrodes gives only a 1-10 % swing.

The stream is shipped anyway because **it is the instrument that settles
the question**. A 1 Hz scalar could never show whether the signal has the
shape of a real skin conductance response; a 20 Hz trace can. Expect to
use this for diagnosis before you use it for a feature, and do not build a
user-facing number on it until a trace off a real finger has been looked
at.

---

## 10. SpO2 characteristic

4 bytes, little-endian. Off by default; enable with control opcode `0x09`.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u16 | `ratio_x1000` | Ratio of ratios, R, x1000. **0 means not measured** |
| 2 | u8 | `percent` | SpO2 percentage. **0 means not measured** |
| 3 | u8 | `flags` | see below |

| Bit | Meaning |
|---|---|
| 0 | `UNCALIBRATED` — the percentage comes from an uncalibrated curve |
| 1 | `VALID` — the window held enough clean pulsatile signal for R to mean anything |

### Read this before you display a percentage

**Bit 0 is always set, in every build this firmware has.** It is not a
transient condition you can wait out.

`ratio_x1000` is a real measurement. R falls out of the optics and the
arithmetic — the ratio of each channel's pulsatile component to its steady
one — and needs no calibration:

```
R = (AC_red / DC_red) / (AC_ir / DC_ir)
```

`percent` is not. Turning R into a saturation takes an empirical curve
that every manufacturer derives by desaturating volunteers under a
reference oximeter and fitting the result. This firmware uses the
published default (`SpO2 = 110 − 25R`) which assumes an optical geometry,
LED wavelengths and photodiode response that **nobody has checked against
this board**. The number it produces is an illustration of the arithmetic,
not a measurement of anyone's blood.

So: show R, or show the percentage clearly labelled as uncalibrated, or
show nothing. Do not put a bare number next to a lung icon. If you only
have room for one, R is the honest choice and it is the value a future
calibration would be fitted against — record it alongside a reference
oximeter reading and you are most of the way to fixing this properly.

Readings outside 70-100 % are withheld entirely (`percent` reads 0, R is
still reported). A number in the 50s reads as a medical emergency, and
this firmware has no business generating one.

### What it costs

SpO2 mode lights the red and IR LEDs instead of green, so:

* **Heart rate pauses.** Every HR number this firmware produces comes from
  the green channel. While SpO2 mode is on, `hr_x10` reads 0, HRS
  notifications stop, and `ppg_dc`, `ppg_ac` and `perfusion_x10` in Status
  read 0 as well. RMSSD stops accumulating. Turn SpO2 off to get them
  back.
* `flags` bit 0 of Status — finger present — keeps working, because the
  red/IR DC level answers that question as well as green did.
* The mode takes effect at the **next measurement window**, not
  immediately: switching LEDs underneath a half-collected measurement
  would corrupt both. Sending `0x09` requests a window, so expect a result
  within about 15 seconds; a window already running finishes first.
* Two LEDs instead of one costs roughly twice the optical power for the
  duration of a window. It is forced off on disconnect.

Both LEDs run at the same drive current, which is the conventional
starting point — the ratio of ratios divides absolute intensity out, so
the currents only need to put both channels in a sensible part of the ADC
range.

---

## 11. Workout characteristic

16 bytes, little-endian. The figures for the workout the app started with
opcode `0x0B`, or the last one after it stops. Notified at least once a
minute during a workout, and on start and stop.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u8 | `active` | 1 while a workout is running |
| 1 | u8 | `type` | 1 running, 2 rowing, 3 cycling, 4 other |
| 2 | u16 | `minutes` | Minutes so far |
| 4 | u16 | `hr_minutes` | Minutes priced from heart rate |
| 6 | u16 | `rest_minutes` | Minutes with heart rate under 90 bpm, priced from steps |
| 8 | u16 | `fallback_minutes` | Minutes with no usable heart rate, priced from the activity type |
| 10 | u16 | `avg_hr_x10` | Mean heart rate over `hr_minutes`, bpm x10. 0 if none |
| 12 | u32 | `kcal_x1000` | Kilocalories x1000 for this workout |

The three minute counts always add up to `minutes`.

### Why workouts exist

The everyday calorie figure in the Activity packet comes from steps. It
cannot see rowing, cycling or most of the difference between jogging and
running. Heart rate can. During a workout the ring keeps the optical
sensor on and prices each minute from its mean heart rate with the Keytel
et al. (2005) equations, which use heart rate, weight, age and sex:

```
male    kJ/min = -55.0969 + 0.6309 HR + 0.1988 kg + 0.2017 age
female  kJ/min = -20.4022 + 0.4472 HR - 0.1263 kg + 0.0740 age
```

With sex not given the ring uses the mean of the two; with age not given
it uses 35. Send both with opcode `0x0A` for a better number.

Each minute takes one of three paths:

| Heart rate that minute | Priced by | Counted in |
|---|---|---|
| 90 bpm or more, on at least 30 of its seconds | Keytel, but never below the step estimate and never above 20 kcal/min | `hr_minutes` |
| Present but under 90 bpm | The ordinary step formula: the wearer is resting between efforts | `rest_minutes` |
| Missing (ring slipped, no finger, sensor trouble) | The activity's typical MET: running 8.3, rowing 7.0, cycling 7.5, other 5.0 — or the step formula if higher | `fallback_minutes` |

**Show the split.** A workout made mostly of `fallback_minutes` is mostly
an estimate from the activity type, not a measurement. Say so, or show
the figure with less confidence.

Workout calories are also added to `kcal_x1000` in the Activity packet,
so the day's total already includes them. Don't add them again.

### What it costs, and the limits

The optical sensor stays on for the whole workout, about 15 mA plus the
5 V boost. That is the most power the ring ever draws, so:

* A workout ends by itself after **four hours** with no stop from the app.
* If the sensor sees no finger for **two minutes**, the window closes. The
  workout carries on and checks again every measurement period.
* Attaching the charger ends it, and a start sent while charging is
  refused. `active` stays 0.
* SpO2 mode does not apply during a workout: the sensor uses the green LED,
  because every heart rate comes from green.

A workout is RAM-only, like the other counters. A reboot ends it. Watch
`uptime_s` as section 7 describes.

### How much to trust it

Keytel's equations are population averages fitted to lab exercise, so
for any one person they can be off by a meaningful margin. Here they rest on a
finger-worn heart rate that has **never been compared to a chest strap**.
That is harder during exercise, and hardest on a rowing machine, where
the grip on the handle squeezes the finger under the sensor. Treat the
number as a much better estimate than steps alone, not as a measurement.

---

## 12. Behaviour your app has to expect

**The ring is not always measuring.** By default the optical front end is
off for 45 seconds out of every 60. That is deliberate — the LEDs plus the
5 V boost dominate power draw.

What shuts it down entirely is the ring coming **off a finger**, not the
ring being still. Finding a finger during a window refreshes the motion
timeout, so a ring that is worn keeps its 15 s-in-60 duty cycle however
motionless the wearer is, all night. A ring that is off a hand stops
finding one and falls silent about three minutes later, until the
accelerometer wakes it.

> Earlier revisions of this document said the ring stopped probing after
> three minutes of stillness. That was wrong, in the direction that costs
> battery rather than data. If you are budgeting power for a worn night,
> budget for ~20 % duty, and consider sending opcode `0x04` to widen the
> period before bed. The sleep wear check in section 7 rides on those
> existing windows and adds essentially nothing while the ring is worn.

Consequences:

* HR notifications arrive in bursts, not continuously. Gaps of a minute
  are normal, not a fault.
* If the user opens your app and wants a reading *now*, send opcode
  `0x03`. Do not wait for the next scheduled window.
* After a window starts it takes roughly **4 seconds to settle** and
  another **8 beats** before a BPM appears. Budget 10-15 s from request to
  first reading, and show a progress state rather than a spinner that
  looks stuck.

**Wake on motion.** The ring sleeps until the IMU sees roughly 80 mg of
movement, then immediately opens a window to look for a finger. Picking the
ring up wakes it; it does not need the app to poke it.

**Charging suspends measurement entirely.** In `state == 2` there is no PPG
data at all, the LEDs are off, and HR notifications stop. Show a charging
state, not "no signal". The red LED lights for 3 seconds when a charger is
first detected, so the user gets confirmation without opening the app.

---

## 13. Connection parameters

The firmware does not request specific connection parameters, so you get
whatever the phone proposes. Recommendations:

| Use case | Interval | Notes |
|---|---|---|
| Background HR only | 200-500 ms | Cheapest; HR only updates once a second anyway |
| Raw PPG streaming | 15-30 ms | Needed to keep up with 100 Hz sampling |

Also request a **247-byte MTU** before enabling any stream, and a data
length extension if your stack exposes it.

---

## 14. Caveats worth knowing

These are real limitations of the current hardware and firmware, not
things to paper over in the UI.

**Heart rate is not validated.** The algorithm is a DC tracker, a 4 Hz
low-pass, adaptive-threshold peak detection, and a median of the last 8
intervals. It has been sanity-checked against plausible resting rates only
— never against a reference monitor. Do not present it as medical-grade,
and do not build clinical features on it until it has been validated.

**Battery percentage is a voltage bar.** See section 2. `battery_mv` is a
real measurement from the PMIC with roughly +/-42 mV resolution. The
percentage is not a state of charge.

**GSR is unproven.** The analog front end has never been tested with
anything across the electrodes. `gsr_mv` will report a number; nobody has
confirmed it means anything. Treat it as experimental.

**Battery temperature protection is disabled.** The PMIC's TS pin is
unconnected on this board revision, so the firmware disables the thermal
monitor to allow charging at all. This is a bench workaround, not a
shippable configuration. See `HARDWARE_NOTES.md` item 9.

**Pairing is Just Works, so there is no authentication.** As of v0.4 the
Ring Service does demand an encrypted link and bonds are stored across
reboots, which closes the hole where anyone in range could read biometrics
and write control opcodes. What it does not do is prove who the peer is:
with no display and no keypad the ring cannot offer a passkey, so an
active attacker present during the pairing exchange can still get in the
middle. Once bonded, the link is encrypted and passive eavesdropping is
out.

The bond-clearing gap is closed as of v0.4.1: control opcode `0x05`
erases every bond and disconnects, so a ring can change hands without a
debugger. See section 6.

**Heart rate is broadcast unencrypted, and that is a decision.** The
standard Heart Rate Service (`0x180D`) requires no pairing to read, so
anyone in range who connects can see the wearer's heart rate. It stays
that way by default, because the entire reason for implementing the SIG
service alongside the custom one is that a watch face or a cycling
computer that knows nothing about this ring still works. Requiring
encryption there would break every generic client and buy a first-party
app nothing, since `hr_x10` has always been available in the encrypted
Ring Service status packet.

The trade is exposed rather than hidden: `CONFIG_RING_HRS_OPEN` (Kconfig,
default `y`). Build with `-DCONFIG_RING_HRS_OPEN=n` and the HRS
characteristics move to `CONFIG_BT_HRS_DEFAULT_PERM_RW_ENCRYPT`, so only a
bonded phone can read heart rate; generic clients will discover the
service and fail to subscribe. Nothing in the advertising data changes, so
scan filters keep working either way. The Battery Service is untouched and
stays open in both builds.

**None of the v0.4 or v0.4.1 BLE work has run on hardware.** It compiles
across five build configurations and nothing more. No phone has paired with
this ring, the flash partition that holds the bonds has never been written,
and no bond has ever been cleared.

---

## 15. Quick start

1. Scan for service `0x180D`, connect
2. Request MTU 247
3. Raise security and pair — Just Works, no passkey. Everything in the
   Ring Service is refused until this succeeds
4. Subscribe to Status (`f0a10001-...`)
5. Subscribe to Heart Rate Measurement (`0x2A37`)
6. Write `[0x03]` to Control to request an immediate reading
7. Watch `flags` bit 0 for finger presence and `perfusion_x10` for grip
   quality; show the user a "hold still, don't squeeze" prompt while
   `hr_x10` is still 0
8. Read `battery_mv` from Status, not the Battery Service percentage
