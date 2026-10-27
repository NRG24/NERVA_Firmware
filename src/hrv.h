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
 * ACCURACY. The PPG runs at 100 sps, so a beat found on whole samples
 * is only known to 10 ms, and up to v0.8 that put a floor of ~8.5 ms
 * under RMSSD: a metronome heart with no HRV at all read that much
 * (test_quantisation_noise_floor in tests/test_hrv.c still shows it, for
 * intervals handed over in whole samples). It added in quadrature, so it
 * inflated exactly the low readings -- stress, exertion, illness -- that
 * matter most.
 *
 * Since v0.9.0 hr.c places each peak between samples with a five-point
 * least-squares parabola (peak_offset_q8()), and intervals arrive to the
 * millisecond. On synthetic hearts (test_subsample_timing), RMSSD of a
 * true 3.6 ms, by sensor noise as a share of the pulse swing:
 *
 *   noise                    0     0.5 %   1 %    2 %    3 %
 *   whole samples (<= 0.8)   8.7   7.6     9.8   15.1   17.8
 *   between samples (0.9+)   3.5   3.8     4.1    8.8   12.1
 *
 * So the floor now comes from the sensor's noise, not from the timing.
 * What that noise is on a real finger is not measured yet.
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

/*
 * RMSSD over every successive difference since hrv_session_begin(), not a
 * rolling window: the textbook ultra-short-term figure for one spot
 * reading (spot.h). Independent of the rolling window above, which a
 * reading neither resets nor depends on -- feed both from the same place.
 * Same floor as hrv_rmssd_x10(): 0 under HRV_MIN_DIFFS differences.
 */
struct hrv_session {
	uint64_t sum;		/* squared successive differences, ms^2 */
	uint16_t count;
	uint16_t prev_ibi_ms;
	bool prev_valid;
};

void hrv_session_begin(struct hrv_session *s);
void hrv_session_add(struct hrv_session *s, uint16_t ibi_ms, bool successive);
uint16_t hrv_session_rmssd_x10(const struct hrv_session *s);
uint16_t hrv_session_diffs(const struct hrv_session *s);

#endif /* HRV_H_ */
