#include "history.h"

#include "wdt.h"

#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#if defined(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <string.h>

LOG_MODULE_REGISTER(history, LOG_LEVEL_INF);

BUILD_ASSERT(sizeof(struct hist_record) == 32,
	     "hist_record is a wire and flash format: 32 bytes exactly");

/*
 * Layout of history_partition:
 *
 *   page 0        header: magic, layout version, record and page size
 *   pages 1..N    records, 128 to a page, written in order and wrapping
 *
 * nRF52 flash erases in 4 kB pages and writes 32-bit words, and an erased
 * word reads 0xffffffff, so a slot whose seq is 0xffffffff has never been
 * written. Records are only ever written into erased slots and never
 * rewritten, which is the only kind of write flash can do safely.
 *
 * Where to write next is not stored anywhere -- it is rediscovered at boot
 * as the slot after the highest valid seq. A pointer kept in flash would
 * need its own wear levelling and its own torn-write handling; the records
 * already carry everything needed.
 *
 * A torn write (power lost mid-write, or mid-erase) leaves a slot that is
 * neither erased nor valid. The CRC rejects it on read, and the writer
 * moves on to the next page rather than write around it, so a damaged page
 * costs the rest of that page and nothing else.
 */
#define HIST_PAGE_SIZE		4096U
#define HIST_REC_SIZE		((uint32_t)sizeof(struct hist_record))
#define SLOTS_PER_PAGE		(HIST_PAGE_SIZE / HIST_REC_SIZE)

/* Bytes the CRC covers: everything but the CRC itself. */
#define HIST_CRC_LEN		(HIST_REC_SIZE - 1U)

/* "RHST". Bump HIST_VERSION whenever hist_record changes shape: the next
 * boot sees a header it does not recognise and reformats rather than
 * misreading old records.
 */
#define HIST_MAGIC		0x54534852U
#define HIST_VERSION		2U

struct hist_header {
	uint32_t magic;
	uint16_t version;
	uint16_t record_size;
	uint32_t page_size;
	uint32_t magic_inv;
} __packed;

/* Records read per flash access while scanning or syncing. */
#define CACHE_RECORDS		16U

BUILD_ASSERT(SLOTS_PER_PAGE % CACHE_RECORDS == 0,
	     "a cache chunk must never straddle the end of the ring");

static const struct flash_area *fa;

/*
 * Guards the flash and everything below. The writer is the main loop and
 * the reader is the BLE sync work item, so the two can overlap; nothing
 * else touches this partition.
 */
static K_MUTEX_DEFINE(hist_lock);

static bool ready;
static uint32_t nslots;		/* record slots in the partition */
static uint32_t head;		/* next slot to write */
static uint32_t next_seq;	/* seq the next record gets */
static uint8_t boot_id;

/*
 * The partition is also MCUboot's slot 1 (see the board DTS), and while
 * this is set an upload owns it: nothing here reads or writes it until
 * history_resume() formats it back.
 */
static bool suspended;

/*
 * Lowest seq the next record may take, from settings. A format starts the
 * log empty, and without this its seq would start again at 1 -- and an app
 * syncing "everything after seq N" would skip every new record until the
 * count passed N again. history_suspend() stores next_seq here before an
 * upload wipes the log, so the numbering carries on across the update.
 */
static uint32_t seq_floor;

/* Read cache for the scan at boot and for the one sync reader. */
static struct hist_record cache[CACHE_RECORDS];
static uint32_t cache_base = UINT32_MAX;

/* --- wall clock -------------------------------------------------------- */

static struct k_spinlock clock_lock;
static bool clock_valid;
static int64_t clock_offset_s;	/* Unix seconds minus uptime seconds */

/*
 * The clock in RAM that Zephyr does not zero at boot, so a watchdog or
 * fatal-error reset -- which on this board are the resets that happen --
 * does not throw the time away. A power-on reset leaves garbage here, and
 * the check word rejects it. Off by at most the time between the last
 * save and the reset: one loop pass normally, the 10 s watchdog budget if
 * the loop hung.
 */
#define RETAINED_MAGIC		0x434c4b31U	/* "CLK1" */

struct retained_clock {
	uint32_t magic;
	uint32_t unix_s;
	uint32_t check;
};

static struct retained_clock retained __noinit;

static uint32_t uptime_s(void)
{
	return (uint32_t)(k_uptime_get() / 1000);
}

void history_set_time(uint32_t unix_s)
{
	k_spinlock_key_t key = k_spin_lock(&clock_lock);

	clock_offset_s = (int64_t)unix_s - (int64_t)uptime_s();
	clock_valid = true;
	k_spin_unlock(&clock_lock, key);
}

bool history_unix_now(uint32_t *unix_s)
{
	k_spinlock_key_t key = k_spin_lock(&clock_lock);
	bool valid = clock_valid;

	if (valid) {
		*unix_s = (uint32_t)((int64_t)uptime_s() + clock_offset_s);
	}
	k_spin_unlock(&clock_lock, key);

	return valid;
}

void history_clock_save(void)
{
	uint32_t now;

	if (!history_unix_now(&now)) {
		return;
	}

	retained.magic = RETAINED_MAGIC;
	retained.unix_s = now;
	retained.check = now ^ ~RETAINED_MAGIC;
}

static void clock_restore(void)
{
	if (retained.magic != RETAINED_MAGIC ||
	    retained.check != (retained.unix_s ^ ~RETAINED_MAGIC)) {
		return;
	}

	/*
	 * retained.unix_s was the time at the last save before the reset.
	 * Uptime restarted from zero at the reset, so that time is the new
	 * offset, give or take how long the reset took.
	 */
	k_spinlock_key_t key = k_spin_lock(&clock_lock);

	clock_offset_s = retained.unix_s;
	clock_valid = true;
	k_spin_unlock(&clock_lock, key);

	LOG_INF("clock restored across the reset: unix %u",
		retained.unix_s);
}

/* --- seq floor, in settings ------------------------------------------- */

#define FLOOR_KEY		"hist/seq"

#if defined(CONFIG_SETTINGS)

/*
 * Called from settings_load(), which ble_start() runs before
 * history_init(). Anything unexpected is ignored rather than failed, for
 * the reason profile.c gives: an error here would cost the radio.
 */
static int hist_set(const char *name, size_t len, settings_read_cb read_cb,
		    void *cb_arg)
{
	const char *next;
	uint32_t v;

	if (!settings_name_steq(name, "seq", &next) || next ||
	    len != sizeof(v)) {
		return 0;
	}
	if (read_cb(cb_arg, &v, sizeof(v)) == (ssize_t)sizeof(v) &&
	    v > seq_floor) {
		seq_floor = v;
	}

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ring_history, "hist", NULL, hist_set, NULL,
			       NULL);

static int save_floor(uint32_t seq)
{
	return settings_save_one(FLOOR_KEY, &seq, sizeof(seq));
}

#else

static int save_floor(uint32_t seq)
{
	(void)seq;
	return -ENOTSUP;
}

#endif /* CONFIG_SETTINGS */

/* --- records ----------------------------------------------------------- */

uint8_t history_crc8(const uint8_t *data, uint32_t len)
{
	uint8_t crc = 0;

	for (uint32_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x07U)
					    : (uint8_t)(crc << 1);
		}
	}

	return crc;
}

static bool rec_valid(const struct hist_record *r)
{
	return r->seq != 0U && r->seq != UINT32_MAX &&
	       r->crc == history_crc8((const uint8_t *)r, HIST_CRC_LEN);
}

static bool rec_erased(const struct hist_record *r)
{
	const uint8_t *p = (const uint8_t *)r;

	for (uint32_t i = 0; i < HIST_REC_SIZE; i++) {
		if (p[i] != 0xffU) {
			return false;
		}
	}

	return true;
}

static off_t slot_off(uint32_t slot)
{
	return (off_t)(HIST_PAGE_SIZE + slot * HIST_REC_SIZE);
}

static uint32_t next_page(uint32_t slot)
{
	return ((slot / SLOTS_PER_PAGE + 1U) * SLOTS_PER_PAGE) % nslots;
}

/* Caller holds hist_lock. */
static int read_slot(uint32_t slot, struct hist_record *out)
{
	return flash_area_read(fa, slot_off(slot), out, sizeof(*out));
}

/* Copy one slot out of the cache, filling it first if needed. */
static bool cache_get(uint32_t slot, struct hist_record *out)
{
	uint32_t base = slot - (slot % CACHE_RECORDS);

	if (base != cache_base) {
		k_mutex_lock(&hist_lock, K_FOREVER);
		int err = suspended ? -EBUSY
				    : flash_area_read(fa, slot_off(base), cache,
						      sizeof(cache));
		k_mutex_unlock(&hist_lock);

		if (err) {
			cache_base = UINT32_MAX;
			return false;
		}
		cache_base = base;
	}

	*out = cache[slot - base];
	return true;
}

static bool header_ok(const struct hist_header *h)
{
	return h->magic == HIST_MAGIC && h->magic_inv == ~HIST_MAGIC &&
	       h->version == HIST_VERSION &&
	       h->record_size == HIST_REC_SIZE &&
	       h->page_size == HIST_PAGE_SIZE;
}

/*
 * Erase the whole partition and write a fresh header. Runs on the first
 * boot of this layout -- the partition used to be code space, so it holds
 * whatever an older, larger image left there -- after a layout change, and
 * after an upload has used the partition as slot 1. Caller holds hist_lock.
 */
static int format(void)
{
	struct hist_header h = {
		.magic = HIST_MAGIC,
		.version = HIST_VERSION,
		.record_size = HIST_REC_SIZE,
		.page_size = HIST_PAGE_SIZE,
		.magic_inv = ~HIST_MAGIC,
	};
	uint32_t words[sizeof(h) / sizeof(uint32_t)];
	int err;

	LOG_WRN("history: formatting %u kB", (unsigned int)(fa->fa_size / 1024U));

	for (off_t off = 0; off < (off_t)fa->fa_size; off += HIST_PAGE_SIZE) {
		err = flash_area_erase(fa, off, HIST_PAGE_SIZE);
		/* ~85 ms a page; 44 of them is most of the watchdog budget. */
		ring_wdt_feed();
		if (err) {
			LOG_ERR("history: erase at 0x%lx failed (%d)",
				(unsigned long)off, err);
			return err;
		}
	}

	/* An aligned copy: the flash driver writes whole words. */
	memcpy(words, &h, sizeof(h));
	err = flash_area_write(fa, 0, words, sizeof(words));
	if (err) {
		LOG_ERR("history: header write failed (%d)", err);
	}

	return err;
}

int history_init(void)
{
	struct hist_header h;
	uint32_t best_seq = 0;
	uint32_t best_slot = 0;
	uint32_t count = 0;
	uint8_t last_boot = 0;
	int err;

	clock_restore();

	err = flash_area_open(PARTITION_ID(history_partition), &fa);
	if (err) {
		LOG_ERR("history: partition open failed (%d)", err);
		return err;
	}

	if (fa->fa_size < 2U * HIST_PAGE_SIZE ||
	    (fa->fa_size % HIST_PAGE_SIZE) != 0U) {
		LOG_ERR("history: partition is %u bytes, need whole 4 kB "
			"pages and at least two", (unsigned int)fa->fa_size);
		return -EINVAL;
	}

	nslots = (uint32_t)((fa->fa_size - HIST_PAGE_SIZE) / HIST_REC_SIZE);

	/*
	 * Held throughout, scan included (k_mutex nests, and cache_get()
	 * takes it again): BLE is already up, and an upload that starts now
	 * must wait for history_suspend() rather than land mid-format.
	 */
	k_mutex_lock(&hist_lock, K_FOREVER);

	if (suspended) {
		/* An upload got here first. history_resume() formats. */
		k_mutex_unlock(&hist_lock);
		LOG_WRN("history: slot 1 taken by an upload before init");
		return 0;
	}

	err = flash_area_read(fa, 0, &h, sizeof(h));
	if (err) {
		k_mutex_unlock(&hist_lock);
		LOG_ERR("history: header read failed (%d)", err);
		return err;
	}

	if (!header_ok(&h)) {
		/* First boot of this layout, or the first after an update. */
		LOG_WRN("history: no log in this layout");
		err = format();
		if (err) {
			k_mutex_unlock(&hist_lock);
			return err;
		}
	}

	/*
	 * The newest record is the one with the highest seq, wherever it
	 * sits; the next write goes in the slot after it. ~5.5k slots read 16
	 * at a time from memory-mapped flash: a few milliseconds.
	 */
	for (uint32_t slot = 0; slot < nslots; slot++) {
		struct hist_record r;

		if (!cache_get(slot, &r) || !rec_valid(&r)) {
			continue;
		}

		count++;
		if (r.seq > best_seq) {
			best_seq = r.seq;
			best_slot = slot;
			last_boot = r.boot_id;
		}
	}

	cache_base = UINT32_MAX;

	if (best_seq != 0U) {
		head = (best_slot + 1U) % nslots;
		next_seq = best_seq + 1U;
		boot_id = (uint8_t)(last_boot + 1U);
	} else {
		head = 0;
		next_seq = 1;
		boot_id = 0;
	}
	if (next_seq < seq_floor) {
		next_seq = seq_floor;
	}

	ready = true;

	k_mutex_unlock(&hist_lock);

	LOG_INF("history: %u records, next seq %u at slot %u of %u, boot %u",
		count, next_seq, head, nslots, boot_id);

	return 0;
}

void history_suspend(void)
{
	k_mutex_lock(&hist_lock, K_FOREVER);

	if (!suspended) {
		suspended = true;
		cache_base = UINT32_MAX;

		/*
		 * Before init there is no next_seq yet, and the log cannot
		 * be scanned for one: the upload writes as soon as this
		 * returns. The stored floor then stands, and any records
		 * written since it was saved would be numbered again. Not
		 * reachable in practice -- history_init() runs microseconds
		 * after ble_start(), long before a phone can connect, pair
		 * and send a chunk -- so this only has to be safe, and it
		 * is: nothing is written into the upload.
		 */
		if (ready && next_seq > seq_floor) {
			int err;

			seq_floor = next_seq;
			err = save_floor(seq_floor);
			if (err) {
				LOG_ERR("history: seq floor %u not saved (%d) -- "
					"seq restarts at 1 after the update",
					seq_floor, err);
			}
		}
		ready = false;

		LOG_WRN("history: suspended, slot 1 belongs to an upload");
	}

	k_mutex_unlock(&hist_lock);
}

bool history_suspended(void)
{
	k_mutex_lock(&hist_lock, K_FOREVER);
	bool s = suspended;
	k_mutex_unlock(&hist_lock);

	return s;
}

int history_resume(void)
{
	int err = 0;

	k_mutex_lock(&hist_lock, K_FOREVER);

	if (suspended) {
		/*
		 * Whatever the upload left is not a log. Records carry on
		 * from where they stopped; boot_id is unchanged, since the
		 * ring did not reboot.
		 */
		err = (fa == NULL) ? -ENODEV : format();
		if (!err) {
			head = 0;
			if (next_seq < seq_floor) {
				next_seq = seq_floor;
			}
			if (next_seq == 0U) {
				next_seq = 1U;
			}
			ready = true;
			LOG_INF("history: resumed, next seq %u", next_seq);
		} else {
			LOG_ERR("history: format after upload failed (%d)",
				err);
		}
		suspended = false;
		cache_base = UINT32_MAX;
	}

	k_mutex_unlock(&hist_lock);

	return err;
}

int history_append(struct hist_record *rec)
{
	uint32_t unix_s;
	uint32_t words[sizeof(*rec) / sizeof(uint32_t)];
	struct hist_record check;
	int err = -EIO;

	if (!ready) {
		/* Unlocked peek; rechecked under the lock below. */
		return suspended ? -EBUSY : -ENODEV;
	}

	if (history_unix_now(&unix_s)) {
		rec->time_s = unix_s;
		rec->flags |= HIST_FLAG_TIME_VALID;
	} else {
		rec->time_s = uptime_s();
		rec->flags &= (uint8_t)~HIST_FLAG_TIME_VALID;
	}
	rec->boot_id = boot_id;

	k_mutex_lock(&hist_lock, K_FOREVER);

	if (!ready) {
		/* Suspended since the peek above. */
		k_mutex_unlock(&hist_lock);
		return -EBUSY;
	}

	/*
	 * At most one try per page. A page that will not erase, or a slot
	 * that is not blank -- a torn write, or bytes from before the format
	 * -- skips to the next page; it is never written around.
	 */
	for (uint32_t tries = 0; tries <= nslots / SLOTS_PER_PAGE; tries++) {
		uint32_t slot = head;

		if (slot % SLOTS_PER_PAGE == 0U) {
			/* Into a new page: it holds the oldest records. */
			err = flash_area_erase(fa, slot_off(slot),
					       HIST_PAGE_SIZE);
			if (err) {
				LOG_ERR("history: erase of slot %u failed (%d)",
					slot, err);
				head = next_page(slot);
				continue;
			}
		} else {
			err = read_slot(slot, &check);
			if (err || !rec_erased(&check)) {
				LOG_WRN("history: slot %u not blank, skipping "
					"to the next page", slot);
				head = next_page(slot);
				err = -EIO;
				continue;
			}
		}

		rec->seq = next_seq++;
		rec->crc = history_crc8((const uint8_t *)rec, HIST_CRC_LEN);
		head = (slot + 1U) % nslots;

		/* An aligned copy: the flash driver writes whole words. */
		memcpy(words, rec, sizeof(words));
		err = flash_area_write(fa, slot_off(slot), words,
				       sizeof(words));
		if (!err) {
			err = read_slot(slot, &check);
		}
		if (!err && memcmp(&check, rec, sizeof(check)) != 0) {
			err = -EIO;
		}
		if (err) {
			/* Not retried here: a write that fails is likely to
			 * fail again, and every retry can cost an erase.
			 */
			LOG_ERR("history: write of seq %u failed (%d)",
				rec->seq, err);
		}
		break;
	}

	k_mutex_unlock(&hist_lock);

	return err;
}

uint8_t history_boot_id(void)
{
	return boot_id;
}

/* --- reading back ------------------------------------------------------ */

void history_iter_begin(struct hist_iter *it, uint32_t from_seq)
{
	memset(it, 0, sizeof(*it));
	it->from_seq = from_seq;

	k_mutex_lock(&hist_lock, K_FOREVER);

	if (ready) {
		/*
		 * Oldest data starts at the page after the one being written
		 * -- or at head itself when head sits on a page boundary,
		 * because that page is not erased until the next write
		 * lands in it. Walk from there round to head.
		 */
		uint32_t tail = (head % SLOTS_PER_PAGE == 0U) ? head
							      : next_page(head);

		it->slot = tail;
		it->remaining = (head + nslots - tail) % nslots;
		if (it->remaining == 0U) {
			it->remaining = nslots;
		}
		it->last_seq = next_seq - 1U;
	} else if (suspended && next_seq > 0U) {
		/* Nothing to walk, but the summary still says how far the
		 * numbering has got.
		 */
		it->last_seq = next_seq - 1U;
	}

	/* Records may have been written since this cache was filled. */
	cache_base = UINT32_MAX;

	k_mutex_unlock(&hist_lock);
}

bool history_iter_next(struct hist_iter *it, struct hist_record *out)
{
	while (it->remaining > 0U) {
		if (suspended) {
			/* An upload took the partition mid-walk. */
			it->remaining = 0U;
			break;
		}

		uint32_t slot = it->slot;

		it->slot = (slot + 1U) % nslots;
		it->remaining--;

		if (!cache_get(slot, out) || !rec_valid(out)) {
			continue;
		}

		/*
		 * Anything above last_seq was written after the walk began,
		 * into the page it was about to reach. The next sync gets it.
		 */
		if (out->seq > it->last_seq) {
			continue;
		}
		if (it->oldest_seq == 0U) {
			it->oldest_seq = out->seq;
		}
		if (out->seq < it->from_seq) {
			continue;
		}

		return true;
	}

	return false;
}

#if defined(HISTORY_TESTING)
/* Host tests only: what a power cycle does to this module's RAM. */
void history_test_power_cycle(void)
{
	ready = false;
	suspended = false;
	seq_floor = 0;
	next_seq = 0;
	head = 0;
	cache_base = UINT32_MAX;
}
#endif
