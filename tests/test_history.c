/*
 * On-ring history (history.c) against a fake NOR flash.
 *
 * The fake behaves like the nRF52's: erase is whole 4 kB pages to 0xff,
 * writes are whole 32-bit words and can only clear bits. It also counts
 * every write that lands on a byte that was not erased, because flash will
 * not refuse one -- it silently ANDs the old and new values together, which
 * is exactly the corruption this module must never cause.
 *
 * What this proves: records come back in order with no gaps across
 * reboots, wrap-around and a torn write; a partition full of stale code
 * bytes is formatted rather than misread; nothing is ever written on top
 * of unerased flash; the wall clock survives a soft reset; and a firmware
 * upload that takes the partition as MCUboot's slot 1 neither writes into
 * the log nor restarts its numbering.
 * What it does not: that the real flash driver, the MPSL timeslots or the
 * real noinit section behave -- only the board can say that.
 */

#include "history.h"
#include "sim.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * The v2 record is a wire format: APP_INTEGRATION.md section 16 and
 * tools/history_sync.py read it by these offsets. Moving a field breaks
 * every app silently, so it breaks this build instead.
 */
#define AT(field, off)							\
	_Static_assert(offsetof(struct hist_record, field) == (off),	\
		       #field " moved")
AT(seq, 0);
AT(time_s, 4);
AT(steps, 8);
AT(temp_c100, 10);
AT(skin_c100, 12);
AT(hr_bpm, 14);
AT(hr_min_bpm, 15);
AT(hr_max_bpm, 16);
AT(rmssd_ms, 17);
AT(rest_hr_bpm, 18);
AT(rest_rmssd_ms, 19);
AT(rest_beats, 20);
AT(temp_rise_c10, 21);
AT(motion_pct, 22);
AT(battery_20mv, 23);
AT(flags, 24);
AT(hot_flashes, 25);
AT(boot_id, 26);
AT(flags2, 27);
AT(reserved, 28);
AT(crc, 31);
_Static_assert(sizeof(struct hist_record) == 32, "record is not 32 bytes");

/* --- fake kernel ------------------------------------------------------- */

static int64_t fake_ms;
static bool verbose;

int64_t k_uptime_get(void)
{
	return fake_ms;
}

k_spinlock_key_t k_spin_lock(struct k_spinlock *l)
{
	(void)l;
	return 0;
}

void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key)
{
	(void)l;
	(void)key;
}

int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)mutex;
	(void)timeout;
	return 0;
}

int k_mutex_unlock(struct k_mutex *mutex)
{
	(void)mutex;
	return 0;
}

void zstub_log(const char *fmt, ...)
{
	va_list ap;

	if (!verbose) {
		return;
	}
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* --- fake flash -------------------------------------------------------- */

#define PART_SIZE	0x2c000U	/* matches history_partition in the DTS */
#define PAGE		4096U
#define REC		((uint32_t)sizeof(struct hist_record))
#define CAPACITY	((PART_SIZE - PAGE) / REC)	/* 5504 slots */
#define PER_PAGE	(PAGE / REC)

static uint8_t flash[PART_SIZE];
static struct flash_area area = { .fa_size = PART_SIZE };
static int overwrites;		/* writes onto unerased bytes: must stay 0 */
static int bad_alignment;
static int erases;

int flash_area_open(uint8_t id, const struct flash_area **fa)
{
	(void)id;
	*fa = &area;
	return 0;
}

int flash_area_read(const struct flash_area *fa, off_t off, void *dst,
		    size_t len)
{
	(void)fa;
	if (off < 0 || (size_t)off + len > PART_SIZE) {
		return -22;
	}
	memcpy(dst, &flash[off], len);
	return 0;
}

int flash_area_write(const struct flash_area *fa, off_t off, const void *src,
		     size_t len)
{
	const uint8_t *s = src;

	(void)fa;
	if (off < 0 || (size_t)off + len > PART_SIZE) {
		return -22;
	}
	if ((off % 4) != 0 || (len % 4) != 0) {
		bad_alignment++;
		return -22;
	}
	for (size_t i = 0; i < len; i++) {
		if (flash[off + i] != 0xff) {
			overwrites++;
		}
		flash[off + i] &= s[i];
	}
	return 0;
}

int flash_area_erase(const struct flash_area *fa, off_t off, size_t len)
{
	(void)fa;
	if ((off % PAGE) != 0 || (len % PAGE) != 0 ||
	    (size_t)off + len > PART_SIZE) {
		return -22;
	}
	memset(&flash[off], 0xff, len);
	erases++;
	return 0;
}

/* --- fake settings ----------------------------------------------------- */

void history_test_power_cycle(void);

/* history.c's handler, left where the stub macro puts it. */
extern int (*const settings_stub_set_ring_history)(const char *, size_t,
						   settings_read_cb, void *);

static uint32_t stored_floor;
static bool have_floor;
static int floor_saves;

int settings_save_one(const char *name, const void *value, size_t val_len)
{
	if (strcmp(name, "hist/seq") != 0 || val_len != sizeof(stored_floor)) {
		return -22;
	}
	memcpy(&stored_floor, value, sizeof(stored_floor));
	have_floor = true;
	floor_saves++;
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

static ssize_t read_floor(void *cb_arg, void *data, size_t len)
{
	memcpy(data, cb_arg, len);
	return (ssize_t)len;
}

/*
 * A real power cycle: RAM forgets, settings_load() (inside ble_start())
 * hands back what was saved, then history_init().
 */
static void power_cycle(void)
{
	history_test_power_cycle();
	if (have_floor) {
		(void)settings_stub_set_ring_history("seq", sizeof(stored_floor),
						     read_floor, &stored_floor);
	}
	fake_ms = 500;
}

/* What an upload leaves in slot 1: image bytes, over the whole thing. */
static void upload_scribbles(uint32_t bytes)
{
	uint32_t x = 777;

	for (uint32_t i = 0; i < bytes && i < PART_SIZE; i++) {
		x = x * 1103515245U + 12345U;
		flash[i] = (uint8_t)(x >> 16);
	}
}

/* --- helpers ----------------------------------------------------------- */

/* Pretend the ring reset: uptime starts again, RAM state is rediscovered. */
static void reboot(void)
{
	fake_ms = 500;		/* boot takes a moment before history_init() */
	CHECK(history_init() == 0, "history_init after reboot");
}

static int append_n(int n, uint8_t hr)
{
	int ok = 0;

	for (int i = 0; i < n; i++) {
		struct hist_record r = {
			.hr_bpm = hr,
			.hr_min_bpm = (uint8_t)(hr - 5),
			.hr_max_bpm = (uint8_t)(hr + 9),
			.steps = (uint16_t)i,
			.temp_c100 = -125,	/* sign survives */
			.skin_c100 = 3412,
			.rest_hr_bpm = 51,
			.rest_rmssd_ms = 47,
			.rest_beats = 64,
			.temp_rise_c10 = 17,
			.motion_pct = 3,
			.hot_flashes = 1,
			.battery_20mv = 190,
			.flags = HIST_FLAG_WORN | HIST_FLAG_HOT_FLASH |
				 HIST_FLAG_SPOT_NIGHT,
		};

		fake_ms += 300000;
		if (history_append(&r) == 0) {
			ok++;
		}
	}
	return ok;
}

struct walk {
	uint32_t count;
	uint32_t first;
	uint32_t last;
	uint32_t gaps;		/* consecutive records whose seq is not +1 */
	uint32_t bad_crc;
	uint32_t oldest;
	uint32_t newest;
};

static struct walk walk_from(uint32_t from_seq)
{
	struct hist_iter it;
	struct hist_record r;
	struct walk w = { 0 };

	history_iter_begin(&it, from_seq);
	while (history_iter_next(&it, &r)) {
		if (w.count == 0) {
			w.first = r.seq;
		} else if (r.seq != w.last + 1) {
			w.gaps++;
		}
		if (r.crc != history_crc8((const uint8_t *)&r, REC - 1U)) {
			w.bad_crc++;
		}
		w.last = r.seq;
		w.count++;
	}
	w.oldest = it.oldest_seq;
	w.newest = it.last_seq;
	return w;
}

/* --- tests ------------------------------------------------------------- */

static void test_format_over_garbage(void)
{
	sim_section("a partition full of old code is formatted, not misread");

	/* What the region holds after years as code space: anything. */
	uint32_t x = 12345;

	for (uint32_t i = 0; i < PART_SIZE; i++) {
		x = x * 1103515245U + 12345U;
		flash[i] = (uint8_t)(x >> 16);
	}

	fake_ms = 500;
	CHECK(history_init() == 0, "init succeeds over garbage");
	CHECK(erases == (int)(PART_SIZE / PAGE),
	      "every page erased once by the format (%d)", erases);

	struct walk w = walk_from(0);

	CHECK(w.count == 0, "no records after a format (got %u)", w.count);
	CHECK(history_boot_id() == 0, "first boot is boot 0");
}

static void test_append_and_read(void)
{
	sim_section("records come back in order, intact");

	CHECK(append_n(10, 72) == 10, "ten appends succeed");

	struct walk w = walk_from(0);

	CHECK(w.count == 10, "ten records (got %u)", w.count);
	CHECK(w.first == 1 && w.last == 10, "seq 1..10 (got %u..%u)",
	      w.first, w.last);
	CHECK(w.gaps == 0 && w.bad_crc == 0, "no gaps, no bad CRCs");
	CHECK(w.oldest == 1 && w.newest == 10, "summary says 1..10");

	struct hist_iter it;
	struct hist_record r;

	history_iter_begin(&it, 10);
	CHECK(history_iter_next(&it, &r), "seq 10 readable");
	CHECK(r.hr_bpm == 72 && r.steps == 9 && r.battery_20mv == 190,
	      "fields survive the round trip");
	CHECK(r.hr_min_bpm == 67 && r.hr_max_bpm == 81 &&
	      r.temp_c100 == -125 && r.skin_c100 == 3412 &&
	      r.rest_hr_bpm == 51 && r.rest_rmssd_ms == 47 &&
	      r.rest_beats == 64 && r.temp_rise_c10 == 17 &&
	      r.motion_pct == 3 && r.hot_flashes == 1,
	      "v2 fields survive the round trip");
	CHECK((r.flags & HIST_FLAG_WORN) != 0, "caller's flags kept");
	CHECK((r.flags & (HIST_FLAG_HOT_FLASH | HIST_FLAG_SPOT_NIGHT)) ==
	      (HIST_FLAG_HOT_FLASH | HIST_FLAG_SPOT_NIGHT),
	      "v2 flags kept");
	CHECK((r.flags & HIST_FLAG_TIME_VALID) == 0,
	      "no clock yet: TIME_VALID clear");
	CHECK(r.time_s == (uint32_t)(fake_ms / 1000),
	      "uptime stamp when no clock (%u)", r.time_s);
	CHECK(!history_iter_next(&it, &r), "nothing after the newest");

	w = walk_from(8);
	CHECK(w.count == 3 && w.first == 8, "from_seq 8 gives 8..10");
	w = walk_from(11);
	CHECK(w.count == 0 && w.newest == 10,
	      "from_seq past the end: nothing, summary still right");
}

static void test_clock(void)
{
	sim_section("wall clock: stamping, and surviving a soft reset");

	history_set_time(1790000000U);	/* at the current fake uptime */
	CHECK(append_n(1, 60) == 1, "append with clock set");

	struct hist_iter it;
	struct hist_record r;

	history_iter_begin(&it, 11);
	CHECK(history_iter_next(&it, &r), "seq 11 readable");
	CHECK((r.flags & HIST_FLAG_TIME_VALID) != 0, "TIME_VALID set");
	CHECK(r.time_s == 1790000000U + 300U,
	      "unix stamp advanced with uptime (%u)", r.time_s);

	/* The main loop saves every pass; then a watchdog reset. */
	history_clock_save();

	uint32_t before = 0;

	(void)history_unix_now(&before);
	fake_ms = 0;
	CHECK(history_init() == 0, "init after soft reset");

	uint32_t after = 0;

	CHECK(history_unix_now(&after), "clock valid after soft reset");
	CHECK(after == before, "clock restored to the saved time (%u vs %u)",
	      after, before);
}

static void test_reboot_continues(void)
{
	sim_section("a reboot picks up where the last boot left off");

	/*
	 * boot_id is rediscovered from the newest record, so it advances
	 * past the last boot that WROTE something -- test_clock's soft
	 * reset wrote nothing, so it does not count.
	 */
	struct hist_iter it;
	struct hist_record newest;

	history_iter_begin(&it, 11);
	CHECK(history_iter_next(&it, &newest), "newest record readable");

	reboot();
	CHECK(history_boot_id() == (uint8_t)(newest.boot_id + 1),
	      "boot_id follows the newest record's (%u -> %u)",
	      newest.boot_id, history_boot_id());

	CHECK(append_n(5, 80) == 5, "five more after the reboot");

	struct walk w = walk_from(0);

	CHECK(w.count == 16 && w.last == 16 && w.gaps == 0,
	      "16 records, contiguous (got %u, last %u, gaps %u)", w.count,
	      w.last, w.gaps);
}

static void test_torn_write(void)
{
	sim_section("a torn slot is skipped, never written over");

	/*
	 * Simulate power lost part-way through writing the next slot: the
	 * first word landed, the rest did not. Slot 16 is where seq 17 would
	 * go, mid-page.
	 */
	off_t torn = (off_t)(PAGE + 16U * REC);

	flash[torn] = 0x11;
	flash[torn + 1] = 0x00;

	reboot();
	CHECK(append_n(3, 90) == 3, "appends still succeed");
	CHECK(overwrites == 0, "nothing written over unerased flash (%d)",
	      overwrites);

	struct walk w = walk_from(0);

	CHECK(w.count == 19 && w.gaps == 0 && w.last == 19,
	      "seq stays contiguous across the skipped page (got %u, gaps "
	      "%u)", w.count, w.gaps);
}

static void test_wrap(void)
{
	sim_section("wrapping round: oldest page goes, order holds");

	int n = (int)CAPACITY + 700;

	CHECK(append_n(n, 65) == n, "%d appends all succeed", n);
	CHECK(overwrites == 0 && bad_alignment == 0,
	      "no overwrites (%d), no misaligned writes (%d)", overwrites,
	      bad_alignment);

	struct walk w = walk_from(0);

	CHECK(w.gaps == 0 && w.bad_crc == 0, "contiguous after wrap");
	CHECK(w.last == w.newest, "walk ends at the newest (%u)", w.newest);
	CHECK(w.first == w.oldest, "summary oldest matches the walk");
	CHECK(w.count >= CAPACITY - 2U * PER_PAGE && w.count <= CAPACITY,
	      "kept between capacity-2 pages and capacity (%u of %u)",
	      w.count, CAPACITY);

	/* And it all survives a reboot mid-ring. */
	reboot();
	struct walk w2 = walk_from(0);

	CHECK(w2.count == w.count && w2.last == w.last && w2.gaps == 0,
	      "same records after reboot (%u, last %u)", w2.count, w2.last);
	CHECK(append_n(1, 65) == 1 && walk_from(0).last == w.last + 1,
	      "next seq follows on after reboot");
}

static void test_iter_snapshot(void)
{
	sim_section("a sync only returns what existed when it began");

	struct hist_iter it;
	struct hist_record r;
	uint32_t seen = 0;
	uint32_t last = 0;

	history_iter_begin(&it, 0);
	/* Read a little, then the main loop writes more mid-sync. */
	for (int i = 0; i < 5 && history_iter_next(&it, &r); i++) {
		seen++;
	}
	CHECK(append_n(300, 70) == 300, "300 appends during the sync");
	while (history_iter_next(&it, &r)) {
		CHECK(r.seq <= it.last_seq, "seq %u beyond the snapshot %u",
		      r.seq, it.last_seq);
		last = r.seq;
		seen++;
	}
	CHECK(last == it.last_seq, "walk still reaches the snapshot's newest");
	CHECK(seen > 5, "records kept coming after the writes (%u)", seen);
	CHECK(overwrites == 0, "still no overwrites");
}

static void test_layout_change(void)
{
	sim_section("a header from another layout reformats");

	flash[4] ^= 0x01;	/* the version field */
	erases = 0;
	reboot();
	CHECK(erases == (int)(PART_SIZE / PAGE), "reformatted (%d erases)",
	      erases);
	CHECK(walk_from(0).count == 0, "old records not misread");
	CHECK(append_n(2, 50) == 2 && walk_from(0).last == 2,
	      "fresh log starts at seq 1");
}

static void test_upload_installed(void)
{
	sim_section("an installed update keeps the seq going");

	uint32_t last = walk_from(0).last;

	CHECK(append_n(3, 70) == 3, "three records before the upload");
	last += 3;

	history_suspend();
	CHECK(history_suspended(), "suspended");
	CHECK(floor_saves == 1 && stored_floor == last + 1,
	      "next seq saved as the floor (%u, want %u)", stored_floor,
	      last + 1);
	CHECK(append_n(1, 70) == 0, "appends refused while suspended");

	struct walk w = walk_from(0);

	CHECK(w.count == 0 && w.newest == last,
	      "a sync walks nothing but still reports seq %u (got %u)", last,
	      w.newest);

	/* The upload fills slot 1, MCUboot installs it, the ring reboots. */
	upload_scribbles(150000U);
	power_cycle();
	erases = 0;
	CHECK(history_init() == 0, "init after the update");
	CHECK(erases == (int)(PART_SIZE / PAGE),
	      "slot 1's leftovers formatted (%d erases)", erases);
	CHECK(append_n(2, 70) == 2, "appends work again");

	w = walk_from(0);
	CHECK(w.count == 2 && w.first == last + 1 && w.gaps == 0,
	      "numbering carries on at %u (got %u..%u)", last + 1, w.first,
	      w.last);
	CHECK(overwrites == 0, "nothing written over unerased flash (%d)",
	      overwrites);

	/* And a later ordinary reboot does not drag it back to the floor. */
	reboot();
	CHECK(append_n(1, 70) == 1 && walk_from(0).last == last + 3,
	      "an ordinary reboot continues from the log, not the floor");
}

static void test_upload_abandoned(void)
{
	sim_section("an abandoned upload gives the partition back");

	struct hist_iter it;
	struct hist_record r;
	uint32_t last;

	CHECK(append_n(20, 66) == 20, "twenty records");
	last = walk_from(0).last;

	/* A sync is mid-walk when the upload starts. */
	history_iter_begin(&it, 0);
	CHECK(history_iter_next(&it, &r), "walk started");
	history_suspend();
	CHECK(!history_iter_next(&it, &r), "the walk stops at the suspend");

	upload_scribbles(40000U);	/* a third of the way, then gone */

	erases = 0;
	CHECK(history_resume() == 0, "resume");
	CHECK(!history_suspended(), "no longer suspended");
	CHECK(erases == (int)(PART_SIZE / PAGE), "formatted on resume (%d)",
	      erases);
	CHECK(append_n(2, 66) == 2, "appends work again");

	struct walk w = walk_from(0);

	CHECK(w.count == 2 && w.first == last + 1,
	      "same boot, seq carries on at %u (got %u)", last + 1, w.first);
	CHECK(overwrites == 0, "nothing written over the upload's bytes (%d)",
	      overwrites);
	int before = erases;

	CHECK(history_resume() == 0 && erases == before,
	      "resume when not suspended does nothing");
}

static void test_upload_before_init(void)
{
	sim_section("an upload that starts before history_init()");

	uint32_t last = walk_from(0).last;
	int saves = floor_saves;

	power_cycle();
	history_suspend();	/* BLE is up first; the phone was quick */
	CHECK(floor_saves == saves,
	      "no floor saved: nothing is known yet, the stored one stands");

	erases = 0;
	CHECK(history_init() == 0, "init returns cleanly");
	CHECK(erases == 0, "and does not format under the upload");
	CHECK(append_n(1, 60) == 0, "appends refused");

	upload_scribbles(PART_SIZE);
	CHECK(history_resume() == 0, "resume formats");
	CHECK(append_n(1, 60) == 1, "append after resume");

	uint32_t first = walk_from(0).first;

	CHECK(first == stored_floor && first > 1U && first <= last + 1U,
	      "seq resumes from the stored floor, not 1 (got %u, floor %u)",
	      first, stored_floor);
	CHECK(overwrites == 0, "no overwrites (%d)", overwrites);
}

int main(int argc, char **argv)
{
	verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

	test_format_over_garbage();
	test_append_and_read();
	test_clock();
	test_reboot_continues();
	test_torn_write();
	test_wrap();
	test_iter_snapshot();
	test_layout_change();
	test_upload_installed();
	test_upload_abandoned();
	test_upload_before_init();

	return sim_report("history");
}
