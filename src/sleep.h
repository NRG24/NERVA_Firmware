/*
 * Sleep session tracker, from accelerometer stillness and step count.
 *
 * There is no RTC on this board (see README, "No 32.768 kHz crystal"), so
 * everything here is a duration, never a clock time. An app that wants to
 * show when a session started subtracts sleep_session_minutes() from its
 * own clock at the moment it reads.
 *
 * Algorithm: samples are bucketed into one-minute windows. A minute counts
 * as "still" when the peak deviation from 1 g stays below a threshold and
 * no steps landed in it. Ten consecutive still minutes start a session;
 * three consecutive active minutes end one. The dwell on both ends is
 * deliberate -- reading in bed or a bathroom trip should not look like
 * sleep onset or a full wake-up. A gap in the samples ends a session
 * outright, because the ring reads no accelerometer at all while charging
 * and an hour of that is not an hour of anything.
 *
 * UNVALIDATED, same caveat as steps.c and hr.c: plausible, not compared to
 * a reference (a sleep-stage tracker or even a simple actigraphy log).
 * This can only ever report "still for a while" vs "not"; it has no way to
 * distinguish sleep from, say, sitting motionless at a desk.
 *
 * WEAR DETECTION IS NOT IN HERE, AND THAT IS ON PURPOSE. Stillness alone
 * cannot tell a sleeping hand from a nightstand, so a session is only as
 * trustworthy as whatever corroborates it. The corroboration is a PPG DC
 * reading, which lives behind an I2C device, a 5 V boost and a power
 * model -- none of which belong in a pure logic module. main.c owns the
 * sensor and the schedule; this file only keeps the tally, through
 * sleep_note_wear*() below, and hands back a verdict it never acts on.
 * See main.c, "Sleep wear checks", for the power cost of taking those
 * readings and why the verdict is reported rather than enforced.
 */

#ifndef SLEEP_H_
#define SLEEP_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Whether the ring was confirmed to be on a hand during the current (or
 * most recently closed) sleep session.
 *
 * These values are on the wire -- they are what the app reads out of the
 * activity characteristic -- so they are pinned to the RING_WEAR_* wire
 * constants in ble.h by a BUILD_ASSERT in main.c. Do not renumber one
 * without the other.
 */
enum sleep_wear {
	/*
	 * No check has produced an answer yet, or too few have to call it
	 * either way. NOT a synonym for "not worn": a dead or absent PPG
	 * parks every session here forever, and so does the first check of
	 * a session that is about to come back worn.
	 */
	SLEEP_WEAR_UNKNOWN = 0,

	/*
	 * At least one check during this session saw a PPG DC level above
	 * the finger threshold. One is enough: empty reads ~2,860 against
	 * ~53,000 on skin (hr.c), so a positive has a factor of five of
	 * headroom over anything an unworn ring can produce.
	 */
	SLEEP_WEAR_WORN = 1,

	/*
	 * Several checks in a row saw nothing on the sensor. This is the
	 * nightstand case, and it is the only thing that has ever been
	 * measured about it -- the empty-sensor DC level. The worn-at-rest
	 * level has NOT been measured on a sleeping hand, only on a finger
	 * deliberately pressed against the sensor on a bench, so treat this
	 * as strong evidence rather than proof.
	 */
	SLEEP_WEAR_NOT_WORN = 2,
};

void sleep_init(void);

/*
 * Feed one accelerometer magnitude sample (milli-g) and the current
 * uptime. Buckets internally into one-minute windows; only evaluates a
 * sleep/wake transition when a window closes, so calling this at whatever
 * rate the IMU happens to be polled is fine.
 */
void sleep_feed(int32_t mg, int64_t now_ms);

bool sleep_is_asleep(void);

/* Minutes into the current sleep session, 0 while awake. */
uint16_t sleep_session_minutes(void);

/* Minutes asleep since boot or the last sleep_reset(), including however
 * much of an in-progress session has been confirmed so far.
 */
uint16_t sleep_total_minutes(void);

/*
 * Minutes within the current (or most recently closed) session that saw
 * motion without being enough to end it -- a coarse restlessness count,
 * not a sleep-stage estimate.
 */
uint16_t sleep_restless_minutes(void);

/* --- wear corroboration ------------------------------------------------
 *
 * Bookkeeping only. Nothing in here reads a sensor, changes the sleep
 * state, or withholds a session: the caller takes the readings, the
 * tally is kept here, and the verdict goes out over BLE for the app to
 * weigh. See sleep_wear_state() for why it is reported and not enforced.
 */

/*
 * True when a session is open and interval_ms has passed since the last
 * recorded check (or no check has been recorded in this session yet).
 * Purely a question -- it changes nothing, so a caller that cannot act on
 * a due check may simply not call sleep_note_wear*() and be asked again
 * on the next pass.
 *
 * interval_ms is the caller's policy, not this module's: how often a
 * check is worth its LED current is a power decision that belongs with
 * the power model.
 */
bool sleep_wear_check_due(int64_t now_ms, uint32_t interval_ms);

/*
 * Record one completed check. `worn` is the caller's reading of the PPG
 * DC level against the finger threshold, taken after the front end has
 * settled -- see main.c, which will not record a window too short to
 * trust.
 *
 * Ignored unless a session is open: a check taken while awake says
 * nothing about a session that has not started, and one taken after a
 * session closed would contaminate the verdict for the session the app
 * is still reading.
 */
void sleep_note_wear(bool worn, int64_t now_ms);

/*
 * Record that a check was attempted and could not be taken -- the PPG did
 * not answer, the boost failed, the window was cut short. Costs the
 * session nothing either way but restarts the interval, so a dead sensor
 * is retried every interval_ms rather than on every pass through the
 * main loop.
 */
void sleep_note_wear_unavailable(int64_t now_ms);

/*
 * The verdict for the current (or most recently closed) session. Cleared
 * when a new session starts, not when one ends, so an app that reads
 * after a wake still sees what the night was worth -- same lifetime as
 * sleep_restless_minutes().
 *
 * DELIBERATELY ADVISORY. A SLEEP_WEAR_NOT_WORN session is still reported
 * as sleep, with its minutes still in sleep_total_minutes(). Suppressing
 * it here would trade a false positive that the app can see and filter
 * for a false negative it cannot: the finger threshold this verdict rests
 * on was measured with a finger held against a bench sensor, never with a
 * ring worn loosely on a sleeping hand, so a real night discarded in
 * firmware would be gone with nothing to show that it happened.
 */
enum sleep_wear sleep_wear_state(void);

/* How many checks have been recorded in this session, and how many of
 * those found the ring worn. Both saturate at 255. Exposed so the app can
 * weigh the verdict rather than take it on faith: "worn on 1 of 14" and
 * "worn on 14 of 14" reach the same verdict and do not mean the same
 * thing.
 */
uint8_t sleep_wear_checks(void);
uint8_t sleep_wear_confirmed(void);

/* Clears all counters and returns to the awake state. */
void sleep_reset(void);

#endif /* SLEEP_H_ */
