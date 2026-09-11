# Post-mortem: two days lost to a masked error code

**Symptom:** the MAXM86161 would not respond on I2C. Intermittently it
would start working when the board was physically touched, soldered, or
when SCL was shorted to ground.

**Actual cause:** software. Address-only I2C reads to the MAXM86161 fail,
and the bring-up firmware issued 112 of them at every boot.

**Cost:** two working boards destroyed by rework chasing a hardware fault
that did not exist. Roughly two days.

---

## What was actually wrong

The driver probed the part with `i2c_write_read()` -- register pointer,
repeated START, read -- with no preceding pointer write, and before that the
firmware ran a full 112-address bus scan built out of **bare address-only
reads**:

```c
i2c_read(i2c, &dummy, 1, addr);     /* no register pointer */
```

Once the probe was instrumented to report each access pattern separately,
one boot gave the answer:

```
0x62  bare=-5  ptr_write=0  split=0(0x36)  restart=0(0x36)
```

* `bare` (address-only read) returns **-5 / EIO**
* every access that **writes the register pointer first** succeeds and
  returns PART_ID `0x36`

The MAXM86161 datasheet never documents a bare address read. Figures 12-15
all show a register pointer write, optionally followed by a repeated START.
The bare read was never a legal access; the bus scan was doing it 112 times
per boot and leaving the part unable to answer.

Grounding SCL by hand forced a bus-idle condition that cleared the state,
which is why physical intervention "fixed" it. That is also why it appeared
to correlate with soldering.

## Why it took two days

### 1. The real error code was thrown away

`maxm86161_probe()` returned its own `-ENODEV` when all candidate addresses
failed, discarding whatever the I2C driver actually reported:

```c
dev->addr = 0;
return -ENODEV;          /* -19, every time, regardless of cause */
```

Every log line for two days said `-19`. It carried no information. The
moment per-transaction errnos were logged, the cause was visible in a single
boot.

**This was the single biggest failure.** Everything downstream followed from
it.

### 2. Hardware was diagnosed from software symptoms

Elaborate electrical theories were built -- ESD clamp voltages, probe input
thresholds, lifted pads, LGA bridges -- while the software layer was
returning a placeholder error nobody had questioned. The electrical
reasoning was mostly *correct*; it was aimed at a fault that was not there.

### 3. The diagnostic tool was the fault

The bus scan was added for visibility. It was the thing breaking the bus.
A diagnostic that perturbs the system it measures will send you in circles,
and it was never treated as a suspect.

### 4. Correlation with physical action was read as proof

"It starts working when I solder it" was taken as evidence of a bad joint.
The real mechanism was that physical intervention on SCL cleared a wedged
bus. Identical correlation, completely different cause -- and the hardware
explanation was anchored on early and never seriously re-opened.

### 5. A free canary was ignored

The BQ25120A sits permanently on the same bus at address **0x6A**,
independent of VLED, the boost, and U3. It never appeared in any bus scan.
That anomaly was visible in the logs on board 2 and went unremarked. A
permanently-present device failing to answer points at the bus or the
scanning code -- not at the sensor.

### 6. Irreversible actions were recommended on weak evidence

Reflow was suggested while the fault was still unlocalized. Two boards were
lost. Rework is not a diagnostic step; it destroys the evidence you need.

### 7. Skepticism was applied asymmetrically

When the user said "it's probably software," the response was to marshal
evidence against it. The user's theory was cheaper to test than the hardware
theory and should have been tested first. It was also correct.

---

## Rules to prevent it

1. **Never synthesize an error code during bring-up.** Propagate and log the
   real errno from every transaction. A placeholder error is a blindfold.

2. **Instrument before theorizing.** One boot with per-transaction logging
   beat two days of electrical reasoning. If a theory cannot be tested in
   the next five minutes, build the instrument instead.

3. **Treat your own diagnostics as suspects.** Anything added to observe the
   system can perturb it. When symptoms are weird, disable the diagnostics
   and re-test.

4. **Keep a canary on every shared bus.** An always-present device (here the
   PMIC at 0x6A) answers the question "does the bus work" independently of
   whatever you are debugging. Check it first, every time.

5. **Only access a part the way its datasheet documents.** MAXM86161
   Figures 12-15 define the legal transactions. A bare address read is not
   among them. Do not invent access patterns for scanning convenience.

6. **Localize by measurement before any irreversible action.** Correlation
   with touching, heating, or soldering is not localization. If the fault
   has not been narrowed to a specific net or pin by a measurement, do not
   apply heat.

7. **Test the cheapest hypothesis first, especially someone else's.**
   Reflashing firmware costs three seconds. Reflowing an LGA costs a board.

---

## Concrete changes made

* `maxm86161_probe()` now reports `bare` / `ptr_write` / `split` / `restart`
  results separately with real errnos, and takes whichever access pattern
  works.
* `bus_probe()` polls only the four addresses this board should have, using
  a register-pointer read rather than a bare read.
* The BQ25120A at 0x6A is included in that poll as the bus canary.
