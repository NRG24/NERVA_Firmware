#include "thermo.h"

#include <errno.h>
#include <string.h>

/*
 * Everything inside is in micro-degrees C. The slow baseline moves by
 * diff * dt / (tau + dt) per sample, which truncates to nothing for any
 * diff under (tau + dt) / dt units -- 241 of them at tau 40 min and a
 * 10 s sample. In millidegrees that would leave it stuck a quarter of a
 * degree from the truth; in microdegrees it is 0.0002 C. int32 still
 * holds +-2000 C.
 */
#define UC_PER_C100		10000

#define TEMP_TAU_MS		60000LL
#define BASE_RISE_TAU_MS	(40LL * 60 * 1000)
#define BASE_FALL_TAU_MS	(5LL * 60 * 1000)

/* Worn, off the charger and out of a workout this long before judging. */
#define SETTLE_MS		(30LL * 60 * 1000)
/* Night mode: asleep this long first... */
#define NIGHT_SETTLE_MS		(20LL * 60 * 1000)
/*
 * ...and still allowed this long after waking, because a hot flash is
 * one of the things that wakes people up. sleep.c already takes three
 * active minutes to end a session; this covers the rest of the rise.
 */
#define NIGHT_GRACE_MS		(10LL * 60 * 1000)

/* "Recent": the rise must have happened within this many minutes. */
#define RECENT_MIN		10
#define END_UC			(30 * UC_PER_C100 / 10)	/* 0.3 C */
#define MAX_FLASH_MS		(30LL * 60 * 1000)
#define REFRACTORY_MS		(15LL * 60 * 1000)

/* A gap in the samples this long starts everything again. */
#define GAP_MS			(5LL * 60 * 1000)

static struct {
	uint8_t mode;
	uint8_t rise_c10;

	bool primed;
	int64_t last_ms;
	int32_t temp_uc;
	int32_t base_uc;

	int64_t eligible_since;		/* -1 = not eligible now */
	int64_t asleep_since;		/* -1 = awake */
	int64_t night_ok_until;
	bool ready;

	/* temp once a minute since ready; [next] is the oldest once full */
	int32_t recent_uc[RECENT_MIN + 1];
	uint8_t recent_n;
	uint8_t recent_next;
	int64_t next_recent_ms;

	bool in_flash;
	bool start_pending;
	int64_t flash_start_ms;
	int64_t refractory_until;
	uint8_t count;

	int32_t peak_rise_uc;
} th;

/* One step of an exponential average with time constant tau. */
static int32_t ema_step(int32_t diff, int64_t dt_ms, int64_t tau_ms)
{
	return (int32_t)((int64_t)diff * dt_ms / (tau_ms + dt_ms));
}

static void unready(void)
{
	th.ready = false;
	/* A flash in progress is abandoned, not ended: it is still counted. */
	th.in_flash = false;
}

void thermo_init(void)
{
	memset(&th, 0, sizeof(th));
	th.mode = CONFIG_RING_HOT_FLASH_MODE;
	th.rise_c10 = CONFIG_RING_HOT_FLASH_RISE_C10;
	th.eligible_since = -1;
	th.asleep_since = -1;
}

int thermo_set_config(uint8_t mode, uint8_t rise_c10)
{
	if (mode > THERMO_MODE_ALWAYS || rise_c10 < THERMO_RISE_MIN_C10 ||
	    rise_c10 > THERMO_RISE_MAX_C10) {
		return -EINVAL;
	}

	th.mode = mode;
	th.rise_c10 = rise_c10;
	if (mode == THERMO_MODE_OFF) {
		th.in_flash = false;
		th.start_pending = false;
	}

	return 0;
}

void thermo_get_config(uint8_t *mode, uint8_t *rise_c10)
{
	*mode = th.mode;
	*rise_c10 = th.rise_c10;
}

static void recent_push(int32_t uc)
{
	th.recent_uc[th.recent_next] = uc;
	th.recent_next = (uint8_t)((th.recent_next + 1U) % (RECENT_MIN + 1U));
	if (th.recent_n < RECENT_MIN + 1U) {
		th.recent_n++;
	}
}

/* temp RECENT_MIN minutes ago, or as far back as there is. */
static int32_t recent_oldest(void)
{
	if (th.recent_n < RECENT_MIN + 1U) {
		return th.recent_uc[0];
	}

	return th.recent_uc[th.recent_next];
}

static bool onset_allowed(int64_t now_ms)
{
	switch (th.mode) {
	case THERMO_MODE_ALWAYS:
		return true;
	case THERMO_MODE_NIGHT:
		return now_ms < th.night_ok_until;
	default:
		return false;
	}
}

void thermo_feed(int16_t temp_c100, int64_t now_ms, bool worn, bool blocked,
		 bool asleep)
{
	int32_t sample_uc = (int32_t)temp_c100 * UC_PER_C100;
	bool gap;
	int64_t dt;

	if (temp_c100 == THERMO_NONE) {
		return;
	}

	gap = !th.primed || now_ms < th.last_ms ||
	      (now_ms - th.last_ms) > GAP_MS;
	dt = gap ? 0 : now_ms - th.last_ms;

	if (gap) {
		th.temp_uc = sample_uc;
		th.eligible_since = -1;
		th.asleep_since = -1;
		th.night_ok_until = 0;
		unready();
	} else {
		th.temp_uc += ema_step(sample_uc - th.temp_uc, dt, TEMP_TAU_MS);
	}
	th.primed = true;
	th.last_ms = now_ms;

	if (!worn || blocked) {
		th.eligible_since = -1;
		unready();
	} else if (th.eligible_since < 0) {
		th.eligible_since = now_ms;
	}

	if (!asleep) {
		th.asleep_since = -1;
	} else if (th.asleep_since < 0) {
		th.asleep_since = now_ms;
	}
	if (th.asleep_since >= 0 &&
	    (now_ms - th.asleep_since) >= NIGHT_SETTLE_MS) {
		th.night_ok_until = now_ms + NIGHT_GRACE_MS;
	}

	if (!th.ready && th.eligible_since >= 0 &&
	    (now_ms - th.eligible_since) >= SETTLE_MS) {
		/*
		 * Start the baseline where the ring is now. Seeding it any
		 * earlier would put it on the cold side of the warm-up after
		 * the ring went on, and the first thing seen would be a "rise".
		 */
		th.ready = true;
		th.base_uc = th.temp_uc;
		th.recent_n = 0;
		th.recent_next = 0;
		th.next_recent_ms = now_ms;
	}

	if (!th.ready) {
		return;
	}

	if (now_ms >= th.next_recent_ms) {
		recent_push(th.temp_uc);
		th.next_recent_ms = now_ms + 60000;
	}

	int32_t rise = th.temp_uc - th.base_uc;

	if (rise > th.peak_rise_uc) {
		th.peak_rise_uc = rise;
	}

	if (th.in_flash) {
		bool back = rise <= END_UC;
		bool too_long = (now_ms - th.flash_start_ms) >= MAX_FLASH_MS;

		if (back || too_long) {
			/*
			 * Half an hour and still up is not a hot flash any
			 * more, it is where the ring now sits: take it as the
			 * baseline rather than wait 40 minutes for it.
			 */
			if (!back) {
				th.base_uc = th.temp_uc;
			}
			th.in_flash = false;
			th.refractory_until = now_ms + REFRACTORY_MS;
		}
		/* Frozen while a flash runs, so it cannot chase the flash. */
		return;
	}

	int32_t threshold = (int32_t)th.rise_c10 * (UC_PER_C100 * 10);
	int32_t gained = th.temp_uc - recent_oldest();

	if (onset_allowed(now_ms) && now_ms >= th.refractory_until &&
	    rise >= threshold && gained >= threshold * 3 / 4) {
		th.in_flash = true;
		th.start_pending = true;
		th.flash_start_ms = now_ms;
		th.count++;
		return;
	}

	th.base_uc += ema_step(th.temp_uc - th.base_uc, dt,
			       th.temp_uc > th.base_uc ? BASE_RISE_TAU_MS
						       : BASE_FALL_TAU_MS);
}

bool thermo_take_hot_flash_start(void)
{
	bool pending = th.start_pending;

	th.start_pending = false;
	return pending;
}

bool thermo_in_hot_flash(void)
{
	return th.in_flash;
}

bool thermo_ready(void)
{
	return th.ready;
}

int16_t thermo_temp_c100(void)
{
	return th.primed ? (int16_t)(th.temp_uc / UC_PER_C100) : THERMO_NONE;
}

int16_t thermo_baseline_c100(void)
{
	return th.ready ? (int16_t)(th.base_uc / UC_PER_C100) : THERMO_NONE;
}

uint8_t thermo_take_peak_rise_c10(void)
{
	int32_t c10 = th.peak_rise_uc / (UC_PER_C100 * 10);

	th.peak_rise_uc = 0;
	return (uint8_t)(c10 < 0 ? 0 : c10 > 255 ? 255 : c10);
}

uint8_t thermo_hot_flash_count(void)
{
	return th.count;
}
