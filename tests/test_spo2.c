/*
 * SpO2 tests.
 *
 * The percentage is uncalibrated and cannot be tested against truth — no
 * amount of arithmetic makes an unfitted curve correct. What CAN be
 * tested, and is what these do:
 *
 *   - the ratio of ratios comes out right for a signal with a known
 *     AC/DC on each channel, because R is real physics and not a fit;
 *   - the uncalibrated flag is set on every path, including the ones that
 *     report nothing;
 *   - implausible inputs report nothing rather than a number.
 *
 * R is the value to compare against a reference oximeter when the
 * calibration campaign eventually happens, so it is the one that has to be
 * arithmetically right today.
 */

#include "sim.h"

#include "spo2.h"

#include <zephyr/sys/util.h>	/* the stub under tests/stubs */

#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Feed `seconds` of synthetic PPG at 100 sps with the given DC levels and
 * peak-to-peak swings. A sine is enough here: spo2.c measures peak-to-peak
 * over a window, so the waveform shape does not matter, only its extent.
 */
static void feed(double red_dc, double red_pp, double ir_dc, double ir_pp,
		 int seconds)
{
	const int rate = 100;
	const double beat_hz = 1.0;	/* 60 bpm */

	for (int i = 0; i < seconds * rate; i++) {
		double ph = 2.0 * M_PI * beat_hz * i / rate;
		double red = red_dc + (red_pp / 2.0) * sin(ph);
		double ir = ir_dc + (ir_pp / 2.0) * sin(ph);

		spo2_feed((uint32_t)red, (uint32_t)ir);
	}
}

/* R as the definition gives it, for the test to compare against. */
static double ratio_reference(double red_dc, double red_pp, double ir_dc,
			      double ir_pp)
{
	return (red_pp / red_dc) / (ir_pp / ir_dc);
}

static void test_ratio_is_arithmetically_right(void)
{
	const struct {
		double red_dc, red_pp, ir_dc, ir_pp;
	} cases[] = {
		/* Equal perfusion in both channels: R = 1 by construction. */
		{ 100000, 2000, 100000, 2000 },
		/* Red less pulsatile than IR: R below 1, the healthy end. */
		{ 100000, 1000, 100000, 2000 },
		/* Different DC levels, which must divide out. */
		{  60000, 1200, 120000, 2400 },
		/* Red more pulsatile: R above 1, the desaturated end. */
		{ 100000, 3000, 100000, 2000 },
	};

	sim_section("ratio of ratios matches the definition");

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct spo2_result r;
		char label[96];
		double want;

		spo2_init();
		feed(cases[i].red_dc, cases[i].red_pp, cases[i].ir_dc,
		     cases[i].ir_pp, 8);
		spo2_result(&r);

		want = ratio_reference(cases[i].red_dc, cases[i].red_pp,
				       cases[i].ir_dc, cases[i].ir_pp) * 1000.0;

		snprintf(label, sizeof(label),
			 "R for red %.0f/%.0f, ir %.0f/%.0f",
			 cases[i].red_pp, cases[i].red_dc, cases[i].ir_pp,
			 cases[i].ir_dc);

		CHECK(r.flags & SPO2_FLAG_VALID, "%s: not marked valid", label);
		CHECK_NEAR(r.ratio_x1000, want, 8, label);
	}
}

/*
 * The one rule that must never break, whatever else does: the firmware
 * must not hand an app a percentage that looks calibrated.
 */
static void test_uncalibrated_flag_is_always_set(void)
{
	struct spo2_result r;

	sim_section("the uncalibrated flag is set on every path");

	/* Nothing fed at all. */
	spo2_init();
	spo2_result(&r);
	CHECK(r.flags & SPO2_FLAG_UNCALIBRATED,
	      "flag missing with no data (flags 0x%02x)", r.flags);
	CHECK(r.percent == 0, "reported %u%% with no data", r.percent);
	CHECK(r.ratio_x1000 == 0, "reported a ratio with no data");

	/* An empty sensor: DC far below the finger threshold. */
	spo2_init();
	feed(3000, 100, 3000, 100, 8);
	spo2_result(&r);
	CHECK(r.flags & SPO2_FLAG_UNCALIBRATED,
	      "flag missing on an empty sensor");
	CHECK(!(r.flags & SPO2_FLAG_VALID),
	      "an empty sensor was marked valid");
	CHECK(r.percent == 0, "reported %u%% from an empty sensor", r.percent);

	/* A good signal. */
	spo2_init();
	feed(100000, 1500, 100000, 2000, 8);
	spo2_result(&r);
	CHECK(r.flags & SPO2_FLAG_UNCALIBRATED,
	      "flag missing on a good reading, which is the dangerous case");
	CHECK(r.flags & SPO2_FLAG_VALID, "a good signal was not marked valid");
	CHECK(r.percent > 0, "a good signal produced no percentage");
}

static void test_rejects_what_it_cannot_measure(void)
{
	struct spo2_result r;

	sim_section("implausible inputs report nothing, not a guess");

	/* Finger on, but no pulse at all: AC below the floor. */
	spo2_init();
	feed(100000, 4, 100000, 4, 8);
	spo2_result(&r);
	CHECK(!(r.flags & SPO2_FLAG_VALID),
	      "a flat trace was marked valid");
	CHECK(r.percent == 0, "a flat trace produced %u%%", r.percent);

	/*
	 * An R far outside anything physiological. The curve would put this
	 * well below 70 %, and reporting it would be worse than reporting
	 * nothing: a number in the 50s reads as a medical emergency.
	 */
	spo2_init();
	feed(100000, 9000, 100000, 1000, 8);
	spo2_result(&r);
	CHECK(r.ratio_x1000 > 0,
	      "the ratio itself should still be reported for diagnosis");
	CHECK(r.percent == 0,
	      "an R of %u.%03u produced %u%%, which is outside the "
	      "reportable range and should have been withheld",
	      r.ratio_x1000 / 1000, r.ratio_x1000 % 1000, r.percent);
}

/*
 * A plausible-looking reading should land somewhere plausible. This is not
 * an accuracy test -- it cannot be -- but a sanity check that the curve is
 * wired up the right way round, because a sign error would read high when
 * it should read low and nothing else here would notice.
 */
static void test_curve_points_the_right_way(void)
{
	struct spo2_result low_r, high_r;

	sim_section("a higher ratio means a lower saturation");

	spo2_init();
	feed(100000, 1000, 100000, 2000, 8);	/* R = 0.5 */
	spo2_result(&low_r);

	spo2_init();
	feed(100000, 1800, 100000, 2000, 8);	/* R = 0.9 */
	spo2_result(&high_r);

	CHECK(low_r.percent > 0 && high_r.percent > 0,
	      "expected both to report (%u%%, %u%%)", low_r.percent,
	      high_r.percent);
	CHECK(low_r.percent > high_r.percent,
	      "R=0.5 gave %u%% and R=0.9 gave %u%% -- the curve is inverted",
	      low_r.percent, high_r.percent);

	printf("    R=0.5 -> %u%%, R=0.9 -> %u%% (uncalibrated curve)\n",
	       low_r.percent, high_r.percent);
}

int main(void)
{
	printf("SpO2 tests\n");

	test_ratio_is_arithmetically_right();
	test_uncalibrated_flag_is_always_set();
	test_rejects_what_it_cannot_measure();
	test_curve_points_the_right_way();

	return sim_report("test_spo2");
}
