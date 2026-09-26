#include "profile.h"

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(profile, LOG_LEVEL_INF);

#define PROFILE_TREE		"ring"
#define WEIGHT_KEY		"weight"

struct profile_state {
	/* What flash holds, as far as this module knows. */
	bool have_stored;
	uint16_t stored_kg_x10;

	/* What should be in flash once profile_service() gets to it. */
	bool dirty;
	uint16_t pending_kg_x10;

	/* When the last write was attempted, successful or not. */
	bool attempted;
	int64_t last_attempt_ms;
};

static struct profile_state pf;

#if defined(CONFIG_SETTINGS)

/*
 * Called from inside settings_load() once per stored key under "ring/".
 *
 * Anything unexpected is skipped rather than failed: returning an error
 * here would propagate out of settings_load(), and ble_start() treats that
 * as fatal to BLE (see the comment there). A lost weight costs the wearer
 * a resend; a lost radio costs them the ring.
 */
static int profile_set(const char *name, size_t len,
		       settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (!settings_name_steq(name, WEIGHT_KEY, &next) || next) {
		return 0;
	}

	uint16_t kg_x10;

	if (len != sizeof(kg_x10)) {
		LOG_WRN("stored weight is %u bytes, expected %u -- ignored",
			(unsigned int)len, (unsigned int)sizeof(kg_x10));
		return 0;
	}

	if (read_cb(cb_arg, &kg_x10, sizeof(kg_x10)) != (ssize_t)sizeof(kg_x10)) {
		LOG_WRN("stored weight could not be read -- ignored");
		return 0;
	}

	pf.have_stored = true;
	pf.stored_kg_x10 = kg_x10;

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ring_profile, PROFILE_TREE, NULL, profile_set,
			       NULL, NULL);

static int save_weight(uint16_t kg_x10)
{
	return settings_save_one(PROFILE_TREE "/" WEIGHT_KEY, &kg_x10,
				 sizeof(kg_x10));
}

#else

static int save_weight(uint16_t kg_x10)
{
	(void)kg_x10;
	return -ENOTSUP;
}

#endif /* CONFIG_SETTINGS */

bool profile_saved_weight(uint16_t *kg_x10)
{
	if (!pf.have_stored) {
		return false;
	}

	*kg_x10 = pf.stored_kg_x10;
	return true;
}

void profile_note_weight(uint16_t kg_x10)
{
	/*
	 * Compared against flash, not against the last pending value: a
	 * burst that goes 70 -> 80 -> 70 before the interval is up ends
	 * where it started, and should cost no write at all.
	 */
	pf.pending_kg_x10 = kg_x10;
	pf.dirty = !(pf.have_stored && pf.stored_kg_x10 == kg_x10);
}

void profile_service(int64_t now_ms)
{
	if (!pf.dirty) {
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

	int err = save_weight(pf.pending_kg_x10);

	if (err) {
		LOG_ERR("saving body weight failed (%d) -- kept in RAM, will "
			"retry", err);
		return;
	}

	pf.have_stored = true;
	pf.stored_kg_x10 = pf.pending_kg_x10;
	pf.dirty = false;

	LOG_INF("body weight saved: %u.%u kg", pf.stored_kg_x10 / 10U,
		pf.stored_kg_x10 % 10U);
}

#if defined(PROFILE_TESTING)
/* Host tests only: forget everything, as a reboot would. */
void profile_test_reset(void)
{
	memset(&pf, 0, sizeof(pf));
}
#endif
