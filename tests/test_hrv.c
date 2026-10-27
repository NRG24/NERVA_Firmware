/*
 * RMSSD tests.
 *
 * The arithmetic is checkable against hand-computed values, which most of
 * this firmware's signal processing is not, so this suite leans on that:
 * feed a known interval sequence, compare against the textbook definition
 * computed in floating point here in the test. If the integer pipeline
 * drifts from the definition, these fail.
 */

#include "sim.h"

#include "hr.h"
#include "hrv.h"

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>

/* The definition, in floating point, for the tests to compare against. */
static double rmssd_reference(const uint16_t *ibi, int n)
{
	double sum = 0;

	for (int i = 1; i < n; i++) {
		double d = (double)ibi[i] - (double)ibi[i - 1];

		sum += d * d;
	}

	return sqrt(sum / (n - 1));
}

static void feed_all(const uint16_t *ibi, int n)
{
	hrv_init();
	for (int i = 0; i < n; i++) {
		hrv_add_interval(ibi[i], i > 0);
	}
}

static void test_matches_the_definition(void)
{
	sim_section("RMSSD matches the textbook definition");

	/* A steady rate: no successive differences at all. */
	{
		uint16_t ibi[20];

		for (int i = 0; i < 20; i++) {
			ibi[i] = 1000;
		}
		feed_all(ibi, 20);
		CHECK(hrv_rmssd_x10() == 0,
		      "a perfectly regular rate gave RMSSD %u, expected 0",
		      hrv_rmssd_x10());
		CHECK(hrv_diffs() == 19, "expected 19 differences, got %u",
		      hrv_diffs());
	}

	/* Alternating +/- 20 ms: every successive difference is 40 ms. */
	{
		uint16_t ibi[21];

		for (int i = 0; i < 21; i++) {
			ibi[i] = (i % 2) ? 1020 : 980;
		}
		feed_all(ibi, 21);
		CHECK(hrv_rmssd_x10() == 400,
		      "alternating 980/1020 gave %u, expected 400 (40.0 ms)",
		      hrv_rmssd_x10());
	}

	/* An irregular sequence, against the reference implementation. */
	{
		uint16_t ibi[] = { 800, 812, 796, 804, 820, 790, 806, 798,
				   814, 802, 788, 810, 800, 796, 818 };
		int n = (int)(sizeof(ibi) / sizeof(ibi[0]));
		double want_x10 = rmssd_reference(ibi, n) * 10.0;

		feed_all(ibi, n);
		CHECK_NEAR(hrv_rmssd_x10(), want_x10, 2,
			   "irregular sequence vs the definition");
	}
}

static void test_not_enough_beats(void)
{
	sim_section("too few differences reports nothing, not a guess");

	hrv_init();
	CHECK(hrv_rmssd_x10() == 0, "reported a value with no data at all");
	CHECK(hrv_diffs() == 0, "counted differences with no data");

	/* Nine differences: still below the floor. */
	for (int i = 0; i < 10; i++) {
		hrv_add_interval(900 + (i % 3) * 20, i > 0);
	}
	CHECK(hrv_diffs() == 9, "expected 9 differences, got %u", hrv_diffs());
	CHECK(hrv_rmssd_x10() == 0,
	      "reported %u from 9 differences; the floor is 10",
	      hrv_rmssd_x10());

	/* The tenth crosses it. */
	hrv_add_interval(920, true);
	CHECK(hrv_diffs() == 10, "expected 10 differences, got %u",
	      hrv_diffs());
	CHECK(hrv_rmssd_x10() > 0, "still silent at the floor");
}

/*
 * The whole point of the `successive` flag. A rejected beat in the middle
 * of a run must not produce a difference spanning it -- that difference is
 * roughly a whole interval wide and, squared, swamps every real one.
 */
static void test_non_successive_intervals_form_no_difference(void)
{
	sim_section("a break in the run does not manufacture a difference");

	/* Twelve steady intervals, then a gap, then a very different rate. */
	hrv_init();
	for (int i = 0; i < 12; i++) {
		hrv_add_interval(1000, i > 0);
	}

	uint16_t steady = hrv_rmssd_x10();
	uint8_t steady_n = hrv_diffs();

	CHECK(steady == 0, "steady run gave RMSSD %u", steady);

	/* Not successive: a 500 ms interval after 1000 ms would be a 500 ms
	 * difference if it were wrongly paired.
	 */
	hrv_add_interval(500, false);
	CHECK(hrv_diffs() == steady_n,
	      "a non-successive interval added a difference (%u -> %u)",
	      steady_n, hrv_diffs());
	CHECK(hrv_rmssd_x10() == 0,
	      "a non-successive interval moved RMSSD to %u", hrv_rmssd_x10());

	/* The one after it IS successive with the 500, and legitimately
	 * contributes.
	 */
	hrv_add_interval(520, true);
	CHECK(hrv_diffs() == steady_n + 1,
	      "the interval after the break did not contribute");
}

static void test_window_rolls(void)
{
	sim_section("the window rolls rather than growing without bound");

	hrv_init();

	/* Fill well past the window with a large, constant difference. */
	for (int i = 0; i < 200; i++) {
		hrv_add_interval((i % 2) ? 1100 : 900, i > 0);
	}

	uint8_t n = hrv_diffs();

	CHECK(n <= 64, "window holds %u differences, expected at most 64", n);
	CHECK(n == 64, "window did not fill to 64, got %u", n);
	CHECK(hrv_rmssd_x10() == 2000,
	      "alternating 900/1100 should give 200.0 ms, got %u.%u",
	      hrv_rmssd_x10() / 10, hrv_rmssd_x10() % 10);

	/*
	 * Now a long run of steady intervals: the old spread must age out
	 * completely. It takes more than 64 of them, not exactly 64 -- the
	 * first steady interval still forms a real difference against the
	 * last alternating one, and that transition has to age out too.
	 */
	for (int i = 0; i < 80; i++) {
		hrv_add_interval(1000, true);
	}
	CHECK(hrv_rmssd_x10() == 0,
	      "old differences did not age out; RMSSD still %u",
	      hrv_rmssd_x10());
}

/*
 * What whole-sample beat timing does, checked rather than asserted: at
 * 100 sps intervals are multiples of 10 ms, and that alone puts a floor
 * under RMSSD even for a metronome-steady heart. hr.c no longer hands over
 * whole samples (test_subsample_timing below); this pins the number the
 * docs compare against.
 */
static void test_quantisation_noise_floor(void)
{
	sim_section("10 ms beat quantisation inflates RMSSD as documented");

	/*
	 * A metronome-steady heart at 1003.7 ms, sampled at 100 sps. The
	 * period is deliberately NOT a multiple of the 10 ms sample interval:
	 * at exactly 1000.0 ms every beat would land on a sample boundary and
	 * quantisation would vanish, which is a degenerate case rather than a
	 * test. Here each beat rounds to the nearest sample and the measured
	 * intervals wander between 1000 and 1010 while the true rate never
	 * moves at all.
	 */
	hrv_init();
	double t = 0;
	uint16_t prev_tick = 0;
	int fed = 0;

	for (int i = 0; i < 80; i++) {
		t += 1003.7;
		uint16_t tick = (uint16_t)((int)((t + 5.0) / 10.0) * 10);

		if (i > 0) {
			hrv_add_interval((uint16_t)(tick - prev_tick), fed > 0);
			fed++;
		}
		prev_tick = tick;
	}

	uint16_t floor_x10 = hrv_rmssd_x10();

	printf("    quantisation-only RMSSD: %u.%u ms\n",
	       floor_x10 / 10, floor_x10 % 10);
	CHECK(floor_x10 > 0,
	      "quantisation produced no apparent HRV at all, so this test is "
	      "not exercising what it claims to");
	CHECK(floor_x10 <= 100,
	      "a metronome heart produced %u.%u ms of apparent HRV, above the "
	      "~7 ms hrv.h claims as the floor", floor_x10 / 10,
	      floor_x10 % 10);
}


/* --- the hr.c -> hrv.c plumbing --------------------------------------- */

/*
 * Everything above tests hrv.c against intervals handed to it directly.
 * This tests the part that decides WHICH intervals get handed over, which
 * is where a silent total failure would live: if hr.c never marks an
 * interval trusted, RMSSD is permanently 0 and nothing else here notices.
 *
 * Feeds a synthetic PPG waveform through the real beat detector, at the
 * real 100 sps, with a DC level and pulse amplitude in the range the
 * board actually measured (see the constants in hr.c).
 */
#define PPG_DC		53000
#define PPG_AC		1500
#define PPG_RATE_HZ	100

/* One PPG sample for a pulse of period `period_samples`, phase `i`. */
static uint32_t ppg_sample(int i, int period_samples)
{
	double ph = (2.0 * M_PI * (i % period_samples)) / period_samples;
	/* Fundamental plus a second harmonic: a recognisable systolic peak
	 * rather than a pure tone.
	 */
	double v = 0.8 * sin(ph) + 0.25 * sin(2 * ph + 0.9);

	return (uint32_t)(PPG_DC + PPG_AC * v);
}

/* Run a clean pulse train and report what reached hrv. */
static void run_clean_pulse(struct hr *h, int beats, int period_samples,
			    int *trusted_out, int *successive_out)
{
	int trusted = 0, successive = 0;

	for (int i = 0; i < beats * period_samples; i++) {
		uint16_t bpm = 0;

		if (hr_update(h, ppg_sample(i, period_samples), &bpm)) {
			if (hr_last_ibi_trusted(h)) {
				trusted++;
				if (hr_last_ibi_successive(h)) {
					successive++;
				}
				hrv_add_interval(hr_last_ibi_ms(h),
						 hr_last_ibi_successive(h));
			}
		}
	}

	*trusted_out = trusted;
	*successive_out = successive;
}

static void test_detector_feeds_hrv(void)
{
	struct hr h;
	int trusted = 0, successive = 0;

	sim_section("the beat detector actually produces trusted intervals");

	hr_init(&h, PPG_RATE_HZ);
	hrv_init();

	/* 60 bpm is 100 samples a beat at 100 sps. 40 beats. */
	run_clean_pulse(&h, 40, 100, &trusted, &successive);

	CHECK(trusted > 20,
	      "a clean 60 bpm pulse train produced only %d trusted intervals "
	      "-- if this is 0, RMSSD can never report anything at all",
	      trusted);
	CHECK(successive > 20,
	      "only %d intervals were marked successive out of %d trusted",
	      successive, trusted);
	CHECK(hrv_diffs() > 10,
	      "only %u differences reached hrv from 40 clean beats",
	      hrv_diffs());

	/*
	 * A metronome pulse has no real HRV, so what comes out is the
	 * quantisation floor and detector jitter, not physiology. It must
	 * still be a small number rather than a wild one.
	 */
	CHECK(hrv_rmssd_x10() < 400,
	      "a metronome pulse train gave RMSSD %u.%u ms, which is far too "
	      "much for a signal with no variability in it",
	      hrv_rmssd_x10() / 10, hrv_rmssd_x10() % 10);
}

/*
 * The gate that matters: an interval wildly out of step with its
 * neighbours must not be handed to RMSSD as trusted.
 */
static void test_outlier_interval_is_not_trusted(void)
{
	struct hr h;
	int trusted_before, successive_before;
	int trusted_after = 0, successive_after = 0;
	int i;

	sim_section("an interval that disagrees with the median is rejected");

	hr_init(&h, PPG_RATE_HZ);
	hrv_init();

	/* Settle into a clean 60 bpm rhythm first. */
	run_clean_pulse(&h, 20, 100, &trusted_before, &successive_before);
	CHECK(trusted_before > 5, "detector never settled");

	/*
	 * Now a stretch at roughly half the rate. The first interval across
	 * that change is ~2x the established median, well outside the
	 * agreement band, and must not be trusted.
	 */
	int flagged_untrusted = 0;

	for (i = 0; i < 6 * 200; i++) {
		uint16_t bpm = 0;

		if (hr_update(&h, ppg_sample(i, 200), &bpm)) {
			if (hr_last_ibi_trusted(&h)) {
				trusted_after++;
				if (hr_last_ibi_successive(&h)) {
					successive_after++;
				}
			} else {
				flagged_untrusted++;
			}
		}
	}

	CHECK(flagged_untrusted > 0,
	      "the abrupt rate change produced no untrusted interval at all, "
	      "so the agreement gate is not gating anything (%d trusted, %d "
	      "successive after the change)", trusted_after, successive_after);
}


/*
 * BUG: the HRV trust gate reused the heart rate's 40 % median-agreement
 * band, which is far too loose when the error is going to be squared.
 *
 * A metronome pulse train with a motion artifact partway through each beat
 * gets the artifact ACCEPTED as a real beat -- it falls outside the
 * refractory window and inside the plausible bpm range -- producing an
 * alternating 600/400 ms pattern. At 40 % that sails through and invents
 * 200 ms of RMSSD from a rhythm with none. At the conventional 20 % it is
 * rejected.
 *
 * Note what this does NOT cover: the separate fix that clears
 * prev_ibi_trusted when a crossing is rejected rather than accepted.
 * Reverting that alone does not make this fail, because the tightened
 * gate catches the same intervals first. It is kept as defence in depth,
 * and this comment exists so nobody mistakes it for something the suite
 * is pinning.
 */
static void test_artifact_does_not_invent_hrv(void)
{
	struct hr h;
	int period = 100;		/* 60 bpm at 100 sps */
	/*
	 * 60 samples in: late enough that the detector has dropped below
	 * its re-arm threshold so the artifact DOES produce a crossing,
	 * early enough to be inside the refractory window so that crossing
	 * is rejected. Found by scanning positions against the unfixed
	 * code -- 20-50 and 70+ produce no crossing at all and prove
	 * nothing.
	 */
	int spike_at = 60;
	int bad_successive = 0;
	int beats = 0;

	sim_section("a motion artifact does not invent HRV from a metronome");

	hr_init(&h, PPG_RATE_HZ);
	hrv_init();

	for (int i = 0; i < 30 * period; i++) {
		uint32_t raw = ppg_sample(i, period);
		uint16_t bpm = 0;

		/* A sharp artifact partway into each beat. */
		if ((i % period) == spike_at) {
			raw += PPG_AC * 3;
		}

		if (hr_update(&h, raw, &bpm)) {
			beats++;
			if (hr_last_ibi_trusted(&h)) {
				uint16_t ms = hr_last_ibi_ms(&h);

				/*
				 * The real rhythm is 1000 ms. Anything far off
				 * that which still claims to be successive is
				 * the bug: it was measured from the artifact.
				 */
				if (hr_last_ibi_successive(&h) &&
				    (ms < 800 || ms > 1200)) {
					bad_successive++;
				}
				hrv_add_interval(ms,
						 hr_last_ibi_successive(&h));
			}
		}
	}

	CHECK(beats > 10, "detector found only %d beats in 30 s", beats);
	CHECK(bad_successive == 0,
	      "%d intervals distorted by the artifact were still handed to "
	      "RMSSD as trusted and successive", bad_successive);

	/*
	 * And the end-to-end consequence: the underlying rhythm is a
	 * metronome, so whatever RMSSD survives must stay near the
	 * quantisation floor rather than picking up the artifact.
	 */
	CHECK(hrv_rmssd_x10() < 400,
	      "a metronome rhythm with artifacts gave RMSSD %u.%u ms",
	      hrv_rmssd_x10() / 10, hrv_rmssd_x10() % 10);
}

/* --- sub-sample beat timing ---------------------------------------------- */

/*
 * A PPG whose beats fall at arbitrary times, not on samples: beat k starts
 * at beat_t[k] ms and the pulse shape is stretched over that beat's own
 * interval. Sampled at 100 sps exactly as the sensor would.
 */
#define RR_MAX	200

static double beat_t[RR_MAX + 1];

/* +- this many counts of white noise on every sample; 0 = clean. */
static int ppg_noise;
static uint32_t noise_seed = 12345;

static int noise(void)
{
	if (ppg_noise == 0) {
		return 0;
	}
	noise_seed = noise_seed * 1103515245U + 12345U;
	return (int)((noise_seed >> 16) % (2U * ppg_noise + 1U)) - ppg_noise;
}

static uint32_t ppg_at(double t_ms, int nbeats)
{
	int k = 0;

	while (k + 1 < nbeats && beat_t[k + 1] <= t_ms) {
		k++;
	}
	double ph = 2.0 * M_PI * (t_ms - beat_t[k]) /
		    (beat_t[k + 1] - beat_t[k]);
	double v = 0.8 * sin(ph) + 0.25 * sin(2 * ph + 0.9);

	return (uint32_t)(PPG_DC + PPG_AC * v + noise());
}

/*
 * Run `n` intervals from rr[] through the real detector and return the
 * RMSSD, in ms, of every trusted, successive interval it reported. The
 * true RMSSD of rr[] is returned through *truth.
 */
static double measure_rmssd(const double *rr, int n, double *truth,
			    int *diffs)
{
	struct hr h;
	struct hrv_session ses;
	double sum = 0;

	beat_t[0] = 0;
	for (int i = 0; i < n; i++) {
		beat_t[i + 1] = beat_t[i] + rr[i];
	}
	for (int i = 1; i < n; i++) {
		double d = rr[i] - rr[i - 1];

		sum += d * d;
	}
	*truth = sqrt(sum / (n - 1));

	hr_init(&h, PPG_RATE_HZ);
	hrv_session_begin(&ses);
	for (int k = 0; k * 10.0 < beat_t[n - 1]; k++) {
		uint16_t bpm;

		if (hr_update(&h, ppg_at(k * 10.0, n + 1), &bpm) &&
		    hr_last_ibi_trusted(&h)) {
			hrv_session_add(&ses, hr_last_ibi_ms(&h),
					hr_last_ibi_successive(&h));
		}
	}

	*diffs = hrv_session_diffs(&ses);
	return hrv_session_rmssd_x10(&ses) / 10.0;
}

static void test_subsample_timing(void)
{
	double rr[RR_MAX];
	double truth, got;
	int diffs;

	sim_section("sub-sample beat timing: RMSSD close to the truth");

	/*
	 * A metronome at 1003.7 ms: no HRV at all, so everything measured
	 * is error. Whole-sample timing reads ~8 ms here (the floor
	 * test_quantisation_noise_floor documents).
	 */
	for (int i = 0; i < 120; i++) {
		rr[i] = 1003.7;
	}
	got = measure_rmssd(rr, 120, &truth, &diffs);
	printf("    metronome:     true %5.1f ms, measured %5.1f ms (%d diffs)\n",
	       truth, got, diffs);
	CHECK(diffs > 60, "only %d differences from 120 clean beats", diffs);
	CHECK(got <= 2.0, "a metronome read %.1f ms of HRV", got);

	/*
	 * Low HRV, ~4 ms -- the stressed, exhausted or unwell reading that
	 * whole-sample timing buries under its own 8 ms.
	 */
	for (int i = 0; i < 120; i++) {
		rr[i] = 1001.3 + 3.0 * sin(2 * M_PI * i / 3.1);
	}
	got = measure_rmssd(rr, 120, &truth, &diffs);
	printf("    low HRV:       true %5.1f ms, measured %5.1f ms\n", truth,
	       got);
	CHECK(fabs(got - truth) <= 1.5, "low HRV: %.1f ms against a true %.1f",
	      got, truth);

	/* Relaxed, ~28 ms: should be right to within a few percent. */
	for (int i = 0; i < 120; i++) {
		rr[i] = (i % 2) ? 1003.4 + 14.1 : 1003.4 - 14.1;
	}
	got = measure_rmssd(rr, 120, &truth, &diffs);
	printf("    moderate HRV:  true %5.1f ms, measured %5.1f ms\n", truth,
	       got);
	CHECK(fabs(got - truth) <= 0.05 * truth + 1.0,
	      "moderate HRV: %.1f ms against a true %.1f", got, truth);

	/* And at a resting 52 bpm, a longer beat than everything above. */
	for (int i = 0; i < 120; i++) {
		rr[i] = 1153.9 + 6.0 * sin(2 * M_PI * i / 4.7);
	}
	got = measure_rmssd(rr, 120, &truth, &diffs);
	printf("    52 bpm:        true %5.1f ms, measured %5.1f ms\n", truth,
	       got);
	CHECK(fabs(got - truth) <= 1.5, "52 bpm: %.1f ms against a true %.1f",
	      got, truth);

	/*
	 * The same low-HRV heart under sensor noise, independent on every
	 * sample. Noise moves a fitted vertex, so this is where placing
	 * peaks between samples could have made things worse. Measured
	 * 2026-10-27, RMSSD of a true 3.6 ms:
	 *
	 *   noise (of pulse swing)   0     0.5 %   1 %    2 %    3 %
	 *   whole samples            8.7   7.6     9.8   15.1   17.8
	 *   3-point parabola         3.5   4.5     6.0   10.8   13.8
	 *   5-point fit (hr.c)       3.5   3.8     4.1    8.8   12.1
	 *
	 * Better than whole samples at every level; past ~1 % the noise
	 * itself, not the timing, is what RMSSD measures. The real sensor's
	 * level is not known yet (no recordings on hand). These pin the two
	 * middle columns.
	 */
	for (int i = 0; i < 120; i++) {
		rr[i] = 1001.3 + 3.0 * sin(2 * M_PI * i / 3.1);
	}
	ppg_noise = PPG_AC / 100;
	noise_seed = 12345;	/* the table above was made from this seed */
	got = measure_rmssd(rr, 120, &truth, &diffs);
	printf("    low HRV, 1 %% noise: true %5.1f ms, measured %5.1f ms (%d "
	       "diffs)\n", truth, got, diffs);
	CHECK(diffs > 60, "noise cost the detector its beats (%d diffs)", diffs);
	CHECK(fabs(got - truth) <= 1.5,
	      "low HRV, 1 %% noise: %.1f ms against a true %.1f", got, truth);

	ppg_noise = PPG_AC * 2 / 100;
	noise_seed = 12345;	/* the table above was made from this seed */
	got = measure_rmssd(rr, 120, &truth, &diffs);
	printf("    low HRV, 2 %% noise: true %5.1f ms, measured %5.1f ms\n",
	       truth, got);
	CHECK(got <= 10.0,
	      "low HRV, 2 %% noise: %.1f ms, worse than the 8.8 the 5-point "
	      "fit gave (whole samples: 15.1)", got);
	ppg_noise = 0;
}

static void test_session(void)
{
	struct hrv_session s;
	uint16_t ibi[180];

	sim_section("spot-reading session: whole-reading RMSSD, window untouched");

	/* A rolling window already holding a steady rate. */
	hrv_init();
	for (int i = 0; i < 30; i++) {
		hrv_add_interval(1000, i > 0);
	}
	uint16_t before = hrv_rmssd_x10();
	uint8_t before_n = hrv_diffs();

	/* 180 beats of alternating 950/1050: every difference is 100 ms. */
	hrv_session_begin(&s);
	for (int i = 0; i < 180; i++) {
		ibi[i] = (i % 2) ? 1050 : 950;
		hrv_session_add(&s, ibi[i], i > 0);
	}
	CHECK(hrv_session_diffs(&s) == 179, "session diffs %u, expected 179",
	      hrv_session_diffs(&s));
	CHECK(hrv_session_rmssd_x10(&s) == (uint16_t)(rmssd_reference(ibi, 180)
						      * 10 + 0.5),
	      "session RMSSD x10 %u, reference %.1f", hrv_session_rmssd_x10(&s),
	      rmssd_reference(ibi, 180) * 10);
	CHECK(hrv_rmssd_x10() == before && hrv_diffs() == before_n,
	      "a session changed the rolling window (%u/%u -> %u/%u)", before,
	      before_n, hrv_rmssd_x10(), hrv_diffs());

	/* Same rules as the window: a break forms no difference. */
	hrv_session_begin(&s);
	hrv_session_add(&s, 1000, false);
	hrv_session_add(&s, 1200, false);
	hrv_session_add(&s, 0, true);
	hrv_session_add(&s, 900, true);
	CHECK(hrv_session_diffs(&s) == 0, "non-successive gave %u diffs",
	      hrv_session_diffs(&s));
	CHECK(hrv_session_rmssd_x10(&s) == 0, "RMSSD from no differences");
}

int main(void)
{
	printf("HRV / RMSSD tests\n");

	test_session();

	test_matches_the_definition();
	test_not_enough_beats();
	test_non_successive_intervals_form_no_difference();
	test_window_rolls();
	test_quantisation_noise_floor();
	test_detector_feeds_hrv();
	test_outlier_interval_is_not_trusted();
	test_artifact_does_not_invent_hrv();
	test_subsample_timing();

	return sim_report("test_hrv");
}
