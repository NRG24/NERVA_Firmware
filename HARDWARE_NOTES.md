# Notes for the next board spin

Written after bring-up on the first spin failed at the SWD link. Every claim
below is from the datasheets in this repo's `datasheets/` folder.

---

## Priority summary for the next spin

Items are detailed below; this is the order to work in. Everything here is
confirmed on hardware.

### P0 -- fix these or the next board repeats this one's history

| # | Change | Why |
|---|---|---|
| 13 | **Stiffener + anchored battery connector** | The board resets when flexed. Every other measurement is untrustworthy until this is fixed. |
| 1 | **SM03B-SRSS-TB debug connector** | Hand-tacked SWD wires tore off twice on spin 1 and dominated this session too. |
| 11 | **Clearance around the skin electrodes** | Metal across them crowbars the supply. On a worn ring, sweat or a second ring could do the same. |
| 10 | **Test point on ACC_INT** | The dead INT1 joint was only diagnosable because firmware could interrogate the pin. Cheap, and it turns an all-day hunt into a one-minute check. |

### P1 -- each of these cost hours of debugging

| # | Change | Why |
|---|---|---|
| 6 | **100k pull-up on BOOST_EN** | VLED collapses on every reset, so the PPG restarts from scratch after each flash. |
| 4 | **Bring RESET_N out** | No connect-under-reset recovery path today. |
| 2 | **Consider 3.0 V rail** | Optional, see item 2. Buys SWD safety, OPA2333 margin and ~2x GSR span, at the cost of a different PMIC variant. |

### Explicitly NOT needed -- decided 2026-08-24

| # | Change | Verdict |
|---|---|---|
| 9 | Route TS out | **Not needed.** The battery connector (3.1) has two pins, BAT+ and GND -- there is no thermistor in the pack, so TS could never have provided real protection. Clearing TS_EN in firmware loses nothing. TS is ball C3, dead centre of the 5x5 DSBGA, and is not worth a via-in-pad for zero benefit. |
| 7 | Pull-up on CD | **Actively harmful.** Table 1: CD high with a charger attached *disables* charging. A hardware pull-up would make "not charging" the power-on default until firmware intervenes. The internal 900 kOhm pull-down gives the safer default. Keep firmware driving CD. |
| 6 | Pull-up on BOOST_EN | **Fit as DNP.** It is a bring-up convenience, not a fix. With it fitted the boost runs whenever the MCU is not actively holding the pin low -- including every reset and any firmware hang -- which on a ring battery is a real drain. Populate on prototypes, omit on production. |

### P2 -- quality of life, do if there is room

| # | Change | Why |
|---|---|---|
| 3 | MAXM86161 INTB to a GPIO | Ends FIFO polling. |
| 9 | BQ25120A INT (D2) to a GPIO | Charger events without polling. |
| 12 | R5 as 0402 rather than 0201 | The **value (91k) is correct** -- it just needs to be readable. Not being able to read it sent a whole investigation down a wrong path. |
| 12 | ESD protection on SKIN_SENSE | A skin electrode currently runs straight into an op-amp input. |
| 8 | Confirm the IMU part in the BOM | The board is an LSM6DSV16BX; the filed datasheet is the LSM6DSV. |

---

## 1. Add a real debug connector

`SWDCLK` and `SWDIO` are single-node nets in the netlist -- they terminate at
U5.41 and U5.42 and go nowhere else. That means the only way to attach a
debugger is hand-tacked wire onto pads, which is what eventually tore both
connections off the board.

Use the **SM03B-SRSS-TB** 3-pin JST-SH. It is the exact connector on the
Raspberry Pi Debug Probe (J2/J3 on RP-008194-DS), so the supplied cable mates
directly with no adapter. Pin order is fixed by the Raspberry Pi debug
connector spec (RP-003139-SP):

| Pin | Signal |
|-----|--------|
| 1   | SWCLK  |
| 2   | GND    |
| 3   | SWDIO  |

The spec also asks for **100 Ohm source termination at the target end** on
both signals, placed close to the module pins.

A 1.27 mm 10-pin Cortex Debug header works too and adds RESET and VTref, at
the cost of more board area.

## 2. The 1.8 V rail is a choice, not a constraint

This is what made debugging painful. The ANNA-B402's VCC is on the 1V8 rail,
so its SWD pins have an absolute maximum of VDD + 0.3 V = 2.1 V. Common
3.3 V debug probes -- including the Raspberry Pi one, which has no VTref
sensing and no adjustable target voltage -- are out of spec against that, and
need series resistors or a translator just to attach safely.

**Nothing in the design requires 1.8 V:**

| Part | Digital supply / IO tolerance | Source |
|------|-------------------------------|--------|
| MAXM86161 (U3) | SCL, SDA, INTB, GPIO, LDO_EN abs max **-0.3 V to +6.0 V**; VIH min **1.4 V** | datasheet p.6, p.9 |
| LSM6DSV (U4) | Vdd_IO **1.08 V to 3.6 V** | datasheet p.12 |
| ANNA-B402 (U5) | VCC **1.7 V to 3.6 V** | datasheet p.19 |

The MAXM86161's I2C pins being 6 V tolerant with a 1.4 V input-high
threshold is the key fact: the bus does **not** have to be 1.8 V. A 3.0 V or
3.3 V rail satisfies every part on the board.

The 1.8 V comes from the BQ25120A. Its orderable table (datasheet p.3) lists
**DEFAULT SYS = 1.8 V** for the BQ25120A part number. Options:

* Pick a family variant whose factory default SYS is 3.0 V
* Give the MCU its own regulator instead of running it off SYS
* Keep 1.8 V and put a level translator on the debug header

Note that raising SYS from firmware over I2C does **not** solve the debug
problem. A factory-blank module never runs firmware, so the rail is at its
power-on default exactly when you need the debugger most.

## 3. Route MAXM86161 INTB to a GPIO

U3.14 (INTB, open-drain interrupt) is unconnected. With no interrupt line the
firmware has to poll `FIFO_DATA_COUNT` on a timer, which burns current and
adds latency. One GPIO turns the PPG read path interrupt-driven.

U3.13 (GPIO) is also unconnected -- worth bringing out if sample-sync or
one-shot triggering is ever wanted.

## 4. Bring RESET_N out

Module pin 12 (RESET_N, P0.18) is not connected. Without it there is no way
to do a connect-under-reset, which is the standard recovery path when an
application misbehaves or APPROTECT locks the part. A test point is enough.

## 5. Mechanical

Whatever the debug interface ends up being, anchor it. Both failures on the
first spin were lifted pads from wire tension, not bad solder. Vias survive
handling far better than surface pads, and a connector survives better still.

## 6. Pull BOOST_EN up

BOOST_EN (U1.C1, module pin 15 / P0.12) is driven only by the MCU. On any
reset -- including every debugger flash -- GPIOs revert to inputs, BOOST_EN
goes low, and VLED collapses. The MAXM86161 loses its **only** supply while
SDA and SCL stay pulled to 1V8 through R1/R2.

That makes the whole debug loop fragile: you cannot reflash without
power-cycling the sensor, and a device losing power with its bus lines still
held high can latch into a state where it holds SDA low.

Add a pull-up (100k) from BOOST_EN to the 1V8 rail so VLED stays up across
MCU resets. The GPIO can still pull it low deliberately when the firmware
wants the boost off.

This one change would have made bring-up dramatically less painful -- it is
the difference between "reflash and keep observing" and "reflash and start
the sensor from scratch every time".

## 7. CD needs a pull-up, or firmware must drive it

The BQ25120A's CD pin (U6.E2 -> module pin 13 -> P1.09) has a **900 kOhm
internal pull-down**. On battery only, CD low puts the PMIC in High
Impedance mode, and in Hi-Z **its I2C interface is switched off**
(datasheet 9.3.2, Table 1).

SYS keeps running from BAT in Hi-Z, so the board appears completely healthy
while the PMIC is unreachable. On board 3 this looked exactly like a dead
part until the datasheet explained it.

Options:

* Firmware drives CD high at boot (done -- see `selftest.c`), or
* Add a pull-up to 1V8 so the PMIC comes up in Active Battery mode even
  before firmware runs

Either way, note the trade in Table 1: **CD high disables charging when a
charger is attached.** Production firmware needs to sense VIN and drive CD
low to charge, high to talk to the PMIC. Worth bringing the charger-present
signal to a GPIO on the next spin so that decision can actually be made.

## 8. Check the IMU part number

The board answers `WHO_AM_I = 0x71`, which is an **LSM6DSV16BX**. The
datasheet filed with this project is the LSM6DSV, whose WHO_AM_I is fixed at
0x70. Basic accelerometer access is compatible; the embedded-function
register maps are not. Confirm which part the BOM actually calls for.

## 9. TS is unconnected, and it blocks charging

BQ25120A ball **C3 (TS, battery pack NTC monitor) is not connected** on this
board. The datasheet pin description says to "connect TS to the center tap
of a resistor divider from VIN".

Floating, it reads as a battery outside its safe temperature window. Register
0x02 comes up as **0xA8**: TS_EN = 1, TS_FAULT = 01, which the datasheet
defines as "TS temp < TCOLD or TS temp > THOT (Charging suspended)". The
status register then reports STAT = 11 (FAULT) while the *fault* register
0x01 reads 0x00, because TS faults are reported separately -- an easy
combination to misread as a phantom fault.

**Net effect: the board cannot charge as built.**

Firmware works around it by clearing TS_EN (bit 7 of 0x02), which is the
correct action when no thermistor is fitted. But that also throws away
battery temperature protection, which is a real safety function on a LiPo.

**Next spin:** fit the TS divider per the datasheet, ideally with a real NTC
in the battery pack. If the cell genuinely has no thermistor, fit a fixed
divider that parks TS in the valid window so the charger is happy without
firmware intervention.

Also unconnected on U6, all deliberate-looking but worth confirming:

| Ball | Signal | Consequence |
|---|---|---|
| C3 | TS | **blocks charging, see above** |
| C5 | LS/LDO | load switch output unused |
| D2 | INT | no charger interrupt to the MCU; status must be polled |
| D3 | RESET | no reset output |
| D4 | /PG | no power-good indication |
| E1 | MR | no pushbutton / manual reset |

Routing **INT (D2)** to a GPIO on the next spin would let the MCU learn about
charge start/stop and faults without polling.

## 10. ACC_INT is held low on board 3

`ACC_INT` (U4.4 INT1 -> module pad 36 -> P0.16) is stuck at 0 V and no
interrupt can ever reach the MCU. Localised on 2026-08-23 entirely from
firmware: the wake config reads back correct, the IMU detects motion on all
three axes (`WAKE_UP_SRC` shows `WU_IA`), but the pin will not go high even
when accelerometer data-ready is routed to it at 120 Hz, and it stays low
with the IMU in open-drain mode and an MCU pull-up on the net. Full evidence
chain in `HANDOFF.md` section 6.

Check for a solder bridge to GND on that net first. Beyond this board, the
lesson for the next spin is that **there is no way to tell a dead interrupt
line from a mis-set register without a test point** — bring ACC_INT out to
one, alongside the debug connector in item 1.

## 11. The skin electrodes can be bridged to a power rail

Found 2026-08-24 while trying to bridge U2/U8 to test the GSR front end.
Laying metal across the two electrode pads collapses the board: the light
goes out and comes back when the metal is removed.

That is the BQ25120A's PMID/SYS short-circuit protection doing its job
(datasheet 9.3.2/9.3.3, tDGL_SC = 250 us, hiccup-retry until the short
clears). Nothing was damaged.

**But the GSR node cannot possibly trip it.** SKIN_SENSE is driven from the
U7B output through R5, so the available current is microamps -- orders of
magnitude below the short-circuit limit. For the protection to fire, the
conductive object must be reaching a power net (SYS / PMID / 1V8), not just
the two electrodes.

**Why this matters beyond the bench:** U2 and U8 are exposed pads on the
skin-contact surface of a ring. Anything conductive that spans them and
whatever rail is adjacent will crowbar the supply -- and on a worn device
that includes sweat, a second ring, keys in a pocket, or a charging jig
that is slightly misaligned. Today it was tweezers. The PMIC caught it,
but that is protection, not design margin.

**Next spin:**

* Check what is routed or exposed next to U2 and U8, and pull any rail or
  via out from under the electrode keepout.
* Give the electrode pads a clearance ring with nothing else in it.
* Consider series protection on SKIN_SENSE so a shorted electrode is a
  measurement fault rather than a supply event.

Until that is fixed, do not bridge the electrodes with metal on this board.
Use the powered-down continuity checks instead.

## 12. GSR design review, and how to size R5

Done 2026-08-24 against the netlist and the OPA333/OPA2333 datasheet, after
the front end measured healthy but showed no response to a finger.

### The topology is right

U7 is an OPA2333 in SON-8 with the exposed pad on GND. The netlist maps
exactly onto the datasheet pinout (OUT A 1, -IN A 2, +IN A 3, V- 4, +IN B 5,
-IN B 6, OUT B 7, V+ 8):

* **U7A** buffers the R3/R4 divider off GSR_PWR to make `V_REF_05`.
  Measured 0.500 V with GSR_PWR at 1.8 V, so R4/(R3+R4) ~ 0.28.
* **U7B** is a transimpedance stage: `+IN` at V_REF, the skin path from U8
  to GND at U2, and R5 as feedback with C4 across it for bandwidth.
* **R6/C10** low-pass the output into AIN1.

This is a **constant-voltage exosomatic EDA front end**, and the choices are
deliberate and correct:

* 0.5 V across the skin is the EDA convention.
* Transimpedance makes the output proportional to **conductance**, which is
  the unit EDA is reported in (uS).
* The OPA2333 is specified at 10 uV offset and 0.05 uV/degC drift. At the
  microamp currents involved, an ordinary op-amp's offset would swamp the
  signal. This is the right part, not a generic one.

### Transfer function

    V_OUT = V_REF x (1 + R5 / R_skin) = 0.5 x (1 + R5 / R_skin)

Rest is 0.5 V (electrodes open) and the OPA2333 swings to within ~50 mV of
V+, so the usable span is 0.5 V to ~1.75 V. Saturation at R_skin = 0.4 x R5.

### R5 is 91 kOhm and correctly sized -- NOT the fault

**Corrected 2026-08-24.** An earlier version of this note derived R5 as
~12.7 kOhm from a bench measurement and concluded it was ten times too
small. That was wrong. R5 is **91 kOhm** per the BOM.

The error is worth recording because it is easy to repeat: R5 was derived by
holding a known 100 kOhm across the electrodes and reading 563 mV, via
`R5 = R_known x (V_OUT/V_REF - 1)`. That formula assumes the known resistor
is the *only* thing across the electrodes. It was hand-held, so contact
resistance -- and most likely the skin of the person holding it -- sat in
series. Running it the right way round with the true R5:

    R_effective = 91k / 0.1268 = 718 kOhm

So the electrodes actually saw ~718 kOhm, not 100 kOhm. **The measurement
silently folded the contact impedance into R5.** Any future attempt to
characterise this stage needs the reference resistor soldered or firmly
clamped, with no human in the loop.

91 kOhm gives a good EDA range:

| R_skin | Conductance | V_OUT |
|---|---|---|
| 1 MOhm | 1 uS | 546 mV |
| 200 kOhm | 5 uS | 727 mV |
| 100 kOhm | 10 uS | 955 mV |
| 50 kOhm | 20 uS | 1410 mV |
| 36 kOhm | 28 uS | 1764 mV, saturated |

That covers 1-28 uS over the usable span. **Do not change R5.**

### What the finger test actually showed

Fingers moved the output less than 20 mV, which with R5 = 91 kOhm implies
**over 2 MOhm** of skin plus contact impedance. That is dry fingertips
pressed lightly onto small gold pads, not a circuit fault.

Worn as a ring -- constant pressure, trapped moisture, larger contact area --
the impedance should drop by an order of magnitude or more. **The GSR front
end has not been shown to be faulty at any point.** It should be re-tested
on a worn ring before any component is changed.

### For the next board

1. **Keep R5 at 91 kOhm** -- the value is right. But fit it as an **0402 or
   larger**. It is currently an unmarked 0201, and not being able to read
   the one component that sets sensitivity is what sent this whole
   investigation down a wrong path.
2. **Give the op-amp supply margin.** OPA2333's minimum is 1.8 V and the
   rail *is* 1.8 V -- zero margin, and it is fed through a GPIO. Item 2
   already argues for a 3.0 V rail; this is another reason. Note that
   raising GSR_PWR means **re-scaling R3/R4** to keep the 0.5 V bias, since
   V_REF tracks the supply.
3. **Protect SKIN_SENSE.** A skin-contact electrode currently runs straight
   into an op-amp input with no series resistance and no clamp. Human-body
   ESD is kilovolts. Add series R plus a clamp to the rails.
4. **Expect DC polarisation drift.** 0.5 V DC through gold/ENIG electrodes
   polarises over minutes. Ag/AgCl is the textbook answer and is not
   practical on a ring, so either accept the drift, or reverse the bias
   polarity periodically and difference the readings.
5. **Consider using the full ADC span.** The output only occupies 0.5-1.75 V
   of an 1800 mV range. Biasing V_REF lower, or referencing the ADC
   differently, would recover roughly a third of the resolution.
6. **Powering the front end from a GPIO costs settle time.** Measured
   ~0.5-1 s for the output to reach V_REF from cold. At 17 uA for the
   op-amp plus ~10 uA in the divider, leaving it powered continuously is
   cheap and removes the transient entirely.

## 13. The battery connection resets the MCU when the board flexes

Confirmed 2026-08-24 by measurement, not inference. Pressing or flexing the
PCB makes the board go dark for several seconds. RTT shows why:

```
*** Booting nRF Connect SDK ***
[00:00:00.002,502] main: ring firmware starting
*** Booting nRF Connect SDK ***          <- second boot
[00:00:00.002,532] main: ring firmware starting
[00:00:03.522,705] BENCH_POWER_LED on
```

**The uptime counter returns to zero**, so this is a full MCU reset, not a
firmware state change. The board was on battery at the time (faults 0x40 =
VIN_UV, no charger), so the interruption is in the battery path: the JST or
its pads momentarily open under flex.

The apparent "five seconds of darkness" is mostly firmware boot time before
the indicator relights, not the length of the power cut.

**Next spin:**

* **Stiffener under the battery pads and the connector.** This is a flex
  assembly with a LiPo hanging off it and nothing resisting bending at the
  joint. That is the whole failure.
* Anchor the connector mechanically as well as electrically -- same argument
  as item 5, which was written after the SWD pads tore off for the same
  reason.
* Consider pads with a larger footprint and via-anchoring rather than
  surface-only, so a flexed joint fails gracefully instead of opening.
* If a JST is kept, choose one with through-hole or staked retention rather
  than surface-mount tabs alone.

This has cost real time twice now: it presented first as "the board is dead
when unplugged" (it was the JST, not the cell) and later as random SWD
dropouts. **A board that reboots when you touch it makes every other
measurement untrustworthy** -- it should be the first thing fixed.

## 10. U7 runs at its absolute minimum supply, from a GPIO

The GSR op-amp's V+ (U7 pin 8) is tied to **GSR_PWR**, which is module pin
14 = **P0.11**, an MCU GPIO on the 1V8 rail.

The OPA333/OPA2333 supply range is **1.8 V to 5.5 V** (datasheet p.7). So
the part sits exactly at its minimum, powered by a pin that is not a rail,
with no allowance for regulator tolerance, GPIO drop, or transient sag.

This is the leading explanation for the GSR front end being dead: the
output reads a hard 0 V rather than idling at V_REF as it should.

Compounding it, the V_REF divider as ordered (R3 = 180k, R4 = 470k) puts
V_REF at **1.30 V on a 1.8 V rail** -- 0.5 V of total output swing. Swapping
them gives 0.50 V and leaves 1.3 V of headroom, which is what the
transimpedance stage needs.

**Next spin:**

* Power U7 from a real rail with margin, not a GPIO. If the GSR front end
  must be power-gated, use a load switch rather than driving the supply pin
  directly from P0.11.
* Consider running it from the 5 V boost instead of 1V8 -- the OPA2333 is
  happy to 5.5 V and the extra headroom makes the whole measurement easier.
* Re-derive R3/R4 for whatever rail you settle on, targeting V_REF at
  roughly a quarter of the supply so the output has room to swing up.
