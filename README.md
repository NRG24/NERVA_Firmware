# Ring board bring-up firmware — MAXM86161 PPG

> **New session? Read [HANDOFF.md](HANDOFF.md) first.** It covers current
> state, the traps that cost days, and the one open issue.

Barebones nRF Connect SDK (Zephyr) app for the board in
`Netlist_New_Switch_2_2026-08-21.asc`. It does one thing: bring the
MAXM86161 up and stream green-channel PPG samples out over RTT.

## What the netlist says

| Ref | Part | Notes |
|-----|------|-------|
| U5 | u-blox ANNA-B402 (nRF52833) | VCC on the **1V8** rail |
| U3 | MAXM86161 | I2C 0x62, VLED from the 5V rail |
| U4 | LSM6DSV | same bus, 0x6B (SA0 → 1V8), INT1 → P0.16 |
| U6 | BQ25120A | charger, CD → P1.09 |
| U1 | TPS61240 | 5V boost, EN → P0.12 |
| U7 | OPA2333 | GSR front end, PWR → P0.11, ADC → P0.03 (AIN1) |

Signal → ANNA pad → nRF52833 port:

```
SDA        U5.10  GPIO_10  P0.20
SCL        U5.11  GPIO_11  P0.14
CD         U5.13  GPIO_13  P1.09
GSR_PWR    U5.14  GPIO_14  P0.11
BOOST_EN   U5.15  GPIO_15  P0.12
GSR_ADC    U5.19  GPIO_19  P0.03  (AIN1)
ACC_INT    U5.36  GPIO_36  P0.16
```

## Four board facts that shape this firmware

1. **BOOST_EN must be driven high first.** MAXM86161 VLED (U3.5) comes from
   the TPS61240's 5V rail, and nothing turns that boost on but firmware. The
   part's digital side runs off 1V8, so it will ID correctly over I2C with
   VLED dead — the symptom is a perfectly flat PPG signal.
2. **INTB is not connected.** U3.14 goes nowhere in the netlist, so there is
   no data-ready line. The firmware polls `FIFO_DATA_COUNT` (0x07) every
   20 ms instead. Fine at 100 sps — the FIFO holds 128 samples, 1.28 s of
   headroom.
3. **LDO_EN is strapped to 1V8.** U3.3 is tied high, so the part is always
   powered and there is no hardware shutdown. The soft `RESET` bit in
   `SYSTEM_CONTROL` (0x0D) is the only way to a known state.
4. **No 32.768 kHz crystal.** Module pins 17/18 (XL1/XL2) are unconnected,
   so LFCLK runs from the internal RC. That is set in the board defconfig.
   It is good enough for this, but BLE connections will need the RC
   calibration that's enabled there, and precision timing will suffer.

There is also **no UART** anywhere on the board. SWD (U5.41/42) is the only
way out, so the console and log backend are both SEGGER RTT.

## SWD logic levels

The ANNA-B402's VCC (U5.9) sits on the **1V8** rail, so its SWD pins are in a
1.8 V domain and the nRF52833's absolute maximum on any digital pin is
VDD + 0.3 V = **2.1 V**.

From the Raspberry Pi Debug Probe schematic (RP-008194-DS), the two
directions are not symmetric:

* **Target -> probe is fine.** The probe reads through a 74AUP1T17GW
  Schmitt-trigger translating buffer running off 3V3. That part exists
  precisely to accept a sub-VCC input swing, so a 1.8 V high from the
  nRF52833 reads cleanly.
* **Probe -> target is out of spec.** SWCLK and SWDIO are driven straight
  from RP2040 GPIO at 3.3 V through only 100 Ohm (R4/R5/R6/R11/R12/R13).
  Into a 1.8 V pin that is ~10 mA through the ESD clamp, continuously while
  the line is high, backfed into a rail the BQ25120A buck cannot sink.

There is no VTref sense and no adjustable target voltage anywhere on the
probe.

**Fix: 1-2 kOhm in series with SWCLK and SWDIO**, at the probe end of the
cable. With 2 kOhm (2.1 kOhm total, including the probe's own 100 Ohm) the
clamp current drops from ~10 mA to ~0.5 mA, which the ESD diodes absorb
without lifting the 1V8 rail. The target still sees a valid high: the pin
sits at the clamp voltage of ~2.3 V, well above the nRF52's VIH of
0.7 x VDD = 1.26 V. The read direction is unaffected because the 74AUP1T17
input is high impedance.

The cost is edge rate. At 2.1 kOhm into roughly 30 pF of cable, connector
and pin capacitance, tau is about 63 ns, so an edge settles in ~190 ns
against a 500 ns half-period at 1 MHz -- about 2.6x margin. Fine at 1 MHz,
marginal above 2 MHz. If connects are flaky, halve it:

```powershell
.uild.ps1 -Rtt -Freq 500000
```

Do not connect the probe to an unpowered board. With 1V8 down, the drive
current backfeeds the whole rail through the ESD diodes and partially powers
the board through its protection structures.
## Bring-up: first connection to a fresh module

Done once per board. Verified working on the second board.

**1. Identify the pads electrically** -- board powered, probe fully
disconnected. The nRF52833 identifies its own pins:

| Pad reads at rest | Is |
|---|---|
| 1.8 V (internal pull-up) | SWDIO |
| 0 V (internal pull-down)  | SWDCLK |

Cross-check against the layout. Do not infer orientation any other way.

**2. Harness** -- 3-pin JST-SH to the probe's **D** socket (not U):

| JST pin | Through | To |
|---|---|---|
| 1 | 2 kOhm | SWDCLK pad (the 0 V one) |
| 2 | direct | GND |
| 3 | 2 kOhm | SWDIO pad (the 1.8 V one) |

Verify on the bench before connecting: pin1-pin3 **open** (a bridge here
puts both pads at VDD/2 = 0.9 V and poisons every reading), each signal pin
~2 kOhm to its board end, pin 2 ~0 Ohm.

**3. Power the board first, then plug in the probe.** Never probe-first into
an unpowered board.

**4. Clear APPROTECT.** ANNA-B402 modules ship **locked**. Until this is
done, every access fails and it looks exactly like a wiring fault:

```bash
pyocd erase -t nrf52833 -f 1000000 --chip
```

The unlock persists -- this is a once-per-module step, not once per session.

**5. Confirm.** A good link looks like:

```
Part number:  NRF52833
DAP IDCODE:   0x2ba01477
 *0  None  Cortex-M4
```

### Reading the failure modes

pyOCD's two errors decode to opposite electrical states, which is useful:

| Error | ACK bits | Means |
|---|---|---|
| `No ACK` | 7 = all ones | Line idle high -- target never responds. Clock not arriving, harness unplugged, or part locked. |
| `Unexpected ACK '0'` | 0 = all zeros | Line held low -- shorted, or the probe is reading a pull-down pin. |

Frequency-independent failure rules out edge rate and the series resistors;
don't keep lowering the clock, look at the link instead.

## Build and flash

Toolchain used: **nRF Connect SDK v3.4.0** (Zephyr 4.4.0), installed to
`C:
cs` with `nrfutil sdk-manager`. The toolchain bundle brings its own
CMake, Ninja, Python, west and ARM GCC, so nothing else needs to be on PATH.

```powershell
.uild.ps1
```

```powershell
.uild.ps1 -Rtt
```

`-Flash` builds and flashes, `-Rtt` builds, flashes, and attaches the RTT
viewer. Under the hood that is:

```bash
west build -b ring_anna/nrf52833 -p always -d build . -- -DBOARD_ROOT=<app dir>
```

run with west's cwd inside `C:
cs3.4.0`, because the app lives outside
the workspace. `BOARD_ROOT` has to be explicit: in NCS 3.x sysbuild is the
top-level CMake source, so the app directory is not picked up as a board
root on its own and the board lookup fails with "No board named 'ring_anna'".

Flashing and RTT both go through pyOCD, since the Raspberry Pi Debug Probe
enumerates as CMSIS-DAP. nRF52833 is a builtin pyOCD target, so no CMSIS
pack install is needed.

```bash
pyocd flash -t nrf52833 -f 1000000 build/ring-fw/zephyr/zephyr.hex
```

```bash
pyocd rtt -t nrf52833 -f 1000000
```

Check the probe sees the part before flashing anything:

```bash
pyocd commander -t nrf52833 -f 1000000 -c "reg" -c "exit"
```

If the module comes locked (APPROTECT), unlock it first:

```bash
pyocd erase -t nrf52833 --chip
```

### Current build

```
FLASH:  38296 B   512 KB    7.30%
RAM:    14144 B   128 KB   10.79%
```

Clean, no warnings.

## What you should see

```
[00:00:00.001,000] <inf> main: ring board bring-up
[00:00:00.002,000] <inf> main: 5V boost enabled (BOOST_EN = P0.12)
[00:00:00.055,000] <inf> main: scanning I2C bus...
[00:00:00.058,000] <inf> main:   ACK at 0x62
[00:00:00.059,000] <inf> main:   ACK at 0x6b
[00:00:00.061,000] <inf> maxm86161: MAXM86161 found at 0x62 (PART_ID 0x36, REV 0x..)
[00:00:00.075,000] <inf> maxm86161: PPG running: LED1 green, PA 0x80 (~15.36 mA), 100 sps
[00:00:00.095,000] <inf> main: ppg   12043 |
[00:00:00.195,000] <inf> main: ppg  198332 |############
...
[00:00:01.095,000] <inf> main: --- 1 s: min 196104 max 201887 p-p 5783 ---
```

`PART_ID` must read **0x36**. With nothing on the sensor the counts sit low
and flat; put a finger on the optical window and they jump by an order of
magnitude and start breathing at heart rate. The 1-second min/max line is
the quick sanity check — a real pulse shows up as a stable peak-to-peak of
a few thousand counts, not noise.

## Configuration in `maxm86161_start_ppg()`

| Setting | Value | Register |
|---------|-------|----------|
| Exposure sequence | LED1 (green, 530 nm) only | 0x20 = 0x01 |
| Sample rate | ~100 sps, no averaging | 0x12 = 0x18 |
| Integration time | 117.3 µs (best SNR) | 0x11[1:0] = 3 |
| ADC full scale | 16 µA | 0x11[3:2] = 2 |
| LED current | ~15.4 mA (0x80 in the 31 mA range) | 0x23 / 0x2A |
| ALC | on | 0x11[7] = 0 |
| Photodiode bias | internal PD, 0–65 pF | 0x15 = 0x01 |

Knobs worth turning first if the signal is weak or saturated: `LED1_PA` in
`main.c`, then the ADC range in `REG_PPG_CONFIG_1`.

## Note on the I2C address

The datasheet contradicts itself. The prose on p.30 gives the address as
`0b1100010x` (7-bit **0x62**, write byte 0xC4); the transaction figures on
pp.32–34 draw `1100011` (7-bit **0x63**). 0x62 is what Maxim's own driver
uses and what parts answer on. `maxm86161_probe()` tries 0x62 first and
falls back to 0x63, and the bus scan at startup will show you which one
actually ACKs.

## Not covered here

The GSR front end (U7/U2/U8), the LSM6DSV, the BQ25120A charger, and BLE.
This is a single-sensor bring-up.

## Next board spin

See [HARDWARE_NOTES.md](HARDWARE_NOTES.md) for the design changes that came
out of first-spin bring-up: debug connector, rail voltage, MAXM86161 INTB
routing, and RESET_N access.

## Bring-up results

See [BRINGUP_RESULTS.md](BRINGUP_RESULTS.md) for what each subsystem
actually did on board 3, including the BQ25120A CD/Hi-Z discovery and the
IMU part-number mismatch. [POSTMORTEM.md](POSTMORTEM.md) covers the
MAXM86161 bare-read trap that cost two days.

## App integration

[APP_INTEGRATION.md](APP_INTEGRATION.md) is the phone-side contract:
UUIDs, packet layouts, control opcodes, duty-cycle behaviour the app has to
expect, and the caveats that should not be papered over in a UI.
