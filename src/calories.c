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

/* Anchors for the partial-minute blend below. MET_WALK_X10 is the band
 * PARTIAL_REF_SPM falls in, so the blend and the table agree exactly at a
 * full minute of ordinary walking.
 */
#define MET_REST_X10		10
#define MET_WALK_X10		28
#define PARTIAL_REF_SPM		99

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

void calories_update_minute(uint32_t steps_this_minute)
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
	 * 21,875,000 before the final divide, well inside uint32_t.
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
