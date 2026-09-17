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
| Firmware Revision | `0x2A26` | `0.4.0` |
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
offset 5   u32[count]        19-bit ADC values, zero-extended
```

Sample rate is **100 Hz**. Packet size adapts to the negotiated MTU:

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

Duty cycle values are clamped in firmware: window 5-300 s, period
window-3600 s. Defaults are a **15 s window every 60 s**, i.e. 25% duty.

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

---

## 7. Behaviour your app has to expect

**The ring is not always measuring.** By default the optical front end is
off for 45 seconds out of every 60, and completely off when the ring has
been still for 3 minutes. That is deliberate — the LEDs plus the 5 V boost
dominate power draw.

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

## 8. Connection parameters

The firmware does not request specific connection parameters, so you get
whatever the phone proposes. Recommendations:

| Use case | Interval | Notes |
|---|---|---|
| Background HR only | 200-500 ms | Cheapest; HR only updates once a second anyway |
| Raw PPG streaming | 15-30 ms | Needed to keep up with 100 Hz sampling |

Also request a **247-byte MTU** before enabling any stream, and a data
length extension if your stack exposes it.

---

## 9. Caveats worth knowing

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

## 10. Quick start

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
