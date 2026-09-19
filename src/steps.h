/*
 * Step counter (pedometer) driven by LSM6DSV accelerometer magnitude.
 *
 * Same technique as hr.c applied to motion instead of PPG: a slow EMA
 * tracks the ~1000 mg gravity baseline (which drifts with how the ring
 * sits on the finger), a fast EMA tracks the swing around it, and a step
 * is a rising crossing of an amplitude-adaptive threshold with a
 * refractory period sized to human cadence.
 *
 * UNVALIDATED, same caveat as hr.c: this has never been checked against a
 * known step count on the actual board, and a finger-worn ring does not
 * move the way a wrist or waist does. Treat the thresholds in steps.c as
 * a starting point, not a calibration.
 */

#ifndef STEPS_H_
#define STEPS_H_

#include <stdint.h>

void steps_init(void);

/*
 * Feed one accelerometer magnitude sample (milli-g, as returned by
 * imu_magnitude_mg()). now_ms should be k_uptime_get(): the refractory
 * period and cadence are measured in real time rather than in samples, so
 * the caller's polling rate may vary.
 *
 * THE RATE STILL HAS TO BE FAST ENOUGH. Walking is 1.5-2.5 Hz and a
 * footfall is a narrow peak, so sampling near Nyquist does not merely
 * add noise, it loses steps outright. Simulated against a 5 min walk
 * (90-130 spm, second harmonic and noise, magnitude swing in brackets):
 *
 *     sample interval   result
 *     20-60 ms          within 1 % of the true count [>=100 mg swing]
 *     80-100 ms         within 2 %, first misses appear [100 mg swing]
 *     150 ms            unreliable above ~110 spm
 *     200 ms            counts essentially nothing below a 250 mg swing
 *
 * So the caller must poll at 60 ms or faster while the ring is moving.
 * main.c does that by dropping its idle poll interval to STEP_POLL_MS
 * whenever recent motion says the wearer may be walking, and going back
 * to the slow idle rate once they are still -- which costs nothing at
 * night, when there are no steps to miss.
 */
void steps_update(int32_t mg, int64_t now_ms);

/* Total steps counted since boot or the last steps_reset(). */
uint32_t steps_count(void);

/*
 * Steps per minute, from the most recent step-to-step intervals. Decays
 * to 0 a few seconds after motion stops (see CADENCE_TIMEOUT_MS).
 */
uint16_t steps_cadence_spm(void);

/* Zero the step count. Does not disturb the gravity baseline, so
 * detection quality is unaffected by when this is called.
 */
void steps_reset(void);

#endif /* STEPS_H_ */
