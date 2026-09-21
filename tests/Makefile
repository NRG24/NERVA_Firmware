# Host-side tests for the activity modules. Nothing here touches Zephyr or
# the board -- see README.md for what that does and does not buy you.
#
#   make            check constants, syntax-check every firmware file, run
#   make run        just the behavioural suites
#   make syntax     just the syntax pass
#   make clean
#
# Runs anywhere with gcc or clang: Linux, macOS, WSL, or MinGW on the same
# Windows box that builds the firmware.

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror
CFLAGS  += -I. -Istubs -I../src
LDLIBS  += -lm

SRC     := ../src/steps.c ../src/sleep.c ../src/calories.c ../src/hrv.c ../src/hr.c
COMMON  := sim.c $(SRC)

TESTS   := test_activity test_mainloop test_hrv

# Every .c in src/, plus the GATT layout assertions.
#
# -Wno-unused-parameter: Zephyr does not enable it, and the GATT callbacks
# legitimately ignore arguments the signature forces on them.
#
# The CONFIG_* values are the production build. Kconfig options left
# undefined take their "off" path, which is the shipping configuration; the
# bench paths are covered by the SYNTAX_BENCH pass below.
SYNTAX_SRC   := $(wildcard ../src/*.c) test_gatt_layout.c
SYNTAX_FLAGS := -std=c11 -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter \
		-I. -Istubs -I../src \
		-D__packed='__attribute__((packed))' \
		-DCONFIG_BT_DIS_FW_REV_STR='"0.0.0"' \
		-DCONFIG_BT_DEVICE_NAME='"Ring PPG"' \
		-DCONFIG_BT_ID_MAX=1
SYNTAX_BENCH := -DCONFIG_RING_BENCH -DCONFIG_RING_BENCH_POWER_LED \
		-DCONFIG_RING_IMU_WAKE_DIAG -DCONFIG_RING_GSR_MONITOR \
		-DCONFIG_RING_WATCHDOG -DCONFIG_RING_WATCHDOG_TIMEOUT_MS=10000

.PHONY: all run clean check-constants syntax

all: check-constants syntax run

test_activity: test_activity.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_mainloop: test_mainloop.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_hrv: test_hrv.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

run: $(TESTS)
	@fail=0; \
	for t in $(TESTS); do \
		./$$t || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then \
		echo ""; echo "all suites passed"; \
	else \
		echo ""; echo "FAILURES"; exit 1; \
	fi

# test_mainloop.c keeps its own copies of main.c's polling constants,
# because pulling them out of a file that includes half of Zephyr is not
# worth it. This makes the duplication safe: if the firmware changes a
# value and the test does not, the build stops here rather than quietly
# testing a policy the firmware no longer has.
check-constants:
	@for c in STEP_POLL_MS IDLE_POLL_MS STEP_MOTION_MG STEP_POLL_HOLD_MS; do \
		fw=`grep -E "^#define[[:space:]]+$$c[[:space:]]" ../src/main.c \
			| awk '{print $$3}'`; \
		tc=`grep -E "^#define[[:space:]]+$$c[[:space:]]" test_mainloop.c \
			| awk '{print $$3}'`; \
		if [ -z "$$fw" ]; then \
			echo "check-constants: $$c not found in ../src/main.c"; \
			exit 1; \
		fi; \
		if [ "$$fw" != "$$tc" ]; then \
			echo "check-constants: $$c is $$fw in main.c but $$tc in test_mainloop.c"; \
			exit 1; \
		fi; \
	done
	@echo "check-constants: main.c and test_mainloop.c agree"

# A syntax check, NOT a build. It proves the firmware parses and type-checks
# against stub headers -- typos, undeclared identifiers, mismatched log
# format specifiers. It does NOT prove the Zephyr API is used correctly,
# because the stubs are ours. Only the real NCS build does that.
#
# Both configurations, because they are different code: the bench
# instruments are #ifdef'd in and would otherwise never be parsed here.
syntax:
	@for f in $(SYNTAX_SRC); do \
		$(CC) $(SYNTAX_FLAGS) $$f || exit 1; \
		$(CC) $(SYNTAX_FLAGS) $(SYNTAX_BENCH) $$f || exit 1; \
	done
	@echo "syntax: `echo $(SYNTAX_SRC) | wc -w` files clean, production and bench"

clean:
	rm -f $(TESTS)
