/*
 * Rough energy-expenditure estimate from step cadence.
 *
 * kcal/min = MET * body weight (kg) * 0.0175, where the 0.0175 comes from
 * the standard ACSM metabolic equation: VO2 (ml/kg/min) = MET * 3.5, and
 * roughly 5 kcal are burned per litre of O2 consumed, so
 *   kcal/min = (MET * 3.5 * weight_kg / 1000) * 5 = MET * weight_kg * 0.0175
 *
 * MET is looked up from steps per minute against bands taken from the
 * Ainsworth Compendium of Physical Activities, not measured on this board.
 * Combined with steps.c's own "unvalidated" pedometer, this is a plausible
 * order-of-magnitude estimate and nothing more -- it has never been
 * checked against indirect calorimetry or even a commercial fitness band
 * on the same wearer.
 *
 * It also only sees stepping. Cycling, rowing, carrying a load up a
 * staircase and lying still all look identical to it, which is the same
 * blind spot every step-driven estimate has.
 */

#ifndef CALORIES_H_
#define CALORIES_H_

#include <stdint.h>

void calories_init(void);

/*
 * Body weight in kilograms x10 (e.g. 700 = 70.0 kg). This is the one input
 * the formula above needs and the firmware has no way to know it, so it
 * defaults to 70.0 kg until the app sets a real one via the BLE control
 * characteristic. Clamped to a sane 20.0-250.0 kg range.
 */
void calories_set_weight(uint16_t weight_kg_x10);

/* The weight in use, after clamping -- which is what profile.c persists. */
uint16_t calories_weight_kg_x10(void);

/*
 * Call once a minute with the number of steps taken since the last call.
 *
 * Deliberately NOT steps_cadence_spm(): that is an instantaneous rate, and
 * sampling it once a minute makes the whole minute inherit whatever the
 * wearer happened to be doing at the instant the tick landed. Simulated
 * over ten minutes of 20 s walking / 40 s rest, the same activity came out
 * as 42.0 kcal when the tick landed mid-stride and 12.3 kcal when it
 * landed during a rest -- a 3.4x spread from nothing but tick phase. A
 * step count over the interval is an average by construction and has no
 * such phase sensitivity.
 */
void calories_update_minute(uint32_t steps_this_minute);

/* Kilocalories x1000, accumulated since boot or the last calories_reset(). */
uint32_t calories_total_x1000(void);

void calories_reset(void);

#endif /* CALORIES_H_ */
