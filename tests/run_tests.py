#!/usr/bin/env python3
"""
Build and run the host test suites without make -- for a Windows box with no
make, using the Clang that `pip install ziglang` provides:

    python tests/run_tests.py            # syntax pass + every suite
    python tests/run_tests.py test_hrv   # just these
    python tests/run_tests.py syntax     # just the syntax pass

Mirrors the Makefile's suite rules (same sources, flags and -D defines), so
the two must change together. Binaries go to tests/bin/, which is ignored.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
OUT = os.path.join(HERE, "bin")
CC = [sys.executable, "-m", "ziglang", "cc"]
CFLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
          "-I" + HERE, "-I" + os.path.join(HERE, "stubs"), "-I" + SRC]

COMMON = ["sim.c"] + [os.path.join(SRC, f) for f in
                      ("steps.c", "sleep.c", "calories.c", "hrv.c", "hr.c", "spo2.c")]

SUITES = {
    "test_activity": (["test_activity.c"] + COMMON, []),
    "test_mainloop": (["test_mainloop.c"] + COMMON, []),
    "test_hrv": (["test_hrv.c"] + COMMON, []),
    "test_spo2": (["test_spo2.c"] + COMMON, []),
    "test_driver": (["test_driver.c", "sim.c", os.path.join(SRC, "maxm86161.c")],
                    ["-DCONFIG_RING_PPG_AVG_LOG2=2"]),
    "test_profile": (["test_profile.c", "sim.c", os.path.join(SRC, "profile.c"),
                      os.path.join(SRC, "calories.c")],
                     ["-DCONFIG_SETTINGS", "-DPROFILE_TESTING"]),
    "test_history": (["test_history.c", "sim.c", os.path.join(SRC, "history.c")],
                     ["-DCONFIG_SETTINGS", "-DHISTORY_TESTING"]),
    "test_hr": (["test_hr.c", "sim.c", os.path.join(SRC, "hr.c")], []),
    "test_thermo": (["test_thermo.c", "sim.c", os.path.join(SRC, "thermo.c")],
                    ["-DCONFIG_RING_HOT_FLASH_MODE=1",
                     "-DCONFIG_RING_HOT_FLASH_RISE_C10=10"]),
    "test_spot": (["test_spot.c", "sim.c", os.path.join(SRC, "spot.c")],
                  ["-DCONFIG_RING_SPOT_NIGHT_MIN=60", "-DCONFIG_RING_SPOT_DAY_MIN=60",
                   "-DCONFIG_RING_SPOT_WINDOW_S=180"]),
    "test_ota": (["test_ota.c", "sim.c", os.path.join(SRC, "ota.c")], []),
}


# The Makefile's syntax pass: every firmware file, production and bench,
# against the stub headers. Keep these defines in step with SYNTAX_FLAGS
# and SYNTAX_BENCH there.
SYNTAX_DEFS = ["-D__packed=__attribute__((packed))",
               '-DCONFIG_BT_DIS_FW_REV_STR="0.0.0"',
               '-DCONFIG_BT_DEVICE_NAME="Ring PPG"',
               "-DCONFIG_BT_ID_MAX=1", "-DCONFIG_SETTINGS",
               "-DCONFIG_RING_HISTORY_EPOCH_S=300",
               "-DCONFIG_RING_MEASURE_PERIOD_S=60",
               "-DCONFIG_BT_ATT_TX_COUNT=3",
               "-DCONFIG_RING_ADV_SLOW_AFTER_S=0",
               "-DCONFIG_RING_PPG_LED_PA=255", "-DCONFIG_RING_PPG_AVG_LOG2=2",
               "-DCONFIG_RING_SPOT_NIGHT_MIN=0", "-DCONFIG_RING_SPOT_DAY_MIN=0",
               "-DCONFIG_RING_SPOT_WINDOW_S=180",
               "-DCONFIG_RING_HOT_FLASH_MODE=0", "-DCONFIG_RING_HOT_FLASH_RISE_C10=10"]
SYNTAX_BENCH = ["-DCONFIG_RING_BENCH", "-DCONFIG_RING_BENCH_POWER_LED",
                "-DCONFIG_RING_IMU_WAKE_DIAG", "-DCONFIG_RING_GSR_MONITOR",
                "-DCONFIG_RING_WATCHDOG", "-DCONFIG_RING_WATCHDOG_TIMEOUT_MS=10000",
                "-DCONFIG_RING_DIAG_OPCODES=1", "-DCONFIG_RING_GSR_STATUS=1",
                "-DCONFIG_RING_CONN_PARAM_MGMT=1"]


def syntax():
    files = sorted(os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith(".c"))
    files.append(os.path.join(HERE, "test_gatt_layout.c"))
    # A real compile to a throwaway object, not -fsyntax-only: zig's cc
    # wrapper then goes looking for an output file that was never written.
    obj = os.path.join(OUT, "syntax.o")
    base = ["-std=c11", "-c", "-o", obj, "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-I" + HERE,
            "-I" + os.path.join(HERE, "stubs"), "-I" + SRC] + SYNTAX_DEFS
    bad = 0
    for f in files:
        for extra in ([], SYNTAX_BENCH):
            r = subprocess.run(CC + base + extra + [f], capture_output=True, text=True)
            if r.returncode:
                bad += 1
                print("SYNTAX %s (%s)\n%s" % (os.path.basename(f),
                                              "bench" if extra else "production",
                                              r.stderr[:1500]))
    print("%-14s %d files clean, production and bench" % ("syntax", len(files))
          if not bad else "%-14s %d failures" % ("syntax", bad))
    return bad == 0


def main(names):
    os.makedirs(OUT, exist_ok=True)
    failed = []
    # No names: the syntax pass and every suite. Otherwise just those named
    # ("syntax" is one of the names).
    suites = [n for n in names if n != "syntax"] if names else list(SUITES)
    if (not names or "syntax" in names) and not syntax():
        failed.append("syntax")
    for name in suites:
        srcs, defs = SUITES[name]
        exe = os.path.join(OUT, name + ".exe")
        srcs = [s if os.path.isabs(s) else os.path.join(HERE, s) for s in srcs]
        b = subprocess.run(CC + CFLAGS + defs + ["-o", exe] + srcs,
                           capture_output=True, text=True)
        if b.returncode:
            print("BUILD FAILED %s\n%s" % (name, b.stderr))
            failed.append(name)
            continue
        r = subprocess.run([exe], capture_output=True, text=True, cwd=HERE)
        tail = [l for l in r.stdout.splitlines() if "checks" in l or "FAIL" in l]
        print("%-14s %s" % (name, " | ".join(tail[-3:]) or r.stdout[-200:]))
        if r.returncode:
            failed.append(name)
    print("\nall suites passed" if not failed else "\nFAILED: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
