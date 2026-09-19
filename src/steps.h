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
 * imu_magnitude_mg()). now_ms should be k_uptime_get(); the detector uses
 * it for the refractory period and cadence, not a fixed sample rate, so it
 * tolerates being called at whatever cadence the caller happens to poll
 * the IMU.
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
