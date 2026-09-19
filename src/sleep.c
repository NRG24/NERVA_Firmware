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
 * Longer than this since the last sample and the bucket is not a minute
 * of anything -- RING_CHARGING reads no IMU at all, so a ring that spent
 * an hour on a charger would otherwise come back, close its stale bucket,
 * and have that hour judged as one still minute. A ring on a charger is
 * also not a ring being slept in, so a session in progress ends here.
 *
 * Has to be comfortably above MINUTE_MS: a bucket legitimately stays open
 * for a whole minute plus one poll interval.
 */
#define FEED_GAP_LIMIT_MS	(2 * MINUTE_MS)

/*
 * Negative wear checks needed before a session is called SLEEP_WEAR_NOT_WORN.
 *
 * Two, not one. A single empty reading is not only produced by a ring on a
 * table: a ring that has rotated so the sensor sits off the pad, or one worn
 * loosely enough to break optical contact for a few seconds, reads empty too,
 * and the worn-at-rest DC level has never been measured on this board (only
 * a finger pressed against a bench sensor has). A second check one interval
 * later costs one more window and turns "the sensor saw nothing once" into
 * "the sensor has seen nothing for half an hour", which a hand does not do
 * and a nightstand does.
 *
 * A positive needs no such dwell and gets none -- see SLEEP_WEAR_WORN.
 */
#define WEAR_NEGATIVE_CONFIRM	2

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

	/* Wear corroboration for the current or last session. Saturating,
	 * because the counts are u8 on the wire and a session long enough to
	 * overflow one is already past any question of what it means.
	 */
	bool wear_checked;
	int64_t wear_check_ms;
	uint8_t wear_checks;
	uint8_t wear_confirmed;

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

/* --- wear corroboration ------------------------------------------------ */

bool sleep_wear_check_due(int64_t now_ms, uint32_t interval_ms)
{
	if (!sl.asleep) {
		return false;
	}

	if (!sl.wear_checked) {
		return true;
	}

	/*
	 * Signed on purpose. now_ms going backwards relative to the stored
	 * stamp cannot happen from k_uptime_get(), but this module is fed
	 * whatever the caller passes, and unsigned arithmetic would turn a
	 * small backwards step into an enormous elapsed time and a check on
	 * every pass -- the LEDs held on, at night, which is the one failure
	 * this whole mechanism must not have.
	 */
	return (now_ms - sl.wear_check_ms) >= (int64_t)interval_ms;
}

static void record_check(int64_t now_ms)
{
	sl.wear_checked = true;
	sl.wear_check_ms = now_ms;
}

void sleep_note_wear(bool worn, int64_t now_ms)
{
	if (!sl.asleep) {
		return;
	}

	record_check(now_ms);

	if (sl.wear_checks < UINT8_MAX) {
		sl.wear_checks++;
	}

	if (worn && sl.wear_confirmed < UINT8_MAX) {
		sl.wear_confirmed++;
	}
}

void sleep_note_wear_unavailable(int64_t now_ms)
{
	if (!sl.asleep) {
		return;
	}

	/*
	 * The interval restarts but no counter moves. A check that could not
	 * be taken is not a check that found nothing: counting it would let a
	 * dead PPG talk itself into SLEEP_WEAR_NOT_WORN after
	 * WEAR_NEGATIVE_CONFIRM failures and declare every session a
	 * nightstand.
	 */
	record_check(now_ms);
}

enum sleep_wear sleep_wear_state(void)
{
	if (sl.wear_confirmed > 0) {
		return SLEEP_WEAR_WORN;
	}

	if (sl.wear_checks >= WEAR_NEGATIVE_CONFIRM) {
		return SLEEP_WEAR_NOT_WORN;
	}

	return SLEEP_WEAR_UNKNOWN;
}

uint8_t sleep_wear_checks(void)
{
	return sl.wear_checks;
}

uint8_t sleep_wear_confirmed(void)
{
	return sl.wear_confirmed;
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

			/*
			 * A new session, so the last one's evidence no longer
			 * applies -- cleared here rather than when a session
			 * ends, which is what leaves the verdict readable
			 * after a wake (see sleep_wear_state()).
			 *
			 * wear_checked false makes the first check due
			 * immediately, so the app hears something about a new
			 * session within one window rather than one interval.
			 */
			sl.wear_checked = false;
			sl.wear_check_ms = 0;
			sl.wear_checks = 0;
			sl.wear_confirmed = 0;
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

	if (!sl.primed) {
		open_bucket(now_ms);
		sl.primed = true;
		return;
	}

	if (now_ms - sl.bucket_start_ms > FEED_GAP_LIMIT_MS) {
		/*
		 * The feed stopped for longer than a bucket, so there is no
		 * honest verdict to reach about the time that passed. End any
		 * session rather than extend one across a gap: whatever the
		 * ring was doing, it was not being worn on a sleeping hand.
		 * Minutes already credited to total_minutes stay credited --
		 * that sleep did happen.
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

	evaluate_minute(now_ms, sl.bucket_peak_dev_mg, steps_in_minute);

	open_bucket(now_ms);
}
