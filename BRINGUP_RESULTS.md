# Bring-up results, board 3

First board where every populated subsystem was exercised. Recorded from
RTT logs, not from inference.

## Subsystem status

| Subsystem | Part | Addr / pin | Result |
|---|---|---|---|
| PPG | MAXM86161 (U3) | I2C 0x62 | **PASS** - PART_ID 0x36, REV 0x0a, green 530nm streaming at 100 sps |
| IMU | LSM6DSV**16BX** (U4) | I2C 0x6b | **PASS** - WHO_AM_I 0x71, accel 981 / -266 / -37 mg |
| PMIC | BQ25120A (U6) | I2C 0x6a | **PASS** - status 0x03 (ready), faults 0x40 (VIN_UV, correct: no charger) |
| Boost | TPS61240 (U1) | BOOST_EN P0.12 | **PASS** - 5 V present, VLED feeding U3 |
| Charge detect | CD | P1.09 | **PASS** - readable, and now driven |
| GSR | OPA2333 (U7) + electrodes | AIN1 / P0.03 | **FAIL** - hard 0 mV; should idle at V_REF ~900 mV. Fault is the U7 front end, see Discovery 4 |
| BLE | ANNA-B402 (U5) | - | not yet exercised |

Confirmed pin mapping, all verified in hardware:

| Signal | Module pin | nRF52833 |
|---|---|---|
| SDA | 10 | P0.20 |
| SCL | 11 | P0.14 |
| CD | 13 | P1.09 |
| GSR_PWR | 14 | P0.11 |
| BOOST_EN | 15 | P0.12 |
| GSR_ADC | 19 | P0.03 = AIN1 |
| ACC_INT | 36 | P0.16 |

## Discovery 1: the PMIC disables its own I2C on battery

**The single most surprising finding of this run.**

The BQ25120A did not answer at 0x6a. Every access pattern failed with EIO
-- `restart=-5 ptr_write=-5 split=-1 reg_write=-5` -- so it was not a
protocol quirk, the part simply was not ACKing.

Cause, from the datasheet (section 9.3.2 and Table 1, p.19):

> "The CD pin is used to put the device in a high-impedance mode when
> battery is present and VIN < VUVLO. Drive CD high to enable the device and
> enter active battery operation when VIN is not valid."

> "the I2C interface is disabled if only battery is present. To resume I2C,
> the CD pin must be toggled."

| CD | VIN < VUVLO (battery only) | VIN > VUVLO (charger in) |
|---|---|---|
| L | **Hi-Z, I2C disabled** | Charge enabled |
| H | **Active Battery, I2C alive** | Charge disabled |

CD has a **900 kOhm internal pull-down**. Firmware left P1.09 as an input,
so CD floated low, so the PMIC sat in High Impedance mode. SYS still runs
from BAT in Hi-Z -- which is why the board worked perfectly and nothing
looked wrong -- but the I2C block is powered down.

Driving P1.09 high fixed it immediately.

**Consequences for firmware:**

* CD **must** be driven high for battery operation, or the PMIC is
  unreachable and the board silently runs in its lowest-power state.
* CD high with a charger attached **disables charging**. Production firmware
  therefore needs a real state machine: sense VIN, drive CD low to charge,
  high to talk to the PMIC. This is not a set-and-forget pin.
* Anything that reads battery voltage or charge state over I2C has to
  sequence CD first.

## Discovery 2: the IMU is not the part in the datasheet folder

`WHO_AM_I` returned **0x71**. The LSM6DSV datasheet in `datasheets/` states
the value is "fixed at 70h" (section 9.12). 0x71 is the **LSM6DSV16BX**, a
different member of the family.

The two are close enough that CTRL1 (0x10) and the accelerometer output
registers (0x28) behave identically, so basic reads work. They are **not**
identical parts -- the 16BX adds audio-band accelerometer features and its
embedded-function register maps differ.

**Action:** check the BOM and what was actually placed. Any driver work
beyond basic accel/gyro reads needs the LSM6DSV16BX datasheet, not the one
currently in this folder.

## Discovery 3: VIN_UV is not a fault here

`faults = 0x40` sets bit 6, VIN_UV. That is correct behaviour with no
charger connected, not a defect. Per the register description it "is set
when the input falls below VSLP" and "shows only one time. Once read,
VIN_UV clears."

Do not treat this bit as an error in production firmware without first
checking whether a charger is expected to be present.

## Discovery 4: GSR IS broken, and 0 mV was the evidence all along

**Superseded 2026-08-23.** The original conclusion below was wrong, and it
sent the next session after the wrong thing.

> ~~Reads 0 mV with GSR_PWR asserted. U2 and U8 are skin-contact electrodes,
> so an open circuit legitimately produces no signal. This test result
> carries no information until the electrodes are bridged.~~

Reading the netlist rather than assuming: U7B is a transimpedance stage with
`IN+` held at `V_REF_05` (the R3/R4 divider off GSR_PWR, buffered by U7A),
the skin path running from U8 to GND at U2, and R5 as feedback. So

    V_OUT_GSR = V_REF x (1 + R5 / R_skin)

**With the electrodes open that rests at V_REF -- about half of GSR_PWR, so
~900 mV off the 1V8 rail. Not 0 mV.** An open circuit does not produce zero
here; it produces the reference. So 0 mV was never uninformative, it was the
fault.

Worse, if `V_REF` is 0 then `V_OUT_GSR` is 0 **regardless of skin
resistance** -- bridging the electrodes could never have shown anything.
That test would have failed no matter what.

Measured, all from firmware:

| Probe | Result | Meaning |
|---|---|---|
| Settle sweep, 1 ms to 2 s | `0` at every point | not a settling problem; the fixed 10 ms wait was never the issue |
| `GSR_PWR` (P0.11) driven | `hi=1 lo=0` | the MCU drives the rail correctly |
| `GSR_ADC` (P0.03) pull-up / pull-down | `0 / 0` | node held low |
| `GSR_ADC` forced high | reads `1` | **not** a short -- held through series R6 |

So the ADC net is clean and the op-amp output is genuinely at 0 V. The fault
is upstream of R6, past anything the MCU can reach.

**Next step is a meter, in this order, with the board running** (the live
monitor in `selftest.c` holds GSR_PWR high continuously after boot, so these
are all probeable live):

1. **U7.8** -- should be ~1.8 V. If 0, the GSR_PWR net is open between
   U5.14 and U7.8, and U7 is simply unpowered.
2. **U7.3 / VREF_DIV** -- should be ~0.9 V. If 0 while U7.8 is good, R3 is
   open or R4 is shorted.
3. **U7.1 / V_REF_05** -- should equal U7.3. If U7.3 is right and this is 0,
   op-amp A is dead.
4. **U7.7 / V_OUT_GSR** -- should equal V_REF with the electrodes open.

## PPG signal, for reference

Green LED1 at PA 0x80 (~15.4 mA), 100 sps, ADC range 16 uA, TINT 117.3 us:

* baseline around 3050 counts with nothing on the sensor
* 1-second peak-to-peak around 370 counts, AC amplitude around 85

The HR estimator reports values in the 113-117 bpm range and they vary
between windows, which is more plausible than the earlier rock-steady
readings. **This has not been validated against a reference monitor** -- do
not trust the number until it has been.


## Discovery 5: the board cannot charge as built (TS unconnected)

With a charger attached, the PMIC reported `STAT = 11 (FAULT)` while the
fault register 0x01 read `0x00` -- no VIN_OV, VIN_UV, BAT_UVLO or BAT_OCP.

The cause is in a different register. 0x02 read **0xA8**:

* `TS_EN = 1` (TS monitoring on)
* `TS_FAULT = 01` = "TS temp < TCOLD or TS temp > THOT (Charging suspended)"

BQ25120A ball **C3 (TS) is unconnected** in the netlist. Floating, it reads
as a battery outside its safe temperature range, so the charger correctly
refuses to charge.

Charge current itself was confirmed correct from silicon: `0x03 = 0x14` ->
ICHRG_RANGE = 0 (5-35 mA, 1 mA steps), code 5, so **10 mA**, CE = 0
(enabled), not high-Z. That matches the BQ25120A default in the orderable
table, and ISET (C1) is tied to GND so the internal default applies.

Firmware now clears TS_EN when it sees the fault. That trades away battery
temperature protection -- acceptable on the bench, not in a product. See
HARDWARE_NOTES.md item 9.

## Still to do

* GSR with electrodes bridged
* BLE bring-up (advertising, and HR over the standard Heart Rate Service)
* ACC_INT (P0.16) is wired but unused -- the IMU is polled, not
  interrupt-driven
* MAXM86161 INTB is not routed at all, so the PPG FIFO is polled. See
  HARDWARE_NOTES.md item 3.
