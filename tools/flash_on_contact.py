#!/usr/bin/env python3
"""
Press a pogo jig onto the SWD pads; this flashes the moment it gets through.

    python tools/flash_on_contact.py             # bench image, 5 V held on
    python tools/flash_on_contact.py other.hex   # anything else

Start it first, then press the jig on and hold still. A failed attempt costs
about 0.7 s, so the contact only has to be steady for the ~10 s the flash
itself takes. A module that ships locked (APPROTECT) is mass-erased by pyOCD
inside the same attempt, and pyOCD writes UICR so it stays unlocked.

The default image is release/ring-fw-v0.4-bench.hex. It has CD on P1.00 (the
2026-09-23 board), and RING_BENCH_POWER_LED takes a boost enable that is
never released, so the 5 V rail comes up at boot and stays up, with the green
LED lit as proof the firmware is running. Unlike the LED test, it clears
TS_EN and manages CD, so the battery charges.

Why firmware first: BOOST_EN has no pull-down on this board and the TPS61240
EN input draws 0.01 uA, so until firmware drives P0.12 the enable floats and
the boost turns on and off at random. A probe tip nearby is enough.

After the flash it reads P0.12 back over SWD while you keep holding, so when
the meter goes on the 5 V rail, "firmware drives EN high" is measured rather
than assumed.

Wiring: clip the probe's GND to battery negative. The 2 kOhm series resistors
on SWCLK and SWDIO go in the jig wiring, not on the board. Power the board
before pressing the jig on.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_HEX = os.path.join(ROOT, "release", "ring-fw-v0.4-bench.hex")

P0_PIN_CNF12 = 0x50000730  # bit 0 = DIR, 1 = output
P0_OUT = 0x50000504
BOOST_EN = 12  # P0.12, active high (vled_boost in the DTS)

FAILED = re.compile(r"No ACK|Unexpected ACK|communication failure|Error|"
                    r"Failed", re.I)

# What each failure means to someone holding the jig.
STATES = [
    (re.compile(r"No ACK", re.I),
     "no contact (data line idle high)"),
    (re.compile(r"Unexpected ACK '0'", re.I),
     "touching, but data line held low: SWDIO/SWCLK swapped, or no 1.8 V"),
    (re.compile(r"Unexpected ACK|Fault|Transfer", re.I),
     "contact lost mid-transfer, hold still"),
    (re.compile(r"timeout", re.I),
     "attempt timed out"),
]

WORD = re.compile(r"^([0-9a-f]{8}):\s+([0-9a-f]{8})", re.I | re.M)
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def pyocd(args, timeout):
    p = subprocess.run([sys.executable, "-m", "pyocd"] + args,
                       capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def describe(out):
    for pat, why in STATES:
        if pat.search(out):
            return why
    lines = out.strip().splitlines()
    return lines[-1][:100] if lines else "unknown failure"


def flash(hexfile, freq):
    try:
        rc, out = pyocd(["flash", "-t", "nrf52833", "-f", str(freq),
                         hexfile], 300)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    return rc == 0 and not FAILED.search(out), out


def read_boost_en():
    """(PIN_CNF[12], P0.OUT) as the running firmware left them, or None.

    connect_mode=attach so the firmware is neither halted nor reset.
    auto_unlock=false so that if the chip has somehow re-locked, pyOCD
    refuses instead of mass-erasing the image that was just written.
    """
    try:
        rc, out = pyocd(["commander", "-t", "nrf52833", "-f", "1000000",
                         "-O", "connect_mode=attach",
                         "-O", "auto_unlock=false",
                         "-c", "read32 0x%08x" % P0_PIN_CNF12,
                         "-c", "read32 0x%08x" % P0_OUT,
                         "-c", "exit"], 30)
    except subprocess.TimeoutExpired:
        return None
    if rc != 0 or FAILED.search(out):
        return None
    words = {int(a, 16): int(v, 16) for a, v in WORD.findall(out)}
    if P0_PIN_CNF12 not in words or P0_OUT not in words:
        return None
    return words[P0_PIN_CNF12], words[P0_OUT]


def beep():
    try:
        import winsound
        winsound.Beep(1500, 300)
    except Exception:
        print("\a", end="", flush=True)


def verify(bench):
    print("\nKeep holding ~5 s for the BOOST_EN readback "
          "(or let go, the meter answers it too).")
    time.sleep(2)  # boot, then regulator-fixed's 5 ms startup delay

    for _ in range(4):
        r = read_boost_en()
        if r is None:
            time.sleep(0.5)
            continue

        cnf, out = r
        is_output = cnf & 1
        high = (out >> BOOST_EN) & 1
        print("P0.12 PIN_CNF 0x%08x  P0.OUT 0x%08x" % (cnf, out))

        if not is_output:
            print("P0.12 is not configured as an output, so the firmware "
                  "is not\nrunning (watchdog loop or a fault?). Copy this "
                  "output to Claude.")
        elif high:
            print("BOOST_EN is an output, driven HIGH by the firmware.\n"
                  "Measure the 5 V rail now: 5.0 V means the boost is "
                  "good. Anything\nlower with EN high is the boost "
                  "section itself, not firmware.")
        elif bench:
            print("BOOST_EN is an output but driven LOW. The bench image "
                  "should hold\nit high -- copy this output to Claude.")
        else:
            print("BOOST_EN is an output, driven LOW. Normal for the "
                  "production image:\nthe boost only runs during a "
                  "measurement window, about 15 s a minute.")
        return

    print("Could not read P0.12 back (jig released?). The flash itself "
          "succeeded:\nthe green LED and the 5 V rail on the meter answer "
          "the same question.")


def capture_rtt(seconds):
    """Print the firmware's RTT log for `seconds`, without disturbing it.

    The boot messages are still in the RTT up-buffer, so attaching after
    the flash's own reset still shows the boot from the start. Same
    attach and no-unlock rules as read_boost_en().
    """
    print("\nReading the firmware log for %d s -- keep holding." % seconds)
    log = os.path.join(tempfile.gettempdir(), "ring_rtt.log")
    with open(log, "wb") as f:
        p = subprocess.Popen([sys.executable, "-m", "pyocd", "rtt",
                              "-t", "nrf52833", "-f", "1000000",
                              "-M", "attach", "-O", "auto_unlock=false"],
                             stdout=f, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()

    with open(log, "rb") as f:
        text = ANSI.sub("", f.read().decode("utf-8", "replace"))

    lines = [l for l in text.splitlines() if l.strip()]
    if not lines:
        print("(nothing read -- contact lost?)")
    for l in lines:
        print("  " + l)
    print("(saved to %s)" % log)


def log_only(args):
    """Wait for contact without flashing, then read the running log.

    Zephyr's RTT drops new output once the up-buffer is full rather than
    overwriting, so the boot messages are still there however long ago
    the board started.
    """
    print("Read-only: nothing is flashed or erased. Press the jig on and "
          "hold still.\n")
    deadline = time.time() + args.minutes * 60
    i = 0
    try:
        while time.time() < deadline:
            i += 1
            r = read_boost_en()
            if r is None:
                if i % 15 == 1:
                    print("  attempt %3d  no contact yet" % i, flush=True)
                continue
            beep()
            cnf, out = r
            print("\nCONTACT on attempt %d.  P0.12 is %s, driven %s." % (
                i, "an output" if cnf & 1 else "NOT an output",
                "HIGH" if (out >> BOOST_EN) & 1 else "low"))
            capture_rtt(args.rtt or 20)
            return 0
    except KeyboardInterrupt:
        print("\nstopped after %d attempts" % i)
        return 1

    print("\ngave up after %d attempts" % i)
    return 1


def main():
    ap = argparse.ArgumentParser(
        description="Flash as soon as a hand-held SWD jig makes contact.")
    ap.add_argument("hexfile", nargs="?", default=DEFAULT_HEX)
    ap.add_argument("-f", "--freq", type=int, nargs="+",
                    default=[1000000, 1000000, 250000],
                    help="SWD clocks, tried round-robin (default: 1 MHz, "
                         "1 MHz, 250 kHz)")
    ap.add_argument("-m", "--minutes", type=float, default=10,
                    help="give up after this long (default 10)")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip the BOOST_EN readback after flashing")
    ap.add_argument("--rtt", type=int, default=0, metavar="SECONDS",
                    help="after flashing, print the firmware log for this "
                         "long (jig must stay on)")
    ap.add_argument("--rtt-only", action="store_true",
                    help="do not flash: wait for contact, then print the "
                         "running firmware's log (--rtt seconds, default 20)")
    args = ap.parse_args()

    if args.rtt_only:
        return log_only(args)

    if not os.path.isfile(args.hexfile):
        print("no such file: %s" % args.hexfile)
        return 2

    print("Image: %s" % os.path.relpath(args.hexfile, ROOT))
    print("Board powered? Then press the jig on and hold still. "
          "Ctrl+C to stop.\n")

    start = time.time()
    deadline = start + args.minutes * 60
    last_state = None
    last_note = start
    i = 0

    try:
        while time.time() < deadline:
            i += 1
            freq = args.freq[(i - 1) % len(args.freq)]
            ok, out = flash(args.hexfile, freq)

            if ok:
                beep()
                print("\nFLASHED on attempt %d at %d Hz, %.0f s after "
                      "start." % (i, freq, time.time() - start))
                if "APPROTECT" in out:
                    print("(The chip was locked; pyOCD unlocked it first.)")
                if not args.no_verify:
                    verify("bench" in os.path.abspath(args.hexfile).lower())
                if args.rtt > 0:
                    capture_rtt(args.rtt)
                return 0

            state = describe(out)
            now = time.time()
            if state != last_state or now - last_note > 15:
                print("  %4.0f s  attempt %3d  %s"
                      % (now - start, i, state), flush=True)
                last_state = state
                last_note = now
    except KeyboardInterrupt:
        print("\nstopped after %d attempts" % i)
        return 1

    print("\ngave up after %d attempts, %.0f s" % (i, time.time() - start))
    return 1


if __name__ == "__main__":
    sys.exit(main())
