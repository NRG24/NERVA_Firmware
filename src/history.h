/*
 * On-ring history: a summary record every few minutes, kept in flash, so
 * nothing is lost while no phone is connected.
 *
 * Everything else this firmware reports lives in RAM and is only ever seen
 * by a phone that happens to be connected at the time. This is the one
 * place data outlives the connection -- and a reboot.
 *
 * The log is a ring of fixed 32-byte records in its own flash partition
 * (history_partition, 176 kB), separate from the settings partition that
 * holds the bonds, so nothing here can damage a bond. When the ring is
 * full the oldest 4 kB page is erased and reused: about 19 days at the
 * default 5-minute epoch.
 *
 * Record layout v2 (v0.8.0): 32 bytes, adding temperature, heart-rate
 * range, resting heart rate and HRV from spot readings, motion and hot
 * flashes to v1's 16. A ring that boots v2 over a v1 log reformats it --
 * sync before upgrading.
 *
 * Records carry a sequence number that never repeats, even across reboots,
 * so a phone syncs with "everything after the last seq I have" and never
 * needs to know where anything sits in flash. See APP_INTEGRATION.md,
 * section 16, for the sync protocol.
 *
 * TIME. There is no RTC on this board. The phone sends Unix time on every
 * connection (control opcode 0x0D); from then until the next reboot records
 * are stamped in Unix seconds and carry HIST_FLAG_TIME_VALID. Before that,
 * and after a reboot nobody has re-synced, they carry seconds since boot
 * instead, plus a boot_id so the phone can tell which boot they belong to.
 */

#ifndef HISTORY_H_
#define HISTORY_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/* Bits in hist_record.flags. Every one but TIME_VALID is "at any point in
 * the epoch", not a snapshot of the moment it was written.
 */
#define HIST_FLAG_TIME_VALID	BIT(0)	/* time_s is Unix seconds, not uptime */
#define HIST_FLAG_WORN		BIT(1)	/* a window saw a finger */
#define HIST_FLAG_CHARGING	BIT(2)	/* on the charger */
#define HIST_FLAG_ASLEEP	BIT(3)	/* sleep.c reported asleep */
#define HIST_FLAG_WORKOUT	BIT(4)	/* a workout was running */
#define HIST_FLAG_HOT_FLASH	BIT(5)	/* a hot flash was in progress */
#define HIST_FLAG_SPOT_NIGHT	BIT(6)	/* rest_* came from a night spot reading */
#define HIST_FLAG_SPOT_DAY	BIT(7)	/* rest_* came from a daytime still spot */

/* "No reading" for the two temperature fields. */
#define HIST_TEMP_NONE		INT16_MIN

/*
 * One epoch, exactly as stored in flash and as sent over the air.
 * Little-endian, 32 bytes, every field on its natural alignment.
 */
struct hist_record {
	uint32_t seq;		/* never 0, never repeats; 0xffffffff = empty */
	uint32_t time_s;	/* end of the epoch: Unix s, or uptime s */
	uint16_t steps;		/* steps taken during the epoch */
	int16_t temp_c100;	/* ring (nRF die) temperature, mean, 0.01 C */
	int16_t skin_c100;	/* PPG sensor face temperature, mean, 0.01 C */
	uint8_t hr_bpm;		/* mean over the epoch's readings; 0 = none */
	uint8_t hr_min_bpm;	/* lowest 1 s reading; 0 = none */
	uint8_t hr_max_bpm;	/* highest 1 s reading; 0 = none */
	uint8_t rmssd_ms;	/* rolling RMSSD at the epoch's end; 0 = none */
	uint8_t rest_hr_bpm;	/* mean HR of a completed spot reading; 0 = none */
	uint8_t rest_rmssd_ms;	/* RMSSD of that spot reading; 0 = none */
	uint8_t rest_beats;	/* successive differences behind rest_rmssd */
	uint8_t temp_rise_c10;	/* peak rise over the settled baseline, 0.1 C */
	uint8_t motion_pct;	/* share of accelerometer reads with motion */
	uint8_t battery_20mv;	/* battery mV / 20; 0 = not measured */
	uint8_t flags;		/* HIST_FLAG_* */
	uint8_t hot_flashes;	/* hot flashes that started in this epoch */
	uint8_t boot_id;	/* +1 per boot, wraps at 256 */
	uint8_t reserved[4];	/* 0 */
	uint8_t crc;		/* CRC-8 (poly 0x07, init 0) of bytes 0-30 */
} __packed;

/*
 * Mount the partition, formatting it if it does not hold a log in this
 * layout, and find where the last boot left off. Call once at boot, before
 * the watchdog starts: a first-time format erases every page, ~4 s.
 *
 * Also restores the wall clock if a soft reset left it in retained RAM.
 */
int history_init(void);

/*
 * Stamp and store one epoch. Fills seq, time_s, boot_id, crc and the
 * TIME_VALID flag; the caller fills the measurements and the other flags.
 * Main loop only. Can erase a 4 kB page (~85 ms) when it crosses into one.
 */
int history_append(struct hist_record *rec);

/* Wall clock, from control opcode 0x0D. Any thread. */
void history_set_time(uint32_t unix_s);

/* Current Unix time, if the phone has set it since boot. Any thread. */
bool history_unix_now(uint32_t *unix_s);

/*
 * Keep the wall clock in RAM that survives a soft reset (watchdog, fatal
 * error), so a reboot of that kind does not cost the timestamps. Cheap;
 * call it every pass of the main loop. A power loss still clears it.
 */
void history_clock_save(void);

/* boot_id this boot's records carry. */
uint8_t history_boot_id(void);

/*
 * The partition doubles as MCUboot's slot 1, so an over-the-air update
 * overwrites the log (OTA.md). src/ota.c calls this as an upload starts:
 * it saves the next seq to settings, so numbering carries on after the
 * update, then stops every read and write of the partition. Appends
 * return -EBUSY and a sync walks nothing until history_resume() or a
 * reboot. Any thread; blocks behind an append or a format in progress.
 */
void history_suspend(void);

bool history_suspended(void);

/*
 * Give the partition back to the log after an upload that was abandoned:
 * format it (~4 s, feeding the watchdog) and carry on with the same seq.
 * No-op unless suspended. Main loop only.
 */
int history_resume(void);

/*
 * Reading the log back for a sync. One reader at a time -- the BLE sync
 * work item is the only caller.
 */
struct hist_iter {
	uint32_t slot;		/* next slot to read */
	uint32_t remaining;	/* slots left to walk */
	uint32_t from_seq;	/* skip anything older */
	uint32_t last_seq;	/* newest record that existed at begin */
	uint32_t oldest_seq;	/* first valid record walked; 0 = none yet */
};

/* Start a walk over every record with seq >= from_seq, oldest first. */
void history_iter_begin(struct hist_iter *it, uint32_t from_seq);

/* Next record, or false at the end. */
bool history_iter_next(struct hist_iter *it, struct hist_record *out);

/* CRC over bytes 0-30, exposed for the host tests. */
uint8_t history_crc8(const uint8_t *data, uint32_t len);

#endif /* HISTORY_H_ */
