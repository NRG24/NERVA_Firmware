#include "calories.h"

#include <zephyr/sys/util.h>

#include <stddef.h>
#include <string.h>

#define DEFAULT_WEIGHT_KG_X10	700	/* 70.0 kg */
#define MIN_WEIGHT_KG_X10	200	/* 20.0 kg */
#define MAX_WEIGHT_KG_X10	2500	/* 250.0 kg */

/*
 * MET x10 by cadence upper bound, in steps per minute. 1 MET (x10 = 10) is
 * resting energy expenditure; the walking bands are the Ainsworth
 * Compendium's "walking, various speeds" entries mapped onto typical
 * cadence for each. See calories.h for why this is a rough estimate, not a
 * calibration.
 */
struct met_band {
	uint16_t max_cadence_spm;
	uint16_t met_x10;
};

static const struct met_band bands[] = {
	{  59,  10 },		/* below walking cadence: resting */
	{  79,  20 },		/* slow amble, ~2.0 mph */
	{  99,  28 },		/* easy walk, ~2.5-3.0 mph */
	{ 119,  35 },		/* brisk walk, ~3.5 mph */
	{ 139,  43 },		/* fast/power walk, ~4.0 mph */
	{ UINT16_MAX, 50 },	/* jogging cadence and above */
};

/* Anchors for the partial-minute blend below. MET_WALK_X10 is the band
 * PARTIAL_REF_SPM falls in, so the blend and the table agree exactly at a
 * full minute of ordinary walking.
 */
#define MET_REST_X10		10
#define MET_WALK_X10		28
#define PARTIAL_REF_SPM		99

/* Used by the heart-rate formula when the app has not sent an age. */
#define DEFAULT_AGE_YEARS	35
#define MIN_AGE_YEARS		10
#define MAX_AGE_YEARS		100

/*
 * MET x10 to fall back on for a workout minute with no usable heart rate.
 * Compendium of Physical Activities (2011) values for a middling effort:
 * running at 5 mph (8.3), stationary rowing at 100 W (7.0), general
 * bicycling (7.5). OTHER has no activity behind it and is a mid-range
 * guess, not a table entry.
 */
static const uint8_t workout_fallback_met_x10[] = {
	[WORKOUT_NONE]	= 10,
	[WORKOUT_RUN]	= 83,
	[WORKOUT_ROW]	= 70,
	[WORKOUT_CYCLE]	= 75,
	[WORKOUT_OTHER]	= 50,
};

static uint16_t weight_kg_x10 = DEFAULT_WEIGHT_KG_X10;
static uint8_t age_years;		/* 0 = not given */
static uint8_t sex = CAL_SEX_UNSPECIFIED;
static uint32_t total_kcal_x1000;

static struct calories_workout wk;
static uint32_t wk_hr_sum_x10;		/* for avg_hr_x10 */

void calories_init(void)
{
	weight_kg_x10 = DEFAULT_WEIGHT_KG_X10;
	age_years = 0;
	sex = CAL_SEX_UNSPECIFIED;
	total_kcal_x1000 = 0;
	memset(&wk, 0, sizeof(wk));
	wk_hr_sum_x10 = 0;
}

void calories_set_weight(uint16_t kg_x10)
{
	/*
	 * A malformed or wildly wrong write here poisons every estimate
	 * from this point on, so clamp to a physically plausible range
	 * rather than trusting the app.
	 */
	if (kg_x10 < MIN_WEIGHT_KG_X10) {
		kg_x10 = MIN_WEIGHT_KG_X10;
	} else if (kg_x10 > MAX_WEIGHT_KG_X10) {
		kg_x10 = MAX_WEIGHT_KG_X10;
	}

	weight_kg_x10 = kg_x10;
}

uint16_t calories_weight_kg_x10(void)
{
	return weight_kg_x10;
}

void calories_set_body(uint8_t age, uint8_t sex_code)
{
	if (age != 0) {
		age = CLAMP(age, MIN_AGE_YEARS, MAX_AGE_YEARS);
	}
	age_years = age;

	sex = (sex_code == CAL_SEX_FEMALE || sex_code == CAL_SEX_MALE)
	    ? sex_code : CAL_SEX_UNSPECIFIED;
}

uint8_t calories_age_years(void)
{
	return age_years;
}

uint8_t calories_sex(void)
{
	return sex;
}

static uint16_t met_x10_for_cadence(uint16_t cadence_spm)
{
	for (size_t i = 0; i < ARRAY_SIZE(bands); i++) {
		if (cadence_spm <= bands[i].max_cadence_spm) {
			return bands[i].met_x10;
		}
	}

	return bands[ARRAY_SIZE(bands) - 1].met_x10;
}

/*
 * kcal_x1000_per_min = met_x10 * weight_kg_x10 * 175 / 1000
 *
 * Derived from kcal/min = MET * weight_kg * 0.0175 (see calories.h)
 * with met_x10 = MET*10 and weight_kg_x10 = weight_kg*10:
 *   MET * weight_kg = met_x10 * weight_kg_x10 / 100
 *   kcal/min * 1000 = (met_x10 * weight_kg_x10 / 100) * 0.0175 * 1000
 *                    = met_x10 * weight_kg_x10 * 175 / 1000
 *
 * Largest possible product (met_x10=83, weight_kg_x10=2500) is
 * 36,312,500 before the final divide, well inside uint32_t.
 */
static uint32_t met_x10_minute_x1000(uint16_t met_x10)
{
	return ((uint32_t)met_x10 * weight_kg_x10 * 175U) / 1000U;
}

/* The step formula's price for one minute, kcal x1000. */
static uint32_t step_minute_x1000(uint32_t steps_this_minute)
{
	uint16_t spm = (steps_this_minute > UINT16_MAX)
		     ? UINT16_MAX : (uint16_t)steps_this_minute;
	uint16_t met_x10 = met_x10_for_cadence(spm);

	/*
	 * A minute that was only partly spent walking lands in a low band --
	 * 20 s at 110 spm is 37 steps, which the table calls resting even
	 * though a third of the minute was not. So also price the minute as
	 * a fraction of a walking minute and take whichever is higher:
	 *
	 *   blended MET = 1.0 + (steps / PARTIAL_REF_SPM) * (2.8 - 1.0)
	 *
	 * capped at the reference so a sustained fast walk keeps the higher
	 * band value rather than being pulled down to it. PARTIAL_REF_SPM is
	 * an ordinary walking cadence and MET_WALK_X10 its band, so a full
	 * minute at that cadence gives the same answer either way and the
	 * two agree at the seam.
	 */
	uint32_t partial_steps = MIN(steps_this_minute, (uint32_t)PARTIAL_REF_SPM);
	uint16_t blended_x10 = (uint16_t)(MET_REST_X10 +
		(partial_steps * (MET_WALK_X10 - MET_REST_X10)) / PARTIAL_REF_SPM);

	met_x10 = MAX(met_x10, blended_x10);

	return met_x10_minute_x1000(met_x10);
}

void calories_update_minute(uint32_t steps_this_minute)
{
	calories_update_minute_hr(steps_this_minute, 0);
}

/*
 * Keytel, in integers. Every coefficient is scaled by 1e5 and the inputs
 * arrive x10 (bpm x10, kg x10), so each product is kJ/min x1e5:
 *
 *   0.6309 kJ/min per bpm  = 63090 per bpm  = 6309 per bpm_x10
 *   0.1988 kJ/min per kg   = 19880 per kg   = 1988 per kg_x10
 *   0.2017 kJ/min per year = 20170 per year
 *
 * Largest male sum (250 bpm, 250 kg, 100 y) is about 22.8 million; x10
 * for the kcal conversion below is well inside int32_t.
 */
static int32_t keytel_male_kj_x1e5(int32_t hr_x10, int32_t w_x10, int32_t age)
{
	return -5509690 + 6309 * hr_x10 + 1988 * w_x10 + 20170 * age;
}

static int32_t keytel_female_kj_x1e5(int32_t hr_x10, int32_t w_x10,
				     int32_t age)
{
	return -2040220 + 4472 * hr_x10 - 1263 * w_x10 + 7400 * age;
}

uint32_t calories_keytel_x1000(uint16_t hr_x10)
{
	int32_t age = age_years ? age_years : DEFAULT_AGE_YEARS;
	int32_t kj;

	switch (sex) {
	case CAL_SEX_MALE:
		kj = keytel_male_kj_x1e5(hr_x10, weight_kg_x10, age);
		break;
	case CAL_SEX_FEMALE:
		kj = keytel_female_kj_x1e5(hr_x10, weight_kg_x10, age);
		break;
	default:
		/*
		 * No sex given: the mean of the two. It sits between the
		 * equations rather than picking one, which is the honest
		 * answer to not knowing, and costs at most half the spread.
		 */
		kj = (keytel_male_kj_x1e5(hr_x10, weight_kg_x10, age) +
		      keytel_female_kj_x1e5(hr_x10, weight_kg_x10, age)) / 2;
		break;
	}

	if (kj <= 0) {
		return 0;
	}

	/* kJ x1e5 -> kcal x1000: divide by 4.184 and by 100. */
	return (uint32_t)(((int64_t)kj * 10) / 4184);
}

static uint32_t workout_minute_x1000(uint32_t steps_this_minute,
				     uint16_t hr_x10)
{
	uint32_t by_steps = step_minute_x1000(steps_this_minute);

	if (hr_x10 >= WORKOUT_KEYTEL_MIN_HR_X10) {
		/*
		 * Floored at the step price: a heart rate that reads low
		 * while the wearer is visibly running is more likely the
		 * sensor slipping than the wearer coasting.
		 */
		uint32_t by_hr = MIN(calories_keytel_x1000(hr_x10),
				     (uint32_t)WORKOUT_MAX_KCAL_X1000);

		wk.hr_minutes++;
		wk_hr_sum_x10 += hr_x10;
		wk.avg_hr_x10 = (uint16_t)(wk_hr_sum_x10 / wk.hr_minutes);

		return MAX(by_hr, by_steps);
	}

	if (hr_x10 > 0) {
		wk.rest_minutes++;
		return by_steps;
	}

	wk.fallback_minutes++;

	uint8_t type = wk.type < ARRAY_SIZE(workout_fallback_met_x10)
		     ? wk.type : WORKOUT_OTHER;

	return MAX(met_x10_minute_x1000(workout_fallback_met_x10[type]),
		   by_steps);
}

void calories_update_minute_hr(uint32_t steps_this_minute, uint16_t hr_x10)
{
	if (!wk.active) {
		total_kcal_x1000 += step_minute_x1000(steps_this_minute);
		return;
	}

	uint32_t kcal = workout_minute_x1000(steps_this_minute, hr_x10);

	if (wk.minutes < UINT16_MAX) {
		wk.minutes++;
	}
	wk.kcal_x1000 += kcal;
	total_kcal_x1000 += kcal;
}

void calories_workout_start(uint8_t type)
{
	if (type == WORKOUT_NONE) {
		calories_workout_stop();
		return;
	}

	if (type >= ARRAY_SIZE(workout_fallback_met_x10)) {
		type = WORKOUT_OTHER;
	}

	if (!wk.active) {
		memset(&wk, 0, sizeof(wk));
		wk_hr_sum_x10 = 0;
		wk.active = true;
	}

	wk.type = type;
}

void calories_workout_stop(void)
{
	wk.active = false;
}

bool calories_workout_active(void)
{
	return wk.active;
}

void calories_workout_get(struct calories_workout *out)
{
	*out = wk;
}

uint32_t calories_total_x1000(void)
{
	return total_kcal_x1000;
}

void calories_reset(void)
{
	total_kcal_x1000 = 0;
}
