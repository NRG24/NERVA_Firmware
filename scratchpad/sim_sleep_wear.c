/*
 * Host simulation for the sleep tracker and its PPG wear corroboration.
 *
 *   gcc -std=c11 -Wall -Wextra -O1 -I src -I scratchpad/sim_stubs \
 *       scratchpad/sim_sleep_wear.c src/sleep.c -o /tmp/sim_sleep_wear
 *   /tmp/sim_sleep_wear
 *
 * WHAT THIS IS. src/sleep.c compiled for real, driven minute by minute,
 * with steps_count() stubbed. Everything it asserts about sleep.c is a
 * property of the shipped code.
 *
 * WHAT THIS IS NOT. main.c is not compiled -- it cannot be, it is half
 * Zephyr -- so the window scheduler in part B is a HAND MODEL of main.c's
 * policy, written from the same constants. It can prove that the policy is
 * self-consistent and it can catch a power regression in the design. It
 * cannot prove that main.c implements it, and no result here is a result
 * from hardware. There is no hardware.
 */

#include "sleep.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- stubs -------------------------------------------------------------- */

static uint32_t sim_steps;

uint32_t steps_count(void)
{
	return sim_steps;
}

/* --- harness ------------------------------------------------------------ */

static int failures;
static int checks_run;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks_run++;                                                  \
		if (!(cond)) {                                                 \
			failures++;                                            \
			printf("  FAIL %s:%d: ", __func__, __LINE__);          \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

#define MINUTE_MS 60000

/*
 * Mirrors main.c's constants. Kept as literals rather than #included so a
 * change to main.c that this simulation was never re-run against shows up
 * as a disagreement here rather than silently tracking.
 */
#define SIM_WEAR_INTERVAL_MS	(30 * MINUTE_MS)	/* Kconfig default */
#define SIM_WEAR_WINDOW_MS	6000			/* Kconfig default */
#define SIM_STILL_TIMEOUT_MS	180000			/* STILL_TIMEOUT_MS */
#define SIM_MEASURE_PERIOD_MS	60000			/* MEASURE_PERIOD_MS */
#define SIM_MEASURE_WINDOW_MS	15000			/* MEASURE_WINDOW_MS */
#define SIM_NO_FINGER_TIMEOUT_MS 6000			/* NO_FINGER_TIMEOUT_MS */
#define SIM_WEAR_SETTLE_MS	4000			/* WEAR_SETTLE_MS */

/* main.c polls the IMU every 200 ms while idle and still. */
#define SIM_POLL_MS		200

/*
 * Feed the tracker `minutes` of accelerometer samples at the idle poll rate.
 * peak_mg is the largest deviation from 1 g that occurs within each minute,
 * so it is the number sleep.c's still/active test actually sees.
 *
 * on_poll, when given, runs once per poll with the current uptime -- that is
 * where part B hangs its window scheduler.
 */
static int64_t feed(int64_t t_ms, int minutes, int32_t peak_mg,
		    void (*on_poll)(int64_t))
{
	int polls = (minutes * MINUTE_MS) / SIM_POLL_MS;

	for (int i = 0; i < polls; i++) {
		/* One elevated sample per minute, the rest at rest. Matches a
		 * real feed better than holding the peak for a whole minute,
		 * and is the harder case for a peak-hold bucket.
		 */
		bool peak_sample = ((i % (MINUTE_MS / SIM_POLL_MS)) == 0);

		sleep_feed(1000 + (peak_sample ? peak_mg : 0), t_ms);

		if (on_poll) {
			on_poll(t_ms);
		}

		t_ms += SIM_POLL_MS;
	}

	return t_ms;
}

/* ======================================================================== */
/* Part A: sleep.c behaviour                                                */
/* ======================================================================== */

/* A1. The regressions: everything sleep.c did before wear tracking. */
static void t_baseline_unchanged(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;

	t = feed(t, 9, 10, NULL);
	CHECK(!sleep_is_asleep(), "asleep after 9 still minutes, wanted 10");

	t = feed(t, 2, 10, NULL);
	CHECK(sleep_is_asleep(), "not asleep after 11 still minutes");
	CHECK(sleep_session_minutes() >= 10, "session %u min, wanted >= 10",
	      sleep_session_minutes());

	/* Two active minutes must not end it; three must. */
	t = feed(t, 2, 400, NULL);
	CHECK(sleep_is_asleep(), "2 active minutes ended the session");
	t = feed(t, 1, 400, NULL);
	CHECK(!sleep_is_asleep(), "3 active minutes did not end the session");
	CHECK(sleep_restless_minutes() == 3, "restless %u, wanted 3",
	      sleep_restless_minutes());

	/* A gap ends a session outright. */
	sleep_init();
	sim_steps = 0;
	t = 100000;
	t = feed(t, 11, 10, NULL);
	CHECK(sleep_is_asleep(), "no session before the gap");
	sleep_feed(1000, t + 10 * MINUTE_MS);
	CHECK(!sleep_is_asleep(), "session survived a 10 minute feed gap");
}

/* A2. Nightstand: eight hours still, every check says nothing on the sensor. */
static void t_nightstand(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	t = feed(t, 11, 5, NULL);
	CHECK(sleep_is_asleep(), "no session");

	CHECK(sleep_wear_state() == SLEEP_WEAR_UNKNOWN,
	      "verdict %d before any check, wanted UNKNOWN", sleep_wear_state());
	CHECK(sleep_wear_check_due(t, SIM_WEAR_INTERVAL_MS),
	      "first check of a session is not due immediately");

	sleep_note_wear(false, t);
	CHECK(sleep_wear_state() == SLEEP_WEAR_UNKNOWN,
	      "verdict %d after ONE negative -- one empty reading is not a "
	      "nightstand", sleep_wear_state());
	CHECK(!sleep_wear_check_due(t + SIM_WEAR_INTERVAL_MS - 1,
				    SIM_WEAR_INTERVAL_MS),
	      "check came due early");

	t += SIM_WEAR_INTERVAL_MS;
	CHECK(sleep_wear_check_due(t, SIM_WEAR_INTERVAL_MS),
	      "check not due after a full interval");
	sleep_note_wear(false, t);
	CHECK(sleep_wear_state() == SLEEP_WEAR_NOT_WORN,
	      "verdict %d after two negatives, wanted NOT_WORN",
	      sleep_wear_state());

	/* The whole point: the verdict is advisory. */
	CHECK(sleep_is_asleep(),
	      "NOT_WORN suppressed the session -- it must not");
	CHECK(sleep_total_minutes() > 0,
	      "NOT_WORN zeroed sleep_total_minutes -- it must not");

	CHECK(sleep_wear_checks() == 2 && sleep_wear_confirmed() == 0,
	      "counts %u/%u, wanted 0/2", sleep_wear_confirmed(),
	      sleep_wear_checks());
}

/* A3. Worn: one positive is enough, and it is not undone by later negatives. */
static void t_worn(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	t = feed(t, 11, 5, NULL);

	sleep_note_wear(true, t);
	CHECK(sleep_wear_state() == SLEEP_WEAR_WORN,
	      "verdict %d after one positive, wanted WORN", sleep_wear_state());

	/* Ring taken off at 2 am: still WORN, but the counts say what happened
	 * and the app can read the difference.
	 */
	for (int i = 1; i <= 6; i++) {
		t += SIM_WEAR_INTERVAL_MS;
		sleep_note_wear(false, t);
	}
	CHECK(sleep_wear_state() == SLEEP_WEAR_WORN,
	      "later negatives revoked WORN -- the session WAS worn for part "
	      "of its length, and that is what WORN claims");
	CHECK(sleep_wear_checks() == 7 && sleep_wear_confirmed() == 1,
	      "counts %u/%u, wanted 1/7", sleep_wear_confirmed(),
	      sleep_wear_checks());
}

/* A4. A PPG that never answers must not talk itself into NOT_WORN. */
static void t_ppg_dead(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	t = feed(t, 11, 5, NULL);

	for (int i = 0; i < 20; i++) {
		CHECK(sleep_wear_check_due(t, SIM_WEAR_INTERVAL_MS),
		      "check %d not due", i);
		sleep_note_wear_unavailable(t);
		CHECK(!sleep_wear_check_due(t, SIM_WEAR_INTERVAL_MS),
		      "unavailable did not restart the interval -- a dead PPG "
		      "would be retried on every pass");
		t += SIM_WEAR_INTERVAL_MS;
	}

	CHECK(sleep_wear_state() == SLEEP_WEAR_UNKNOWN,
	      "verdict %d after 20 failed checks, wanted UNKNOWN",
	      sleep_wear_state());
	CHECK(sleep_wear_checks() == 0, "failed checks counted: %u",
	      sleep_wear_checks());
}

/* A5. Nothing is recorded, and nothing is due, while awake. */
static void t_awake_is_inert(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	t = feed(t, 5, 5, NULL);

	CHECK(!sleep_is_asleep(), "asleep too early");
	CHECK(!sleep_wear_check_due(t, SIM_WEAR_INTERVAL_MS),
	      "wear check due while awake -- the LEDs would run for nothing");

	sleep_note_wear(true, t);
	sleep_note_wear(false, t);
	CHECK(sleep_wear_checks() == 0,
	      "checks recorded while awake: %u", sleep_wear_checks());
	CHECK(sleep_wear_state() == SLEEP_WEAR_UNKNOWN, "verdict %d while awake",
	      sleep_wear_state());
}

/* A6. A new session starts from nothing; the old verdict survives until then. */
static void t_verdict_lifetime(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	t = feed(t, 11, 5, NULL);
	sleep_note_wear(false, t);
	/* Fed through, not jumped: a 30 minute hole in the feed is a charge,
	 * and sleep.c ends the session on one.
	 */
	t = feed(t, 31, 5, NULL);
	sleep_note_wear(false, t);
	CHECK(sleep_wear_state() == SLEEP_WEAR_NOT_WORN, "session 1 verdict %d",
	      sleep_wear_state());

	/* Wake up. */
	t = feed(t, 3, 400, NULL);
	CHECK(!sleep_is_asleep(), "session 1 did not end");
	CHECK(sleep_wear_state() == SLEEP_WEAR_NOT_WORN,
	      "verdict %d lost at wake -- the app reads it after the session, "
	      "same as restless_minutes", sleep_wear_state());
	CHECK(!sleep_wear_check_due(t + SIM_WEAR_INTERVAL_MS,
				    SIM_WEAR_INTERVAL_MS),
	      "check due while awake");

	/* Second session, worn this time. */
	t = feed(t, 11, 5, NULL);
	CHECK(sleep_is_asleep(), "session 2 did not start");
	CHECK(sleep_wear_state() == SLEEP_WEAR_UNKNOWN,
	      "session 2 inherited session 1's verdict (%d)", sleep_wear_state());
	CHECK(sleep_wear_checks() == 0 && sleep_wear_confirmed() == 0,
	      "session 2 inherited counts %u/%u", sleep_wear_confirmed(),
	      sleep_wear_checks());
	CHECK(sleep_wear_check_due(t, SIM_WEAR_INTERVAL_MS),
	      "session 2's first check not due immediately");
	sleep_note_wear(true, t);
	CHECK(sleep_wear_state() == SLEEP_WEAR_WORN, "session 2 verdict %d",
	      sleep_wear_state());
}

/* A7. The app's reset opcode clears wear along with everything else. */
static void t_reset(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	t = feed(t, 11, 5, NULL);
	sleep_note_wear(false, t);
	t += SIM_WEAR_INTERVAL_MS;
	sleep_note_wear(false, t);
	CHECK(sleep_wear_state() == SLEEP_WEAR_NOT_WORN, "setup");

	sleep_reset();
	CHECK(sleep_wear_state() == SLEEP_WEAR_UNKNOWN,
	      "verdict %d survived sleep_reset()", sleep_wear_state());
	CHECK(sleep_wear_checks() == 0 && sleep_wear_confirmed() == 0,
	      "counts survived sleep_reset(): %u/%u", sleep_wear_confirmed(),
	      sleep_wear_checks());
	CHECK(!sleep_is_asleep(), "still asleep after sleep_reset()");
}

/* A8. Time going backwards must not become a check storm. */
static void t_time_backwards(void)
{
	int64_t t = 10000000;

	sleep_init();
	sim_steps = 0;
	t = feed(t, 11, 5, NULL);
	sleep_note_wear(false, t);

	CHECK(!sleep_wear_check_due(t - 5000, SIM_WEAR_INTERVAL_MS),
	      "a backwards timestamp made a check due -- unsigned arithmetic "
	      "here would hold the LEDs on all night");
}

/* ======================================================================== */
/* Part B: hand model of main.c's window policy                             */
/* ======================================================================== */

/*
 * Reproduces the RING_IDLE window decision and the RING_MEASURING close,
 * including the two rules that decide what this costs:
 *
 *   - a finger found during an ORDINARY window refreshes last_motion, so a
 *     worn ring never goes stale and keeps its 15 s-in-60 duty cycle. That
 *     is pre-existing behaviour and it is what STILL_TIMEOUT_MS really
 *     gates: not stillness, but being off a finger.
 *   - a finger found during a WEAR-CHECK window does not, which is the
 *     whole reason wear_check_window exists in main.c.
 *
 * Every run is done twice, with the wear check on and off, so what the
 * change costs is measured as a delta rather than asserted.
 */
struct model {
	bool worn;		/* does the PPG see skin during a window? */
	bool wear_enabled;	/* CONFIG_RING_SLEEP_WEAR_CHECK */
	/* The part starts, the FIFO comes back empty for the whole window.
	 * An empty read is not a failed one, so PPG_FAIL_LIMIT never trips
	 * and hr_state is never primed -- main.c checks hr_primed() rather
	 * than reading that as a confident "not worn".
	 */
	bool no_samples;
	int64_t last_motion;
	int64_t next_window;
	int64_t window_started;
	bool measuring;
	bool wear_window;
	bool waiting_for_finger;
	uint32_t led_ms;
	int windows;
	int wear_windows;
};

static struct model m;

static void model_poll(int64_t now)
{
	if (m.measuring) {
		uint32_t len = m.wear_window ? SIM_WEAR_WINDOW_MS
					     : SIM_MEASURE_WINDOW_MS;

		/* hr_finger_present() is false when no sample ever arrived, so
		 * a silent part looks exactly like an empty sensor to the
		 * last_motion refresh -- and the ring goes stale, which is what
		 * makes this case interesting.
		 */
		bool saw_finger = m.worn && !m.no_samples;

		if (saw_finger) {
			m.waiting_for_finger = false;

			/* main.c: only a NON-wear window may refresh this. */
			if (!m.wear_window) {
				m.last_motion = now;
			}
		}

		bool give_up = m.waiting_for_finger &&
			(now - m.window_started) > SIM_NO_FINGER_TIMEOUT_MS;
		bool done = (now - m.window_started) >= (int64_t)len;

		if (done || give_up) {
			m.led_ms += (uint32_t)(now - m.window_started);
			if ((now - m.window_started) >= SIM_WEAR_SETTLE_MS &&
			    !m.no_samples) {
				sleep_note_wear(m.worn, now);
			} else {
				sleep_note_wear_unavailable(now);
			}
			m.measuring = false;
			m.next_window = now + SIM_MEASURE_PERIOD_MS;
		}
		return;
	}

	bool stale = (now - m.last_motion) > SIM_STILL_TIMEOUT_MS;
	bool scheduled = (now >= m.next_window) && !stale;
	bool wear_due = m.wear_enabled &&
			sleep_wear_check_due(now, SIM_WEAR_INTERVAL_MS);
	bool wear_only = wear_due && !scheduled;

	if (scheduled || wear_due) {
		m.measuring = true;
		m.wear_window = wear_only;
		m.waiting_for_finger = !wear_only;
		m.window_started = now;
		m.windows++;
		if (wear_only) {
			m.wear_windows++;
		}
	}
}

/*
 * window_phase_ms slides the duty cycle relative to sleep.c's one-minute
 * buckets. It has to be varied explicitly: everything else in the model is
 * relative to the start time, so moving the start moves both clocks together
 * and changes nothing.
 */
static void model_run_at(bool worn, bool wear_enabled, int minutes,
			 int32_t peak_mg, int64_t window_phase_ms)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	memset(&m, 0, sizeof(m));
	m.worn = worn;
	m.wear_enabled = wear_enabled;
	m.last_motion = t;
	m.next_window = t + window_phase_ms;

	feed(t, minutes, peak_mg, model_poll);
}

static void model_run(bool worn, bool wear_enabled, int minutes, int32_t peak_mg)
{
	model_run_at(worn, wear_enabled, minutes, peak_mg, SIM_MEASURE_PERIOD_MS);
}

#define NIGHT_MINUTES 480		/* eight hours */

/*
 * B1. A worn ring. Its ordinary windows already answer the wear question, so
 * the check should cost almost nothing -- and, critically, must not turn the
 * night into continuous measurement by refreshing last_motion.
 */
static void t_worn_night(void)
{
	uint32_t led_off, led_on;
	enum sleep_wear verdict_off;

	model_run(true, false, NIGHT_MINUTES, 5);
	led_off = m.led_ms;
	verdict_off = sleep_wear_state();

	model_run(true, true, NIGHT_MINUTES, 5);
	led_on = m.led_ms;

	printf("\n    worn 8 h:        %u s LED without, %u s with "
	       "(+%d s), %d wear windows\n",
	       led_off / 1000U, led_on / 1000U,
	       (int)(led_on - led_off) / 1000, m.wear_windows);

	CHECK(sleep_wear_state() == SLEEP_WEAR_WORN, "verdict %d, wanted WORN",
	      sleep_wear_state());
	CHECK(verdict_off == SLEEP_WEAR_WORN,
	      "verdict %d with the check disabled -- a worn ring is confirmed "
	      "by the windows it was opening anyway, and that is free",
	      verdict_off);

	CHECK(m.wear_windows <= 2,
	      "%d wear-only windows on a worn ring -- its scheduled windows "
	      "already answer this", m.wear_windows);
	CHECK((led_on - led_off) <= 20000,
	      "the check cost %u ms of extra LED on a worn night",
	      led_on - led_off);

	/*
	 * One alignment proves nothing: whether a wear-only window opens at
	 * all depends on where sleep onset falls inside the 75 s duty cycle
	 * (a 15 s window every 60 s of idle). Sweep the phase so the bound
	 * quoted in the docs is a bound and not one lucky run.
	 *
	 * The delta is SIGNED, and genuinely goes both ways: a 6 s wear window
	 * pushes next_window out by a full period like any other window, so at
	 * some phases it displaces a 15 s scheduled window and the night ends
	 * up costing less LED than it would have. That is an accident of the
	 * duty cycle, not a saving to claim -- what matters is that the
	 * magnitude stays down in the seconds.
	 */
	int64_t worst_extra = 0, best_extra = 0;
	int worst_windows = 0;

	for (int64_t off = 1000; off <= 75000; off += 2000) {
		int64_t a, b;

		model_run_at(true, false, NIGHT_MINUTES, 5, off);
		a = (int64_t)m.led_ms;
		model_run_at(true, true, NIGHT_MINUTES, 5, off);
		b = (int64_t)m.led_ms;

		if (b - a > worst_extra) {
			worst_extra = b - a;
		}
		if (b - a < best_extra) {
			best_extra = b - a;
		}
		if (m.wear_windows > worst_windows) {
			worst_windows = m.wear_windows;
		}
		CHECK(sleep_wear_state() == SLEEP_WEAR_WORN,
		      "phase %d ms: verdict %d", (int)off, sleep_wear_state());
	}

	printf("    worn, 38 phases: LED delta %+d s to %+d s, "
	       "at most %d wear window(s)\n",
	       (int)(best_extra / 1000), (int)(worst_extra / 1000),
	       worst_windows);

	CHECK(worst_windows <= 1,
	      "%d wear-only windows in the worst phase, wanted at most 1 per "
	      "session -- the ordinary windows answer the rest", worst_windows);
	CHECK(worst_extra <= 8000,
	      "worst-case extra LED %d ms over a worn night", (int)worst_extra);
	CHECK(best_extra >= -20000,
	      "the check displaced %d ms of scheduled measurement -- that is "
	      "lost heart rate, not a saving", (int)best_extra);
}

/*
 * B2. A ring on a nightstand. This is the case the whole change exists for,
 * and the only one that pays for it.
 */
static void t_nightstand_night(void)
{
	uint32_t led_off, led_on;
	enum sleep_wear verdict_off;

	model_run(false, false, NIGHT_MINUTES, 5);
	led_off = m.led_ms;
	verdict_off = sleep_wear_state();

	model_run(false, true, NIGHT_MINUTES, 5);
	led_on = m.led_ms;

	printf("    nightstand 8 h:  %u s LED without, %u s with "
	       "(+%d s), %d wear windows, %u/%u worn\n",
	       led_off / 1000U, led_on / 1000U,
	       (int)(led_on - led_off) / 1000, m.wear_windows,
	       sleep_wear_confirmed(), sleep_wear_checks());

	CHECK(verdict_off == SLEEP_WEAR_UNKNOWN,
	      "verdict %d with the check disabled -- an unworn ring goes stale "
	      "three minutes in and opens no further windows, which is exactly "
	      "why the nightstand case was unfixable before", verdict_off);
	CHECK(sleep_wear_state() == SLEEP_WEAR_NOT_WORN,
	      "verdict %d with the check enabled, wanted NOT_WORN",
	      sleep_wear_state());

	CHECK(sleep_is_asleep(),
	      "the session was suppressed -- the verdict is advisory");
	CHECK(sleep_wear_confirmed() == 0, "%u positive checks on a table",
	      sleep_wear_confirmed());

	/* The figure main.c and Kconfig quote: ~16 windows, ~96 s. */
	CHECK(m.wear_windows >= 14 && m.wear_windows <= 18,
	      "%d wear windows over 8 h, wanted ~16", m.wear_windows);
	CHECK((led_on - led_off) >= 80000 && (led_on - led_off) <= 130000,
	      "the check cost %u ms of extra LED, docs say ~96 s",
	      led_on - led_off);
}

/*
 * B3. A PPG that starts but delivers nothing must not be read as a verdict,
 * and must not be retried on every pass through the loop.
 */
static void t_silent_ppg(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	memset(&m, 0, sizeof(m));
	m.worn = true;			/* irrelevant -- no samples reach hr.c */
	m.no_samples = true;
	m.wear_enabled = true;
	m.last_motion = t;
	m.next_window = t + SIM_MEASURE_PERIOD_MS;

	feed(t, NIGHT_MINUTES, 5, model_poll);

	CHECK(sleep_wear_state() == SLEEP_WEAR_UNKNOWN,
	      "verdict %d from a PPG that produced no samples -- an empty FIFO "
	      "is not an empty sensor", sleep_wear_state());
	CHECK(sleep_wear_checks() == 0, "%u checks counted from no data",
	      sleep_wear_checks());

	/* It goes stale like any ring that finds no finger, so the wear check
	 * is the only thing opening windows -- one per interval, not one per
	 * 200 ms poll, which is what sleep_note_wear_unavailable() buys.
	 */
	CHECK(m.wear_windows >= 14 && m.wear_windows <= 18,
	      "%d wear windows over 8 h against a silent PPG, wanted ~16 -- "
	      "far more means the interval is not being restarted",
	      m.wear_windows);

	printf("    silent PPG:      %d wear windows, %u s of LED, verdict "
	       "unknown\n", m.wear_windows, m.led_ms / 1000U);
}

/* B4. An awake, still-but-not-asleep ring opens no wear window at all. */
static void t_awake_costs_nothing(void)
{
	int64_t t = 100000;

	sleep_init();
	sim_steps = 0;
	memset(&m, 0, sizeof(m));
	m.worn = false;
	m.wear_enabled = true;
	m.last_motion = t;
	m.next_window = t + SIM_MEASURE_PERIOD_MS;

	/* Nine still minutes at a time, never the ten a session needs. */
	for (int i = 0; i < 20; i++) {
		t = feed(t, 9, 5, model_poll);
		t = feed(t, 3, 400, model_poll);
	}

	CHECK(!sleep_is_asleep(), "a session opened where none should");
	CHECK(m.wear_windows == 0,
	      "%d wear windows opened with no session -- LED current for a "
	      "question nobody asked", m.wear_windows);
}

/* ======================================================================== */

int main(void)
{
	struct {
		const char *name;
		void (*fn)(void);
	} const tests[] = {
		{ "baseline sleep behaviour unchanged", t_baseline_unchanged },
		{ "nightstand reaches NOT_WORN, advisory only", t_nightstand },
		{ "one positive is enough, and sticks", t_worn },
		{ "dead PPG stays UNKNOWN", t_ppg_dead },
		{ "awake records nothing, asks nothing", t_awake_is_inert },
		{ "verdict lifetime across sessions", t_verdict_lifetime },
		{ "sleep_reset clears wear", t_reset },
		{ "backwards time is not a check storm", t_time_backwards },
		{ "worn night: the check is nearly free", t_worn_night },
		{ "nightstand night: the check is what fixes it",
		  t_nightstand_night },
		{ "silent PPG yields no verdict", t_silent_ppg },
		{ "awake ring opens no wear window", t_awake_costs_nothing },
	};

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int before = failures;

		printf("%-48s", tests[i].name);
		fflush(stdout);
		tests[i].fn();
		printf("%s\n", (failures == before) ? " ok" : " FAILED");
	}

	printf("\n%d checks, %d failures\n", checks_run, failures);
	return failures ? 1 : 0;
}
