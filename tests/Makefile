# Host-side tests for the activity modules. Nothing here touches Zephyr or
# the board -- see README.md for what that does and does not buy you.
#
#   make            build, check constants, run everything
#   make run        just run
#   make clean
#
# Runs anywhere with gcc or clang: Linux, macOS, WSL, or MinGW on the same
# Windows box that builds the firmware.

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror
CFLAGS  += -I. -Istubs -I../src
LDLIBS  += -lm

SRC     := ../src/steps.c ../src/sleep.c ../src/calories.c
COMMON  := sim.c $(SRC)

TESTS   := test_activity test_mainloop

.PHONY: all run clean check-constants

all: check-constants run

test_activity: test_activity.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_mainloop: test_mainloop.c $(COMMON)
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

clean:
	rm -f $(TESTS)
