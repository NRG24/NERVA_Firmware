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
 * Longer than this between two samples and the filters are stale: the
 * ring was charging (RING_CHARGING reads no IMU at all) or the part
 * stopped answering. Ramping a 32-sample baseline EMA from a stale value
 * towards a new orientation manufactures a slow swing that looks like
 * motion, so re-prime instead and start clean.
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
	int64_t last_step_ms;
	uint32_t total;

	uint32_t interval_ms[INTERVAL_HISTORY];
	uint8_t interval_count;
	uint8_t interval_next;
};

static struct step_state st;

void steps_init(void)
{
	memset(&st, 0, sizeof(st));
}

void steps_reset(void)
{
	st.total = 0;
	st.interval_count = 0;
	st.interval_next = 0;
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
		st.interval_count = 0;
		st.interval_next = 0;
		st.last_sample_ms = now_ms;
		st.primed = true;
		return;
	}

	st.last_sample_ms = now_ms;

	st.baseline += (mg - st.baseline) >> BASELINE_SHIFT;

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
			if (st.last_step_ms != 0 && since <= STEP_INTERVAL_MAX_MS) {
				st.interval_ms[st.interval_next] = (uint32_t)since;
				st.interval_next = (st.interval_next + 1) %
						   INTERVAL_HISTORY;
				if (st.interval_count < INTERVAL_HISTORY) {
					st.interval_count++;
				}
			} else {
				/* A long gap breaks cadence continuity; the
				 * step itself still counts.
				 */
				st.interval_count = 0;
				st.interval_next = 0;
			}

			st.total++;
			st.last_step_ms = now_ms;
		}
	} else if (st.above && st.smooth < (threshold / 2)) {
		st.above = false;
	}

	if (st.amplitude <= AMPLITUDE_FLOOR_MG ||
	    (st.last_step_ms != 0 &&
	     (now_ms - st.last_step_ms) > CADENCE_TIMEOUT_MS)) {
		st.interval_count = 0;
		st.interval_next = 0;
	}
}
