/*
 * Body weight persistence (profile.c).
 *
 * The settings backend is faked here: a single key/value "flash" that
 * counts writes and can be told to fail. settings_load() is modelled by
 * calling profile.c's handler directly with a read callback over that
 * store, which is what the real one does per key.
 *
 * What this proves: the weight survives a simulated reboot, junk in flash
 * cannot take the boot down, and flash is written only when the value
 * actually changes and never more often than PROFILE_SAVE_MIN_INTERVAL_MS.
 * What it does not: that NVS on the real part behaves -- only a board can
 * say that.
 */

#include "calories.h"
#include "profile.h"
#include "sim.h"

#include <zephyr/settings/settings.h>

#include <stdio.h>
#include <string.h>

/* Defined by SETTINGS_STATIC_HANDLER_DEFINE in profile.c (see the stub). */
extern int (*const settings_stub_set_ring_profile)(const char *, size_t,
						    settings_read_cb, void *);

void profile_test_reset(void);

/* --- fake flash -------------------------------------------------------- */

static struct {
	char key[32];
	uint8_t val[8];
	size_t len;
	bool present;

	int writes;
	int fail_with;		/* nonzero: settings_save_one returns this */
} flash;

int settings_save_one(const char *name, const void *value, size_t val_len)
{
	if (flash.fail_with) {
		return flash.fail_with;
	}

	snprintf(flash.key, sizeof(flash.key), "%s", name);
	memcpy(flash.val, value, val_len);
	flash.len = val_len;
	flash.present = true;
	flash.writes++;

	return 0;
}

int settings_name_steq(const char *name, const char *key, const char **next)
{
	size_t n = strlen(key);

	*next = NULL;
	if (strncmp(name, key, n) != 0) {
		return 0;
	}
	if (name[n] == '\0') {
		return 1;
	}
	if (name[n] == '/') {
		*next = &name[n + 1];
		return 1;
	}
	return 0;
}

int settings_load(void)
{
	return 0;
}

void zstub_log(const char *fmt, ...)
{
	(void)fmt;
}

struct read_ctx {
	const uint8_t *data;
	size_t len;
	ssize_t result;		/* < 0: pretend the read failed */
};

static ssize_t read_cb(void *cb_arg, void *data, size_t len)
{
	struct read_ctx *ctx = cb_arg;

	if (ctx->result < 0) {
		return ctx->result;
	}

	size_t n = len < ctx->len ? len : ctx->len;

	memcpy(data, ctx->data, n);
	return (ssize_t)n;
}

/* Deliver one stored key the way settings_load() would. "ring/" is the
 * tree, so the handler sees what follows it.
 */
static int load_key(const char *subkey, const void *data, size_t len)
{
	struct read_ctx ctx = { data, len, 0 };

	return settings_stub_set_ring_profile(subkey, len, read_cb, &ctx);
}

/* A reboot: RAM forgotten, flash replayed through the handler. */
static void reboot(void)
{
	profile_test_reset();
	if (flash.present) {
		CHECK(strcmp(flash.key, "ring/weight") == 0,
		      "stored under \"%s\", wanted \"ring/weight\"", flash.key);
		(void)load_key("weight", flash.val, flash.len);
	}
}

static void wipe_flash(void)
{
	memset(&flash, 0, sizeof(flash));
	profile_test_reset();
}

/* --- tests ------------------------------------------------------------- */

static void test_first_boot(void)
{
	uint16_t w = 0xBEEF;

	sim_section("first boot: nothing saved, nothing claimed");
	wipe_flash();

	CHECK(!profile_saved_weight(&w), "claimed a saved weight on empty flash");
	CHECK(w == 0xBEEF, "wrote the output with nothing to report");

	profile_service(0);
	CHECK(flash.writes == 0, "wrote flash with nothing pending");
}

static void test_round_trip(void)
{
	uint16_t w = 0;

	sim_section("a weight survives a reboot");
	wipe_flash();

	profile_note_weight(823);
	profile_service(1000);
	CHECK(flash.writes == 1, "%d writes, wanted 1", flash.writes);

	reboot();
	CHECK(profile_saved_weight(&w), "no saved weight after reboot");
	CHECK(w == 823, "restored %u, wanted 823", w);
}

static void test_junk_in_flash(void)
{
	uint16_t w;
	uint8_t three[3] = { 1, 2, 3 };
	uint16_t good = 650;

	sim_section("junk in flash is skipped, never fatal");

	/* A non-zero return here would fail settings_load(), and ble_start()
	 * treats that as fatal to BLE.
	 */
	wipe_flash();
	CHECK(load_key("weight", three, sizeof(three)) == 0,
	      "wrong-size record failed the load");
	CHECK(!profile_saved_weight(&w), "accepted a 3-byte weight");

	wipe_flash();
	CHECK(load_key("height", &good, sizeof(good)) == 0,
	      "unknown key failed the load");
	CHECK(!profile_saved_weight(&w), "took an unknown key as weight");

	wipe_flash();
	CHECK(load_key("weight/old", &good, sizeof(good)) == 0,
	      "nested key failed the load");
	CHECK(!profile_saved_weight(&w), "took a nested key as weight");

	wipe_flash();
	{
		struct read_ctx ctx = { (const uint8_t *)&good, sizeof(good), -5 };

		CHECK(settings_stub_set_ring_profile("weight", sizeof(good),
						     read_cb, &ctx) == 0,
		      "failed read failed the load");
	}
	CHECK(!profile_saved_weight(&w), "kept a weight whose read failed");
}

static void test_resend_is_free(void)
{
	sim_section("resending the stored weight costs no write");
	wipe_flash();

	profile_note_weight(700);
	profile_service(0);
	reboot();
	flash.writes = 0;

	/* The app is told to resend on every connection. */
	for (int i = 0; i < 50; i++) {
		profile_note_weight(700);
		profile_service((int64_t)i * 120000);
	}
	CHECK(flash.writes == 0, "%d writes for an unchanged weight",
	      flash.writes);
}

static void test_rate_limit(void)
{
	uint16_t w = 0;
	int64_t t = 5000;

	sim_section("a burst writes at most once a minute, and the last lands");
	wipe_flash();

	/* A slider dragged from 60.0 to 90.0 kg over three seconds. */
	for (uint16_t v = 600; v <= 900; v += 10) {
		profile_note_weight(v);
		profile_service(t);
		t += 100;
	}
	CHECK(flash.writes == 1, "%d writes during the burst, wanted 1",
	      flash.writes);

	/* Keep polling as the main loop would, up to just short of the gap. */
	for (int64_t now = t; now < 5000 + PROFILE_SAVE_MIN_INTERVAL_MS;
	     now += 20) {
		profile_service(now);
	}
	CHECK(flash.writes == 1, "wrote again inside the interval");

	profile_service(5000 + PROFILE_SAVE_MIN_INTERVAL_MS);
	CHECK(flash.writes == 2, "%d writes once the interval passed, wanted 2",
	      flash.writes);

	reboot();
	CHECK(profile_saved_weight(&w) && w == 900,
	      "restored %u, wanted the last value, 900", w);
}

static void test_burst_back_to_start(void)
{
	sim_section("a burst that ends where it started writes nothing more");
	wipe_flash();

	profile_note_weight(700);
	profile_service(0);			/* write 1 */
	profile_note_weight(750);
	profile_service(1000);			/* inside the interval */
	profile_note_weight(700);		/* back to what flash holds */
	profile_service(PROFILE_SAVE_MIN_INTERVAL_MS * 2);

	CHECK(flash.writes == 1, "%d writes, wanted 1", flash.writes);
}

static void test_failed_write(void)
{
	uint16_t w = 0;
	int attempts_before;

	sim_section("a failed write retries once per interval, not every pass");
	wipe_flash();
	flash.fail_with = -28;		/* -ENOSPC */

	profile_note_weight(810);
	for (int64_t now = 0; now < PROFILE_SAVE_MIN_INTERVAL_MS * 3;
	     now += 20) {
		profile_service(now);
	}
	CHECK(flash.writes == 0, "a failing backend recorded a write");
	CHECK(!profile_saved_weight(&w), "claimed a weight that never landed");

	/* Now let it through; the pending value must still be there. */
	flash.fail_with = 0;
	attempts_before = flash.writes;
	profile_service(PROFILE_SAVE_MIN_INTERVAL_MS * 4);
	CHECK(flash.writes == attempts_before + 1, "did not retry after failing");

	reboot();
	CHECK(profile_saved_weight(&w) && w == 810,
	      "restored %u after a recovered failure, wanted 810", w);
}

static void test_backwards_time(void)
{
	sim_section("time stepping backwards is \"not yet\", not a write storm");
	wipe_flash();

	profile_note_weight(700);
	profile_service(100000);
	profile_note_weight(710);
	for (int i = 0; i < 100; i++) {
		profile_service(99000 - i);
	}
	CHECK(flash.writes == 1, "%d writes with time running backwards",
	      flash.writes);
}

static void test_clamped_value_is_saved(void)
{
	uint16_t w = 0;

	sim_section("the clamped weight is what gets saved (as main.c does it)");
	wipe_flash();
	calories_init();

	calories_set_weight(9999);			/* 999.9 kg */
	profile_note_weight(calories_weight_kg_x10());
	profile_service(0);

	reboot();
	CHECK(profile_saved_weight(&w) && w == 2500,
	      "saved %u, wanted the 250.0 kg clamp", w);

	/* And a restored value goes back through the same clamp. */
	calories_init();
	calories_set_weight(w);
	CHECK(calories_weight_kg_x10() == 2500, "restore did not apply");
}

int main(void)
{
	printf("Profile persistence tests\n");

	test_first_boot();
	test_round_trip();
	test_junk_in_flash();
	test_resend_is_free();
	test_rate_limit();
	test_burst_back_to_start();
	test_failed_write();
	test_backwards_time();
	test_clamped_value_is_saved();

	return sim_report("test_profile");
}
