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
 * IT ALSO CANNOT TELL WHETHER THE RING IS BEING WORN. A ring left on a
 * nightstand is perfectly still and logs a full night of "sleep" -- this
 * is confirmed behaviour, not a hypothetical. The only wear signal on this
 * board is the PPG DC level, and the power model deliberately stops
 * opening PPG windows after three minutes without motion, which is exactly
 * the case that would need checking. An app that cares has to corroborate
 * a session some other way; see APP_INTEGRATION.md.
 */

#ifndef SLEEP_H_
#define SLEEP_H_

#include <stdbool.h>
#include <stdint.h>

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

/* Clears all counters and returns to the awake state. */
void sleep_reset(void);

#endif /* SLEEP_H_ */
