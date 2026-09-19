#include "calories.h"

#include <zephyr/sys/util.h>

#include <stddef.h>

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

static uint16_t weight_kg_x10 = DEFAULT_WEIGHT_KG_X10;
static uint32_t total_kcal_x1000;

void calories_init(void)
{
	weight_kg_x10 = DEFAULT_WEIGHT_KG_X10;
	total_kcal_x1000 = 0;
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

static uint16_t met_x10_for_cadence(uint16_t cadence_spm)
{
	for (size_t i = 0; i < ARRAY_SIZE(bands); i++) {
		if (cadence_spm <= bands[i].max_cadence_spm) {
			return bands[i].met_x10;
		}
	}

	return bands[ARRAY_SIZE(bands) - 1].met_x10;
}

void calories_update_minute(uint16_t cadence_spm)
{
	uint16_t met_x10 = met_x10_for_cadence(cadence_spm);

	/*
	 * kcal_x1000_per_min = met_x10 * weight_kg_x10 * 175 / 1000
	 *
	 * Derived from kcal/min = MET * weight_kg * 0.0175 (see calories.h)
	 * with met_x10 = MET*10 and weight_kg_x10 = weight_kg*10:
	 *   MET * weight_kg = met_x10 * weight_kg_x10 / 100
	 *   kcal/min * 1000 = (met_x10 * weight_kg_x10 / 100) * 0.0175 * 1000
	 *                    = met_x10 * weight_kg_x10 * 175 / 1000
	 *
	 * Largest possible product (met_x10=50, weight_kg_x10=2500) is
	 * 218,750,000 before the final divide, well inside uint32_t.
	 */
	uint32_t kcal_x1000_per_min =
		((uint32_t)met_x10 * weight_kg_x10 * 175U) / 1000U;

	total_kcal_x1000 += kcal_x1000_per_min;
}

uint32_t calories_total_x1000(void)
{
	return total_kcal_x1000;
}

void calories_reset(void)
{
	total_kcal_x1000 = 0;
}
