/*
 * Spot readings: a few minutes of heart rate and HRV taken on a schedule,
 * at moments when the reading is worth having.
 *
 * The ordinary measurement windows are 25 s long and open on a timer,
 * moving or not. That is enough for a heart rate, but not for an HRV
 * number anyone should build on: the beat detector spends ~10 s of each
 * window priming, and RMSSD wants a minute or more of clean, consecutive
 * beats from a still hand. hrv.c stitches its window together across
 * several of them, so the figure in a history record is an average over
 * whatever the last few windows happened to catch.
 *
 * Spot readings are the opposite trade: rare, long, and timed for when
 * the reading means something. They feed the two numbers the app builds
 * stress and burnout scores on:
 *
 *   NIGHT  resting heart rate and RMSSD while asleep. Night-to-night, over
 *          weeks, these are the most robust strain/recovery signal a
 *          wearable has: resting HR creeping up and nightly HRV sliding
 *          down against the wearer's own baseline. First reading 15 min
 *          after sleep onset, then every night_min.
 *   DAY    the same, awake, worn, and still for 3 minutes, every day_min.
 *          Daytime HRV against the night baseline is the stress reading.
 *          "Still" is the accelerometer's word, not the motion timeout's:
 *          main.c refreshes that one whenever a window finds a finger.
 *   HOT_FLASH  one reading straight away when thermo.c sees a hot flash
 *          start, for the heart-rate rise that goes with one.
 *   MANUAL one reading now, because the app asked (opcode 0x14: the
 *          "check my stress" button). Judged like a day reading, and
 *          leaves the schedule alone.
 *
 * A reading runs window_s seconds, green LED only, with an RMSSD of its
 * own (hrv_session in hrv.h) over every beat it saw. It is valid with at least 30
 * one-second heart rates and the accelerometer quiet for 90 % of it; a
 * night or day reading with sustained motion is cut short. An invalid
 * reading is retried after 10 minutes instead of waiting a full interval.
 *
 * Pure logic: main.c opens and closes the window and feeds what it reads.
 */

#ifndef SPOT_H_
#define SPOT_H_

#include <stdbool.h>
#include <stdint.h>

enum spot_kind {
	SPOT_NONE = 0,
	SPOT_NIGHT = 1,
	SPOT_DAY = 2,
	SPOT_HOT_FLASH = 3,
	SPOT_MANUAL = 4,
};

/* On the wire: opcode 0x12. */
struct spot_config {
	uint16_t night_min;	/* between night readings; 0 = off */
	uint16_t day_min;	/* between daytime readings; 0 = off */
	uint16_t window_s;	/* length of every reading */
};

#define SPOT_INTERVAL_MIN_MIN	10
#define SPOT_INTERVAL_MAX_MIN	720
#define SPOT_WINDOW_MIN_S	60
#define SPOT_WINDOW_MAX_S	600

struct spot_inputs {
	int64_t now_ms;
	bool asleep;		/* sleep.c */
	bool worn;		/* main.c's last wear verdict */
	int64_t still_ms;	/* since the accelerometer last saw motion */
	bool blocked;		/* charging, workout, ship mode */
};

struct spot_result {
	uint8_t kind;		/* enum spot_kind */
	bool valid;
	uint8_t hr_bpm;		/* mean of the one-second readings; 0 = none */
	uint8_t rmssd_ms;	/* 0 = too few clean beats */
	uint8_t beats;		/* successive differences behind rmssd_ms */
	uint8_t motion_pct;
};

void spot_init(void);

/* Within the SPOT_* limits above. Zero turns a kind off. */
bool spot_config_valid(const struct spot_config *cfg);

/* Out-of-range values are refused, -EINVAL. */
int spot_set_config(const struct spot_config *cfg);
void spot_get_config(struct spot_config *cfg);

/* thermo.c saw a hot flash start. */
void spot_request_hot_flash(void);

/* The app asked for a reading now. */
void spot_request_now(void);

/*
 * Forget any manual or hot-flash request not yet started. For the
 * charger: spot_due() only runs while the ring is idle, so without this a
 * request made on the charger would wait there and start -- unasked, maybe
 * hours later -- the moment the ring came off it.
 */
void spot_drop_pending(void);

/* What, if anything, should start now. Main loop, idle state only. */
enum spot_kind spot_due(const struct spot_inputs *in);

/* The window for `kind` opened. */
void spot_begin(enum spot_kind kind, int64_t now_ms);

/* The window for `kind` could not open (PPG down): retry later. */
void spot_not_started(enum spot_kind kind, int64_t now_ms);

/* The reading running now, or SPOT_NONE. */
enum spot_kind spot_active(void);

uint32_t spot_window_ms(void);

/* One heart rate a second, 0.1 bpm, while a reading runs. */
void spot_feed_hr(uint16_t bpm_x10);

/* Every accelerometer read while a reading runs. */
void spot_feed_motion(bool moving);

/* Enough motion that the reading is already lost. */
bool spot_should_stop(void);

/*
 * The window closed. `usable` is false when something other than the
 * reading took it over (a workout). Fills *out and schedules the next.
 */
void spot_end(int64_t now_ms, uint16_t rmssd_x10, uint8_t diffs, bool usable,
	      struct spot_result *out);

#endif /* SPOT_H_ */
