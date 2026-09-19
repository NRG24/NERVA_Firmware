/*
 * Tests main.c's RING_IDLE polling policy, not just the filters.
 *
 * The distinction matters. steps.c can be perfectly correct and the
 * firmware can still count nothing, because what reaches the detector is
 * decided by how often main.c reads the accelerometer -- which is exactly
 * the bug this file exists to guard. So this emulates the RING_IDLE case:
 * sample the wearer, update last_step_motion, feed the detectors, run the
 * per-minute calorie tick, and pick the next sleep interval the same way
 * main.c does.
 *
 * The constants below are copies. `make check-constants` greps main.c and
 * fails if they drift, which is cheaper than a build-system dependency on
 * a Zephyr header.
 */

#include "sim.h"

#include "calories.h"
#include "sleep.h"
#include "steps.h"

#include <zephyr/sys/util.h>	/* the stub under tests/stubs */

#include <stdio.h>
#include <stdlib.h>

/* --- mirrors main.c ---------------------------------------------------- */
#define STEP_POLL_MS		40
#define IDLE_POLL_MS		200
#define STEP_MOTION_MG		50
#define STEP_POLL_HOLD_MS	10000

struct ring {
	int64_t now;
	int64_t last_step_motion;
	int64_t next_activity_min;
	uint32_t steps_at_last_min;

	/* observability, for the tests */
	long polls;
	long fast_polls;
};

static void ring_boot(struct ring *r)
{
	r->now = 0;
	r->last_step_motion = 0;
	r->next_activity_min = 60000;
	r->steps_at_last_min = 0;
	r->polls = 0;
	r->fast_polls = 0;

	steps_init();
	sleep_init();
	calories_init();
}

/* One pass of main.c's RING_IDLE case. Returns the chosen sleep interval. */
static int ring_idle_pass(struct ring *r, int32_t mg)
{
	if (abs(mg - 1000) > STEP_MOTION_MG) {
		r->last_step_motion = r->now;
	}

	steps_update(mg, r->now);
	sleep_feed(mg, r->now);

	if (r->now >= r->next_activity_min) {
		uint32_t steps_now = steps_count();

		calories_update_minute(steps_now >= r->steps_at_last_min
				       ? steps_now - r->steps_at_last_min : 0);
		r->steps_at_last_min = steps_now;
		r->next_activity_min = r->now + 60000;
	}

	bool maybe_walking =
		(r->now - r->last_step_motion) < STEP_POLL_HOLD_MS;

	return maybe_walking ? STEP_POLL_MS : IDLE_POLL_MS;
}

/* Run the loop for `secs` of simulated time against a wearer. */
static void ring_run(struct ring *r, struct sim_wearer *w, int secs)
{
	int64_t end = r->now + (int64_t)secs * 1000;
	int dt = IDLE_POLL_MS;

	while (r->now < end) {
		int32_t mg = sim_sample(w, dt);

		dt = ring_idle_pass(r, mg);
		r->now += dt;
		r->polls++;
		if (dt == STEP_POLL_MS) {
			r->fast_polls++;
		}
	}
}

/*
 * THE regression test. Before the poll-rate fix this counted 0 of 1100
 * steps: steps.c was fine, but at a 200 ms idle poll nothing that looked
 * like a footfall ever reached it.
 */
static void test_walking_is_counted_end_to_end(void)
{
	const double swings[] = { 100, 150, 250, 400 };
	const double cadences[] = { 90, 110, 130 };

	sim_section("walking is counted through main.c's polling policy");

	for (size_t s = 0; s < ARRAY_SIZE(swings); s++) {
		for (size_t c = 0; c < ARRAY_SIZE(cadences); c++) {
			struct sim_wearer w;
			struct ring r;
			char label[80];

			ring_boot(&r);
			sim_wearer_init(&w, cadences[c], swings[s], 8);
			ring_run(&r, &w, 600);

			snprintf(label, sizeof(label),
				 "10 min at %.0f spm, %.0f mg swing",
				 cadences[c], swings[s]);
			CHECK_NEAR(steps_count(), cadences[c] * 10, 3, label);
		}
	}
}

/*
 * The other half of the trade: the fast poll must not follow the ring to
 * bed. A still ring has to stay on the idle interval, because that is the
 * power model the whole firmware is built around.
 */
static void test_still_ring_keeps_the_idle_rate(void)
{
	struct sim_wearer w;
	struct ring r;
	double hz;

	sim_section("a still ring stays on the slow poll");

	ring_boot(&r);
	sim_wearer_init(&w, 0, 0, 4);
	ring_run(&r, &w, 8 * 3600);

	hz = r.polls / (8.0 * 3600.0);
	CHECK_NEAR(hz, 1000.0 / IDLE_POLL_MS, 2,
		   "average poll rate over 8 still hours (Hz)");

	/*
	 * Not zero: main.c sets last_step_motion at boot on the same
	 * assumption last_motion makes -- the ring was just handled -- so the
	 * first STEP_POLL_HOLD_MS is deliberately fast. What matters is that
	 * it expires and never comes back while the ring is still. One extra
	 * poll of slack for the boundary sample.
	 */
	long boot_window_polls = STEP_POLL_HOLD_MS / STEP_POLL_MS + 1;

	CHECK(r.fast_polls <= boot_window_polls,
	      "%ld fast polls during 8 hours of stillness, expected at most "
	      "%ld (the boot hold window) -- the ring would burn current all "
	      "night", r.fast_polls, boot_window_polls);

	/* And it should have logged the night as sleep. */
	CHECK(sleep_is_asleep(), "8 still hours did not register as sleep");
	CHECK(sleep_total_minutes() > 7 * 60,
	      "only %u minutes of sleep logged over 8 hours",
	      sleep_total_minutes());
}

static void test_walking_raises_the_rate(void)
{
	struct sim_wearer w;
	struct ring r;
	double hz;

	sim_section("walking raises the poll rate, and only while walking");

	ring_boot(&r);
	sim_wearer_init(&w, 110, 150, 8);
	ring_run(&r, &w, 3600);

	hz = r.polls / 3600.0;
	CHECK_NEAR(hz, 1000.0 / STEP_POLL_MS, 5,
		   "average poll rate over an hour of walking (Hz)");

	/* Then stop, and confirm it falls back within the hold window. */
	long before = r.polls;

	sim_wearer_init(&w, 0, 0, 4);
	ring_run(&r, &w, 600);

	double after_hz = (r.polls - before) / 600.0;

	CHECK(after_hz < 2.0 * (1000.0 / IDLE_POLL_MS),
	      "poll rate stayed at %.1f Hz for ten minutes after walking "
	      "stopped", after_hz);
}

/* A day shaped like a person: a night, then errands. */
static void test_a_plausible_day(void)
{
	struct sim_wearer walking, resting;
	struct ring r;
	uint32_t kcal;

	sim_section("a plausible day end to end");

	ring_boot(&r);
	sim_wearer_init(&walking, 110, 150, 8);
	sim_wearer_init(&resting, 0, 0, 4);

	ring_run(&r, &resting, 8 * 3600);		/* asleep */

	uint16_t slept = sleep_total_minutes();

	for (int i = 0; i < 6; i++) {			/* six short walks */
		ring_run(&r, &walking, 120);
		ring_run(&r, &resting, 480);
	}

	CHECK_NEAR(slept, 8 * 60, 5, "minutes of sleep logged for 8 hours");
	CHECK(!sleep_is_asleep(), "still asleep after an hour of walking about");
	CHECK_NEAR(steps_count(), 6 * 2 * 110, 5, "steps over six 2 min walks");

	/*
	 * Nine hours: eight asleep plus an hour up. At 70 kg that is roughly
	 * 9 x 60 x 1.225 = 662 kcal of basal, plus twelve minutes of walking
	 * on top. A wide band -- this is a smoke test for the magnitude, not
	 * a calorimetry claim.
	 */
	kcal = calories_total_x1000() / 1000;
	CHECK(kcal > 600 && kcal < 800,
	      "nine hours came to %u kcal, expected roughly 700", kcal);
}

int main(void)
{
	printf("main loop policy tests\n");

	test_walking_is_counted_end_to_end();
	test_still_ring_keeps_the_idle_rate();
	test_walking_raises_the_rate();
	test_a_plausible_day();

	return sim_report("test_mainloop");
}
