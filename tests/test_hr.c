/*
 * The heart-rate detector (hr.c) against synthetic PPG.
 *
 * Each case pins a claim hr.h makes: the right rate across the range, glitch
 * and drift immunity, silence with no finger and with no pulse, no locking
 * onto the harmonic, and following a change of rate.
 *
 * The waveform is an assumption -- a fundamental plus a second harmonic on
 * the DC level the 31 mA front end reads, ~100,000 counts, with a pulse of
 * ~0.6 %. The glitch pattern is the one measured on ring 2 (single samples,
 * mostly downward, ~70 ms apart). Accuracy here is accuracy against that
 * model; tools/hr_eval.py scores the same code against real recordings and
 * a WHOOP, which is the number that matters.
 */

#include "hr.h"
#include "sim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Strict C11 does not promise M_PI. */
#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif

#define FS		100
#define DC		100000.0
#define PULSE		600.0

static uint32_t seed = 1;

/* Uniform in [-1, 1], deterministic. */
static double noise(void)
{
	seed = seed * 1103515245U + 12345U;
	return ((double)((seed >> 8) & 0xFFFF) / 32767.5) - 1.0;
}

struct gen {
	double bpm;
	double phase;
	double harmonic;	/* second-harmonic amplitude, share of fundamental */
	double drift;		/* counts of slow wander */
	double noise;		/* counts of white noise */
	double glitch;		/* counts; one sample in 7 when non-zero */
	double dc;
	int n;
};

static uint32_t gen_sample(struct gen *g)
{
	double t = (double)g->n / FS;
	double v;

	g->phase += 2.0 * M_PI * (g->bpm / 60.0) / FS;
	v = g->dc + PULSE * (sin(g->phase) + g->harmonic * sin(2 * g->phase + 0.9));
	v += g->drift * (0.6 * sin(2 * M_PI * 0.07 * t) + 0.4 * sin(2 * M_PI * 0.23 * t));
	v += g->noise * noise();
	if (g->glitch != 0 && (g->n % 7) == 3) {
		v -= g->glitch * (0.5 + 0.5 * fabs(noise()));
	}
	g->n++;

	return (uint32_t)(v < 0 ? 0 : v);
}

/*
 * Run `secs` seconds; return the mean |error| in bpm over the seconds after
 * `settle_s` that had a reading, and the share of those seconds with one.
 */
static double run(struct hr *h, struct gen *g, int secs, int settle_s,
		  double *coverage, int *first_reading_s)
{
	double err = 0;
	int n = 0, seconds = 0;
	uint16_t bpm = 0;

	if (first_reading_s) {
		*first_reading_s = -1;
	}
	for (int s = 0; s < secs; s++) {
		for (int i = 0; i < FS; i++) {
			hr_update(h, gen_sample(g), &bpm);
		}
		if (bpm && first_reading_s && *first_reading_s < 0) {
			*first_reading_s = s + 1;
		}
		if (s < settle_s) {
			continue;
		}
		seconds++;
		if (bpm) {
			err += fabs(bpm / 10.0 - g->bpm);
			n++;
		}
	}
	if (coverage) {
		*coverage = seconds ? (double)n / seconds : 0;
	}
	return n ? err / n : -1;
}

static void test_clean_rates(void)
{
	const double rates[] = { 45, 60, 75, 90, 120, 150, 180 };

	sim_section("a clean pulse reads its own rate, 45-180 bpm");

	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		static struct hr h;
		struct gen g = { .bpm = rates[i], .harmonic = 0.3, .noise = 20, .dc = DC };
		double cov;
		int first;

		hr_init(&h, FS);
		double err = run(&h, &g, 40, 15, &cov, &first);

		CHECK(err >= 0 && err <= 1.5,
		      "%.0f bpm: mean error %.2f bpm", rates[i], err);
		CHECK(cov >= 0.95, "%.0f bpm: reading only %.0f%% of seconds",
		      rates[i], 100 * cov);
		CHECK(first > 0 && first <= 14,
		      "%.0f bpm: first reading after %d s (want <= 14: 10 s window "
		      "+ 3 agreeing estimates)", rates[i], first);
	}
}

static void test_glitches(void)
{
	static struct hr h;
	struct gen g = { .bpm = 68, .harmonic = 0.3, .noise = 20, .glitch = 1500, .dc = DC };
	double cov;

	sim_section("single-sample glitches every 70 ms do not move the rate");

	hr_init(&h, FS);
	double err = run(&h, &g, 60, 15, &cov, NULL);

	CHECK(err >= 0 && err <= 1.5, "glitched pulse: mean error %.2f bpm", err);
	CHECK(cov >= 0.9, "glitched pulse: reading %.0f%% of seconds", 100 * cov);
}

static void test_drift(void)
{
	static struct hr h;
	struct gen g = { .bpm = 72, .harmonic = 0.3, .noise = 20, .drift = 3000, .dc = DC };
	double cov;

	sim_section("slow drift five times the pulse does not move the rate");

	hr_init(&h, FS);
	double err = run(&h, &g, 60, 15, &cov, NULL);

	CHECK(err >= 0 && err <= 2.0, "drifting pulse: mean error %.2f bpm", err);
	CHECK(cov >= 0.8, "drifting pulse: reading %.0f%% of seconds", 100 * cov);
}

static void test_no_finger(void)
{
	static struct hr h;
	struct gen g = { .bpm = 70, .harmonic = 0.3, .noise = 150, .dc = 5000 };
	double cov;

	sim_section("an empty sensor reports nothing, ever");

	hr_init(&h, FS);
	(void)run(&h, &g, 60, 0, &cov, NULL);

	CHECK(cov == 0, "empty sensor gave a reading %.0f%% of seconds", 100 * cov);
	CHECK(!hr_finger_present(&h), "empty sensor claims a finger");
}

static void test_noise_without_pulse(void)
{
	static struct hr h;
	uint16_t bpm = 0;
	int readings = 0;

	sim_section("a finger with no detectable pulse stays (mostly) silent");

	hr_init(&h, FS);
	seed = 7;
	for (int s = 0; s < 120; s++) {
		for (int i = 0; i < FS; i++) {
			uint32_t v = (uint32_t)(DC + 400 * noise());

			hr_update(&h, v, &bpm);
		}
		if (bpm) {
			readings++;
		}
	}

	CHECK(hr_finger_present(&h), "DC %d should read as a finger",
	      (int)hr_baseline(&h));
	CHECK(readings <= 6,
	      "white noise with no pulse produced a rate in %d of 120 s", readings);
}

static void test_strong_harmonic(void)
{
	static struct hr h;
	struct gen g = { .bpm = 62, .harmonic = 0.9, .noise = 20, .dc = DC };
	double cov;

	sim_section("a strong second harmonic does not double the rate");

	hr_init(&h, FS);
	double err = run(&h, &g, 40, 15, &cov, NULL);

	CHECK(err >= 0 && err <= 1.5,
	      "62 bpm with a 0.9 harmonic: mean error %.2f bpm "
	      "(~62 means it read the harmonic)", err);
}

static void test_rate_change(void)
{
	static struct hr h;
	struct gen g = { .bpm = 60, .harmonic = 0.3, .noise = 20, .dc = DC };
	uint16_t bpm = 0;
	int settled_at = -1;

	sim_section("a step from 60 to 90 bpm is followed within ~15 s");

	hr_init(&h, FS);
	(void)run(&h, &g, 30, 30, NULL, NULL);
	g.bpm = 90;
	for (int s = 0; s < 40; s++) {
		for (int i = 0; i < FS; i++) {
			hr_update(&h, gen_sample(&g), &bpm);
		}
		if (settled_at < 0 && bpm && fabs(bpm / 10.0 - 90) <= 2) {
			settled_at = s + 1;
		}
	}

	CHECK(settled_at > 0 && settled_at <= 16,
	      "reached 90 +/- 2 bpm %d s after the step", settled_at);
	CHECK(bpm && fabs(bpm / 10.0 - 90) <= 2,
	      "40 s after the step it reads %u.%u", bpm / 10, bpm % 10);
}

int main(void)
{
	printf("Heart-rate detector tests\n");

	test_clean_rates();
	test_glitches();
	test_drift();
	test_no_finger();
	test_noise_without_pulse();
	test_strong_harmonic();
	test_rate_change();

	return sim_report("test_hr");
}
