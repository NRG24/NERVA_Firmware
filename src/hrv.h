/*
 * RMSSD -- the root mean square of successive differences between
 * inter-beat intervals, and the most commonly quoted short-window HRV
 * measure.
 *
 * Fed from hr.c's beat detector, but kept in its own module with its own
 * state for one specific reason: hr_init() runs at the top of every
 * measurement window, and the power model opens a window for only 15 s in
 * every 60. RMSSD conventionally wants 30-60 s of beats, so anything
 * living inside struct hr would be wiped before it could ever collect
 * enough. This window survives across measurement windows instead.
 *
 * That makes it NOT a textbook 60-second RMSSD. It is the RMS of the last
 * HRV_WINDOW successive differences, which at a 25 % duty cycle may span
 * several minutes of wall time. Differences are only ever formed between
 * two intervals that were genuinely adjacent (see hrv_add_interval), so no
 * value is manufactured across a gap -- but the span is longer than the
 * literature's, and a reading is an average over it.
 *
 * ACCURACY CEILING, which is a property of the hardware and not of this
 * code: beats are located to the nearest PPG sample, and the PPG runs at
 * 100 sps, so every interval is an exact multiple of 10 ms. That
 * quantisation alone puts a noise floor under RMSSD -- a metronome-steady
 * heart with no HRV at all measures about 8.5 ms of it, which
 * tests/test_hrv.c checks rather than assumes.
 *
 * It adds in quadrature, so a true 40 ms reads about 41 and a true 20
 * reads about 22: readings from a relaxed subject are barely affected,
 * and low-HRV readings -- stress, exertion, illness, the ones a user
 * would most want to trust -- are inflated the most, and proportionally
 * the most. Sampling the PPG faster (the part supports 400 sps) or
 * interpolating the peak is the only way to do better, and both are out
 * of scope here.
 */

#ifndef HRV_H_
#define HRV_H_

#include <stdbool.h>
#include <stdint.h>

void hrv_init(void);

/*
 * Feed one accepted inter-beat interval, in milliseconds.
 *
 * Only pass intervals hr.c reports as trusted -- a mis-detected beat hurts
 * RMSSD far more than it hurts a heart rate, because the error enters
 * squared and twice, once in each of the two differences that interval
 * takes part in.
 *
 * `successive` says whether this interval and the one before it were
 * genuinely adjacent, which is what a "successive difference" means. It
 * has to come from hr.c: a beat that was detected and then rejected still
 * moves the beat clock, so the interval after it is measured from a
 * suspect beat and is not adjacent to anything. When it is false this
 * interval starts a new run rather than forming a difference.
 */
void hrv_add_interval(uint16_t ibi_ms, bool successive);

/*
 * RMSSD in milliseconds x10, or 0 when too few differences have been
 * collected to mean anything (see HRV_MIN_DIFFS in hrv.c).
 */
uint16_t hrv_rmssd_x10(void);

/*
 * How many successive differences the current value is computed over. N
 * differences come from N+1 consecutive accepted beats. Exposed so an app
 * can apply its own bar -- the firmware's floor is deliberately low, and
 * well under the 30-60 s the literature asks for.
 */
uint8_t hrv_diffs(void);

/* Drop the window. Nothing calls this in normal operation. */
void hrv_reset(void);

#endif /* HRV_H_ */
