/*
 * Hot-flash detection against synthetic ring temperature.
 *
 * Every trace is sampled every 10 s, as main.c does, and quantised to the
 * nRF die's 0.25 C steps with a little noise on top, so the smoothing has
 * to earn its keep. The shapes come from what the module is meant to
 * tell apart: a hot flash (a degree or two in a few minutes) against
 * putting the ring on, the slow warming of the hands at sleep onset, a
 * charger, and a ring that simply sits warm for an hour.
 */

#include "sim.h"

#include "thermo.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SAMPLE_MS	10000
#define MIN_MS		60000LL

/* What the trace says at minute m. */
struct trace {
	double (*temp_c)(const struct trace *t, double m);
	bool (*worn)(const struct trace *t, double m);
	bool (*asleep)(const struct trace *t, double m);
	bool (*blocked)(const struct trace *t, double m);
	double base_c;
	double a, b, c, d;	/* shape parameters, per trace */
};

struct outcome {
	int starts;
	double first_start_min;
	double flash_minutes;	/* minutes spent in_flash */
	uint8_t peak_c10;	/* highest thermo_take_peak_rise_c10() */
	bool ready_at_end;
};

static uint32_t lcg = 12345;

static int noise_steps(void)
{
	lcg = lcg * 1103515245U + 12345U;
	/* -1, 0 or +1 quarter-degree steps, mostly 0. */
	switch ((lcg >> 16) % 8) {
	case 0:
		return -1;
	case 1:
		return 1;
	default:
		return 0;
	}
}

static int16_t quantise(double c)
{
	int q = (int)floor(c * 4.0 + 0.5) + noise_steps();

	return (int16_t)(q * 25);
}

static bool always(const struct trace *t, double m)
{
	(void)t;
	(void)m;
	return true;
}

static bool never(const struct trace *t, double m)
{
	(void)t;
	(void)m;
	return false;
}

static struct outcome run(const struct trace *t, double minutes)
{
	struct outcome o = { .first_start_min = -1 };

	lcg = 12345;
	for (int64_t ms = 0; ms <= (int64_t)(minutes * MIN_MS);
	     ms += SAMPLE_MS) {
		double m = (double)ms / MIN_MS;

		thermo_feed(quantise(t->temp_c(t, m)), ms,
			    t->worn ? t->worn(t, m) : true,
			    t->blocked ? t->blocked(t, m) : false,
			    t->asleep ? t->asleep(t, m) : true);

		if (thermo_take_hot_flash_start()) {
			if (o.starts == 0) {
				o.first_start_min = m;
			}
			o.starts++;
		}
		if (thermo_in_hot_flash()) {
			o.flash_minutes += (double)SAMPLE_MS / MIN_MS;
		}
		/* Read the peak every 5 minutes, like a history epoch. */
		if (ms % (5 * MIN_MS) == 0) {
			uint8_t p = thermo_take_peak_rise_c10();

			if (p > o.peak_c10) {
				o.peak_c10 = p;
			}
		}
	}
	o.ready_at_end = thermo_ready();

	return o;
}

/* --- shapes ------------------------------------------------------------- */

static double flat(const struct trace *t, double m)
{
	(void)m;
	return t->base_c;
}

/*
 * A hot flash at minute a: rise of b C over 3 min, hold c min, then an
 * exponential fall with a 4 min time constant.
 */
static double flash_at(double m, double start, double rise, double hold)
{
	if (m < start) {
		return 0;
	}
	if (m < start + 3) {
		return rise * (m - start) / 3;
	}
	if (m < start + 3 + hold) {
		return rise;
	}
	return rise * exp(-(m - start - 3 - hold) / 4);
}

static double one_flash(const struct trace *t, double m)
{
	return t->base_c + flash_at(m, t->a, t->b, t->c);
}

static double two_flashes(const struct trace *t, double m)
{
	return t->base_c + flash_at(m, t->a, t->b, t->c) +
	       flash_at(m, t->d, t->b, t->c);
}

/* Off the hand at 22 C until minute a, then warming to base_c, tau 5 min. */
static double donning(const struct trace *t, double m)
{
	if (m < t->a) {
		return 22.0;
	}
	return t->base_c - (t->base_c - 22.0) * exp(-(m - t->a) / 5);
}

static bool worn_after_a(const struct trace *t, double m)
{
	return m >= t->a;
}

/* b C of warming spread linearly over c minutes, from minute a. */
static double slow_ramp(const struct trace *t, double m)
{
	if (m < t->a) {
		return t->base_c;
	}
	if (m < t->a + t->c) {
		return t->base_c + t->b * (m - t->a) / t->c;
	}
	return t->base_c + t->b;
}

/* Up by b C at minute a and staying there. */
static double step_up(const struct trace *t, double m)
{
	if (m < t->a) {
		return t->base_c;
	}
	if (m < t->a + 3) {
		return t->base_c + t->b * (m - t->a) / 3;
	}
	return t->base_c + t->b;
}

static bool asleep_until_d(const struct trace *t, double m)
{
	return m < t->d;
}

static bool blocked_until_d(const struct trace *t, double m)
{
	return m < t->d;
}

static bool worn_until_d(const struct trace *t, double m)
{
	return m < t->d;
}

/* --- tests -------------------------------------------------------------- */

static void test_api_edges(void)
{
	uint8_t mode, rise;

	sim_section("API edges");

	thermo_init();
	CHECK(thermo_temp_c100() == THERMO_NONE,
	      "temperature before any sample should be THERMO_NONE, got %d",
	      thermo_temp_c100());
	CHECK(thermo_baseline_c100() == THERMO_NONE,
	      "baseline before ready should be THERMO_NONE");
	thermo_get_config(&mode, &rise);
	CHECK(mode == THERMO_MODE_NIGHT && rise == 10,
	      "defaults: mode %u rise %u, expected 1 and 10", mode, rise);

	CHECK(thermo_set_config(3, 10) == -EINVAL, "mode 3 accepted");
	CHECK(thermo_set_config(1, 2) == -EINVAL, "rise 0.2 C accepted");
	CHECK(thermo_set_config(1, 51) == -EINVAL, "rise 5.1 C accepted");
	CHECK(thermo_set_config(2, 50) == 0, "mode 2 rise 5.0 C refused");
	thermo_get_config(&mode, &rise);
	CHECK(mode == 2 && rise == 50, "config did not stick");

	thermo_init();
	thermo_feed(3312, 0, true, false, true);
	CHECK(thermo_temp_c100() == 3312,
	      "first sample should seed the average, got %d",
	      thermo_temp_c100());
	thermo_feed(THERMO_NONE, 10000, true, false, true);
	CHECK(thermo_temp_c100() == 3312, "THERMO_NONE sample was used");
}

static void test_steady_night(void)
{
	struct trace t = { .temp_c = flat, .base_c = 33.0 };
	struct outcome o;

	sim_section("steady night: no events, settles");

	thermo_init();
	o = run(&t, 8 * 60);
	CHECK(o.starts == 0, "steady 33 C produced %d hot flashes", o.starts);
	CHECK(o.ready_at_end, "never settled over 8 h worn");
	CHECK(o.peak_c10 <= 3, "peak rise %u.%u C on a flat trace",
	      o.peak_c10 / 10, o.peak_c10 % 10);
	CHECK(abs(thermo_baseline_c100() - 3300) <= 15,
	      "baseline %d, expected ~3300", thermo_baseline_c100());
}

static void test_night_flash(void)
{
	sim_section("hot flash at night is found once");

	for (int i = 0; i < 3; i++) {
		static const double rises[] = { 1.5, 2.0, 3.0 };
		struct trace t = { .temp_c = one_flash, .base_c = 33.0,
				   .a = 90, .b = rises[i], .c = 5 };
		struct outcome o;

		thermo_init();
		o = run(&t, 180);
		CHECK(o.starts == 1, "%.1f C flash: %d events, expected 1",
		      rises[i], o.starts);
		CHECK(o.first_start_min >= 90 && o.first_start_min <= 95,
		      "%.1f C flash started at %.1f min, expected 90-95",
		      rises[i], o.first_start_min);
		CHECK(o.flash_minutes >= 5 && o.flash_minutes <= 25,
		      "%.1f C flash lasted %.1f min", rises[i], o.flash_minutes);
		CHECK(!thermo_in_hot_flash(), "%.1f C flash never ended",
		      rises[i]);
		CHECK(o.peak_c10 >= (uint8_t)(rises[i] * 10 * 0.7),
		      "%.1f C flash peak recorded as %u.%u C", rises[i],
		      o.peak_c10 / 10, o.peak_c10 % 10);
		CHECK(thermo_hot_flash_count() == 1, "count %u",
		      thermo_hot_flash_count());
	}
}

static void test_small_rise_threshold(void)
{
	struct trace t = { .temp_c = one_flash, .base_c = 33.0,
			   .a = 90, .b = 0.6, .c = 5 };
	struct outcome o;

	sim_section("threshold: 0.6 C ignored at 1.0, found at 0.5");

	thermo_init();
	o = run(&t, 180);
	CHECK(o.starts == 0, "0.6 C rise at a 1.0 C threshold: %d events",
	      o.starts);

	thermo_init();
	(void)thermo_set_config(THERMO_MODE_NIGHT, 5);
	o = run(&t, 180);
	CHECK(o.starts == 1, "0.6 C rise at a 0.5 C threshold: %d events",
	      o.starts);
}

static void test_donning(void)
{
	struct trace t = { .temp_c = donning, .worn = worn_after_a,
			   .base_c = 33.5, .a = 20 };
	struct outcome o;

	sim_section("putting the ring on is not a hot flash");

	thermo_init();
	o = run(&t, 240);
	CHECK(o.starts == 0, "22 -> 33.5 C warm-up gave %d events", o.starts);
	CHECK(o.ready_at_end, "not ready 220 min after going on");
	CHECK(abs(thermo_baseline_c100() - 3350) <= 20,
	      "baseline %d after warm-up, expected ~3350",
	      thermo_baseline_c100());

	/* A flash after the settle is still found. */
	t.temp_c = one_flash;
	t.worn = NULL;
	t.base_c = 33.5;
	t.a = 40;
	t.b = 2.0;
	t.c = 5;
	thermo_init();
	o = run(&t, 120);
	CHECK(o.starts == 1, "flash 40 min after a cold start: %d events",
	      o.starts);
}

static void test_sleep_onset_warming(void)
{
	sim_section("slow warming (sleep onset, a duvet) is ignored");

	for (int i = 0; i < 3; i++) {
		static const double rise[] = { 1.5, 2.0, 2.5 };
		static const double over[] = { 30, 45, 60 };
		struct trace t = { .temp_c = slow_ramp, .base_c = 31.5,
				   .a = 60, .b = rise[i], .c = over[i] };
		struct outcome o;

		thermo_init();
		o = run(&t, 240);
		CHECK(o.starts == 0, "%.1f C over %.0f min gave %d events",
		      rise[i], over[i], o.starts);
	}
}

static void test_awake_and_modes(void)
{
	struct trace t = { .temp_c = one_flash, .asleep = never,
			   .base_c = 33.0, .a = 90, .b = 2.0, .c = 5 };
	struct outcome o;

	sim_section("modes: night-only ignores the awake, always does not");

	thermo_init();
	o = run(&t, 180);
	CHECK(o.starts == 0, "awake flash in night mode: %d events", o.starts);

	thermo_init();
	(void)thermo_set_config(THERMO_MODE_ALWAYS, 10);
	o = run(&t, 180);
	CHECK(o.starts == 1, "awake flash in always mode: %d events", o.starts);

	thermo_init();
	(void)thermo_set_config(THERMO_MODE_OFF, 10);
	t.asleep = NULL;
	o = run(&t, 180);
	CHECK(o.starts == 0, "mode off: %d events", o.starts);
}

static void test_wakes_with_the_flash(void)
{
	/* Asleep for 85 min, wakes at minute 90 as the flash begins. */
	struct trace t = { .temp_c = one_flash, .asleep = asleep_until_d,
			   .base_c = 33.0, .a = 90, .b = 2.0, .c = 5, .d = 90 };
	struct outcome o;

	sim_section("a flash that wakes the wearer is still caught");

	thermo_init();
	o = run(&t, 180);
	CHECK(o.starts == 1, "flash on waking: %d events", o.starts);

	/* But an hour after waking, night mode is closed. */
	t.a = 160;
	thermo_init();
	o = run(&t, 240);
	CHECK(o.starts == 0, "flash 70 min after waking: %d events", o.starts);
}

static void test_charging(void)
{
	/*
	 * On the charger until minute 60 and warmer there; a step down to
	 * 33 C as it comes off and goes on the hand, then a flash 20 min
	 * later -- inside the settle period, so not judged.
	 */
	struct trace t = { .temp_c = one_flash, .blocked = blocked_until_d,
			   .base_c = 33.0, .a = 80, .b = 2.0, .c = 5,
			   .d = 60 };
	struct outcome o;

	sim_section("charging blocks detection, then a full settle");

	thermo_init();
	o = run(&t, 150);
	CHECK(o.starts == 0, "flash 20 min off the charger: %d events",
	      o.starts);

	t.a = 100;
	thermo_init();
	o = run(&t, 170);
	CHECK(o.starts == 1, "flash 40 min off the charger: %d events",
	      o.starts);
}

static void test_refractory(void)
{
	sim_section("refractory: two flashes 10 min apart count once");

	{
		struct trace t = { .temp_c = two_flashes, .base_c = 33.0,
				   .a = 60, .b = 2.0, .c = 2, .d = 70 };
		struct outcome o;

		thermo_init();
		o = run(&t, 150);
		CHECK(o.starts == 1, "flashes at 60 and 70 min: %d events",
		      o.starts);
	}
	{
		struct trace t = { .temp_c = two_flashes, .base_c = 33.0,
				   .a = 60, .b = 2.0, .c = 2, .d = 110 };
		struct outcome o;

		thermo_init();
		o = run(&t, 180);
		CHECK(o.starts == 2, "flashes at 60 and 110 min: %d events",
		      o.starts);
	}
}

static void test_plateau(void)
{
	struct trace t = { .temp_c = step_up, .base_c = 32.0,
			   .a = 60, .b = 2.0 };
	struct outcome o;

	sim_section("a rise that stays up ends after 30 min and re-baselines");

	thermo_init();
	o = run(&t, 240);
	CHECK(o.starts == 1, "step up that stays: %d events, expected 1",
	      o.starts);
	CHECK(o.flash_minutes >= 29 && o.flash_minutes <= 31,
	      "held in flash %.1f min, expected 30", o.flash_minutes);
	CHECK(abs(thermo_baseline_c100() - 3400) <= 20,
	      "baseline %d after the plateau, expected ~3400",
	      thermo_baseline_c100());
}

static void test_removed_mid_flash(void)
{
	struct trace t = { .temp_c = one_flash, .worn = worn_until_d,
			   .base_c = 33.0, .a = 60, .b = 2.0, .c = 10,
			   .d = 65 };
	struct outcome o;

	sim_section("taking the ring off ends a flash and the settle");

	thermo_init();
	o = run(&t, 120);
	CHECK(o.starts == 1, "%d events before removal", o.starts);
	CHECK(!thermo_in_hot_flash(), "still in a flash with the ring off");
	CHECK(!o.ready_at_end, "still ready with the ring off");
	CHECK(o.flash_minutes < 6, "%.1f min in flash, removed after ~3",
	      o.flash_minutes);
}

static void test_off_drops_pending_start(void)
{
	sim_section("turning detection off drops a start not yet taken");

	thermo_init();
	(void)thermo_set_config(THERMO_MODE_ALWAYS, 10);
	for (int64_t ms = 0; ms <= 40 * MIN_MS; ms += SAMPLE_MS) {
		thermo_feed(3300, ms, true, false, false);
	}
	/* A sharp 2 C rise, fed but not taken. */
	for (int64_t ms = 40 * MIN_MS + SAMPLE_MS; ms <= 46 * MIN_MS;
	     ms += SAMPLE_MS) {
		thermo_feed(3500, ms, true, false, false);
	}
	CHECK(thermo_in_hot_flash(), "setup: no flash started");
	(void)thermo_set_config(THERMO_MODE_OFF, 10);
	CHECK(!thermo_take_hot_flash_start(),
	      "a start survived switching detection off");
	CHECK(!thermo_in_hot_flash(), "still in a flash with detection off");
}

static void test_gap_restarts(void)
{
	sim_section("a gap in the samples restarts the settle");

	thermo_init();
	for (int64_t ms = 0; ms <= 40 * MIN_MS; ms += SAMPLE_MS) {
		thermo_feed(3300, ms, true, false, true);
	}
	CHECK(thermo_ready(), "not ready after 40 min");
	thermo_feed(3300, 50 * MIN_MS, true, false, true);
	CHECK(!thermo_ready(), "still ready after a 10 min gap");
}

int main(void)
{
	test_api_edges();
	test_steady_night();
	test_night_flash();
	test_small_rise_threshold();
	test_donning();
	test_sleep_onset_warming();
	test_awake_and_modes();
	test_wakes_with_the_flash();
	test_charging();
	test_refractory();
	test_plateau();
	test_removed_mid_flash();
	test_off_drops_pending_start();
	test_gap_restarts();

	(void)always;

	return sim_report("thermo");
}
