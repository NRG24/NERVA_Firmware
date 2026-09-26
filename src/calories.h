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

#include <stdbool.h>
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

/*
 * Age and sex, which the heart-rate formula below needs and the step
 * formula does not. Both default to "not given": age is then taken as
 * DEFAULT_AGE_YEARS and sex as the average of the two equations.
 */
#define CAL_SEX_UNSPECIFIED	0
#define CAL_SEX_FEMALE		1
#define CAL_SEX_MALE		2

/* Age clamped to 10-100; an unknown sex code reads as unspecified. */
void calories_set_body(uint8_t age_years, uint8_t sex);
uint8_t calories_age_years(void);	/* 0 = not given */
uint8_t calories_sex(void);

/* --- workouts ----------------------------------------------------------
 *
 * The step formula above only understands walking. Rowing, cycling and
 * most of running's intensity are invisible to it -- a rowing machine is
 * a wearer sitting still. What does see them is heart rate, so during a
 * workout the app has started, each minute is priced from heart rate
 * instead, with the Keytel et al. (2005) equations:
 *
 *   male    kJ/min = -55.0969 + 0.6309 HR + 0.1988 kg + 0.2017 age
 *   female  kJ/min = -20.4022 + 0.4472 HR - 0.1263 kg + 0.0740 age
 *
 * (J Sports Sci 23(3):289-297), the version that needs no VO2max. They
 * were fitted to submaximal exercise and say nothing useful about a
 * resting heart, so each workout minute takes one of three paths:
 *
 *   HR >= 90 bpm       Keytel, never below what the steps alone would
 *                      say, never above WORKOUT_MAX_KCAL_X1000 a minute.
 *   HR below that      the ordinary step formula -- the wearer is resting
 *                      between sets, and Keytel would price that as
 *                      exercise.
 *   no usable HR       the activity's typical MET from the Compendium of
 *                      Physical Activities, or the step formula if that
 *                      is higher. Counted separately, so an app can see
 *                      how much of a workout was a guess.
 *
 * Workouts are only ever started by the app. Nothing here detects
 * exercise on its own, which is deliberate: the ring's everyday power
 * budget does not change, and a workout costs its LED time only when
 * the wearer asked for one.
 *
 * UNVALIDATED twice over: the equations have an error of their own (they
 * are population regressions, not a calibration of this wearer), and
 * they rest on a heart rate from a finger-worn PPG that has never been
 * compared to a chest strap -- let alone while gripping a rowing handle.
 */

#define WORKOUT_NONE		0
#define WORKOUT_RUN		1
#define WORKOUT_ROW		2
#define WORKOUT_CYCLE		3
#define WORKOUT_OTHER		4

/* Heart rate at or above which a workout minute is priced by Keytel. */
#define WORKOUT_KEYTEL_MIN_HR_X10	900

/* 20 kcal/min: above any sustained effort, below any arithmetic accident. */
#define WORKOUT_MAX_KCAL_X1000		20000

struct calories_workout {
	bool active;
	uint8_t type;			/* WORKOUT_* */
	uint16_t minutes;		/* every minute of the workout */
	uint16_t hr_minutes;		/* priced from heart rate */
	uint16_t rest_minutes;		/* HR below the threshold: steps */
	uint16_t fallback_minutes;	/* no HR: the activity's MET */
	uint16_t avg_hr_x10;		/* over hr_minutes; 0 if none */
	uint32_t kcal_x1000;		/* this workout only */
};

/*
 * Start a workout, discarding the last one's figures. An unknown type is
 * taken as WORKOUT_OTHER; WORKOUT_NONE stops instead. Starting while one
 * is already running changes its type and keeps its figures.
 */
void calories_workout_start(uint8_t type);

/* Stop. The figures stay readable until the next start. */
void calories_workout_stop(void);

bool calories_workout_active(void);
void calories_workout_get(struct calories_workout *out);

/*
 * The per-minute tick with heart rate. hr_x10 is the mean heart rate over
 * the minute in bpm x10, or 0 when too little of the minute had one.
 * Outside a workout the heart rate is ignored and this is exactly
 * calories_update_minute().
 */
void calories_update_minute_hr(uint32_t steps_this_minute, uint16_t hr_x10);

/* Keytel's kcal x1000 per minute for this wearer, unclamped below at 0.
 * Exposed for the host tests; calories_update_minute_hr() is the caller.
 */
uint32_t calories_keytel_x1000(uint16_t hr_x10);

/* Kilocalories x1000, accumulated since boot or the last calories_reset(). */
uint32_t calories_total_x1000(void);

void calories_reset(void);

#endif /* CALORIES_H_ */
