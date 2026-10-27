/*
 * The spot-reading scheduler: when readings fall due, what blocks them,
 * how a reading is judged, and when the next one comes after a good or a
 * bad one.
 */

#include "sim.h"

#include "spot.h"

#include <errno.h>
#include <stdio.h>

#define MIN_MS	60000LL

static struct spot_inputs in(int64_t now, bool asleep, bool worn,
			     int64_t still_ms)
{
	struct spot_inputs i = {
		.now_ms = now,
		.asleep = asleep,
		.worn = worn,
		.still_ms = still_ms,
		.blocked = false,
	};

	return i;
}

/*
 * Run a reading of `seconds` one-second heart rates at `bpm`, with
 * `moving_pct` of 50 accelerometer reads a second showing motion.
 */
static void take(enum spot_kind kind, int64_t now, int seconds, int bpm,
		 int moving_pct, uint16_t rmssd_x10, uint8_t diffs,
		 bool usable, struct spot_result *r)
{
	spot_begin(kind, now);
	for (int s = 0; s < seconds; s++) {
		spot_feed_hr((uint16_t)(bpm * 10));
		for (int k = 0; k < 50; k++) {
			spot_feed_motion(k < moving_pct / 2);
		}
	}
	spot_end(now + seconds * 1000LL, rmssd_x10, diffs, usable, r);
}

static void test_config(void)
{
	struct spot_config c;

	sim_section("config");

	spot_init();
	spot_get_config(&c);
	CHECK(c.night_min == 60 && c.day_min == 60 && c.window_s == 180,
	      "defaults %u/%u/%u", c.night_min, c.day_min, c.window_s);
	CHECK(spot_window_ms() == 180000, "window %u ms", spot_window_ms());

	c = (struct spot_config){ 9, 60, 180 };
	CHECK(spot_set_config(&c) == -EINVAL, "9 min night interval accepted");
	c = (struct spot_config){ 60, 721, 180 };
	CHECK(spot_set_config(&c) == -EINVAL, "721 min day interval accepted");
	c = (struct spot_config){ 60, 60, 59 };
	CHECK(spot_set_config(&c) == -EINVAL, "59 s window accepted");
	c = (struct spot_config){ 60, 60, 601 };
	CHECK(spot_set_config(&c) == -EINVAL, "601 s window accepted");
	c = (struct spot_config){ 0, 30, 120 };
	CHECK(spot_set_config(&c) == 0, "0/30/120 refused");
	CHECK(spot_window_ms() == 120000, "window did not change");
}

static void test_night(void)
{
	struct spot_inputs i;
	struct spot_result r;
	int64_t t0 = 100 * MIN_MS;

	sim_section("night: 15 min after onset, then hourly");

	spot_init();
	i = in(t0 - MIN_MS, false, true, 0);
	CHECK(spot_due(&i) == SPOT_NONE, "due while awake and moving");

	i = in(t0, true, true, 20 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NONE, "due at sleep onset");
	i = in(t0 + 14 * MIN_MS, true, true, 34 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NONE, "due 14 min after onset");
	i = in(t0 + 15 * MIN_MS, true, false, 35 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NONE, "due with the ring not worn");
	i = in(t0 + 15 * MIN_MS, true, true, 35 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NIGHT, "not due 15 min after onset");

	spot_begin(SPOT_NIGHT, i.now_ms);
	CHECK(spot_active() == SPOT_NIGHT, "not active after begin");
	CHECK(spot_due(&i) == SPOT_NONE, "due again while one runs");
	spot_end(i.now_ms, 0, 0, true, &r);	/* reset for take() */

	take(SPOT_NIGHT, t0 + 15 * MIN_MS, 170, 54, 0, 482, 64, true, &r);
	CHECK(r.valid, "clean night reading rejected");
	CHECK(r.kind == SPOT_NIGHT && r.hr_bpm == 54 && r.rmssd_ms == 48 &&
	      r.beats == 64 && r.motion_pct == 0,
	      "result kind %u hr %u rmssd %u beats %u motion %u", r.kind,
	      r.hr_bpm, r.rmssd_ms, r.beats, r.motion_pct);
	CHECK(spot_active() == SPOT_NONE, "still active after end");

	int64_t end = t0 + 15 * MIN_MS + 170000;

	i = in(end + 59 * MIN_MS, true, true, 60 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NONE, "due 59 min after a good reading");
	i = in(end + 60 * MIN_MS, true, true, 60 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NIGHT, "not due 60 min after a good one");
}

static void test_retry_after_bad(void)
{
	struct spot_inputs i;
	struct spot_result r;
	int64_t t0 = 0;

	sim_section("a bad reading is retried in 10 min");

	spot_init();
	i = in(t0, true, true, 0);
	(void)spot_due(&i);
	i = in(t0 + 15 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NIGHT, "not due");

	/* 20 % of reads moving: over the 10 % limit. */
	take(SPOT_NIGHT, i.now_ms, 170, 58, 20, 400, 64, true, &r);
	CHECK(!r.valid, "a restless reading was kept (motion %u%%)",
	      r.motion_pct);
	CHECK(r.motion_pct == 20, "motion %u%%, expected 20", r.motion_pct);

	int64_t end = i.now_ms + 170000;

	i = in(end + 9 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NONE, "retry before 10 min");
	i = in(end + 10 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NIGHT, "no retry at 10 min");

	/* Too few heart rates. */
	take(SPOT_NIGHT, i.now_ms, 20, 58, 0, 400, 64, true, &r);
	CHECK(!r.valid, "a reading with 20 s of heart rate was kept");

	/* A workout took it over. */
	take(SPOT_NIGHT, i.now_ms, 170, 58, 0, 400, 64, false, &r);
	CHECK(!r.valid, "an unusable reading was kept");

	/* Too few beats for HRV: heart rate kept, RMSSD not. */
	take(SPOT_NIGHT, i.now_ms, 170, 58, 0, 400, 12, true, &r);
	CHECK(r.valid && r.rmssd_ms == 0 && r.beats == 12 && r.hr_bpm == 58,
	      "12 beats: valid %d rmssd %u beats %u hr %u", r.valid,
	      r.rmssd_ms, r.beats, r.hr_bpm);

	/* PPG would not come up. */
	spot_not_started(SPOT_NIGHT, 500 * MIN_MS);
	i = in(509 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NONE, "retry before 10 min after no start");
	i = in(510 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NIGHT, "no retry 10 min after no start");
}

static void test_stop_on_motion(void)
{
	sim_section("sustained motion stops a reading early");

	spot_init();
	spot_begin(SPOT_DAY, 0);
	for (int k = 0; k < 500; k++) {
		spot_feed_motion(false);
	}
	for (int k = 0; k < 49; k++) {
		spot_feed_motion(true);
	}
	CHECK(!spot_should_stop(), "stopped on 49 moving reads");
	for (int k = 0; k < 20; k++) {
		spot_feed_motion(true);
	}
	CHECK(spot_should_stop(), "69 of 569 reads moving and not stopped");

	/* A twitch inside a long still reading does not stop it. */
	spot_begin(SPOT_NIGHT, 0);
	for (int k = 0; k < 5000; k++) {
		spot_feed_motion(k % 100 == 0 || (k > 2000 && k < 2060));
	}
	CHECK(!spot_should_stop(), "a 1 s twitch in 100 s stopped it");

	/* A hot-flash reading runs to the end whatever happens. */
	spot_begin(SPOT_HOT_FLASH, 0);
	for (int k = 0; k < 500; k++) {
		spot_feed_motion(true);
	}
	CHECK(!spot_should_stop(), "a hot-flash reading stopped on motion");
}

static void test_day(void)
{
	struct spot_inputs i;
	struct spot_result r;

	sim_section("day: worn, awake, still 3 min, hourly");

	spot_init();
	i = in(10 * MIN_MS, false, true, 2 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NONE, "due after 2 min still");
	i = in(10 * MIN_MS, false, false, 3 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NONE, "due with the ring off");
	i = in(10 * MIN_MS, false, true, 3 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_DAY, "not due after 3 min still");

	take(SPOT_DAY, i.now_ms, 170, 72, 2, 285, 64, true, &r);
	CHECK(r.valid && r.kind == SPOT_DAY && r.rmssd_ms == 29,
	      "day reading: valid %d kind %u rmssd %u", r.valid, r.kind,
	      r.rmssd_ms);

	int64_t end = i.now_ms + 170000;

	i = in(end + 30 * MIN_MS, false, true, 30 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_NONE, "due 30 min after a good one");
	i = in(end + 60 * MIN_MS, false, true, 30 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_DAY, "not due an hour after");

	struct spot_config c = { 60, 0, 180 };

	(void)spot_set_config(&c);
	CHECK(spot_due(&i) == SPOT_NONE, "due with day readings off");
}

static void test_hot_flash(void)
{
	struct spot_inputs i;
	struct spot_result r;

	sim_section("hot flash: at once, any time, and not on the charger");

	spot_init();
	spot_request_hot_flash();
	i = in(5 * MIN_MS, false, true, 0);
	CHECK(spot_due(&i) == SPOT_HOT_FLASH, "hot flash reading not due");
	take(SPOT_HOT_FLASH, i.now_ms, 170, 88, 40, 200, 40, true, &r);
	CHECK(r.kind == SPOT_HOT_FLASH && r.hr_bpm == 88,
	      "hot flash result kind %u hr %u", r.kind, r.hr_bpm);
	CHECK(spot_due(&i) == SPOT_NONE, "hot flash due again after it ran");

	spot_request_hot_flash();
	i.blocked = true;
	CHECK(spot_due(&i) == SPOT_NONE, "due while blocked");
	i.blocked = false;
	CHECK(spot_due(&i) == SPOT_NONE, "a blocked request survived");

	/* It outranks a night reading that is also due. */
	i = in(0, true, true, 0);
	(void)spot_due(&i);
	spot_request_hot_flash();
	i = in(20 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_HOT_FLASH, "night reading outranked it");
}

static void test_manual(void)
{
	struct spot_inputs i;
	struct spot_result r;

	sim_section("manual: at once, judged like a day reading, schedule kept");

	spot_init();
	/* A day reading is due in 60 min from this good one. */
	i = in(0, false, true, 3 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_DAY, "day reading not due");
	take(SPOT_DAY, 0, 170, 70, 0, 300, 64, true, &r);

	spot_request_now();
	i = in(10 * MIN_MS, false, false, 0);	/* moving, verdict stale */
	CHECK(spot_due(&i) == SPOT_MANUAL, "manual reading not due");
	take(SPOT_MANUAL, i.now_ms, 170, 75, 0, 260, 64, true, &r);
	CHECK(r.valid && r.kind == SPOT_MANUAL, "manual: valid %d kind %u",
	      r.valid, r.kind);
	CHECK(spot_due(&i) == SPOT_NONE, "manual due again after it ran");

	i = in(63 * MIN_MS, false, true, 3 * MIN_MS);
	CHECK(spot_due(&i) == SPOT_DAY,
	      "the manual reading moved the day schedule");

	/* Motion stops it like a day reading. */
	spot_begin(SPOT_MANUAL, 0);
	for (int k = 0; k < 100; k++) {
		spot_feed_motion(true);
	}
	CHECK(spot_should_stop(), "a moving manual reading kept going");
	spot_end(0, 0, 0, true, &r);

	/* The charger cancels a pending one. */
	spot_request_now();
	i.blocked = true;
	CHECK(spot_due(&i) == SPOT_NONE, "manual due while blocked");
	i.blocked = false;
	i.still_ms = 0;
	CHECK(spot_due(&i) == SPOT_NONE, "a blocked manual request survived");

	/* So does spot_drop_pending(), which main.c calls on the charger,
	 * where spot_due() never runs.
	 */
	spot_request_now();
	spot_request_hot_flash();
	spot_drop_pending();
	i = in(200 * MIN_MS, false, true, 3 * MIN_MS);
	i.worn = false;		/* nothing else may be due either */
	CHECK(spot_due(&i) == SPOT_NONE,
	      "a request survived spot_drop_pending()");
}

static void test_resleep(void)
{
	struct spot_inputs i;

	sim_section("falling asleep again restarts the 15 min delay");

	spot_init();
	i = in(0, true, true, 0);
	(void)spot_due(&i);
	i = in(10 * MIN_MS, false, true, 0);	/* woke at 10 min */
	(void)spot_due(&i);
	i = in(12 * MIN_MS, true, true, 0);	/* asleep again at 12 */
	(void)spot_due(&i);
	i = in(16 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NONE, "due 4 min after falling asleep");
	i = in(27 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NIGHT, "not due 15 min after re-sleep");
}

static void test_resleep_keeps_schedule(void)
{
	struct spot_inputs i;
	struct spot_result r;

	sim_section("re-sleep never brings the next night reading forward");

	spot_init();
	i = in(0, true, true, 0);
	(void)spot_due(&i);
	i = in(15 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NIGHT, "first night reading not due");
	take(SPOT_NIGHT, i.now_ms, 170, 55, 0, 450, 64, true, &r);

	int64_t next = i.now_ms + 170000 + 60 * MIN_MS;

	/* Stirs 5 min later, back asleep 20 min after the reading. */
	i = in(i.now_ms + 5 * MIN_MS, false, true, 0);
	(void)spot_due(&i);
	i = in(next - 40 * MIN_MS, true, true, 0);
	(void)spot_due(&i);
	i = in(next - 20 * MIN_MS, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NONE,
	      "re-sleep pulled the hourly reading 20 min forward");
	i = in(next, true, true, 0);
	CHECK(spot_due(&i) == SPOT_NIGHT, "hourly reading lost after re-sleep");
}

static void test_config_valid(void)
{
	struct spot_config c = { 0, 0, 60 };

	sim_section("spot_config_valid is the one range rule");

	CHECK(spot_config_valid(&c), "0/0/60 invalid");
	c = (struct spot_config){ 10, 720, 600 };
	CHECK(spot_config_valid(&c), "10/720/600 invalid");
	c = (struct spot_config){ 1, 60, 180 };
	CHECK(!spot_config_valid(&c), "1 min night valid");
	c = (struct spot_config){ 60, 60, 601 };
	CHECK(!spot_config_valid(&c), "601 s window valid");
}

int main(void)
{
	test_config();
	test_night();
	test_retry_after_bad();
	test_stop_on_motion();
	test_day();
	test_hot_flash();
	test_manual();
	test_resleep();
	test_resleep_keeps_schedule();
	test_config_valid();

	return sim_report("spot");
}
