#include "spot.h"

#include <errno.h>
#include <string.h>

/* First night reading this long after sleep onset. */
#define NIGHT_FIRST_MS		(15LL * 60 * 1000)
/* Still this long before a daytime reading. Matches main.c's
 * STILL_TIMEOUT_MS, which is when the ordinary windows stop.
 */
#define DAY_STILL_MS		180000LL
/* An invalid or failed reading tries again this much later. */
#define RETRY_MS		(10LL * 60 * 1000)

#define MIN_HR_SECONDS		30
#define MIN_DIFFS		20
#define MOTION_MAX_PCT		10
/* ~1 s of motion at the 20 ms measuring poll before giving up early. */
#define MOTION_STOP_READS	50U

static struct {
	struct spot_config cfg;

	bool was_asleep;
	int64_t next_night_ms;
	int64_t next_day_ms;
	bool hot_flash_pending;
	bool manual_pending;

	enum spot_kind active;
	uint32_t hr_sum_x10;
	uint16_t hr_count;
	uint32_t reads;
	uint32_t moving_reads;
} sp;

static bool interval_ok(uint16_t min)
{
	return min == 0U ||
	       (min >= SPOT_INTERVAL_MIN_MIN && min <= SPOT_INTERVAL_MAX_MIN);
}

/* Kconfig can only say 0-720; 1-9 would run readings back to back. */
static uint16_t interval_clamp(uint16_t min)
{
	if (min == 0U) {
		return 0U;
	}
	return min < SPOT_INTERVAL_MIN_MIN ? SPOT_INTERVAL_MIN_MIN
	       : min > SPOT_INTERVAL_MAX_MIN ? SPOT_INTERVAL_MAX_MIN : min;
}

void spot_init(void)
{
	memset(&sp, 0, sizeof(sp));
	sp.cfg.night_min = interval_clamp(CONFIG_RING_SPOT_NIGHT_MIN);
	sp.cfg.day_min = interval_clamp(CONFIG_RING_SPOT_DAY_MIN);
	sp.cfg.window_s = CONFIG_RING_SPOT_WINDOW_S < SPOT_WINDOW_MIN_S
			  ? SPOT_WINDOW_MIN_S
			  : CONFIG_RING_SPOT_WINDOW_S > SPOT_WINDOW_MAX_S
			  ? SPOT_WINDOW_MAX_S : CONFIG_RING_SPOT_WINDOW_S;
}

bool spot_config_valid(const struct spot_config *cfg)
{
	return interval_ok(cfg->night_min) && interval_ok(cfg->day_min) &&
	       cfg->window_s >= SPOT_WINDOW_MIN_S &&
	       cfg->window_s <= SPOT_WINDOW_MAX_S;
}

int spot_set_config(const struct spot_config *cfg)
{
	if (!spot_config_valid(cfg)) {
		return -EINVAL;
	}

	sp.cfg = *cfg;
	return 0;
}

void spot_get_config(struct spot_config *cfg)
{
	*cfg = sp.cfg;
}

void spot_request_hot_flash(void)
{
	sp.hot_flash_pending = true;
}

void spot_request_now(void)
{
	sp.manual_pending = true;
}

enum spot_kind spot_due(const struct spot_inputs *in)
{
	/*
	 * Falling asleep can push the next reading later, never earlier: a
	 * restless sleeper re-entering sleep every half hour must not get a
	 * reading every half hour.
	 */
	if (in->asleep && !sp.was_asleep &&
	    sp.next_night_ms < in->now_ms + NIGHT_FIRST_MS) {
		sp.next_night_ms = in->now_ms + NIGHT_FIRST_MS;
	}
	sp.was_asleep = in->asleep;

	if (sp.active != SPOT_NONE) {
		return SPOT_NONE;
	}

	if (in->blocked) {
		/* A hot flash on the charger is not going to be measured. */
		sp.hot_flash_pending = false;
		sp.manual_pending = false;
		return SPOT_NONE;
	}

	if (sp.hot_flash_pending) {
		return SPOT_HOT_FLASH;
	}

	/*
	 * Not gated on `worn`: the wearer just pressed a button, and the
	 * last verdict may be from before the ring went back on. A window
	 * with no finger gives up in 6 s anyway.
	 */
	if (sp.manual_pending) {
		return SPOT_MANUAL;
	}

	if (!in->worn) {
		return SPOT_NONE;
	}

	if (in->asleep) {
		return (sp.cfg.night_min != 0U && in->now_ms >= sp.next_night_ms)
		       ? SPOT_NIGHT : SPOT_NONE;
	}

	return (sp.cfg.day_min != 0U && in->still_ms >= DAY_STILL_MS &&
		in->now_ms >= sp.next_day_ms) ? SPOT_DAY : SPOT_NONE;
}

void spot_begin(enum spot_kind kind, int64_t now_ms)
{
	(void)now_ms;

	sp.active = kind;
	sp.hr_sum_x10 = 0;
	sp.hr_count = 0;
	sp.reads = 0;
	sp.moving_reads = 0;
	if (kind == SPOT_HOT_FLASH) {
		sp.hot_flash_pending = false;
	}
	if (kind == SPOT_MANUAL) {
		sp.manual_pending = false;
	}
}

static void schedule(enum spot_kind kind, int64_t now_ms, bool valid)
{
	int64_t *next;
	int64_t interval;

	switch (kind) {
	case SPOT_NIGHT:
		next = &sp.next_night_ms;
		interval = (int64_t)sp.cfg.night_min * 60000;
		break;
	case SPOT_DAY:
		next = &sp.next_day_ms;
		interval = (int64_t)sp.cfg.day_min * 60000;
		break;
	default:
		return;
	}

	*next = now_ms + ((valid || interval < RETRY_MS) ? interval : RETRY_MS);
}

void spot_not_started(enum spot_kind kind, int64_t now_ms)
{
	if (kind == SPOT_HOT_FLASH) {
		sp.hot_flash_pending = false;
	}
	if (kind == SPOT_MANUAL) {
		sp.manual_pending = false;
	}
	schedule(kind, now_ms, false);
}

enum spot_kind spot_active(void)
{
	return sp.active;
}

uint32_t spot_window_ms(void)
{
	return (uint32_t)sp.cfg.window_s * 1000U;
}

void spot_feed_hr(uint16_t bpm_x10)
{
	if (sp.active == SPOT_NONE || bpm_x10 == 0U ||
	    sp.hr_count == UINT16_MAX) {
		return;
	}

	sp.hr_sum_x10 += bpm_x10;
	sp.hr_count++;
}

void spot_feed_motion(bool moving)
{
	if (sp.active == SPOT_NONE) {
		return;
	}

	sp.reads++;
	if (moving) {
		sp.moving_reads++;
	}
}

static bool too_much_motion(void)
{
	return sp.moving_reads * 100U > sp.reads * MOTION_MAX_PCT;
}

bool spot_should_stop(void)
{
	/*
	 * A hot-flash reading runs to the end regardless: waking up and
	 * throwing the covers off is part of the event, and its heart
	 * rate is wanted even if it is a rough one.
	 */
	if (sp.active == SPOT_NONE || sp.active == SPOT_HOT_FLASH) {
		return false;
	}

	return sp.moving_reads >= MOTION_STOP_READS && too_much_motion();
}

void spot_end(int64_t now_ms, uint16_t rmssd_x10, uint8_t diffs, bool usable,
	      struct spot_result *out)
{
	enum spot_kind kind = sp.active;
	uint32_t hr_bpm = sp.hr_count
			  ? (sp.hr_sum_x10 / sp.hr_count + 5U) / 10U : 0U;
	uint32_t rmssd_ms = (diffs >= MIN_DIFFS) ? (rmssd_x10 + 5U) / 10U : 0U;

	memset(out, 0, sizeof(*out));
	out->kind = (uint8_t)kind;
	out->hr_bpm = (uint8_t)(hr_bpm > 255U ? 255U : hr_bpm);
	out->rmssd_ms = (uint8_t)(rmssd_ms > 255U ? 255U : rmssd_ms);
	out->beats = diffs;
	out->motion_pct = (uint8_t)(sp.reads
				    ? sp.moving_reads * 100U / sp.reads : 0U);
	out->valid = usable && kind != SPOT_NONE &&
		     sp.hr_count >= MIN_HR_SECONDS && !too_much_motion();

	sp.active = SPOT_NONE;
	schedule(kind, now_ms, out->valid);
}
