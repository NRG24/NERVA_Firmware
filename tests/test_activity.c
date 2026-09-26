/*
 * Host-side tests for steps.c, sleep.c and calories.c.
 *
 * Most of these are regression guards: every one marked BUG below is a
 * defect that shipped in the first version of these modules and was found
 * by running exactly this scenario. They are here so it cannot come back.
 *
 * What this suite proves: the algorithms do what their headers claim when
 * fed a plausible signal. What it does NOT prove: that the signal is
 * plausible. Nobody has recorded a real accelerometer trace off this ring,
 * so the wearer model in sim.c is an assumption, and every "accuracy"
 * number here is accuracy against that assumption. See tests/README.md.
 */

#include "sim.h"

#include "calories.h"
#include "sleep.h"
#include "steps.h"

/* ble.h is firmware-side and expects the Zephyr toolchain's macros. */
#define __packed	__attribute__((packed))
#include <zephyr/sys/util.h>	/* the stub under tests/stubs */
#include "ble.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* --- feeding helpers --------------------------------------------------- */

static int64_t now_ms;

static void reset_all(void)
{
	now_ms = 0;
	steps_init();
	sleep_init();
	calories_init();
}

/* Feed both detectors for `secs` at a fixed interval. */
static void feed(struct sim_wearer *w, int secs, int dt_ms)
{
	int64_t end = now_ms + (int64_t)secs * 1000;

	while (now_ms < end) {
		int32_t mg = sim_sample(w, dt_ms);

		steps_update(mg, now_ms);
		sleep_feed(mg, now_ms);
		now_ms += dt_ms;
	}
}

static void feed_still(int secs, int dt_ms, int noise_mg)
{
	struct sim_wearer w;

	sim_wearer_init(&w, 0, 0, noise_mg);
	feed(&w, secs, dt_ms);
}

static void feed_walk(int secs, int dt_ms, double spm, double swing_mg)
{
	struct sim_wearer w;

	sim_wearer_init(&w, spm, swing_mg, 8);
	feed(&w, secs, dt_ms);
}

/* Time passes with no samples at all -- what RING_CHARGING looks like. */
static void feed_gap(int secs)
{
	now_ms += (int64_t)secs * 1000;
}

/* --- the wire contract ------------------------------------------------- */

/*
 * APP_INTEGRATION.md section 7 publishes these offsets to app authors. If
 * a field is added or reordered without updating that table, this fails
 * rather than silently shifting everybody's parser by two bytes.
 */
static void test_wire_format(void)
{
	sim_section("wire format (must match APP_INTEGRATION.md section 7)");

	CHECK(sizeof(struct ring_activity) == 20,
	      "ring_activity is %zu bytes, documented as 20",
	      sizeof(struct ring_activity));

	/* Must stay inside a default 23-byte ATT MTU (3 bytes of header). */
	CHECK(sizeof(struct ring_activity) <= 20,
	      "ring_activity does not fit an unnegotiated MTU");

	CHECK(offsetof(struct ring_activity, steps) == 0, "steps offset");
	CHECK(offsetof(struct ring_activity, kcal_x1000) == 4, "kcal offset");
	CHECK(offsetof(struct ring_activity, cadence_spm) == 8, "cadence offset");
	CHECK(offsetof(struct ring_activity, sleep_session_min) == 10,
	      "sleep_session_min offset");
	CHECK(offsetof(struct ring_activity, sleep_total_min) == 12,
	      "sleep_total_min offset");
	CHECK(offsetof(struct ring_activity, restless_min) == 14,
	      "restless_min offset");
	CHECK(offsetof(struct ring_activity, sleep_state) == 16,
	      "sleep_state offset");
	CHECK(offsetof(struct ring_activity, wear_state) == 17,
	      "wear_state offset");
	CHECK(offsetof(struct ring_activity, wear_checks) == 18,
	      "wear_checks offset");
	CHECK(offsetof(struct ring_activity, wear_confirmed) == 19,
	      "wear_confirmed offset");

	/* APP_INTEGRATION.md section 11, the Workout characteristic. */
	CHECK(sizeof(struct ring_workout) == 16,
	      "ring_workout is %zu bytes, documented as 16",
	      sizeof(struct ring_workout));
	CHECK(offsetof(struct ring_workout, type) == 1, "workout type offset");
	CHECK(offsetof(struct ring_workout, minutes) == 2, "minutes offset");
	CHECK(offsetof(struct ring_workout, hr_minutes) == 4,
	      "hr_minutes offset");
	CHECK(offsetof(struct ring_workout, rest_minutes) == 6,
	      "rest_minutes offset");
	CHECK(offsetof(struct ring_workout, fallback_minutes) == 8,
	      "fallback_minutes offset");
	CHECK(offsetof(struct ring_workout, avg_hr_x10) == 10,
	      "avg_hr_x10 offset");
	CHECK(offsetof(struct ring_workout, kcal_x1000) == 12,
	      "workout kcal offset");
}

/* --- steps ------------------------------------------------------------- */

static void test_step_accuracy(void)
{
	const int dts[] = { 20, 40, 60 };
	const double cadences[] = { 90, 110, 130 };

	sim_section("step count at the rates main.c actually polls");

	for (size_t d = 0; d < ARRAY_SIZE(dts); d++) {
		for (size_t c = 0; c < ARRAY_SIZE(cadences); c++) {
			char label[64];

			reset_all();
			feed_walk(300, dts[d], cadences[c], 150);

			snprintf(label, sizeof(label),
				 "%.0f spm at %d ms", cadences[c], dts[d]);
			CHECK_NEAR(steps_count(), cadences[c] * 5, 3, label);
		}
	}
}

/*
 * BUG: the detector was fed at 200 ms, which is where the firmware's idle
 * poll used to sit. Walking is 1.5-2.5 Hz, so that is barely above Nyquist
 * and steps are lost outright rather than merely mis-timed. This documents
 * the cliff so nobody quietly raises the poll interval again; the policy
 * that keeps the firmware off it is tested in test_mainloop.c.
 */
static void test_slow_polling_is_known_bad(void)
{
	sim_section("the 200 ms cliff (documents why STEP_POLL_MS exists)");

	reset_all();
	feed_walk(300, 200, 110, 150);

	CHECK(steps_count() < 550 / 2,
	      "200 ms polling counted %u of 550 steps -- if this now passes, "
	      "the detector improved and steps.h's table needs redoing",
	      steps_count());
}

static void test_no_false_steps_when_still(void)
{
	const int noises[] = { 2, 8, 20, 40 };

	sim_section("a still ring counts no steps");

	for (size_t i = 0; i < ARRAY_SIZE(noises); i++) {
		reset_all();
		feed_still(1800, 40, noises[i]);

		CHECK(steps_count() == 0,
		      "30 min still with +/-%d mg noise counted %u steps",
		      noises[i], steps_count());
	}
}

static void test_cadence(void)
{
	sim_section("cadence tracks, and decays when walking stops");

	reset_all();
	feed_walk(120, 40, 110, 150);
	CHECK_NEAR(steps_cadence_spm(), 110, 10, "cadence while walking");

	feed_still(10, 40, 4);
	CHECK(steps_cadence_spm() == 0,
	      "cadence %u ten seconds after stopping, expected 0",
	      steps_cadence_spm());
}

/*
 * A property check, not a regression guard -- and the distinction is worth
 * recording. steps.c re-primes after a feed gap, but deleting that
 * re-prime does not make this test fail: the input is a magnitude, so the
 * baseline sits near 1000 mg at rest no matter which way the ring came
 * back. The gap handling that IS load-bearing is in sleep.c, below.
 */
static void test_no_false_steps_across_a_gap(void)
{
	sim_section("a feed gap does not manufacture steps");

	reset_all();
	feed_still(600, 40, 4);

	uint32_t before = steps_count();

	feed_gap(3600);
	feed_still(60, 40, 4);

	CHECK(steps_count() == before,
	      "an hour of charging produced %u steps",
	      steps_count() - before);
}

/* --- sleep ------------------------------------------------------------- */

static void test_sleep_onset_and_wake(void)
{
	sim_section("sleep onset needs dwell, and so does waking");

	reset_all();
	feed_still(8 * 60, 200, 3);
	CHECK(!sleep_is_asleep(), "asleep after only 8 still minutes");

	feed_still(4 * 60, 200, 3);
	CHECK(sleep_is_asleep(), "not asleep after 12 still minutes");
	CHECK(sleep_session_minutes() >= 10,
	      "session reports %u minutes, expected at least 10",
	      sleep_session_minutes());

	/* One restless minute must not end the session. */
	feed_walk(70, 40, 110, 200);
	CHECK(sleep_is_asleep(),
	      "a single minute of movement ended the session");
	CHECK(sleep_restless_minutes() >= 1, "restless minute not counted");

	/* Sustained activity must. */
	feed_walk(3 * 60, 40, 110, 200);
	CHECK(!sleep_is_asleep(),
	      "three active minutes did not end the session");
	CHECK(sleep_session_minutes() == 0,
	      "session minutes still %u while awake",
	      sleep_session_minutes());
}

/*
 * BUG: RING_CHARGING reads no accelerometer, so an hour on a charger
 * arrived as a single stale bucket, was judged one still minute, and the
 * session ran straight through it. A ring on a charger is not a ring being
 * slept in.
 */
static void test_charging_gap_ends_the_session(void)
{
	sim_section("a charging gap ends the session");

	reset_all();
	feed_still(30 * 60, 200, 3);
	CHECK(sleep_is_asleep(), "did not fall asleep in 30 still minutes");

	uint16_t credited = sleep_total_minutes();

	feed_gap(3600);
	feed_still(90, 200, 3);

	CHECK(!sleep_is_asleep(),
	      "still 'asleep' after an hour with no accelerometer data");
	CHECK(sleep_session_minutes() == 0, "session did not close");
	CHECK(sleep_total_minutes() >= credited,
	      "minutes credited before the gap were lost (%u -> %u)",
	      credited, sleep_total_minutes());
}

/*
 * BUG: the gap was measured from the start of the bucket rather than from
 * the last sample, so the shortest outage it could see was a whole bucket
 * long. A short charge -- one or two minutes -- slipped through and was
 * credited as ordinary stillness, extending the session straight over it.
 */
static void test_short_gaps_are_caught_too(void)
{
	const int gaps_s[] = { 20, 45, 75, 110, 200 };

	sim_section("a short feed gap ends the session as well as a long one");

	for (size_t i = 0; i < ARRAY_SIZE(gaps_s); i++) {
		reset_all();
		feed_still(30 * 60, 200, 3);
		CHECK(sleep_is_asleep(), "did not fall asleep before the gap");

		feed_gap(gaps_s[i]);
		feed_still(90, 200, 3);

		CHECK(!sleep_is_asleep(),
		      "a %d s outage did not end the session", gaps_s[i]);
	}
}

/*
 * A legitimate slow pass must NOT look like an outage. The main loop can
 * stall for a few seconds on a bad I2C bus (see the blocking audit in
 * main.c) and that has to stay an ordinary still minute, or a marginal bus
 * would shred every sleep session into fragments.
 */
static void test_a_slow_pass_is_not_a_gap(void)
{
	sim_section("a few seconds of stall does not end a session");

	reset_all();
	feed_still(30 * 60, 200, 3);
	CHECK(sleep_is_asleep(), "did not fall asleep");

	feed_gap(5);			/* the documented worst-case stall */
	feed_still(120, 200, 3);

	CHECK(sleep_is_asleep(),
	      "a 5 s stall on a slow bus ended the sleep session");
}

/*
 * BUG: restless_minutes counted the very minutes that ended the session.
 * It is published as "motion during the session that did not end it", so a
 * still night finishing with the wearer getting out of bed reported three
 * restless minutes -- and carried them all of the next day, since the
 * field is only cleared when the next session starts.
 */
static void test_waking_up_is_not_restlessness(void)
{
	sim_section("the minutes that end a session are not 'restless'");

	reset_all();
	feed_still(40 * 60, 200, 3);
	CHECK(sleep_is_asleep(), "did not fall asleep");
	CHECK(sleep_restless_minutes() == 0,
	      "a perfectly still night logged %u restless minutes",
	      sleep_restless_minutes());

	/* Get up: sustained activity, enough to end the session. */
	feed_walk(4 * 60, 40, 110, 200);

	CHECK(!sleep_is_asleep(), "walking for 4 minutes did not end it");
	CHECK(sleep_restless_minutes() == 0,
	      "getting up logged %u restless minutes for a still night",
	      sleep_restless_minutes());
}

/*
 * A genuine mid-session stir -- one active minute, then settling again --
 * IS restlessness and must survive. This is the other side of the fix
 * above, so that "back out the wake minutes" cannot be implemented by
 * simply zeroing the counter.
 */
static void test_a_genuine_stir_is_still_counted(void)
{
	sim_section("a stir that does not end the session stays counted");

	reset_all();
	feed_still(40 * 60, 200, 3);
	CHECK(sleep_is_asleep(), "did not fall asleep");

	/*
	 * 25 s, not a minute-and-more. Buckets are wall-clock minutes, so a
	 * stir long enough to straddle three boundaries genuinely IS three
	 * active minutes and genuinely should end the session -- measured,
	 * and the reason an earlier version of this test was wrong.
	 */
	feed_walk(25, 40, 110, 200);
	feed_still(10 * 60, 200, 3);		/* settle again */

	CHECK(sleep_is_asleep(), "one stirring minute ended the session");
	CHECK(sleep_restless_minutes() >= 1,
	      "a real mid-session stir was not counted as restless");
}

/*
 * BUG: the feed-gap session end kept the minutes that had been credited
 * provisionally while waiting to see whether the wearer settled, while the
 * wake-confirm path in the same function backed them out. Asleep, two
 * stirring minutes, then onto the charger left sleep_total_min two
 * minutes high.
 */
static void test_a_gap_does_not_bank_provisional_minutes(void)
{
	uint16_t settled, after_gap;

	sim_section("a gap does not bank provisionally-credited awake minutes");

	reset_all();
	feed_still(40 * 60, 200, 3);
	CHECK(sleep_is_asleep(), "did not fall asleep");
	settled = sleep_total_minutes();

	/* A stirring minute: provisionally credited, wake not yet confirmed. */
	feed_walk(25, 40, 110, 200);
	CHECK(sleep_is_asleep(), "one stirring minute ended the session early");

	/* Now the ring goes on the charger before the wake is confirmed. */
	feed_gap(600);
	feed_still(90, 200, 3);

	CHECK(!sleep_is_asleep(), "the gap did not end the session");

	after_gap = sleep_total_minutes();
	CHECK(after_gap <= settled,
	      "total sleep went %u -> %u across a stir-then-charge; the "
	      "provisionally credited awake minute was banked", settled,
	      after_gap);
	CHECK(sleep_restless_minutes() == 0,
	      "the stirring minute stayed on the restless count (%u) after "
	      "the session was ended by a gap", sleep_restless_minutes());
}

/*
 * BUG: end_session() backed provisionally-credited minutes out of
 * total_minutes and restless_minutes whether or not a session was actually
 * in progress, and the feed-gap path calls it in both states. active_run
 * keeps counting while awake -- the awake branch of evaluate_minute()
 * returns before crediting anything -- so an ordinary day subtracted its
 * own activity from the night before it. Wear the ring overnight, walk for
 * an hour and a half, then put it on the charger, and 87 minutes went
 * missing off a 419-minute night; a long enough active stretch zeroed the
 * night outright, because the subtraction is clamped at 0 rather than
 * skipped.
 *
 * This is the ordinary daily pattern, not a corner case, which is why it is
 * worth a test of its own rather than relying on the asleep-path gap tests
 * above.
 */
static void test_an_awake_gap_does_not_eat_banked_sleep(void)
{
	uint16_t banked, after_gap;

	sim_section("a charger gap while awake does not eat banked sleep");

	reset_all();

	/* A night's sleep, banked and closed out by getting up. */
	feed_still(7 * 3600, 200, 5);
	CHECK(sleep_is_asleep(), "did not fall asleep overnight");
	banked = sleep_total_minutes();
	CHECK(banked > 6 * 60, "only %u minutes banked overnight", banked);

	/*
	 * Ninety minutes on the move with no still minute to reset the run,
	 * so active_run reaches ~90 -- more than an hour of it.
	 */
	feed_walk(90 * 60, 40, 110, 200);
	CHECK(!sleep_is_asleep(), "still asleep after 90 minutes of walking");
	CHECK(sleep_total_minutes() == banked,
	      "walking changed the sleep total (%u -> %u)", banked,
	      sleep_total_minutes());

	/* Onto the charger: no accelerometer at all for an hour. */
	feed_gap(3600);
	feed_still(120, 200, 5);

	after_gap = sleep_total_minutes();
	CHECK(after_gap == banked,
	      "the charger gap took %u minutes off the night (%u -> %u)",
	      banked - after_gap, banked, after_gap);
}

/*
 * Characterisation, not a bug guard: this pins WHERE the classifier's
 * cliff sits, because the honest description of this algorithm is not
 * "it detects sleep" but "it detects the absence of a movement above
 * SLEEP_STILL_MG within any SLEEP_ONSET_MINUTES window".
 *
 * The behaviour is close to binary, which is worth knowing before tuning
 * anything: a wearer who twitches at least every few minutes logs no
 * sleep at all, and one who goes quarter-hours without moving logs the
 * whole stretch. Measured, and the reason the caveats in STATUS.md and
 * APP_INTEGRATION.md can put a number on it.
 */
static void test_where_the_sleep_cliff_sits(void)
{
	sim_section("how often a wearer must move to not look asleep");

	/* Moving every 8 minutes: never 10 consecutive still minutes. */
	reset_all();
	for (int i = 0; i < 8 * 60 / 8; i++) {
		feed_still(8 * 60 - 6, 200, 15);
		feed_walk(6, 40, 100, 250);
	}
	CHECK(sleep_total_minutes() == 0,
	      "a wearer moving every 8 minutes logged %u minutes of sleep",
	      sleep_total_minutes());

	/* Moving every 16 minutes: the stillness between is enough. */
	reset_all();
	for (int i = 0; i < 8 * 60 / 16; i++) {
		feed_still(16 * 60 - 6, 200, 15);
		feed_walk(6, 40, 100, 250);
	}
	CHECK(sleep_total_minutes() > 6 * 60,
	      "a wearer moving only every 16 minutes logged just %u minutes "
	      "of sleep over 8 hours", sleep_total_minutes());
}

/*
 * BUG: sleep_reset() kept a step snapshot taken before steps_reset() zeroed
 * the counter, so the next minute's unsigned delta wrapped to ~4 billion
 * and read as the most active minute ever recorded -- costing a minute of
 * stillness and, if a session had been running, a restless minute.
 */
static void test_reset_does_not_poison_the_next_minute(void)
{
	sim_section("resetting the counters does not corrupt the next minute");

	reset_all();
	feed_walk(300, 40, 110, 200);
	CHECK(steps_count() > 0, "no steps to reset");

	/* Exactly what main.c's reset block does, in that order. */
	steps_reset();
	sleep_reset();
	calories_reset();

	CHECK(steps_count() == 0, "steps not cleared");

	feed_still(20 * 60, 200, 3);

	CHECK(sleep_is_asleep(), "20 still minutes after a reset did not sleep");
	CHECK(sleep_session_minutes() >= 20,
	      "session is %u minutes, expected 20 -- a minute was eaten by a "
	      "bogus step delta", sleep_session_minutes());
}

/* --- calories ---------------------------------------------------------- */

static void test_calorie_arithmetic(void)
{
	sim_section("calorie arithmetic");

	/* kcal/min = MET * kg * 0.0175; at rest, 70 kg: 1.225 kcal/min. */
	calories_init();
	calories_set_weight(700);
	for (int i = 0; i < 60; i++) {
		calories_update_minute(0);
	}
	CHECK(calories_total_x1000() == 73500,
	      "an hour of rest at 70 kg gave %u, expected 73500",
	      calories_total_x1000());

	/* Weight is clamped to 20.0-250.0 kg. */
	calories_init();
	calories_set_weight(0);
	calories_update_minute(0);
	CHECK(calories_total_x1000() == 350,
	      "weight 0 should clamp to 20 kg (350), got %u",
	      calories_total_x1000());

	calories_init();
	calories_set_weight(65535);
	calories_update_minute(0);
	CHECK(calories_total_x1000() == 4375,
	      "weight 65535 should clamp to 250 kg (4375), got %u",
	      calories_total_x1000());

	/* Walking has to cost more than resting. */
	calories_init();
	calories_set_weight(700);
	calories_update_minute(110);

	uint32_t walking = calories_total_x1000();

	calories_init();
	calories_set_weight(700);
	calories_update_minute(0);
	CHECK(walking > calories_total_x1000() * 2,
	      "a minute of walking (%u) is not meaningfully above rest (%u)",
	      walking, calories_total_x1000());
}

/*
 * BUG: calories_update_minute() used to take an instantaneous cadence, so
 * a whole minute inherited whatever the wearer happened to be doing at the
 * instant the tick fired. The same ten minutes of intermittent walking
 * came out as 42.0 kcal or 12.3 kcal depending only on tick phase.
 */
static void test_calories_do_not_depend_on_tick_phase(void)
{
	uint32_t mid_walk, after_rest;
	uint32_t last;

	sim_section("calorie totals do not depend on when the tick lands");

	/* Ten minutes of 20 s walking, 40 s still. Tick lands mid-stride. */
	reset_all();
	calories_set_weight(700);
	last = 0;
	for (int m = 0; m < 10; m++) {
		feed_walk(20, 40, 110, 200);
		calories_update_minute(steps_count() - last);
		last = steps_count();
		feed_still(40, 40, 4);
	}
	mid_walk = calories_total_x1000();

	/* Identical activity, tick lands after the rest instead. */
	reset_all();
	calories_set_weight(700);
	last = 0;
	for (int m = 0; m < 10; m++) {
		feed_walk(20, 40, 110, 200);
		feed_still(40, 40, 4);
		calories_update_minute(steps_count() - last);
		last = steps_count();
	}
	after_rest = calories_total_x1000();

	CHECK_NEAR(mid_walk, after_rest, 10,
		   "same activity, different tick phase");

	/*
	 * And the answer should be in the right neighbourhood: a third of
	 * each minute at ~3.5 MET and the rest at 1 MET is about 2.2
	 * kcal/min for 70 kg, so roughly 22 kcal over ten minutes. Wide
	 * tolerance -- this is checking for a plausible magnitude, not
	 * calibration, which no simulation can give.
	 */
	CHECK(mid_walk > 14000 && mid_walk < 30000,
	      "ten minutes of intermittent walking gave %u.%03u kcal, "
	      "expected roughly 22", mid_walk / 1000, mid_walk % 1000);
}

/* --- workouts ----------------------------------------------------------- */

/* The published equations, in floating point, straight from the paper. */
static double keytel_kcal(double hr, double kg, double age, int sex)
{
	double m = -55.0969 + 0.6309 * hr + 0.1988 * kg + 0.2017 * age;
	double f = -20.4022 + 0.4472 * hr - 0.1263 * kg + 0.0740 * age;
	double kj = sex == CAL_SEX_MALE ? m
		  : sex == CAL_SEX_FEMALE ? f : (m + f) / 2.0;

	return kj > 0 ? kj / 4.184 : 0;
}

static void test_keytel_matches_the_paper(void)
{
	int worst = 0;
	int cases = 0;

	sim_section("workout: integer Keytel matches the published equations");

	/* One worked by hand: 30 y, 70 kg man at 150 bpm.
	 * -55.0969 + 94.635 + 13.916 + 6.051 = 59.505 kJ/min = 14.222 kcal/min.
	 */
	calories_init();
	calories_set_weight(700);
	calories_set_body(30, CAL_SEX_MALE);
	CHECK(abs((int)calories_keytel_x1000(1500) - 14222) <= 2,
	      "70 kg, 30 y man at 150 bpm: %u, worked by hand as 14222",
	      calories_keytel_x1000(1500));

	for (int sex = 0; sex <= 2; sex++) {
		for (int kg = 45; kg <= 130; kg += 17) {
			for (int age = 18; age <= 78; age += 12) {
				for (int hr = 90; hr <= 190; hr += 10) {
					calories_init();
					calories_set_weight((uint16_t)(kg * 10));
					calories_set_body((uint8_t)age, (uint8_t)sex);

					double want = keytel_kcal(hr, kg, age, sex)
						      * 1000.0;
					int got = (int)calories_keytel_x1000(
						(uint16_t)(hr * 10));
					int err = abs(got - (int)lround(want));

					if (err > worst) {
						worst = err;
					}
					cases++;
				}
			}
		}
	}
	/* 1/1000 kcal from integer truncation, nothing more. */
	CHECK(worst <= 2, "worst disagreement with the paper: %d kcal/1000",
	      worst);
	printf("    worst disagreement over %d cases: %d kcal/1000\n", cases,
	       worst);

	/* No age given: 35. No sex given: the mean of the two equations. */
	calories_init();
	calories_set_weight(700);
	CHECK(abs((int)calories_keytel_x1000(1400) -
		  (int)lround(keytel_kcal(140, 70, 35, 0) * 1000)) <= 2,
	      "defaults are not age 35 / mean of both equations");
}

static void test_rowing_is_seen_by_heart_rate(void)
{
	struct calories_workout w;

	sim_section("workout: rowing costs what heart rate says, not what steps say");

	/* 30 minutes on a rowing machine: no steps at all, HR 140. */
	calories_init();
	calories_set_weight(800);
	calories_set_body(40, CAL_SEX_MALE);
	calories_workout_start(WORKOUT_ROW);
	for (int m = 0; m < 30; m++) {
		calories_update_minute_hr(0, 1400);
	}
	calories_workout_get(&w);

	uint32_t per_min = w.kcal_x1000 / 30;
	uint32_t want = (uint32_t)lround(keytel_kcal(140, 80, 40, CAL_SEX_MALE)
					 * 1000);

	CHECK(abs((int)per_min - (int)want) <= 2,
	      "rowing minute %u, Keytel says %u", per_min, want);
	CHECK(w.minutes == 30 && w.hr_minutes == 30,
	      "%u minutes, %u priced by HR, wanted 30/30", w.minutes,
	      w.hr_minutes);
	CHECK(w.avg_hr_x10 == 1400, "average HR %u, wanted 1400", w.avg_hr_x10);
	printf("    30 min rowing at 140 bpm, 80 kg: %u kcal "
	       "(the step formula alone: %u)\n",
	       w.kcal_x1000 / 1000, 30 * 1400 / 1000);

	/* The same half hour outside a workout is billed as sitting still. */
	calories_init();
	calories_set_weight(800);
	for (int m = 0; m < 30; m++) {
		calories_update_minute_hr(0, 1400);
	}
	CHECK(calories_total_x1000() == 30 * 1400,
	      "outside a workout heart rate must be ignored: %u, wanted %u",
	      calories_total_x1000(), 30 * 1400);
}

static void test_workout_minute_paths(void)
{
	struct calories_workout w;
	uint32_t before;

	sim_section("workout: each minute takes the right path");

	/* Below 90 bpm: the step formula, and counted as rest. */
	calories_init();
	calories_set_weight(700);
	calories_workout_start(WORKOUT_OTHER);
	calories_update_minute_hr(0, 850);
	calories_workout_get(&w);
	CHECK(w.kcal_x1000 == 1225, "85 bpm minute cost %u, wanted rest 1225",
	      w.kcal_x1000);
	CHECK(w.rest_minutes == 1 && w.hr_minutes == 0,
	      "85 bpm counted as %u rest / %u HR", w.rest_minutes, w.hr_minutes);

	/* No heart rate: the activity's MET. Rowing 7.0 x 70 kg x 0.0175. */
	calories_init();
	calories_set_weight(700);
	calories_workout_start(WORKOUT_ROW);
	calories_update_minute_hr(0, 0);
	calories_workout_get(&w);
	CHECK(w.kcal_x1000 == 8575, "no-HR rowing minute cost %u, wanted 8575",
	      w.kcal_x1000);
	CHECK(w.fallback_minutes == 1, "no-HR minute not counted as fallback");

	/* ...but never below what the steps say (running, 170 spm -> 5.0). */
	calories_init();
	calories_set_weight(700);
	calories_workout_start(WORKOUT_OTHER);	/* fallback 5.0 */
	calories_update_minute_hr(170, 0);
	calories_workout_get(&w);
	CHECK(w.kcal_x1000 == 6125, "no-HR 170 spm minute cost %u, wanted 6125",
	      w.kcal_x1000);

	/* A low-reading HR during a visible run is floored at the steps. */
	calories_init();
	calories_set_weight(700);
	calories_set_body(30, CAL_SEX_FEMALE);
	calories_workout_start(WORKOUT_RUN);
	calories_update_minute_hr(170, 950);
	calories_workout_get(&w);
	CHECK(w.kcal_x1000 >= 6125,
	      "95 bpm while running at 170 spm cost %u, below the step price",
	      w.kcal_x1000);

	/* An absurd HR is capped, not believed. */
	calories_init();
	calories_set_weight(2500);
	calories_set_body(100, CAL_SEX_MALE);
	calories_workout_start(WORKOUT_RUN);
	calories_update_minute_hr(0, 2500);
	calories_workout_get(&w);
	CHECK(w.kcal_x1000 == WORKOUT_MAX_KCAL_X1000,
	      "250 bpm at 250 kg cost %u, wanted the %u cap", w.kcal_x1000,
	      WORKOUT_MAX_KCAL_X1000);

	/* Workout calories are part of the day's total too. */
	calories_init();
	calories_set_weight(700);
	calories_update_minute_hr(0, 0);
	before = calories_total_x1000();
	calories_workout_start(WORKOUT_CYCLE);
	calories_update_minute_hr(0, 1300);
	calories_workout_get(&w);
	CHECK(calories_total_x1000() == before + w.kcal_x1000,
	      "total %u is not rest %u + workout %u", calories_total_x1000(),
	      before, w.kcal_x1000);
}

static void test_workout_lifecycle(void)
{
	struct calories_workout w;

	sim_section("workout: start, stop, restart");

	calories_init();
	calories_set_weight(700);
	calories_workout_start(WORKOUT_RUN);
	calories_update_minute_hr(0, 1500);
	calories_workout_stop();
	calories_workout_get(&w);
	CHECK(!w.active && w.minutes == 1 && w.kcal_x1000 > 0,
	      "a stopped workout lost its figures");

	/* After stopping, minutes no longer count toward it. */
	calories_update_minute_hr(0, 1500);
	calories_workout_get(&w);
	CHECK(w.minutes == 1, "a stopped workout kept counting (%u)", w.minutes);

	/* Starting again begins from zero. */
	calories_workout_start(WORKOUT_ROW);
	calories_workout_get(&w);
	CHECK(w.active && w.minutes == 0 && w.kcal_x1000 == 0 &&
	      w.type == WORKOUT_ROW, "a new workout inherited the last one");

	/* Starting while running changes the type and keeps the figures. */
	calories_update_minute_hr(0, 1500);
	calories_workout_start(WORKOUT_CYCLE);
	calories_workout_get(&w);
	CHECK(w.minutes == 1 && w.type == WORKOUT_CYCLE,
	      "re-start mid-workout: %u minutes, type %u", w.minutes, w.type);

	/* NONE stops; an unknown type is OTHER. */
	calories_workout_start(WORKOUT_NONE);
	CHECK(!calories_workout_active(), "WORKOUT_NONE did not stop");
	calories_workout_start(200);
	calories_workout_get(&w);
	CHECK(w.active && w.type == WORKOUT_OTHER,
	      "unknown type 200 read as %u, wanted OTHER", w.type);

	/* Body data is clamped and sanitised. */
	calories_set_body(3, 9);
	CHECK(calories_age_years() == 10 && calories_sex() == CAL_SEX_UNSPECIFIED,
	      "age 3 / sex 9 stored as %u / %u", calories_age_years(),
	      calories_sex());
	calories_set_body(0, CAL_SEX_FEMALE);
	CHECK(calories_age_years() == 0, "age 0 must stay \"not given\"");
}

int main(void)
{
	printf("activity module tests\n");

	test_wire_format();

	test_step_accuracy();
	test_slow_polling_is_known_bad();
	test_no_false_steps_when_still();
	test_cadence();
	test_no_false_steps_across_a_gap();

	test_sleep_onset_and_wake();
	test_charging_gap_ends_the_session();
	test_short_gaps_are_caught_too();
	test_a_slow_pass_is_not_a_gap();
	test_waking_up_is_not_restlessness();
	test_a_genuine_stir_is_still_counted();
	test_a_gap_does_not_bank_provisional_minutes();
	test_an_awake_gap_does_not_eat_banked_sleep();
	test_where_the_sleep_cliff_sits();
	test_reset_does_not_poison_the_next_minute();

	test_calorie_arithmetic();
	test_calories_do_not_depend_on_tick_phase();

	test_keytel_matches_the_paper();
	test_rowing_is_seen_by_heart_rate();
	test_workout_minute_paths();
	test_workout_lifecycle();

	return sim_report("test_activity");
}
