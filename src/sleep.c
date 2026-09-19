#include "sleep.h"

#include "steps.h"

#include <zephyr/sys/util.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MINUTE_MS		60000

/*
 * Deviation from 1 g below which a minute counts as still. Loosely matches
 * STILL_MG in main.c, which gates the general motion timeout; this one has
 * a full minute of dwell behind it rather than a single sample, so it can
 * afford to be a little tighter without false-triggering on noise.
 */
#define SLEEP_STILL_MG		100

/*
 * Consecutive still minutes, with no steps, before a session starts. Ten
 * minutes costs the first ten minutes of a real nap but keeps "lying down
 * to read" from registering as sleep onset.
 */
#define SLEEP_ONSET_MINUTES	10

/*
 * Consecutive active minutes before a session ends. A single trip to the
 * bathroom is one or two minutes of activity; this rides through it rather
 * than fragmenting one night into several sessions.
 */
#define WAKE_CONFIRM_MINUTES	3

struct sleep_state {
	bool primed;

	/* current one-minute bucket */
	int64_t bucket_start_ms;
	int32_t bucket_peak_dev_mg;
	uint32_t bucket_step_snapshot;

	bool asleep;
	uint16_t still_run;
	uint16_t active_run;

	int64_t session_start_ms;
	uint16_t session_minutes;
	uint16_t restless_minutes;

	uint32_t total_minutes;
};

static struct sleep_state sl;

void sleep_init(void)
{
	memset(&sl, 0, sizeof(sl));
}

void sleep_reset(void)
{
	uint32_t step_snapshot = sl.bucket_step_snapshot;
	int64_t bucket_start = sl.bucket_start_ms;
	bool was_primed = sl.primed;

	memset(&sl, 0, sizeof(sl));

	/* Keep the bucket boundary and step snapshot: restarting them would
	 * just make the very next bucket look artificially active from a
	 * negative step delta.
	 */
	sl.bucket_step_snapshot = step_snapshot;
	sl.bucket_start_ms = bucket_start;
	sl.primed = was_primed;
}

bool sleep_is_asleep(void)
{
	return sl.asleep;
}

uint32_t sleep_session_start_s(void)
{
	return sl.asleep ? (uint32_t)(sl.session_start_ms / 1000) : 0;
}

uint16_t sleep_session_minutes(void)
{
	return sl.asleep ? sl.session_minutes : 0;
}

uint16_t sleep_total_minutes(void)
{
	return (sl.total_minutes > UINT16_MAX) ? UINT16_MAX
					       : (uint16_t)sl.total_minutes;
}

uint16_t sleep_restless_minutes(void)
{
	return sl.restless_minutes;
}

static void evaluate_minute(int64_t bucket_end_ms, int32_t peak_dev_mg,
			    uint32_t steps_in_minute)
{
	bool minute_still = (peak_dev_mg < SLEEP_STILL_MG) &&
			    (steps_in_minute == 0);

	if (minute_still) {
		sl.still_run++;
		sl.active_run = 0;
	} else {
		sl.active_run++;
		sl.still_run = 0;
	}

	if (!sl.asleep) {
		if (sl.still_run >= SLEEP_ONSET_MINUTES) {
			sl.asleep = true;
			/* Back-date onset to when the stillness actually
			 * started, not to the minute the threshold tripped.
			 */
			sl.session_start_ms = bucket_end_ms -
				(int64_t)SLEEP_ONSET_MINUTES * MINUTE_MS;
			sl.session_minutes = SLEEP_ONSET_MINUTES;
			sl.restless_minutes = 0;
			sl.total_minutes += SLEEP_ONSET_MINUTES;
		}
		return;
	}

	/* Already asleep. */
	if (minute_still) {
		sl.session_minutes++;
		sl.total_minutes++;
		return;
	}

	sl.restless_minutes++;

	if (sl.active_run >= WAKE_CONFIRM_MINUTES) {
		/*
		 * Session over. Back out the minutes counted provisionally
		 * while waiting to see whether stillness would resume --
		 * they were awake, not asleep.
		 */
		uint16_t overcounted = WAKE_CONFIRM_MINUTES - 1;

		sl.session_minutes -= MIN(overcounted, sl.session_minutes);
		sl.total_minutes -= MIN((uint32_t)overcounted, sl.total_minutes);

		sl.asleep = false;
		sl.session_minutes = 0;
		sl.still_run = 0;
		sl.active_run = 0;
	} else {
		/* Provisionally still counts as sleep until the wake is
		 * confirmed, so a single stirring minute does not cost the
		 * session.
		 */
		sl.session_minutes++;
		sl.total_minutes++;
	}
}

void sleep_feed(int32_t mg, int64_t now_ms)
{
	int32_t dev = abs(mg - 1000);

	if (!sl.primed) {
		sl.bucket_start_ms = now_ms;
		sl.bucket_step_snapshot = steps_count();
		sl.primed = true;
		return;
	}

	if (dev > sl.bucket_peak_dev_mg) {
		sl.bucket_peak_dev_mg = dev;
	}

	if (now_ms - sl.bucket_start_ms < MINUTE_MS) {
		return;
	}

	uint32_t steps_now = steps_count();
	uint32_t steps_in_minute = steps_now - sl.bucket_step_snapshot;

	evaluate_minute(now_ms, sl.bucket_peak_dev_mg, steps_in_minute);

	sl.bucket_peak_dev_mg = 0;
	sl.bucket_step_snapshot = steps_now;
	sl.bucket_start_ms = now_ms;
}
