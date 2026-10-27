# App integration guide

Everything a phone app needs to talk to the ring. Firmware as of v0.7.0
(2026-10-12). New in v0.6: the ring keeps its own history in flash and a
phone syncs it -- section 16.

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
| Firmware Revision | `0x2A26` | `0.8.0` (production; see `prj.conf`) |
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
| History | `f0a1000a-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Notify | Encrypted CCC |
| Wellness | `f0a1000b-1e5c-4a2b-8d3f-9c7b6e5a4d21` | Read, Notify | Encrypted read (v0.8.0+, section 17) |

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
| 6 | i16 | `gsr_mv` | millivolts, **-1 = not measured**. Always -1 in production from v0.7.0: the GSR front end cannot resolve skin conductance on this board |
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
| 247 (requested) | 10, one notification every ~100 ms (up to 40 after a backlog) |

**Request a 247-byte MTU.** At the default you get ~25 notifications per
second, which most phones will struggle to sustain and which will drop
packets. Watch `seq` for gaps.

**The ring drops, it never waits.** It has three ATT transmit buffers. When
the link cannot keep up, a notification that finds no free buffer is
discarded and `seq` still advances, so the loss shows as a gap. This applies
to every notification the ring sends, including Heart Rate and Status. It
used to wait for a buffer instead, which on a weak link stalled the main
loop long enough for the watchdog to reset the ring.

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
| `0x0C` | none | Power off (PMIC ship mode). On battery the ring turns off at once and the link drops; on a charger it turns off when the charger is unplugged. ~150 nA while off. **Only plugging a charger in wakes it** — this board has no button |
| `0x0D` | `u32 unix_s` | Set the ring's clock to Unix time (seconds, UTC), little-endian. **Send on every connection.** History records are stamped with it until the next reboot. See section 16 |
| `0x10` | `u8 led_pa, u8 led_rge, u8 adc_rge, u8 avg_log2` | **Diagnostic, test builds only** (refused with ATT `0x13` in production, v0.7.0+). Tune the heart-rate PPG front end: green LED current code, LED range (0 = 31 mA, 1 = 62 mA full scale), ADC range (0-3 = 4/8/16/32 µA), on-chip averaging (0-2 = 1/2/4 pulses; output stays 100 sps). A running window restarts with the new settings. RAM only, a reboot restores the defaults: `0xFF, 0, 2, 2` from v0.6.2 (31 mA, 4x averaging), `0x80, 0, 2, 0` in v0.6.1. Out-of-range values get ATT `0x13`. For `tools/ppg_sweep.py`, not for an app's settings screen |
| `0x11` | `i8 dbm` | **Diagnostic, test builds only** (refused in production, v0.7.0+). Radio TX power on this connection, -40 to +8 dBm (the controller rounds to a supported level). RAM only, this connection only |
| `0x0E` | `u32 from_seq` | Sync history: the ring notifies every stored record with `seq >= from_seq` on the History characteristic, oldest first, then an end-of-sync summary. Subscribe to History first, or the write fails with ATT `0xFD`; from v0.8.0 an MTU under 37 fails it with ATT `0x11`. See section 16 |
| `0x12` | `u16 night_min, u16 day_min, u16 window_s` | v0.8.0+, every build. Spot reading schedule (section 17): minutes between night readings and between daytime still readings (0 = off, else 10-720), and the length of each (60-600 s). RAM only: send on every connection if you want something other than the build's defaults. Production defaults `60, 60, 180`; test builds `0, 0, 180`. Out of range: ATT `0x13` |
| `0x13` | `u8 mode, u8 rise_c10` | v0.8.0+, every build. Hot-flash detection (section 17): mode 0 off, 1 while asleep only, 2 always; onset threshold in tenths of a degree, 3-50. RAM only. Production default `1, 10`; test builds `0, 10`. Out of range: ATT `0x13` |
| `0x14` | none | v0.8.0+. Take a spot reading now (section 17): the "check my stress" button. A 3-minute green window, judged like a daytime reading; the result arrives on Wellness (`spot_kind` 4) and in the history as a `SPOT_DAY` reading. Needs the ring on a finger and the hand still |

Duty cycle values are clamped in firmware: window 5-300 s, period
window-3600 s. Defaults are a **25 s window every 60 s**, i.e. ~42% duty (15 s before v0.6.2; the new heart-rate detector needs ~12 s before its first reading). The daily build runs 25 s every 600 s, ~4%.

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

**Steps arrive in bursts of eight (v0.7.0).** Worn on 2026-10-12, the step
count was far too high: every hand movement that crossed the detector's
threshold counted. In simulation, 30 minutes of irregular hand movement
read as 545 steps. The counter now holds candidate steps back until eight
have arrived in a steady rhythm, each interval within 30 % of the one
before. It then credits all eight at once and counts live while the rhythm
holds. The same simulation now reads 0, and simulated walks are still
counted to within 1-3 %. For an app this means:

* The count jumps by 8 at the start of a walk, rather than ticking from 1.
* Fewer than eight steps -- crossing a room -- are not counted at all.
* Anything rhythmic with the hand counts. Brushing teeth, for example,
  will still read as walking. Every pedometer shares this limitation.

These are simulated numbers. `tools/steps_record.py` and
`tools/steps_eval.py` exist to measure them on real walks.

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

> **v0.6:** the live counters below still reset, but every 5 minutes the
> ring now writes a summary (steps in that interval, heart rate, flags) to
> flash, and those survive reboots. For anything historical -- a day's
> steps, last night -- build from the history sync in section 16 rather
> than from these counters.

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

The power model runs the PPG for 25 s in every 60, which is not enough
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
timeout, so a ring that is worn keeps its 25 s-in-60 duty cycle however
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
* After a window starts it takes roughly **12 seconds** before a BPM
  appears (v0.6.2: 10 s of signal for the first spectrum, then agreeing
  estimates). Budget 15 s from request to first reading, and show a
  progress state rather than a spinner that looks stuck.

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

**Heart rate is only lightly validated.** Since v0.6.2 the rate comes from
a spectrum: the raw PPG is despiked, band-passed, and once a second a 10 s
spectrum picks the strongest rhythm, gated on how clearly it stands out,
reported as the median of recent estimates. Replayed offline against ~8
minutes of WHOOP-referenced recordings (`tools/hr_eval.py`) it averaged
6.8 bpm of error with 60 % of readings within 5 bpm, against 46 bpm and 0 %
for the v0.6.1 detector -- and 2-4 bpm with the hand still and close. It
has not yet run on a ring against a reference, nor during exercise, and
motion is the known weak spot. Expect:

* **Nothing for the first ~12 s** of a window: 10 s of signal, then agreeing
  estimates. "No reading" is the answer whenever the spectrum is unclear --
  the detector is built to stay silent rather than guess.
* **About 12 s of reading per window.** Windows are 25 s from v0.6.2, so
  roughly half of each one carries a heart rate.

Do not present it as medical-grade, and do not build clinical features on
it.

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
9. Write `[0x0D, <u32 unix time>]` to set the ring's clock — every
   connection
10. Subscribe to History (`f0a1000a-...`), then write
    `[0x0E, <u32 last stored seq + 1>]` and store what arrives. Section 16

---

## 16. On-ring history and sync

New in v0.6; **record layout v2 from v0.8.0**. v0.6 history has been
verified on ring 2 (58 records, no gaps, no bad CRCs); v2 is host-tested
and compile-verified, not yet run on a ring.

Every `CONFIG_RING_HISTORY_EPOCH_S` seconds (300 by default) the ring
writes one 32-byte record to its own flash, whether or not anything is
connected. The log holds **5,504 records, about 19 days at 5 minutes**,
and when it is full the oldest 4 kB page (128 records, ~10.7 hours) is
erased to make room. The app downloads whatever it has not seen yet.

> **Upgrading to v0.8.0 erases the log.** The first boot of the new layout
> sees a v1 header and reformats. Sync before flashing.

### Record format (v2, v0.8.0+)

32 bytes, little-endian, identical in flash and on the wire. Pinned by
compile-time assertions in `tests/test_history.c`; `tools/history_sync.py`
`parse_record_v2()` is the reference parser.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u32 | `seq` | 1, 2, 3, ... never repeats, **survives reboots**. The sync cursor |
| 4 | u32 | `time_s` | **End** of the epoch. Unix seconds if flag bit 0 is set, otherwise seconds since that boot |
| 8 | u16 | `steps` | Steps **during this epoch**, not a running total |
| 10 | i16 | `temp_c100` | Ring temperature (nRF die), mean over the epoch, 0.01 C. `-32768` = none. Reads a few degrees under skin once worn; on the charger it reads the charger's heat |
| 12 | i16 | `skin_c100` | PPG sensor face temperature, mean of the readings taken in windows that saw a finger, 0.01 C. `-32768` = none (no window, or no finger) |
| 14 | u8 | `hr_bpm` | Mean of the once-a-second heart rates during the epoch. **0 = no reading** |
| 15 | u8 | `hr_min_bpm` | Lowest one-second heart rate. 0 = none |
| 16 | u8 | `hr_max_bpm` | Highest one-second heart rate. 0 = none |
| 17 | u8 | `rmssd_ms` | Rolling RMSSD at the end of the epoch (section 8). 0 = none. Only set when the epoch had heart rate |
| 18 | u8 | `rest_hr_bpm` | Mean heart rate of a **spot reading** that finished in this epoch and passed (section 17). 0 = none |
| 19 | u8 | `rest_rmssd_ms` | RMSSD of that spot reading, from its own beats only. 0 = fewer than 20 clean differences |
| 20 | u8 | `rest_beats` | Successive differences behind `rest_rmssd_ms`: every clean beat of the reading, ~150 for 3 min at rest, capped at 255 |
| 21 | u8 | `temp_rise_c10` | Highest ring temperature above its settled baseline during the epoch, 0.1 C. 0 until the baseline has settled |
| 22 | u8 | `motion_pct` | Share of the epoch the accelerometer read more than 50 mg away from 1 g, time-weighted. **255 = not read** (charging) |
| 23 | u8 | `battery_20mv` | Battery millivolts / 20 (190 = 3.80 V). 0 = not measured yet this boot |
| 24 | u8 | `flags` | See below |
| 25 | u8 | `hot_flashes` | Hot flashes that **started** in this epoch |
| 26 | u8 | `boot_id` | Which boot wrote it, +1 per boot that wrote records, wraps at 256 |
| 27 | 4 bytes | reserved | 0 |
| 31 | u8 | `crc` | CRC-8 over bytes 0-30: poly `0x07`, init `0x00`, no reflection, no final XOR (CRC-8/SMBUS; `"123456789"` gives `0xF4`) |

`flags`:

| Bit | Meaning |
|---|---|
| 0 | `TIME_VALID`: `time_s` is Unix time |
| 1 | `WORN`: a measurement window found a finger during the epoch |
| 2 | `CHARGING`: on the charger at some point in the epoch |
| 3 | `ASLEEP`: the sleep tracker said asleep at some point in the epoch |
| 4 | `WORKOUT`: a workout was running |
| 5 | `HOT_FLASH`: a hot flash was in progress at some point in the epoch |
| 6 | `SPOT_NIGHT`: `rest_*` came from a night spot reading |
| 7 | `SPOT_DAY`: `rest_*` came from a daytime still spot reading |

Bits 1-5 mean "at any point in the epoch". Absence of `WORN` is "not
seen", not "not worn".

**Verify the CRC and drop records that fail it.** The ring already
discards torn records, so a failure on the phone means a transport bug.

The v1 layout (16 bytes: `seq, time_s, u8 hr_bpm, u8 rmssd_ms, u16 steps,
u8 battery_20mv, u8 flags, u8 boot_id, u8 crc`, CRC over bytes 0-14) is
what v0.6-v0.7 rings send, in packet type `0x01`. An app that wants to
read an older ring keeps both parsers; `history_sync.py` does.

### Sync protocol

1. Subscribe to the History characteristic (`f0a1000a-…`). It is
   notify-only and, like the rest of the Ring Service, needs the encrypted
   link.
2. Write `[0x0D, unix_s (u32 LE)]` to Control. Do it first, so the end
   summary carries a valid clock.
3. Write `[0x0E, from_seq (u32 LE)]` to Control. Use `last_seq_stored + 1`,
   or `0` the first time. A `from_seq` older than the oldest record simply
   starts at the oldest.
4. Notifications arrive on History. The first byte says which kind:

   **Records, v2: `0x03`** (v0.8.0+)

   ```
   offset 0   u8   0x03
   offset 1   u8   n                    records in this packet
   offset 2   32 x n bytes              records, oldest first
   ```

   `n` is up to 7 at a 247-byte MTU. At the default 23 a 32-byte record
   does not fit a notification at all, so **request a bigger MTU first**
   (37 bytes is the minimum, 247 the useful size): below 37 the `0x0E`
   write itself fails with ATT `0x11`, Insufficient Resources. iOS
   negotiates a big MTU on its own; Android needs `requestMtu(247)`.

   **Records, v1: `0x01`** (v0.6-v0.7): the same, with 16-byte records,
   up to 15 a packet. The type changed with the layout so an app that only
   knows `0x01` ignores v2 records instead of misreading them.

   **End — `0x02`, always last, always 20 bytes**

   | Offset | Type | Field |
   |---|---|---|
   | 0 | u8 | `0x02` |
   | 1 | u32 | `oldest_seq` — oldest record still stored (0 = log empty) |
   | 5 | u32 | `newest_seq` — newest record when the sync began (0 = empty) |
   | 9 | u32 | `uptime_s` — ring's seconds since boot, now |
   | 13 | u32 | `unix_s` — ring's clock now, 0 if not set this boot |
   | 17 | u8 | `boot_id` — the boot running now |
   | 18 | u16 | `epoch_s` — seconds per record |

5. On the end packet, store `newest_seq` as your cursor **only after the
   records are safely saved**. Next sync starts from `newest_seq + 1`.

Records arrive in strictly increasing `seq`. They are contiguous except
where the log wrapped (`oldest_seq` > your cursor + 1 — you were away
longer than ~19 days; the gap is lost) or where a record failed in flash.
Records written *during* a sync are left for the next one.

Unlike the live streams, the sync **never drops**: a notification that
finds the link full is held and resent, so a sync is either complete or
interrupted, never holey. If the link drops mid-sync, keep the records
you have, advance your cursor to the last one, and sync again from there.
While a sync runs, the live notifications (Status, HR, …) share the same
buffers and may drop — expect a gap in live data for the few seconds a
sync takes.

### Turning `time_s` into wall-clock time

There is no RTC. The ring knows Unix time only after a phone has sent
`0x0D` in the current boot. It keeps that time across a watchdog or crash
reset (in RAM the reset does not clear), but **a power loss or ship mode
loses it** — and a battery-contact bounce on this board revision is a
power loss.

Per record:

```
if flags & TIME_VALID:
    t = time_s                                   # Unix seconds
elif boot_id == end.boot_id and end.unix_s != 0:
    t = end.unix_s - (end.uptime_s - time_s)     # same boot as now: exact
else:
    t = unknown                                  # a past boot nobody synced
```

For the last case the order is still right (`seq`), and the record is
still `epoch_s` long, so a chain of them can be placed by walking
backwards in steps of `epoch_s` from the first dated record after them —
an estimate that ignores however long the ring was off between boots.
Show such records as "approximate" or leave them off a timeline.

`boot_id` only advances on a boot that writes a record, so a ring that
reboots twice within 5 minutes shows one boot.

### What it costs, and limits

* One 32-byte flash write per epoch; a 4 kB page erase (~85 ms) every 128
  records. Negligible power. The partition wears one page-cycle per
  ~19 days; the flash is rated for 10,000.
* Nothing is recorded while the ring is powered off (ship mode or a flat
  battery) — that is a gap in `time_s`, not missing `seq`s.
* A record holds a heart rate only if a measurement window ran in its
  epoch. In the daily build (`.\build.ps1 -Daily`) that is one 25 s window
  every 10 minutes, about 12 s of it with a heart rate; the demo build runs none unless the app asks.
* The log lives at flash `0x50000`-`0x7BFFF`, separate from the bonds at
  `0x7C000`. Reflashing the firmware over SWD does not erase it; the first
  boot of v0.6 formats it once (~4 s at boot, before measuring starts; a
  chip erase over SWD means one more format).

---

## 17. Temperature, hot flashes and spot readings

New in v0.8.0. **Host-tested and compile-verified only.** Production
builds have both features on; test builds have them off until the app
turns them on with `0x12`/`0x13`. Ring temperature is logged in every
build.

### Wellness characteristic, `f0a1000b-...`

Read, Notify. 16 bytes, little-endian. Notified when a hot flash starts
or ends, when a spot reading finishes, when `0x13` is written, and once a
minute.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | i16 | `temp_c100` | Ring temperature, smoothed (tau 60 s), 0.01 C. `-32768` before the first sample |
| 2 | i16 | `baseline_c100` | Settled baseline the hot-flash detector compares against. `-32768` until settled |
| 4 | i16 | `skin_c100` | PPG sensor face, last window that saw a finger. `-32768` = none this boot |
| 6 | u8 | `flags` | bit 0 baseline settled, bit 1 hot flash in progress, bit 2 spot reading running, bit 3 last spot reading passed |
| 7 | u8 | `hot_flashes` | Since boot, wraps at 256 |
| 8 | u8 | `spot_kind` | Last finished spot reading: 0 none yet, 1 night, 2 day, 3 hot flash, 4 manual (`0x14`) |
| 9 | u8 | `spot_hr_bpm` | Its mean heart rate |
| 10 | u8 | `spot_rmssd_ms` | Its RMSSD; 0 = fewer than 20 clean differences |
| 11 | u8 | `spot_beats` | Differences behind it |
| 12 | u32 | `spot_uptime_s` | When it finished, on the Status `uptime_s` clock. 0 = none |

### Spot readings

The ordinary windows are 25 s, too short for an HRV number worth
trending. A spot reading is a 3-minute green window (`window_s`) at a
moment chosen for it, with an RMSSD over its own beats only (the rolling
HRV characteristic is left alone):

* **Night**: 15 min after the sleep tracker says asleep, then every
  `night_min` while asleep.
* **Day**: awake, worn, and the accelerometer still for 3 minutes, at
  most every `day_min`.
* **Hot flash**: one straight after a hot flash starts.
* **Manual**: one now, on opcode `0x14`. Leaves the schedule alone.

A reading passes with at least 30 one-second heart rates and the
accelerometer quiet (under 50 mg from 1 g) for 90 % of it; a night or day
reading with sustained motion is cut short. A failed one is retried after
10 minutes. Passing night and day readings land in that epoch's history
record as `rest_hr_bpm` / `rest_rmssd_ms` / `rest_beats` with
`SPOT_NIGHT` or `SPOT_DAY` set. These are the numbers for stress and
burnout scores; see `../mac-handoff/WELLNESS_ANALYTICS.md`.

Cost at the production schedule: ~5 % extra LED duty while asleep, less
by day.

### Hot flashes

The ring watches its own temperature (nRF die, every 10 s) for the shape
of a hot flash: at least `rise_c10` (default 1.0 C) above a settled
baseline, three quarters of it gained within 10 minutes. It ends when the
temperature is back within 0.3 C of the baseline, or after 30 minutes. 15
minutes refractory after each.

Measured on synthetic traces (`tests/test_thermo.c`): a 1.5 C flash is
flagged ~3 min after it begins; a 1.0 C rise is not (threshold is
inclusive of noise); steady warming slower than ~0.07 C/min, like the
hands warming at sleep onset, never triggers.

Nothing is judged until the ring has been worn, off the charger and out
of a workout for 30 minutes, and in mode 1 until the wearer has been
asleep for 20 (with 10 minutes' grace after waking, since hot flashes
wake people).

**Unvalidated.** The die reads the finger through the ring, attenuated
and a minute or so late, at 0.25 C resolution, and nobody has worn this
board through a hot flash. A hand around a hot mug or out from under a
duvet looks the same. Ask the wearer to confirm or dismiss each event;
that is the data that would tune `rise_c10`.
