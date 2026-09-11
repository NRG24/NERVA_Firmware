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
#include "hr.h"
#include "imu.h"
#include "maxm86161.h"
#include "selftest.h"
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

static enum ring_state state = RING_IDLE;
static uint32_t measure_window_ms = MEASURE_WINDOW_MS;
static uint32_t measure_period_ms = MEASURE_PERIOD_MS;
static atomic_t force_measure;

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

static const struct ble_control_cbs control_cbs = {
	.stream_ppg = cb_stream_ppg,
	.stream_imu = NULL,
	.measure_now = cb_measure_now,
	.set_duty = cb_set_duty,
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

		publish_status();
		win_count = 0;
	}
}

/*
 * Probe the PPG and keep the health flag honest.
 *
 * Boot used to sit here for up to 30 s (60 attempts, 500 ms apart) before
 * anything else ran, including BLE. A sensor that is absent at boot is now
 * simply recorded as absent and retried when it is actually needed.
 */
static int ppg_probe(int attempts, int gap_ms)
{
	for (int i = 1; i <= attempts; i++) {
		if (maxm86161_probe(&ppg, I2C_BUS) == 0) {
			ppg_present = true;
			subsystem_flags |= RING_FLAG_PPG_OK;
			return 0;
		}
		if (i < attempts) {
			k_msleep(gap_ms);
		}
	}

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
	 * This also recovers a part that was wedged when the board booted --
	 * previously that state persisted until the next reset.
	 */
	if (!ppg_present && ppg_probe(2, 50) != 0) {
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
	int64_t window_started = 0;
	int64_t last_motion = 0;
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
	(void)ble_start();

	/*
	 * Three quick attempts, not sixty slow ones. ppg_on() re-probes when a
	 * window opens, so a sensor that is late or briefly wedged is no longer
	 * written off until the next reset.
	 */
	if (ppg_probe(3, 100) != 0) {
		LOG_WRN("MAXM86161 not responding at boot -- will retry when a "
			"measurement window opens");
	}

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
		subsystem_flags |= RING_FLAG_IMU_OK;
	}

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

	(void)imu_arm_wake(80);
	last_motion = k_uptime_get();
	next_window = k_uptime_get() + measure_period_ms;

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
		 * Fed here and nowhere else. Feeding from a timer or a separate
		 * thread would keep the board alive while this loop was wedged,
		 * which is precisely the failure it exists to catch.
		 *
		 * BLOCKING AUDIT -- keep this current, because exceeding the
		 * timeout reboots a working board.
		 *
		 * The two long paths are mutually exclusive. maxm86161_indicate()
		 * blocks 3 s, but only on the pass that enters RING_CHARGING, and
		 * slow_sense() is gated on state != RING_CHARGING, so its 800 ms
		 * GSR settle cannot land in that same pass.
		 *
		 *   charger attach : ~55 ms pmic + 3000 ms indicate + ~50 ms = 3.1 s
		 *   ordinary pass  : ~55 ms pmic + ~900 ms slow_sense      = 1.0 s
		 *   measuring      : FIFO read, filters, notifies          < 50 ms
		 *
		 * Worst case ~3.1 s against a 10 s budget, so roughly 3x margin.
		 * Add a longer blocking call and raise
		 * CONFIG_RING_WATCHDOG_TIMEOUT_MS with it.
		 */
		ring_wdt_feed();

		/* --- charger, checked in every state --- */
		if (now >= next_pmic) {
			bool charging = false;
			int pmic_rc = pmic_service(I2C_BUS, &charging);

			next_pmic = now + PMIC_POLL_MS;

			/*
			 * Report what is actually true. This flag used to be
			 * set unconditionally, right next to a call whose only
			 * return value was "charging" -- so an unreachable PMIC
			 * and a healthy battery-powered board both came out as
			 * "PMIC OK, not charging" and the app believed it.
			 */
			if (pmic_rc == 0) {
				subsystem_flags |= RING_FLAG_PMIC_OK;
			} else {
				subsystem_flags &= ~RING_FLAG_PMIC_OK;
			}

			if (charging && state != RING_CHARGING) {
				if (state == RING_MEASURING) {
					ppg_off();
				}
				LOG_INF("state -> %s", state_name[RING_CHARGING]);
				(void)regulator_enable(VLED_BOOST);
				k_msleep(20);
				(void)maxm86161_indicate(&ppg, LEDC_LED3, 3000);
				(void)regulator_disable(VLED_BOOST);
#if BENCH_POWER_LED
				/* indicate() ends in leds_off; put green back. */
				(void)maxm86161_led_solid(&ppg, LEDC_LED1,
							 BENCH_LED_PA);
#endif
				state = RING_CHARGING;
				publish_status();
			} else if (!charging && state == RING_CHARGING) {
				LOG_INF("state -> %s", state_name[RING_IDLE]);
				state = RING_IDLE;
				next_window = now;
				publish_status();
			}
		}

		/* --- battery and GSR, slow and cheap to skip --- */
		if (now >= next_slow && state != RING_CHARGING) {
			slow_sense();
			next_slow = now + SLOW_SENSE_MS;
			publish_status();
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
			k_msleep(200);
			break;

		case RING_IDLE: {
			int mg = imu_magnitude_mg();

			/* Gravity alone reads ~1000 mg; deviation is motion. */
			if (mg >= 0 && abs(mg - 1000) > STILL_MG) {
				last_motion = now;
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
					LOG_INF("state -> %s%s", state_name[state],
						forced ? " (app requested)" : "");
					publish_status();
				} else {
					next_window = now + measure_period_ms;
				}
			} else {
				k_msleep(ble_imu_streaming() ? 20 : 200);
			}
			break;
		}

		case RING_MEASURING: {
			int count = maxm86161_fifo_read(&ppg, samples,
							ARRAY_SIZE(samples));

			if (count > 0) {
				ble_publish_ppg(samples, (uint8_t)count);
				for (int i = 0; i < count; i++) {
					report(samples[i]);
				}
			} else if (count < 0) {
				LOG_ERR("FIFO read failed (%d)", count);
			}

			if (ble_imu_streaming()) {
				struct imu_accel a;

				if (imu_read_accel(&a) == 0) {
					ble_publish_imu(a.x, a.y, a.z);
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
				publish_status();
			} else {
				k_msleep(POLL_INTERVAL_MS);
			}
			break;
		}
		}
	}

	return 0;
}
