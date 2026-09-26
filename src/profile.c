#include "profile.h"

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(profile, LOG_LEVEL_INF);

#define PROFILE_TREE		"ring"

/*
 * One stored value. Each is small, fixed-size and written whole, so a
 * record is just bytes: this module never interprets them beyond copying
 * them in and out, and a size mismatch is the only corruption it checks.
 */
#define RECORD_MAX_LEN		2

struct record {
	const char *key;	/* under PROFILE_TREE */
	uint8_t len;

	/* What flash holds, as far as this module knows. */
	bool have_stored;
	uint8_t stored[RECORD_MAX_LEN];

	/* What should be in flash once profile_service() gets to it. */
	bool dirty;
	uint8_t pending[RECORD_MAX_LEN];
};

enum { REC_WEIGHT, REC_BODY, REC_COUNT };

struct profile_state {
	struct record rec[REC_COUNT];

	/*
	 * When the last write was attempted, successful or not. Shared by
	 * every record: the limit protects the flash, and the flash does not
	 * care which key a write belongs to.
	 */
	bool attempted;
	int64_t last_attempt_ms;
};

static struct profile_state pf = {
	.rec = {
		[REC_WEIGHT] = { .key = "weight", .len = sizeof(uint16_t) },
		[REC_BODY]   = { .key = "body",   .len = 2 },
	},
};

#if defined(CONFIG_SETTINGS)

/*
 * Called from inside settings_load() once per stored key under "ring/".
 *
 * Anything unexpected is skipped rather than failed: returning an error
 * here would propagate out of settings_load(), and ble_start() treats that
 * as fatal to BLE (see the comment there). A lost profile costs the wearer
 * a resend; a lost radio costs them the ring.
 */
static int profile_set(const char *name, size_t len,
		       settings_read_cb read_cb, void *cb_arg)
{
	for (int i = 0; i < REC_COUNT; i++) {
		struct record *r = &pf.rec[i];
		const char *next;
		uint8_t buf[RECORD_MAX_LEN];

		if (!settings_name_steq(name, r->key, &next) || next) {
			continue;
		}

		if (len != r->len) {
			LOG_WRN("stored %s is %u bytes, expected %u -- ignored",
				r->key, (unsigned int)len, (unsigned int)r->len);
			return 0;
		}

		if (read_cb(cb_arg, buf, r->len) != (ssize_t)r->len) {
			LOG_WRN("stored %s could not be read -- ignored", r->key);
			return 0;
		}

		memcpy(r->stored, buf, r->len);
		r->have_stored = true;
		return 0;
	}

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ring_profile, PROFILE_TREE, NULL, profile_set,
			       NULL, NULL);

static int save_record(const struct record *r)
{
	char name[24];

	(void)snprintf(name, sizeof(name), PROFILE_TREE "/%s", r->key);

	return settings_save_one(name, r->pending, r->len);
}

#else

static int save_record(const struct record *r)
{
	(void)r;
	return -ENOTSUP;
}

#endif /* CONFIG_SETTINGS */

static bool saved(int idx, void *out)
{
	const struct record *r = &pf.rec[idx];

	if (!r->have_stored) {
		return false;
	}

	memcpy(out, r->stored, r->len);
	return true;
}

static void note(int idx, const void *value)
{
	struct record *r = &pf.rec[idx];

	/*
	 * Compared against flash, not against the last pending value: a
	 * burst that goes 70 -> 80 -> 70 before the interval is up ends
	 * where it started, and should cost no write at all.
	 */
	memcpy(r->pending, value, r->len);
	r->dirty = !(r->have_stored && memcmp(r->stored, value, r->len) == 0);
}

bool profile_saved_weight(uint16_t *kg_x10)
{
	return saved(REC_WEIGHT, kg_x10);
}

void profile_note_weight(uint16_t kg_x10)
{
	note(REC_WEIGHT, &kg_x10);
}

bool profile_saved_body(uint8_t *age_years, uint8_t *sex)
{
	uint8_t v[2];

	if (!saved(REC_BODY, v)) {
		return false;
	}

	*age_years = v[0];
	*sex = v[1];
	return true;
}

void profile_note_body(uint8_t age_years, uint8_t sex)
{
	uint8_t v[2] = { age_years, sex };

	note(REC_BODY, v);
}

void profile_service(int64_t now_ms)
{
	bool any_dirty = false;

	for (int i = 0; i < REC_COUNT; i++) {
		any_dirty |= pf.rec[i].dirty;
	}

	if (!any_dirty) {
		return;
	}

	/*
	 * Signed, for the same reason as sleep_wear_check_due(): a small
	 * backwards step must read as "not yet", not as an enormous
	 * elapsed time and a write on every pass.
	 */
	if (pf.attempted &&
	    (now_ms - pf.last_attempt_ms) < PROFILE_SAVE_MIN_INTERVAL_MS) {
		return;
	}

	pf.attempted = true;
	pf.last_attempt_ms = now_ms;

	/*
	 * Every dirty record in this one pass. The app sends weight, age and
	 * sex together, so this is normally one or two small writes a minute
	 * at worst -- not one per record per interval.
	 */
	for (int i = 0; i < REC_COUNT; i++) {
		struct record *r = &pf.rec[i];

		if (!r->dirty) {
			continue;
		}

		int err = save_record(r);

		if (err) {
			LOG_ERR("saving %s failed (%d) -- kept in RAM, will retry",
				r->key, err);
			continue;
		}

		memcpy(r->stored, r->pending, r->len);
		r->have_stored = true;
		r->dirty = false;

		LOG_INF("profile saved: %s", r->key);
	}
}

#if defined(PROFILE_TESTING)
/* Host tests only: forget everything, as a reboot would. */
void profile_test_reset(void)
{
	pf.attempted = false;
	pf.last_attempt_ms = 0;
	for (int i = 0; i < REC_COUNT; i++) {
		pf.rec[i].have_stored = false;
		pf.rec[i].dirty = false;
		memset(pf.rec[i].stored, 0, sizeof(pf.rec[i].stored));
		memset(pf.rec[i].pending, 0, sizeof(pf.rec[i].pending));
	}
}
#endif
