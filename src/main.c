/*
 * Ring firmware: optical heart rate with motion-gated duty cycling.
 *
 * Board wiring taken from Netlist_New_Switch_2_2026-08-21.asc:
 *   U3  MAXM86161   I2C 0x62, VLED from the 5V boost, LDO_EN strapped to 1V8
 *   U4  LSM6DSV16BX I2C 0x6b, INT1 -> P0.16
 *   U6  BQ25120A    I2C 0x6a, CD -> P1.09
 *   U1  TPS61240    5V boost, BOOST_EN on P0.12
 *   U5  ANNA-B402   SCL P0.14, SDA P0.20
 *
 * Power model
 * -----------
 * The optical front end dominates draw: green LED1 at ~15 mA plus the 5V
 * boost. Running it continuously is pointless when the ring is on a desk
 * and wasteful even when worn. So:
 *
 *   IDLE       PPG off, boost off, IMU wake-on-motion armed. Sleeps here
 *              until the ring is picked up.
 *   MEASURING  PPG on for measure_window_ms, then back to IDLE until the
 *              next window. Duty cycle is window/period.
 *   CHARGING   Everything optical off. A charger delivering 20 mA should
 *              not be fighting the LEDs.
 *
 * The phone can override the duty cycle or force a window at any time via
 * the Ring Service control characteristic. See APP_INTEGRATION.md.
 */

#include "ble.h"
#include "calories.h"
#include "hr.h"
#include "hrv.h"
#include "imu.h"
#include "maxm86161.h"
#include "selftest.h"
#include "sleep.h"
#include "steps.h"
#include "wdt.h"

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <stdlib.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define VLED_BOOST	DEVICE_DT_GET(DT_NODELABEL(vled_boost))
#define I2C_BUS		DEVICE_DT_GET(DT_NODELABEL(i2c0))

/* Which LED drives the exposure. LED1 green 530nm gives the best pulse SNR. */
#define PPG_LED		LEDC_LED1
/* 0.12 mA per LSB in the 31 mA range: 0x80 is about 15.4 mA. */
#define PPG_LED_PA	0x80

/*
 * BENCH ONLY -- remove before anything ships.
 *
 * Holds the green LED (LED1) on solid as a power indicator, so "is the board
 * actually running?" is answerable by looking at it instead of by trying to
 * attach a debugger. Several hours went into faults that turned out to be an
 * unpowered board presenting as a dead SWD link.
 *
 * It keeps the 5 V boost enabled permanently, so it costs real current and
 * defeats the whole idle power model.
 *
 * Controlled from Kconfig (CONFIG_RING_BENCH_POWER_LED, under RING_BENCH).
 * It used to be a hand-edited 1/0 on this line, with a whole duplicate
 * main.c.bench kept alongside for the other setting -- which is exactly how
 * a bench instrument survives into a shipped image.
 */
#if defined(CONFIG_RING_BENCH_POWER_LED)
#define BENCH_POWER_LED		1
#else
#define BENCH_POWER_LED		0
#endif
/* ~5.8 mA in the 31 mA range: clearly visible, not hot. */
#define BENCH_LED_PA		0x30

/* Matches PPG_SR_100HZ programmed in the driver. */
#define SAMPLE_RATE_HZ		100
#define POLL_INTERVAL_MS	20

/* --- duty cycle defaults, overridable from the app --------------------- */

/*
 * How long the PPG runs each time it wakes. Has to cover DC settling plus
 * enough beats for a stable median: roughly 4 s to settle, then 8 beats,
 * which at 50 bpm is another 10 s.
 */
#define MEASURE_WINDOW_MS	15000
/* How often a measurement window starts while the ring is being worn. */
#define MEASURE_PERIOD_MS	60000
/* Abandon a window early if no finger shows up. */
#define NO_FINGER_TIMEOUT_MS	6000
/* Charger state is cheap to poll and needs to be responsive. */
#define PMIC_POLL_MS		2000
/* Battery and GSR change slowly and cost I2C traffic. */
#define SLOW_SENSE_MS		30000
/* Deviation from 1 g that counts as movement. */
#define STILL_MG		120
/*
 * No movement for this long means it is off a finger: stop probing on a
 * timer and wait for the IMU interrupt instead.
 */
#define STILL_TIMEOUT_MS	180000

/* --- pedometer sampling ------------------------------------------------ */

/*
 * How fast the IMU is polled while the ring might be walking.
 *
 * Not a tuning preference. Walking is 1.5-2.5 Hz and a footfall is a
 * narrow peak, so the idle 200 ms poll sits barely above Nyquist and the
 * detector loses steps outright rather than merely adding noise -- in
 * simulation it counted essentially nothing at 200 ms below a 250 mg
 * magnitude swing, against 0-1 % error at 40 ms. See the table in steps.h.
 *
 * The cost is bounded: an extra ~20 accelerometer reads a second, each one
 * a 6-byte I2C burst, and only while the wearer is actually moving. Set
 * against the optical front end's ~15 mA for 15 s in every 60 while worn,
 * it is noise. While the ring is still -- which is the whole of the night,
 * and the case the idle power model was built for -- polling stays at
 * IDLE_POLL_MS, and there are no steps to miss.
 */
#define STEP_POLL_MS		40
#define IDLE_POLL_MS		200

/*
 * Deviation from 1 g that switches to the fast poll. Deliberately well
 * below STILL_MG: that threshold decides wear and measurement windows,
 * where a false positive lights the LEDs, but this one only costs a
 * faster poll, and it has to trip on gentle walking that never deviates
 * 120 mg in the first place -- exactly the motion the 200 ms poll cannot
 * see.
 */
#define STEP_MOTION_MG		50

/*
 * How long the fast poll is held after the last qualifying motion. Covers
 * the pause at a kerb or between strides without flapping between rates.
 */
#define STEP_POLL_HOLD_MS	10000

/* --- GSR stream -------------------------------------------------------- */

/*
 * Sample interval for the raw GSR stream. Skin conductance responses peak
 * roughly 1.4 s after onset, so 10 Hz is the floor at which the rise is
 * resolvable at all; 20 Hz is used so ordinary loop jitter cannot drop the
 * effective rate below that floor. The stream is opt-in and off by
 * default, which is what pays for the extra conversions -- and for
 * GSR_PWR being held on the whole time, which suspends the analog front
 * end's duty cycling entirely.
 */
#define GSR_STREAM_INTERVAL_MS	50

/* Loop cadence needed to hit that interval, whatever state the ring is in. */
#define GSR_STREAM_POLL_MS	20

enum ring_state {
	RING_IDLE,
	RING_MEASURING,
	RING_CHARGING,
};

static const char *const state_name[] = {
	[RING_IDLE] = "idle",
	[RING_MEASURING] = "measuring",
	[RING_CHARGING] = "charging",
};

static struct maxm86161 ppg;
static struct hr hr_state;
static uint32_t samples[FIFO_DEPTH];

/* Cleared when the PPG stops answering, so ppg_on() knows to re-probe. */
static bool ppg_present;

/*
 * Consecutive failed FIFO reads.
 *
 * A part that wedged after a successful boot probe used to log an error
 * forever: ppg_present stayed true, so ppg_on() never re-probed and
 * RING_FLAG_PPG_OK never cleared, and the app was told the sensor was
 * healthy. Only a reset recovered it -- and a part wedging mid-run is the
 * failure this board actually shows, see POSTMORTEM.md.
 *
 * Five in a row at the 20 ms poll interval is a tenth of a second: short
 * enough to recover quickly, long enough not to fire on one bad read.
 */
static uint8_t ppg_fail_count;
#define PPG_FAIL_LIMIT		5

/*
 * Whether imu_init() actually configured the device -- NOT whether the bus
 * works.
 *
 * imu_init() can fail after WHO_AM_I has already succeeded: the CTRL1 write
 * is the last thing it does, and if that write fails the part is left in its
 * power-down default at ODR 0. It then answers reads perfectly well and
 * returns zeros, so "a read succeeded" is not evidence the accelerometer is
 * running. Without this, the idle path set RING_FLAG_IMU_OK for a device
 * that was never started and whose wake interrupt was never armed.
 */
static bool imu_ready;

/*
 * Consecutive failed imu_magnitude_mg() reads while idle.
 *
 * imu_ready used to be cleared only by a failed init, never by a part that
 * had been working and then stopped answering, so an IMU that died mid-
 * session stayed dead until reset -- no wake-on-motion for the rest of the
 * run. This is the PPG's ppg_fail_count pattern applied to the IMU: at the
 * limit, drop imu_ready and let the existing retry path re-init the part.
 *
 * RING_IDLE sleeps 200 ms per pass, so ten in a row is about two seconds:
 * long enough that a single bus transient does not tear down a healthy
 * device, short enough that a real death is picked up almost immediately.
 */
static uint8_t imu_fail_count;
#define IMU_FAIL_LIMIT		10

/*
 * Consecutive re-inits that bought nothing.
 *
 * imu_fail_count used to pull next_imu_retry forward to `now`, which threw
 * the 60 s backoff away: a part whose imu_init() succeeds but whose reads
 * always fail cycled init -> ten failed reads -> init, about two seconds per
 * lap, for ever. On a marginal bus that is roughly six and a half seconds of
 * I2C traffic every ten seconds, spent re-discovering the same answer.
 *
 * The backoff is now honoured, and this counts the laps: every time
 * IMU_FAIL_LIMIT trips, including the first, which follows the boot
 * imu_init() rather than a re-init. Three init-then-fail cycles in a row
 * with no good read in between is not a transient; it is the split failure
 * where the part answers WHO_AM_I and configures cleanly but never produces
 * a sample. Stop, say so once, and leave it until a reset. Any successful
 * read clears the count, so a part that really does recover is never
 * written off.
 */
static uint8_t imu_reinit_count;
#define IMU_REINIT_LIMIT	3

/* Set when IMU_REINIT_LIMIT trips; only a reset clears it. */
static bool imu_unrecoverable;

/*
 * Consecutive failures of the pedometer's own accelerometer read inside a
 * measurement window. Separate from imu_fail_count, which belongs to the
 * IMU health machinery in RING_IDLE and decides whether the part is dead;
 * this one only decides whether to keep asking during this window. Three
 * is enough to distinguish a wedged part from one bad transaction, and
 * cheap to be wrong about -- the cost is some steps missed in one window.
 */
static uint8_t act_fail_count;
#define ACT_FAIL_LIMIT		3

/* How often to retry a failed imu_init(). It is idempotent and cheap. */
#define IMU_RETRY_MS		60000

/* Deviation from 1 g that arms the wake interrupt. */
#define WAKE_THRESHOLD_MG	80

/*
 * Consecutive failed PMIC polls.
 *
 * A PMIC error must not move the state machine -- that was the charger bug.
 * But the only exit from RING_CHARGING is a successful poll reporting "not
 * charging", so a PMIC that never answers again would park the ring there
 * forever and it would never measure again. After this many failures in a
 * row the charger can no longer be confirmed, so idle is the better guess.
 *
 * 15 x PMIC_POLL_MS = 30 s: far beyond any transient, and the cost of being
 * wrong is running the LEDs against a 20 mA charger, not a hazard.
 */
static uint8_t pmic_fail_count;
#define PMIC_FAIL_LIMIT		15

static enum ring_state state = RING_IDLE;
static uint32_t measure_window_ms = MEASURE_WINDOW_MS;
static uint32_t measure_period_ms = MEASURE_PERIOD_MS;
static atomic_t force_measure;

/*
 * steps.c/sleep.c/calories.c have no locking of their own -- unlike
 * latest_status/latest_activity in ble.c, their state is more than one
 * scalar and is read-modify-written every pass through RING_IDLE and
 * RING_MEASURING. Calling their reset/setter functions straight from a
 * BLE control callback, which runs on the BT RX thread, would race the
 * main loop's steps_update()/sleep_feed()/calories_update_minute() --
 * concretely, sleep_feed() reads steps_count() and diffs it against a
 * snapshot, so a steps_reset() landing in between the two makes that
 * diff go deeply negative and wrap to a huge uint32_t, which
 * evaluate_minute() would read as an enormous step count and
 * misclassify the minute. Same atomic-handoff pattern as force_measure
 * above: the callback only sets a flag/value, and only the main loop
 * ever calls into steps.c/sleep.c/calories.c.
 */
static atomic_t activity_reset_pending;
static atomic_t pending_weight_kg_x10;

/*
 * Requested GSR stream state, handed over from the BLE callback.
 *
 * gsr_stream_start() sleeps 800 ms for the analog front end to settle and
 * touches the ADC and a GPIO; the control callback runs on the BT RX
 * thread, where doing any of that would be wrong. -1 means nothing
 * pending, 0 and 1 are the requested state.
 */
static atomic_t gsr_stream_request = ATOMIC_INIT(-1);
static bool gsr_stream_active;

static uint16_t last_hr_x10;
static uint16_t battery_mv;
static int16_t gsr_mv = -1;
static uint8_t subsystem_flags;

/* --- BLE control ------------------------------------------------------- */

static void cb_measure_now(void)
{
	atomic_set(&force_measure, 1);
}

static void cb_set_duty(uint16_t window_s, uint16_t period_s)
{
	/* Clamp to something the hardware and the battery can survive. */
	if (window_s < 5) {
		window_s = 5;
	}
	if (window_s > 300) {
		window_s = 300;
	}
	if (period_s < window_s) {
		period_s = window_s;
	}
	if (period_s > 3600) {
		period_s = 3600;
	}

	measure_window_ms = (uint32_t)window_s * 1000U;
	measure_period_ms = (uint32_t)period_s * 1000U;

	LOG_INF("duty set: %u s window every %u s (%u%%)", window_s, period_s,
		(100U * window_s) / period_s);
}

static void cb_stream_ppg(bool on)
{
	if (on) {
		atomic_set(&force_measure, 1);
	}
}

static void cb_set_weight(uint16_t weight_kg_x10)
{
	/*
	 * 0 is the "nothing pending" sentinel the main loop looks for, so a
	 * literal 0 from the app is nudged to 1 -- calories_set_weight()
	 * clamps anything below 20.0 kg up to the same 20.0 kg floor anyway,
	 * so this changes nothing about the value that is actually applied.
	 */
	atomic_set(&pending_weight_kg_x10, weight_kg_x10 ? weight_kg_x10 : 1);
}

static void cb_reset_activity(void)
{
	atomic_set(&activity_reset_pending, 1);
}

static void cb_stream_gsr(bool on)
{
	atomic_set(&gsr_stream_request, on ? 1 : 0);
}

static const struct ble_control_cbs control_cbs = {
	.stream_ppg = cb_stream_ppg,
	.stream_imu = NULL,
	.measure_now = cb_measure_now,
	.set_duty = cb_set_duty,
	.set_weight = cb_set_weight,
	.reset_activity = cb_reset_activity,
	.stream_gsr = cb_stream_gsr,
};

/* --- status ------------------------------------------------------------ */

static void publish_status(void)
{
	struct ring_status s = {
		.state = (uint8_t)state,
		.flags = subsystem_flags,
		.hr_x10 = last_hr_x10,
		.battery_mv = battery_mv,
		.gsr_mv = gsr_mv,
		.ppg_dc = (uint32_t)hr_baseline(&hr_state),
		.ppg_ac = (uint16_t)hr_amplitude(&hr_state),
		.uptime_s = (uint32_t)(k_uptime_get() / 1000),
	};

	if (state == RING_CHARGING) {
		s.flags |= RING_FLAG_CHARGER;
	}
	if (hr_finger_present(&hr_state)) {
		s.flags |= RING_FLAG_FINGER;
	}
	if (s.ppg_dc > 0) {
		s.perfusion_x10 = (uint16_t)((s.ppg_ac * 1000U) / s.ppg_dc);
	}

	ble_publish_status(&s);
}

/*
 * Start, stop and feed the raw GSR stream. Called from the main loop in
 * every state, because skin conductance has nothing to do with whether the
 * optical front end happens to be running.
 *
 * BLOCKING: the start path sleeps 800 ms for the front end to settle. That
 * is charged to the watchdog budget by the caller, which feeds immediately
 * afterwards.
 */
static void service_gsr_stream(int64_t now, int64_t *next_sample)
{
	static int16_t batch[GSR_MAX_SAMPLES_DEFAULT_MTU];
	static uint8_t batch_n;
	atomic_val_t request = atomic_set(&gsr_stream_request, -1);

	if (request == 1 && !gsr_stream_active) {
		if (gsr_stream_start() == 0) {
			gsr_stream_active = true;
			batch_n = 0;
			*next_sample = now;
		} else {
			LOG_ERR("GSR stream could not start");
		}
	} else if (request == 0 && gsr_stream_active) {
		gsr_stream_stop();
		gsr_stream_active = false;
		batch_n = 0;
	}

	/*
	 * A phone that walks away without turning the stream off would
	 * otherwise leave the analog front end powered until the battery
	 * died; the stream flag is cleared on disconnect.
	 *
	 * Deliberately NOT ble_gsr_streaming(), which also requires the CCC
	 * subscription: an app that sends the opcode before subscribing
	 * would have the front end powered up and then torn straight back
	 * down on the very next pass.
	 */
	if (gsr_stream_active && !ble_gsr_stream_requested()) {
		gsr_stream_stop();
		gsr_stream_active = false;
		batch_n = 0;
		return;
	}

	if (!gsr_stream_active || now < *next_sample) {
		return;
	}

	*next_sample = now + GSR_STREAM_INTERVAL_MS;

	int16_t raw = 0;

	if (gsr_stream_raw(&raw) != 0) {
		return;
	}

	batch[batch_n++] = raw;

	/*
	 * Publish a full batch only. At the default MTU that is seven
	 * samples, so a notification every 350 ms -- comfortably inside one
	 * unfragmented packet, which is the whole reason the batch is sized
	 * against the default rather than the negotiated MTU.
	 */
	if (batch_n >= ARRAY_SIZE(batch)) {
		ble_publish_gsr(batch, batch_n);
		batch_n = 0;
	}
}

/*
 * Say what the activity counters are doing, over RTT.
 *
 * Not a diagnostic that can be compiled out: until a phone has
 * successfully subscribed to the Ring Service -- which per STATUS.md R10
 * has never happened -- this log line is the ONLY way to find out whether
 * the pedometer counts anything on a real wrist, finger or bench shake.
 * Bringing these features up without it means walking a hundred steps and
 * then guessing.
 *
 * Only speaks when something changed. A still ring overnight has nothing
 * to report and would otherwise push the boot messages out of an 8 kB RTT
 * buffer with identical lines; a sleep transition is exactly the event
 * worth seeing, so it is never suppressed.
 */
static void log_activity(void)
{
	static uint32_t last_steps;
	static bool last_asleep;
	static bool primed;

	uint32_t steps = steps_count();
	bool asleep = sleep_is_asleep();

	if (primed && steps == last_steps && asleep == last_asleep) {
		return;
	}

	primed = true;
	last_steps = steps;
	last_asleep = asleep;

	uint32_t kcal_x1000 = calories_total_x1000();

	LOG_INF("activity: %u steps, %u spm, %u.%03u kcal, %s (%u min, "
		"%u restless, %u total)", steps, steps_cadence_spm(),
		kcal_x1000 / 1000U, kcal_x1000 % 1000U,
		asleep ? "asleep" : "awake", sleep_session_minutes(),
		sleep_restless_minutes(), sleep_total_minutes());
}

static void publish_activity(void)
{
	struct ring_activity a = {
		.steps = steps_count(),
		.cadence_spm = steps_cadence_spm(),
		.kcal_x1000 = calories_total_x1000(),
		.sleep_state = sleep_is_asleep() ? SLEEP_STATE_ASLEEP
						 : SLEEP_STATE_AWAKE,
		.sleep_session_min = sleep_session_minutes(),
		.sleep_total_min = sleep_total_minutes(),
		.restless_min = sleep_restless_minutes(),
	};

	ble_publish_activity(&a);
}

static void publish_hrv(void)
{
	struct ring_hrv h = {
		.rmssd_x10 = hrv_rmssd_x10(),
		.rmssd_beats = hrv_diffs(),
	};

	ble_publish_hrv(&h);
}

/* Every call site that used to publish_status() alone now also publishes
 * activity, so the two characteristics never drift out of sync on the app
 * side -- a status update with stale steps/sleep data would be confusing
 * in exactly the way the health flags in publish_status() are not.
 */
static void publish_all(void)
{
	publish_status();
	publish_activity();
	publish_hrv();
}

static void report(uint32_t raw)
{
	static uint32_t win_count;

	uint8_t tag = FIFO_TAG(raw);
	uint32_t value = FIFO_DATA(raw);
	uint16_t bpm_x10 = 0;

	if (tag != TAG_PPG1_LEDC1) {
		LOG_WRN("unexpected FIFO tag 0x%02x (data %u)", tag, value);
		return;
	}

	bool beat = hr_update(&hr_state, value, &bpm_x10);

	/*
	 * Feed HRV only the intervals the detector vouches for. hr.c decides
	 * both parts of that: trusted means the interval passed the bpm range
	 * and agreed with the median, successive means the one before it did
	 * too and was genuinely adjacent. An untrusted interval is still
	 * offered, with successive false, so that it breaks the run rather
	 * than silently letting the next difference span the gap it left.
	 */
	if (beat && hr_last_ibi_trusted(&hr_state)) {
		hrv_add_interval(hr_last_ibi_ms(&hr_state),
				 hr_last_ibi_successive(&hr_state));
	}

	win_count++;

	if (beat && bpm_x10 > 0) {
		LOG_INF("beat   %u.%u bpm", bpm_x10 / 10U, bpm_x10 % 10U);
	}

	if (win_count >= SAMPLE_RATE_HZ) {
		int32_t amp = hr_amplitude(&hr_state);
		int32_t dc = hr_baseline(&hr_state);
		int pi = dc > 0 ? (int)((amp * 1000) / dc) : 0;

		if (bpm_x10 > 0) {
			last_hr_x10 = bpm_x10;
			ble_notify_hr((uint16_t)((bpm_x10 + 5U) / 10U));
			LOG_INF("dc %d  ac %d  PI %d.%d%%  HR %u.%u bpm %s",
				dc, amp, pi / 10, pi % 10,
				bpm_x10 / 10U, bpm_x10 % 10U,
				ble_is_connected() ? "[BLE]" : "");
		} else {
			LOG_INF("dc %d  ac %d  PI %d.%d%%  (%s)", dc, amp,
				pi / 10, pi % 10,
				hr_finger_present(&hr_state)
					? "finger on, acquiring" : "no finger");
		}

		publish_all();
		win_count = 0;
	}
}

/*
 * Probe the PPG and keep the health flag honest.
 *
 * Boot used to sit here for up to 30 s (60 attempts, 500 ms apart) before
 * anything else ran, including BLE. A sensor that is absent at boot is now
 * simply recorded as absent and retried when it is actually needed.
 *
 * Feeds the watchdog per attempt, and that is not cheating: the loop is
 * bounded and making definite progress. It is slow for a specific reason --
 * every failing I2C transaction costs CONFIG_I2C_NRFX_TRANSFER_TIMEOUT
 * (500 ms), and one maxm86161_probe() can issue eight of them across its two
 * candidate addresses, so roughly 4 s per attempt on a stalled bus. Without a
 * feed here the watchdog would reset the board from inside the routine that
 * exists to recover it.
 */
static int ppg_probe(int attempts, int gap_ms)
{
	for (int i = 1; i <= attempts; i++) {
		ring_wdt_feed();

		if (maxm86161_probe(&ppg, I2C_BUS) == 0) {
			ppg_present = true;
			ppg_fail_count = 0;
			subsystem_flags |= RING_FLAG_PPG_OK;
			return 0;
		}
		if (i < attempts) {
			k_msleep(gap_ms);
		}
	}

	ring_wdt_feed();
	ppg_present = false;
	subsystem_flags &= ~RING_FLAG_PPG_OK;
	return -ENODEV;
}

static int ppg_on(void)
{
	int err = regulator_enable(VLED_BOOST);

	if (err) {
		LOG_ERR("boost enable failed (%d)", err);
		return err;
	}

	k_msleep(20);

	/*
	 * Give a missing sensor another chance now that it is actually needed.
	 * With ppg_fail_count this also covers a part that wedged mid-run, not
	 * just one that was already dead at boot.
	 *
	 * ONE attempt, not two. On a stalled bus each attempt costs up to 4 s in
	 * I2C timeouts, and retrying 50 ms after eight consecutive timeouts tells
	 * you nothing the next measurement window will not.
	 */
	if (!ppg_present && ppg_probe(1, 0) != 0) {
		(void)regulator_disable(VLED_BOOST);
		return -ENODEV;
	}

	err = maxm86161_start_ppg(&ppg, PPG_LED, PPG_LED_PA);
	if (err) {
		LOG_ERR("PPG start failed (%d)", err);
		(void)regulator_disable(VLED_BOOST);
		return err;
	}

	hr_init(&hr_state, SAMPLE_RATE_HZ);
	return 0;
}

static void ppg_off(void)
{
	(void)maxm86161_leds_off(&ppg);
	(void)regulator_disable(VLED_BOOST);
#if BENCH_POWER_LED
	/* Boost stays up via the held enable in main(), so this relights. */
	(void)maxm86161_led_solid(&ppg, LEDC_LED1, BENCH_LED_PA);
#endif
	last_hr_x10 = 0;
}

static void slow_sense(void)
{
	uint8_t pct_of_reg;
	int mv = selftest_battery_mv(I2C_BUS, &pct_of_reg);

	if (mv > 0) {
		battery_mv = (uint16_t)mv;
		/* BAS can only carry a percentage; the millivolts are the
		 * real number. See the note in selftest.h.
		 */
		ble_notify_battery(battery_gauge_pct(mv));
		LOG_INF("battery %d mV", mv);
	}

	int g = selftest_gsr_mv();

	gsr_mv = (g >= 0) ? (int16_t)g : -1;
}

/*
 * Why did we just boot?
 *
 * On a sealed board with awkward SWD access this is often the only thing
 * separating "someone plugged the charger in" from "the firmware crashed
 * and something reset it". The nRF latches the cause across the reset;
 * clear it afterwards so the next boot reports its own reason rather than
 * an accumulated history.
 *
 * This is also the readout for the watchdog: wdt.c installs no callback,
 * because the nRF fires its TIMEOUT event only ~61 us before the reset
 * lands, which is not enough to get a line out over RTT.
 */
static void report_reset_cause(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) != 0) {
		return;
	}

	(void)hwinfo_clear_reset_cause();

	if (cause & RESET_WATCHDOG) {
		/*
		 * Deliberately does not quote the timeout: that symbol only
		 * exists when CONFIG_RING_WATCHDOG is set, and naming it here
		 * made a watchdog-disabled build fail to compile.
		 */
		LOG_WRN("reset cause: WATCHDOG (0x%08x) -- the main loop stopped "
			"feeding it", cause);
	} else if (cause & RESET_SOFTWARE) {
		LOG_WRN("reset cause: SOFTWARE (0x%08x) -- fatal error handler "
			"or sys_reboot", cause);
	} else {
		LOG_INF("reset cause: 0x%08x%s%s%s%s", cause,
			(cause & RESET_POR) ? " power-on" : "",
			(cause & RESET_PIN) ? " pin" : "",
			(cause & RESET_DEBUG) ? " debug" : "",
			(cause & RESET_LOW_POWER_WAKE) ? " lp-wake" : "");
	}
}

int main(void)
{
	int64_t next_pmic = 0;
	int64_t next_window = 0;
	int64_t next_slow = 0;
	int64_t next_imu_retry = 0;
	int64_t next_activity_min = 0;
	int64_t next_gsr_sample = 0;
	int64_t window_started = 0;
	int64_t last_motion = 0;
	int64_t last_step_motion = 0;
	uint32_t steps_at_last_min = 0;
	bool waiting_for_finger = false;

	LOG_INF("ring firmware starting");
	report_reset_cause();

	if (!device_is_ready(I2C_BUS) || !device_is_ready(VLED_BOOST)) {
		LOG_ERR("i2c0 or boost regulator not ready");
		return -ENODEV;
	}

	/* Boost up briefly so the MAXM86161 can be found and identified. */
	(void)regulator_enable(VLED_BOOST);
	k_msleep(200);

	/*
	 * Which I2C line is down?
	 *
	 * R1/R2 pull SDA and SCL to 1V8, so an idle healthy bus reads 1 on
	 * both. Reading P0.IN costs no bus traffic and works even when every
	 * transaction times out -- which is exactly when you need to know
	 * whether a line is being held low, and which one.
	 *
	 *   SDA = U5.10 = P0.20,  SCL = U5.11 = P0.14
	 *
	 * nRF52833 P0.IN is at 0x50000000 + 0x510.
	 */
	uint32_t in = *(volatile uint32_t *)0x50000510UL;
	int sda = (in >> 20) & 1U;
	int scl = (in >> 14) & 1U;

	LOG_INF("BUS   idle levels: SDA(P0.20)=%d SCL(P0.14)=%d  -> %s", sda,
		scl, (sda && scl) ? "both released, bus looks healthy"
		: (!sda && !scl) ? "BOTH HELD LOW -- shorted rail or GND bridge"
		: !sda ? "SDA HELD LOW -- stuck slave or bridge on U5.10"
		       : "SCL HELD LOW -- bridge on U5.11 (no slave can do this)");

	/*
	 * Did recovery actually free the line? Nine SCL pulses release a slave
	 * that latched mid-byte, but nothing here ever checked, so a failed
	 * recovery has been indistinguishable from a dead bus. Retry a few
	 * times -- a slave stuck late in a byte can need more than one pass --
	 * and report the level after each.
	 */
	for (int try = 1; try <= 5; try++) {
		int err_rec = i2c_recover_bus(I2C_BUS);

		in = *(volatile uint32_t *)0x50000510UL;
		sda = (in >> 20) & 1U;
		scl = (in >> 14) & 1U;

		LOG_INF("BUS   recover %d: rc=%d -> SDA=%d SCL=%d", try, err_rec,
			sda, scl);

		if (sda && scl) {
			LOG_INF("BUS   released after %d recovery pass(es)", try);
			break;
		}
		k_msleep(5);
	}

	if (!sda) {
		LOG_ERR("BUS   SDA still low after recovery -- not a latched "
			"slave; suspect a bridge from U5.10/SDA to GND");
	}

	/*
	 * BLE before the sensors.
	 *
	 * Boot used to spend up to 30 s retrying the MAXM86161 probe (60
	 * attempts, 500 ms apart) before bt_enable() was even reached, so a
	 * board with a sensor fault was also invisible to the phone -- two
	 * problems presenting as one, with no way to read status until the
	 * sensor was fixed. The radio does not touch the I2C bus, so it goes
	 * first and the sensors settle behind it.
	 */
	ble_set_control_cbs(&control_cbs);

	/*
	 * ble_start()'s return used to be discarded, which made the one
	 * failure that matters completely silent: settings_load() failing
	 * leaves the stack unfinalised, advertising never starts, and the
	 * ring looks like a board with a flat battery. There is no way to
	 * report it over BLE -- BLE is what failed -- so it goes to the log
	 * loudly, and on a bench build to the red LED as well.
	 */
	int ble_rc = ble_start();

	if (ble_rc) {
		LOG_ERR("BLE did not start (%d) -- no advertising, no app, no "
			"status; sensing continues blind", ble_rc);
	}

	/*
	 * Three quick attempts, not sixty slow ones. ppg_on() re-probes when a
	 * window opens, so a sensor that is late or briefly wedged is no longer
	 * written off until the next reset.
	 */
	if (ppg_probe(3, 100) != 0) {
		LOG_WRN("MAXM86161 not responding at boot -- will retry when a "
			"measurement window opens");
	}

#if defined(CONFIG_RING_BENCH)
	/*
	 * Three red blinks when BLE failed to start, so the failure is
	 * visible on a bench with no debugger attached.
	 *
	 * Deliberately here rather than beside the LOG_ERR above: the LED is
	 * driven through the MAXM86161, and up there the part has not been
	 * probed yet, so `ppg` still holds no I2C address and the blink would
	 * go nowhere. The boost is already up from the boot probe. This runs
	 * before ring_wdt_start(), so the ~1.5 s it costs is not charged to
	 * any watchdog budget.
	 */
	if (ble_rc) {
		for (int i = 0; i < 3; i++) {
			(void)maxm86161_indicate(&ppg, LEDC_LED3, 300);
			k_msleep(200);
		}
	}
#endif

	/*
	 * Load-bearing, not a diagnostic. Without this the charge current is
	 * left at the 10 mA power-up default and TS_EN blocks charging
	 * outright. It used to sit inside the self-test suite; selftest_run()
	 * below is bench-only and compiles to nothing in a production build,
	 * which would have taken charging with it.
	 */
	if (pmic_configure(I2C_BUS) == 0) {
		subsystem_flags |= RING_FLAG_PMIC_OK;
	}

	selftest_run(I2C_BUS);

	if (imu_init(I2C_BUS) == 0) {
		imu_ready = true;
		subsystem_flags |= RING_FLAG_IMU_OK;
	}

	steps_init();
	sleep_init();
	calories_init();
	hrv_init();

	/* Nothing to measure yet: shut the optics down and wait for motion. */
	ppg_off();

#if BENCH_POWER_LED
	/*
	 * This enable is deliberately never released, so VLED survives every
	 * ppg_off() and state change and the indicator cannot go dark while
	 * the board is alive.
	 */
	(void)regulator_enable(VLED_BOOST);
	k_msleep(20);
	(void)maxm86161_led_solid(&ppg, LEDC_LED1, BENCH_LED_PA);
	LOG_WRN("BENCH_POWER_LED on: green held solid, boost never sleeps");
#endif

	(void)imu_arm_wake(WAKE_THRESHOLD_MG);
	last_motion = k_uptime_get();
	/* Same assumption last_motion makes: the ring was just handled. Left
	 * at 0 this would instead depend on how long boot happened to take.
	 */
	last_step_motion = k_uptime_get();
	next_window = k_uptime_get() + measure_period_ms;

	/*
	 * A minute from now, not immediately. The other periodic timers start
	 * at 0 so their first poll happens on the first pass, which is right
	 * for a poll and wrong for an accumulator: firing at boot would credit
	 * a minute of resting calories to a minute that has not happened.
	 */
	next_activity_min = k_uptime_get() + 60000;

	/*
	 * Watchdog last, once every subsystem has had its chance.
	 *
	 * On the nRF52833 the watchdog cannot be stopped once started -- there
	 * is no STOP task on this part, only a power-on reset clears it. Arming
	 * it before this point would turn any slow or failing init into a
	 * reboot loop, on a board that is already awkward to reflash.
	 */
	(void)ring_wdt_start();

	LOG_INF("state -> %s", state_name[state]);

	while (1) {
		int64_t now = k_uptime_get();

		/*
		 * Fed from this loop only -- never from a timer or a separate
		 * thread, which would keep the board alive while this loop was
		 * wedged, precisely the failure the watchdog exists to catch.
		 *
		 * BLOCKING AUDIT -- keep this current, because exceeding the
		 * timeout reboots a board that is merely slow.
		 *
		 * What dominates is NOT the k_msleep() calls, it is I2C timeouts:
		 * CONFIG_I2C_NRFX_TRANSFER_TIMEOUT is 500 ms and every transaction
		 * on a stalled bus costs the full amount. An earlier version of
		 * this comment counted only the sleeps, concluded "3x margin", and
		 * missed that one pass could reach ~10 s.
		 *
		 * Feed points, so the longest UN-FED span is what matters:
		 *   here, at the top of every pass
		 *   after the charger block  (it holds the 3 s LED indication)
		 *   after slow_sense()       (~1.3 s: battery reads + GSR settle)
		 *   inside ppg_probe(), per attempt (~4 s each on a stalled bus)
		 *
		 * Longest un-fed span, stalled bus:
		 *   charger attach  : ~0.5 s pmic + 3.0 s indicate        = 3.5 s
		 *   window opening  : ~0.5 s imu + 4.0 s one probe attempt = 4.5 s
		 *   measuring       : ~0.5 s failed FIFO read
		 *                     + ~0.5 s ppg_off() on the 5th failure,
		 *                       doubled under BENCH (led_solid too) = 1.5 s
		 *   measuring, pedometer feed: the imu_magnitude_mg() call added
		 *                     for steps/sleep tracking is skipped by the
		 *                     `break` on the 5th-failure path above, so
		 *                     it never stacks with ppg_off() in the same
		 *                     pass. On its own: ~0.5 s FIFO + ~0.5 s this
		 *                     read = 1.0 s, under the 1.5 s figure above.
		 *   GSR stream start: 0.8 s settle inside gsr_stream_start(),
		 *                     once per stream, fed straight after
		 *   idle, IMU retry : ~1.5 s imu_init() (fed straight after)
		 *
		 * ~4.5 s against the 10 s budget. Both ppg_off() and imu_init()
		 * stop at their first failed write rather than grinding through
		 * every register, which is what keeps those two bounded.
		 *
		 * Add a blocking call longer than the remaining margin and either
		 * put a feed beside it or raise CONFIG_RING_WATCHDOG_TIMEOUT_MS.
		 */
		ring_wdt_feed();

		/*
		 * --- activity control, checked in every state ---
		 *
		 * The only place steps_reset()/sleep_reset()/calories_reset()/
		 * calories_set_weight() are called -- see the comment above
		 * activity_reset_pending's declaration for why the BLE
		 * callbacks only set these instead of calling straight in.
		 */
		if (atomic_cas(&activity_reset_pending, 1, 0)) {
			/* steps_reset() first: sleep_reset() re-snapshots the
			 * step count and would otherwise keep a snapshot from
			 * before the counter was zeroed.
			 */
			steps_reset();
			sleep_reset();
			calories_reset();
			steps_at_last_min = 0;
			LOG_INF("activity counters reset (app request)");
		}

		{
			uint16_t w = (uint16_t)atomic_set(&pending_weight_kg_x10, 0);

			if (w != 0) {
				calories_set_weight(w);
				LOG_INF("body weight set: %u.%u kg (app request)",
					w / 10U, w % 10U);
			}
		}

		/* --- raw GSR stream, checked in every state --- */
		service_gsr_stream(now, &next_gsr_sample);
		ring_wdt_feed();	/* the start path holds 800 ms */

		/* --- charger, checked in every state --- */
		if (now >= next_pmic) {
			bool charging = false;
			int pmic_rc = pmic_service(I2C_BUS, &charging);

			next_pmic = now + PMIC_POLL_MS;

			if (pmic_rc != 0) {
				/*
				 * Unreachable. Report it and change nothing else.
				 *
				 * `charging` is still its false initialiser here,
				 * and acting on that reads as "charger removed":
				 * the ring would leave RING_CHARGING and start the
				 * PPG and the 5 V boost while still sitting on a
				 * 20 mA charger -- the one thing that state exists
				 * to prevent. A single transient -116 on this
				 * board's bus was enough to trigger it.
				 *
				 * A value that could not be read must not move the
				 * state machine.
				 */
				subsystem_flags &= ~RING_FLAG_PMIC_OK;

				/*
				 * ...but do not park here forever. The only exit
				 * from RING_CHARGING is a successful poll saying
				 * "not charging", so a PMIC that never answers
				 * again would strand the ring with the optics off
				 * and nothing to re-evaluate it -- while the PPG
				 * and IMU might be perfectly healthy. After 30 s
				 * the charger can no longer be confirmed, and idle
				 * is the better guess.
				 */
				if (pmic_fail_count < PMIC_FAIL_LIMIT) {
					pmic_fail_count++;

					if (pmic_fail_count == PMIC_FAIL_LIMIT &&
					    state == RING_CHARGING) {
						LOG_WRN("PMIC unreachable for %u s -- "
							"leaving charging state, "
							"charger can no longer be "
							"confirmed",
							(PMIC_FAIL_LIMIT *
							 PMIC_POLL_MS) / 1000U);
						state = RING_IDLE;
						next_window = now;
						publish_all();
					}
				}
			} else {
				pmic_fail_count = 0;

				/*
				 * Report what is actually true. This flag used to
				 * be set unconditionally, right next to a call
				 * whose only return value was "charging" -- so an
				 * unreachable PMIC and a healthy battery-powered
				 * board both came out as "PMIC OK, not charging"
				 * and the app believed it.
				 */
				subsystem_flags |= RING_FLAG_PMIC_OK;

				if (charging && state != RING_CHARGING) {
					if (state == RING_MEASURING) {
						ppg_off();
					}
					LOG_INF("state -> %s",
						state_name[RING_CHARGING]);
					(void)regulator_enable(VLED_BOOST);
					k_msleep(20);
					(void)maxm86161_indicate(&ppg, LEDC_LED3,
								3000);
					(void)regulator_disable(VLED_BOOST);
#if BENCH_POWER_LED
					/* indicate() ends in leds_off; relight. */
					(void)maxm86161_led_solid(&ppg, LEDC_LED1,
								 BENCH_LED_PA);
#endif
					state = RING_CHARGING;
					publish_all();
				} else if (!charging &&
					   state == RING_CHARGING) {
					LOG_INF("state -> %s",
						state_name[RING_IDLE]);
					state = RING_IDLE;
					next_window = now;
					publish_all();
				}
			}
		}

		/* The charger block above can hold the LED on for 3 s. */
		ring_wdt_feed();

		/* --- battery and GSR, slow and cheap to skip --- */
		if (now >= next_slow && state != RING_CHARGING) {
			slow_sense();
			next_slow = now + SLOW_SENSE_MS;
			publish_all();

			/* Battery reads plus the 800 ms GSR settle. */
			ring_wdt_feed();
		}

		/*
		 * --- calories, once a minute ---
		 *
		 * steps_update()/sleep_feed() run inline wherever the IMU is
		 * already being read (below); calories only needs how many
		 * steps that left behind since the last tick, which is an
		 * average over the minute rather than a sample of whatever was
		 * happening at the instant it fired. No I2C traffic here, so no
		 * watchdog feed is needed beside it.
		 */
		if (now >= next_activity_min) {
			uint32_t steps_now = steps_count();

			/*
			 * Not while charging. The ring reads no accelerometer
			 * in that state, so steps and sleep already stop dead;
			 * accruing resting calories through a two-hour charge
			 * would credit ~147 kcal at 70 kg to a wearer the ring
			 * has no reason to believe is wearing it, and would
			 * contradict the rule sleep.c applies to the very same
			 * gap. The deadline still moves, so coming off the
			 * charger does not fire a catch-up burst.
			 */
			if (state != RING_CHARGING) {
				/* Clamped: the control characteristic can zero
				 * the step counter between two ticks, and
				 * unsigned, that wraps.
				 */
				calories_update_minute(
					steps_now >= steps_at_last_min
					? steps_now - steps_at_last_min : 0);
			}

			steps_at_last_min = steps_now;
			next_activity_min = now + 60000;

			log_activity();
		}

		/*
		 * Bench diagnostics. Both are no-ops in a production build --
		 * see Kconfig, CONFIG_RING_IMU_WAKE_DIAG and RING_GSR_MONITOR.
		 *
		 * Called in every state, not just idle: with a charger attached
		 * the ring parks in RING_CHARGING within ~56 ms of boot, which is
		 * precisely when the debug link is healthy enough to watch it, so
		 * an idle-only diagnostic can never be observed on the bench.
		 */
		imu_wake_diag();
		selftest_gsr_monitor();

		switch (state) {
		case RING_CHARGING:
			/* Skin conductance does not care that the ring is on
			 * a charger, so the stream keeps its cadence here.
			 */
			k_msleep(ble_gsr_streaming() ? GSR_STREAM_POLL_MS : 200);
			break;

		case RING_IDLE: {
			int mg;

			/*
			 * Retry a failed init before reading anything.
			 *
			 * A transient bus error during boot used to leave the IMU
			 * dead for the life of the session -- no wake-on-motion,
			 * ever -- exactly the "never recovers" problem that was
			 * fixed for the PPG. imu_init() is idempotent (WHO_AM_I,
			 * then CTRL3 and CTRL1; no reset, no destructive state), so
			 * calling it again is safe. Re-arm the wake interrupt too:
			 * a re-initialised part has forgotten it.
			 */
			if (!imu_ready && !imu_unrecoverable &&
			    now >= next_imu_retry) {
				next_imu_retry = now + IMU_RETRY_MS;

				if (imu_init(I2C_BUS) == 0) {
					imu_ready = true;
					imu_fail_count = 0;
					(void)imu_arm_wake(WAKE_THRESHOLD_MG);
					LOG_INF("IMU recovered, wake re-armed");
				}

				/* Up to three I2C timeouts on a stalled bus. */
				ring_wdt_feed();
			}

			mg = imu_magnitude_mg();

			/*
			 * Keep the IMU flag current, the way the PPG and PMIC flags
			 * now are -- it used to be set once at boot and never
			 * re-evaluated, so a cold joint on U4 (HANDOFF.md section 6)
			 * or a stuck SDA still reported the IMU as healthy.
			 *
			 * imu_ready matters as much as the read: a successful read
			 * only proves the bus works. An unconfigured part sitting in
			 * power-down answers reads and returns zeros, so testing the
			 * read alone would report a dead IMU as healthy -- the very
			 * thing this flag was made live to stop.
			 *
			 * KNOWN BEHAVIOUR -- a part that dies mid-run is
			 * re-initialised rather than merely reported:
			 * IMU_FAIL_LIMIT consecutive failed reads (~2 s at the
			 * 200 ms idle poll) clear imu_ready, and the retry block at
			 * the top of this case calls imu_init() again IMU_RETRY_MS
			 * later. This is the same recovery ppg_fail_count gives the
			 * PPG. Only counted while imu_ready is set -- once it is
			 * clear the retry path owns the device and there is nothing
			 * left to count down to.
			 *
			 * The retry is 60 s away, not immediate. Setting
			 * next_imu_retry to `now` here discarded the backoff
			 * entirely, and for the part this code exists to handle --
			 * one whose init succeeds and whose reads do not -- that is
			 * a two-second loop of init, ten failed reads, init, with no
			 * exit: roughly 6.5 s of I2C in every 10 s, for ever, on a
			 * bus that is already marginal.
			 *
			 * IMU_REINIT_LIMIT laps of that without a single good read
			 * in between is the split failure, and no further re-init is
			 * going to change it, so stop and say so once.
			 */
			if (imu_ready && mg < 0) {
				if (imu_fail_count < IMU_FAIL_LIMIT &&
				    ++imu_fail_count == IMU_FAIL_LIMIT) {
					imu_ready = false;
					next_imu_retry = now + IMU_RETRY_MS;

					if (imu_reinit_count < IMU_REINIT_LIMIT &&
					    ++imu_reinit_count == IMU_REINIT_LIMIT) {
						imu_unrecoverable = true;
						LOG_ERR("IMU unrecoverable: %d "
							"init-then-fail cycles of "
							"%d failed reads with no good "
							"read in between -- no further "
							"retries until reset",
							IMU_REINIT_LIMIT,
							IMU_FAIL_LIMIT);
					} else {
						LOG_WRN("IMU unresponsive after %d "
							"reads -- marking "
							"uninitialised, re-init in "
							"%u s", IMU_FAIL_LIMIT,
							IMU_RETRY_MS / 1000U);
					}
				}
			} else if (mg >= 0) {
				imu_fail_count = 0;
				imu_reinit_count = 0;
			}

			if (!imu_ready || mg < 0) {
				subsystem_flags &= ~RING_FLAG_IMU_OK;
			} else {
				subsystem_flags |= RING_FLAG_IMU_OK;
			}

			/* Gravity alone reads ~1000 mg; deviation is motion. */
			if (mg >= 0 && abs(mg - 1000) > STILL_MG) {
				last_motion = now;
			}

			/*
			 * Feed the pedometer and sleep tracker off the same
			 * magnitude read used for the motion timeout above --
			 * no extra I2C traffic for either.
			 *
			 * imu_ready, not just mg >= 0. A part that was never
			 * configured answers reads perfectly well and returns
			 * zeros (the failure the comment above describes), and
			 * zeros are not a failed read: they are a magnitude of
			 * 0 mg, a full 1 g away from rest. That would refresh
			 * last_step_motion on every pass -- pinning the ring to
			 * the fast poll for as long as it ran -- and make every
			 * sleep bucket look active, so a session could never
			 * start. Feeding nothing is the honest answer when
			 * there is no working accelerometer; sleep.c's gap
			 * handling closes any session that was open.
			 */
			if (imu_ready && mg >= 0) {
				if (abs(mg - 1000) > STEP_MOTION_MG) {
					last_step_motion = now;
				}
				steps_update(mg, now);
				sleep_feed(mg, now);
			}

			if (ble_imu_streaming()) {
				struct imu_accel a;

				if (imu_read_accel(&a) == 0) {
					ble_publish_imu(a.x, a.y, a.z);
				}
			}

			if (imu_take_wake_event()) {
				imu_clear_int();
				last_motion = now;
				next_window = now;	/* go look for a finger */
				LOG_INF("motion wake");
			}

			bool forced = atomic_cas(&force_measure, 1, 0);
			bool stale = (now - last_motion) > STILL_TIMEOUT_MS;

			if (forced || (now >= next_window && !stale)) {
				if (ppg_on() == 0) {
					state = RING_MEASURING;
					window_started = now;
					waiting_for_finger = !forced;
					/* Each window gets its own budget of
					 * accelerometer retries.
					 */
					act_fail_count = 0;
					LOG_INF("state -> %s%s", state_name[state],
						forced ? " (app requested)" : "");
					publish_all();
				} else {
					next_window = now + measure_period_ms;
				}
			} else {
				/*
				 * Poll fast enough to actually see footfalls
				 * while the ring is moving, and fall back to
				 * the idle rate once it is still. See
				 * STEP_POLL_MS.
				 */
				bool maybe_walking =
					(now - last_step_motion) < STEP_POLL_HOLD_MS;

				k_msleep(ble_imu_streaming() ? 20
					 : ble_gsr_streaming() ? GSR_STREAM_POLL_MS
					 : maybe_walking ? STEP_POLL_MS
					 : IDLE_POLL_MS);
			}
			break;
		}

		case RING_MEASURING: {
			int count = maxm86161_fifo_read(&ppg, samples,
							ARRAY_SIZE(samples));

			if (count > 0) {
				ppg_fail_count = 0;
				ble_publish_ppg(samples, (uint8_t)count);
				for (int i = 0; i < count; i++) {
					report(samples[i]);
				}
			} else if (count < 0) {
				LOG_ERR("FIFO read failed (%d)", count);

				/*
				 * Logging this forever was the whole bug. Give up
				 * after PPG_FAIL_LIMIT in a row, mark the part
				 * absent and fall back to idle so the next window
				 * re-probes it -- and so the status flag stops
				 * claiming a sensor that has not answered in a
				 * tenth of a second is healthy.
				 */
				if (ppg_fail_count < PPG_FAIL_LIMIT &&
				    ++ppg_fail_count == PPG_FAIL_LIMIT) {
					LOG_WRN("PPG unresponsive after %d reads -- "
						"marking absent, will re-probe next "
						"window", PPG_FAIL_LIMIT);
					ppg_off();
					ppg_present = false;
					subsystem_flags &= ~RING_FLAG_PPG_OK;
					state = RING_IDLE;
					next_window = now + measure_period_ms;
					publish_all();
					break;
				}
			}

			if (ble_imu_streaming()) {
				struct imu_accel a;

				if (imu_read_accel(&a) == 0) {
					ble_publish_imu(a.x, a.y, a.z);
				}
			}

			/*
			 * Keep the pedometer and sleep tracker fed during a
			 * measurement window too, not just while idle -- a
			 * window covers up to 15 s in every 60, and skipping
			 * it would silently undercount steps taken while
			 * worn and moving. Gated on imu_ready so a part
			 * already known dead is not charged a full I2C
			 * timeout here as well as in RING_IDLE.
			 *
			 * The IMU health machinery deliberately lives in
			 * RING_IDLE and is not duplicated here, so this read
			 * has no fail counter behind it to stop it retrying.
			 * Without a limit, an IMU that wedges while the PPG
			 * stays healthy would cost a full I2C timeout on
			 * every 20 ms pass for the whole window -- roughly
			 * 500 ms of stalled bus per pass, for 15 s of
			 * window, achieving nothing. Give up on the feed for
			 * the rest of this window instead; the next window
			 * starts fresh, and RING_IDLE still owns deciding
			 * whether the part is actually dead.
			 */
			if (imu_ready && act_fail_count < ACT_FAIL_LIMIT) {
				int act_mg = imu_magnitude_mg();

				if (act_mg >= 0) {
					act_fail_count = 0;
					if (abs(act_mg - 1000) > STEP_MOTION_MG) {
						/*
						 * Keeps the fast idle poll from
						 * being re-earned from scratch
						 * when the window closes: the
						 * hold would otherwise have
						 * expired mid-window and the
						 * first idle pass would use the
						 * slow rate that counts nothing.
						 */
						last_step_motion = now;
					}
					steps_update(act_mg, now);
					sleep_feed(act_mg, now);
				} else if (act_fail_count < ACT_FAIL_LIMIT) {
					act_fail_count++;
				}
			}

			if (hr_finger_present(&hr_state)) {
				waiting_for_finger = false;
				last_motion = now;
			}

			bool give_up = waiting_for_finger &&
				(now - window_started) > NO_FINGER_TIMEOUT_MS;
			bool done = (now - window_started) >= measure_window_ms;

			/* Keep measuring as long as the app wants raw data. */
			if (ble_ppg_streaming()) {
				done = false;
				give_up = false;
			}

			if (done || give_up) {
				ppg_off();
				state = RING_IDLE;
				next_window = now + measure_period_ms;
				LOG_INF("state -> %s%s", state_name[state],
					give_up ? " (no finger)" : "");
				publish_all();
			} else {
				k_msleep(POLL_INTERVAL_MS);
			}
			break;
		}
		}
	}

	return 0;
}
