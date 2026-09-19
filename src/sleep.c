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

/*
 * A gap this long between two SAMPLES means the feed stopped: RING_CHARGING
 * reads no IMU at all, so a ring that spent time on a charger would
 * otherwise come back, close its stale bucket, and have the whole outage
 * judged as one still minute. A ring on a charger is also not a ring being
 * slept in, so a session in progress ends here.
 *
 * Measured from the last sample, NOT from the start of the bucket. Against
 * the bucket the shortest detectable outage is a whole bucket long, so a
 * gap of one to two minutes -- a short charge, or the IMU dropping out and
 * recovering -- slipped through and was credited as ordinary stillness.
 *
 * 15 s is chosen to sit above every legitimate pause and far below a
 * bucket. The main loop feeds every 20-200 ms; the worst documented
 * un-fed span on a stalled I2C bus is around 4.5 s (see the blocking audit
 * in main.c), and anything approaching 10 s trips the watchdog and reboots
 * the board anyway.
 */
#define FEED_GAP_LIMIT_MS	15000

struct sleep_state {
	bool primed;

	/* current one-minute bucket */
	int64_t bucket_start_ms;
	int64_t last_sample_ms;
	int32_t bucket_peak_dev_mg;
	uint32_t bucket_step_snapshot;

	bool asleep;
	uint16_t still_run;
	uint16_t active_run;

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
	int64_t bucket_start = sl.bucket_start_ms;
	int64_t last_sample = sl.last_sample_ms;
	bool was_primed = sl.primed;

	memset(&sl, 0, sizeof(sl));

	/* Keep the bucket boundary -- restarting it would stretch the
	 * current bucket to nearly two minutes. The step snapshot is
	 * re-taken rather than kept, because the caller resets the step
	 * counter alongside this and a snapshot from before that reset is
	 * larger than the count it will be subtracted from.
	 */
	sl.bucket_step_snapshot = steps_count();
	sl.bucket_start_ms = bucket_start;
	sl.last_sample_ms = last_sample;
	sl.primed = was_primed;
}

bool sleep_is_asleep(void)
{
	return sl.asleep;
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

static void evaluate_minute(int32_t peak_dev_mg, uint32_t steps_in_minute)
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
			/*
			 * The session starts when the stillness did, not when
			 * the threshold tripped, so it opens already holding
			 * the minutes that earned it rather than counting up
			 * from zero ten minutes late.
			 */
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

		/* session_minutes needs no correction of its own: the
		 * session is ending this minute regardless, so it is
		 * zeroed below either way.
		 */
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

/* Start a fresh bucket at now_ms with nothing carried over from the last. */
static void open_bucket(int64_t now_ms)
{
	sl.bucket_start_ms = now_ms;
	sl.bucket_peak_dev_mg = 0;
	sl.bucket_step_snapshot = steps_count();
}

void sleep_feed(int32_t mg, int64_t now_ms)
{
	int32_t dev = abs(mg - 1000);
	int64_t since_sample = now_ms - sl.last_sample_ms;

	sl.last_sample_ms = now_ms;

	if (!sl.primed) {
		open_bucket(now_ms);
		sl.primed = true;
		return;
	}

	if (since_sample > FEED_GAP_LIMIT_MS) {
		/*
		 * The feed stopped, so there is no honest verdict to reach
		 * about the time that passed. End any session rather than
		 * extend one across a gap: whatever the ring was doing, it was
		 * not being worn on a sleeping hand. Minutes already credited
		 * to total_minutes stay credited -- that sleep did happen.
		 */
		sl.asleep = false;
		sl.session_minutes = 0;
		sl.still_run = 0;
		sl.active_run = 0;
		open_bucket(now_ms);
		return;
	}

	if (dev > sl.bucket_peak_dev_mg) {
		sl.bucket_peak_dev_mg = dev;
	}

	if (now_ms - sl.bucket_start_ms < MINUTE_MS) {
		return;
	}

	uint32_t steps_now = steps_count();
	/*
	 * Clamped rather than subtracted blind: steps_count() is reset from
	 * the control characteristic, and a snapshot taken before that reset
	 * exceeds the count it is subtracted from. Unsigned, that wraps to
	 * billions and reads as the most active minute ever recorded.
	 */
	uint32_t steps_in_minute = (steps_now >= sl.bucket_step_snapshot)
				 ? (steps_now - sl.bucket_step_snapshot) : 0;

	evaluate_minute(sl.bucket_peak_dev_mg, steps_in_minute);

	open_bucket(now_ms);
}
