#include "steps.h"

#include <zephyr/sys/util.h>

#include <stdbool.h>
#include <string.h>

/*
 * Baseline tracks gravity (~1000 mg) so the detector rides out slow tilt
 * changes as the ring settles on the finger. Smooth is the fast component
 * that actually swings during a step -- a heel or hand strike is roughly
 * 0.3-0.6 s wide, much faster than a heartbeat, so it needs a much shorter
 * time constant than hr.c's equivalent filters.
 */
#define BASELINE_SHIFT		5	/* ~1/32 EMA */
#define SMOOTH_SHIFT		1	/* ~1/2  EMA */

#define THRESHOLD_NUM		3
#define THRESHOLD_DEN		10

/* Below this swing, "motion" is noise or hand tremor, not a step. */
#define AMPLITUDE_FLOOR_MG	80

/* How often the peak-to-peak amplitude estimate is refreshed. */
#define AMP_WINDOW_MS		500

/* Fastest plausible footfall is sprinting, a little over 4 Hz; refuse
 * anything faster as a double-count of the same impact.
 */
#define STEP_REFRACTORY_MIN_MS	250

/* An interval longer than this is a pause, not a step of a walk -- it
 * breaks cadence continuity but does not undo the step itself.
 */
#define STEP_INTERVAL_MAX_MS	2000

/* No step for this long: report cadence as stopped. */
#define CADENCE_TIMEOUT_MS	3000

#define INTERVAL_HISTORY	4

/*
 * Regulation: no step counts until a rhythm does.
 *
 * A ring is on the hand, and the hand moves all day without walking. Every
 * swing that crossed the threshold used to count, so gestures and reaching
 * read as steps -- found wearing the ring 2026-10-12, and pinned by
 * test_no_steps_from_hand_movement (545 "steps" from 30 minutes of
 * irregular hand movement before this).
 *
 * Commercial pedometers solve it the same way: candidate steps are held
 * back until REGULATION_STEPS of them have arrived with each interval
 * within RHYTHM_TOL_PCT of the one before, then credited all at once, and
 * from then on every candidate that keeps the rhythm counts immediately.
 * A walk loses nothing -- the held steps are credited, not dropped -- and
 * a burst of gestures almost never manages eight evenly spaced swings.
 *
 * Once counting, RHYTHM_MISSES irregular intervals in a row end the walk.
 * One is forgiven: a missed footfall shows up as a single long interval,
 * and ending the walk there would throw the next eight steps away.
 */
#define REGULATION_STEPS	8
#define RHYTHM_TOL_PCT		30
#define RHYTHM_MISSES		2

/* Shortest rhythm a missed footfall is inferred from: 180 steps a minute. */
#define MISSED_RHYTHM_MIN_MS	330

/*
 * Longer than this between two samples and the detector's time-based state
 * no longer describes anything: the ring was charging (RING_CHARGING reads
 * no IMU at all) or the part stopped answering. The amplitude window would
 * close on its first sample back with a peak-to-peak straddling the gap,
 * the refractory timer would compare against a step from an hour ago, and
 * `above` could still be latched. Re-prime instead.
 *
 * Note what this is NOT protecting against: the baseline going stale. The
 * input is a magnitude, so it sits near 1000 mg at rest whatever
 * orientation the ring came back in, and removing this re-prime does not
 * make the test suite produce a single false step. This is hygiene for the
 * windows, not a fix for a demonstrated fault.
 */
#define FEED_GAP_LIMIT_MS	2000

struct step_state {
	bool primed;

	int32_t baseline;
	int32_t smooth;
	int32_t amplitude;
	int32_t win_max, win_min;
	int64_t win_start_ms;
	bool above;

	int64_t last_sample_ms;
	int64_t last_step_ms;		/* last candidate, counted or held */
	uint32_t total;

	/* Regulation, see REGULATION_STEPS. */
	bool walking;			/* rhythm established, steps count */
	uint8_t held;			/* candidates in the current run */
	uint8_t misses;			/* irregular intervals in a row */
	uint32_t rhythm_ms;		/* the interval the run is keeping */

	uint32_t interval_ms[INTERVAL_HISTORY];
	uint8_t interval_count;
	uint8_t interval_next;
};

static struct step_state st;

void steps_init(void)
{
	memset(&st, 0, sizeof(st));
}

/* Forget any run in progress: nothing held, not walking. */
static void end_run(void)
{
	st.walking = false;
	st.held = 0;
	st.misses = 0;
	st.rhythm_ms = 0;
	st.interval_count = 0;
	st.interval_next = 0;
}

void steps_reset(void)
{
	st.total = 0;
	end_run();
	/* Leave baseline/smooth/amplitude alone: they are mid-EMA, and
	 * zeroing them would just force the detector to re-settle.
	 */
}

uint32_t steps_count(void)
{
	return st.total;
}

uint16_t steps_cadence_spm(void)
{
	if (st.interval_count == 0) {
		return 0;
	}

	uint32_t sum = 0;

	for (uint8_t i = 0; i < st.interval_count; i++) {
		sum += st.interval_ms[i];
	}

	uint32_t avg_ms = sum / st.interval_count;

	return (avg_ms == 0) ? 0 : (uint16_t)(60000U / avg_ms);
}

static void note_interval(uint32_t ms)
{
	st.interval_ms[st.interval_next] = ms;
	st.interval_next = (st.interval_next + 1) % INTERVAL_HISTORY;
	if (st.interval_count < INTERVAL_HISTORY) {
		st.interval_count++;
	}
}

/*
 * One threshold crossing that passed the refractory window. `since` is the
 * interval from the previous candidate, `linked` whether that interval is
 * short enough to belong to the same walk.
 */
static void candidate_step(uint32_t since, bool linked)
{
	bool rhythmic = false;
	/*
	 * About twice the rhythm: one footfall in between fell under the
	 * threshold. A swinging hand does this -- it lifts one footfall of
	 * each stride and sinks the other. Credit the missed step and keep
	 * the rhythm, rather than break it and lose the run.
	 */
	bool missed_one = false;

	/*
	 * Only once a walk is established, and only at a rhythm a footfall
	 * can have. While a run is still being held the rhythm can lock onto
	 * half a step as the threshold settles, and then every real step
	 * would look like a missed one and count double (+16 % on
	 * stop-and-go walks, found by test_stop_and_go).
	 */
	if (linked && st.walking && st.rhythm_ms >= MISSED_RHYTHM_MIN_MS) {
		uint32_t twice = 2U * st.rhythm_ms;
		uint32_t d2 = (since > twice) ? since - twice : twice - since;

		missed_one = d2 * 100U <= twice * RHYTHM_TOL_PCT;
	}

	if (linked) {
		if (st.rhythm_ms == 0) {
			/* Second candidate of a run: it sets the rhythm. */
			rhythmic = true;
		} else {
			uint32_t d = (since > st.rhythm_ms) ? since - st.rhythm_ms
							    : st.rhythm_ms - since;

			rhythmic = d * 100U <= st.rhythm_ms * RHYTHM_TOL_PCT;
		}
	}

	if (st.walking) {
		if (rhythmic) {
			st.total++;
			st.misses = 0;
			st.rhythm_ms = since;
			note_interval(since);
		} else if (missed_one) {
			st.total += 2;
			st.misses = 0;
		} else if (linked && ++st.misses < RHYTHM_MISSES) {
			/* One off interval, likely a missed footfall: count
			 * the step, keep the rhythm it broke.
			 */
			st.total++;
		} else {
			/* The rhythm is gone. This candidate may start the next run. */
			end_run();
			st.held = 1;
		}
		return;
	}

	if (rhythmic) {
		st.held++;
		st.rhythm_ms = since;
	} else {
		/* Out of step: this candidate starts a fresh run. */
		st.held = 1;
		st.rhythm_ms = 0;
	}

	if (st.held >= REGULATION_STEPS) {
		/* A rhythm: credit the run so far, count from now on. */
		st.total += st.held;
		st.held = 0;
		st.walking = true;
		st.misses = 0;
		st.interval_count = 0;
		st.interval_next = 0;
		note_interval(since);
	}
}

void steps_update(int32_t mg, int64_t now_ms)
{
	if (!st.primed || (now_ms - st.last_sample_ms) > FEED_GAP_LIMIT_MS) {
		st.baseline = mg;
		st.smooth = 0;
		st.amplitude = 0;
		st.win_max = 0;
		st.win_min = 0;
		st.win_start_ms = now_ms;
		st.above = false;
		st.last_step_ms = 0;
		end_run();
		st.last_sample_ms = now_ms;
		st.primed = true;
		return;
	}

	st.last_sample_ms = now_ms;

	st.baseline += (mg - st.baseline) >> BASELINE_SHIFT;

	/*
	 * The level, not its rate of change. Detecting on the difference
	 * (2026-10-15) was meant to favour sharp footfalls over a slow arm
	 * swing, but it lost gentle walks below the amplitude floor entirely
	 * (0 of 900 steps at 90 spm, 100 mg) and only partly fixed the swing.
	 */
	int32_t ac = mg - st.baseline;

	st.smooth += (ac - st.smooth) >> SMOOTH_SHIFT;

	st.win_max = MAX(st.win_max, st.smooth);
	st.win_min = MIN(st.win_min, st.smooth);

	if (now_ms - st.win_start_ms >= AMP_WINDOW_MS) {
		int32_t pp = st.win_max - st.win_min;

		st.amplitude += (pp - st.amplitude) >> 1;
		st.win_max = st.smooth;
		st.win_min = st.smooth;
		st.win_start_ms = now_ms;
	}

	int32_t threshold = (st.amplitude * THRESHOLD_NUM) / THRESHOLD_DEN;

	if (!st.above && st.smooth > threshold &&
	    st.amplitude > AMPLITUDE_FLOOR_MG) {
		st.above = true;

		int64_t since = (st.last_step_ms == 0) ? 0
						: (now_ms - st.last_step_ms);

		if (st.last_step_ms == 0 || since >= STEP_REFRACTORY_MIN_MS) {
			candidate_step((uint32_t)since,
				       st.last_step_ms != 0 &&
				       since <= STEP_INTERVAL_MAX_MS);
			st.last_step_ms = now_ms;
		}
	} else if (st.above && st.smooth < (threshold / 2)) {
		st.above = false;
	}

	/*
	 * Stopped moving, or stopped for longer than a stride: the walk is
	 * over, and anything held without reaching a rhythm was not one.
	 */
	if (st.amplitude <= AMPLITUDE_FLOOR_MG ||
	    (st.last_step_ms != 0 &&
	     (now_ms - st.last_step_ms) > CADENCE_TIMEOUT_MS)) {
		end_run();
	}
}
