# Flashing

Target: ANNA-B402 (nRF52833) over SWD. Connect SWDCLK, SWDIO, GND.

**Target runs at 1.8 V.** Most probes drive 3.3 V, above the chip's 2.1 V
limit. Put **2 kΩ in series with SWDCLK and SWDIO**, keep SWD at 1 MHz, and
power the board before connecting the probe.

Modules ship locked (Nordic APPROTECT), so erase first. Both commands must
run in one session — a reset between them re-locks the part.

```bash
pyocd erase -t nrf52833 -f 1000000 --chip
```

```bash
pyocd flash -t nrf52833 -f 1000000 zephyr.hex
```

`No ACK` means SWD wiring or power, not firmware.
