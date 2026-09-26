#include "selftest.h"

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(selftest, LOG_LEVEL_INF);

/* --- addresses, from the datasheets in this repo -------------------- */

#define PMIC_ADDR		0x6a	/* BQ25120A, datasheet p.33  */
#define PMIC_REG_STATUS		0x00
#define PMIC_REG_FAULTS		0x01
#define PMIC_REG_TSCTRL		0x02	/* TS control + fault masks, p.38 */
#define PMIC_TS_EN		BIT(7)
#define PMIC_REG_FASTCHG	0x03	/* fast charge control, p.39 */
/*
 * ICHRG_RANGE = 0 selects 5-35 mA in 1 mA steps, ICHRG = 5 mA + code.
 * Code 15 gives 20 mA. CE = 0 (enabled), HZ_MODE = 0.
 */
#define PMIC_FASTCHG_20MA	((15u << 2) | 0u)
#define PMIC_REG_VBATREG	0x05	/* VBATREG = 3.6V + code*10mV, p.40 */
#define PMIC_REG_VBMON		0x0a	/* voltage based battery monitor, p.45 */
#define PMIC_VBMON_READ		BIT(7)

#define IMU_ADDR		0x6b	/* LSM6DSV, SA0 tied to 1V8  */
#define IMU_REG_WHO_AM_I	0x0f
/* 0x70 = LSM6DSV, 0x71 = LSM6DSV16BX. Both use CTRL1 at 0x10 and
 * accelerometer output at 0x28, so basic reads work either way.
 */
#define IMU_WHO_LSM6DSV		0x70
#define IMU_WHO_LSM6DSV16BX	0x71
#define IMU_REG_CTRL1		0x10
#define IMU_REG_OUTX_L_A	0x28

/* CTRL1: OP_MODE_XL 000 = high performance, ODR_XL 0110 = 120 Hz */
#define IMU_CTRL1_120HZ_HP	0x06

/* --- board pins, from Netlist_New_Switch_2 -------------------------- */

/* GSR_PWR = U5.14 = GPIO_14 = P0.11 */
#define GSR_PWR_PIN		11
/*
 * GSR scaling.
 *
 * U7B is a transimpedance stage biased at V_REF, so
 *
 *     V_OUT = V_REF x (1 + R5 / R_skin)   ->   G = (V_OUT/V_REF - 1) / R5
 *
 * V_REF measured 500 mV at U7.1. R5 is 91 kOhm, from the BOM.
 *
 * Do NOT try to derive R5 from a hand-held reference resistor. That was
 * attempted here and gave 12.7 kOhm, which is wrong by a factor of seven:
 * the formula assumes the reference is the only path across the electrodes,
 * and contact impedance -- including the skin of whoever is holding it --
 * sits in series and is silently absorbed into the answer. Solder or clamp
 * the reference, with no human in the loop.
 *
 * Both constants must be revisited on any board that changes R5, R3, R4 or
 * the GSR_PWR rail, since V_REF tracks the supply.
 */
#define GSR_VREF_MV		500
#define GSR_R5_OHMS		91000

/* GSR_ADC = U5.19 = GPIO_19 = P0.03 = AIN1 */
#define GSR_ADC_PIN		3
/* ACC_INT = U5.36 = GPIO_36 = P0.16 */
#define ACC_INT_PIN		16
/*
 * CD (BQ25120A E2) = U5.16 = GPIO_16 = P1.00 on the 2026-09-23 board.
 *
 * It was U5.13 = P1.09 on the 2026-08-21 board. Driving the old pin on the
 * new board leaves CD floating on its 900k pull-down, the PMIC drops into
 * Hi-Z and its I2C goes silent -- exactly the -5 seen on first bring-up.
 */
#define CD_PIN			0

/*
 * Set once the live monitor is driving GSR_PWR. Without this, slow_sense()
 * calls selftest_gsr_mv() every 30 s, which powers the front end back down
 * when it finishes -- putting a dip in the trace at exactly 33.3 s on every
 * boot that looks indistinguishable from a real GSR event.
 */
static bool gsr_pwr_owned;

static const struct adc_dt_spec gsr_adc =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

static int reg_read(const struct device *i2c, uint8_t addr, uint8_t reg,
		    uint8_t *buf, size_t len)
{
	/* Always pointer-first. Never a bare address read -- see POSTMORTEM.md */
	return i2c_write_read(i2c, addr, &reg, 1, buf, len);
}

/*
 * On battery only (VIN < VUVLO) the BQ25120A sits in High Impedance mode
 * while CD is low, and in Hi-Z its I2C interface is switched off
 * (datasheet 9.3.2, Table 1). CD has a 900k internal pull-down, so leaving
 * CD as an input guarantees Hi-Z and a silent PMIC. Drive CD high for
 * Active Battery mode, which is also what you want for normal running.
 *
 * Note the trade: with a charger attached, CD high disables charging. Drop
 * it low again when VIN is valid and you want to charge.
 */
static void pmic_enable_i2c(void)
{
	const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

	gpio_pin_configure(gpio1, CD_PIN, GPIO_OUTPUT_ACTIVE);
	k_msleep(10);
}

/*
 * Everything the BQ25120A needs written before it will charge at all.
 *
 * Deliberately separate from test_pmic(): these are not diagnostics. They
 * used to live inside the self-test suite, which meant bench-gating that
 * suite would also have removed the charge-current and TS writes and
 * stopped charging working, with nothing in the log to say why.
 *
 * Returns 0, or a negative errno if the PMIC could not be reached.
 */
int pmic_configure(const struct device *i2c)
{
	uint8_t chg = 0, ts = 0, faults = 0;
	int err;

	pmic_enable_i2c();

	err = reg_read(i2c, PMIC_ADDR, PMIC_REG_FAULTS, &faults, 1);
	if (err) {
		LOG_ERR("PMIC  BQ25120A @0x%02x unreachable (%d) -- not "
			"configured; charging will not work", PMIC_ADDR, err);
		return err;
	}

	/*
	 * Charge current. The part powers up at its orderable default of
	 * 10 mA because ISET (C1) is tied to GND. ICHRG_RANGE (b7): 0 means
	 * 5 mA + code x 1 mA, 1 means 40 mA + code x 10 mA.
	 */
	if (reg_read(i2c, PMIC_ADDR, PMIC_REG_FASTCHG, &chg, 1) == 0) {
		uint8_t code = (chg >> 2) & 0x1f;
		int ichrg_ma = (chg & BIT(7)) ? (40 + code * 10) : (5 + code);

		if (ichrg_ma != 20) {
			uint8_t w[2] = { PMIC_REG_FASTCHG, PMIC_FASTCHG_20MA };

			if (i2c_write(i2c, w, sizeof(w), PMIC_ADDR) == 0) {
				LOG_INF("PMIC  charge current %d mA -> 20 mA",
					ichrg_ma);
			}
		}
	}

	/*
	 * TS (ball C3) is unconnected on this board, so the NTC monitor reads
	 * out of range and suspends charging with TS_FAULT = 01 while the
	 * fault register reads clean. With no thermistor fitted, disabling the
	 * monitor is the only way to charge at all.
	 *
	 * HARDWARE_NOTES.md item 9 records that the battery connector has only
	 * BAT+ and GND -- there is no thermistor in the pack, so TS never had
	 * anything to measure and clearing TS_EN gives up no protection that
	 * existed. It does mean charging runs with no temperature limit at all,
	 * on a cell worn against skin, so it is warned every boot rather than
	 * logged once at info level. The fix is a fixed divider on TS.
	 */
	if (reg_read(i2c, PMIC_ADDR, PMIC_REG_TSCTRL, &ts, 1) == 0 &&
	    (ts & PMIC_TS_EN)) {
		uint8_t w[2] = { PMIC_REG_TSCTRL, (uint8_t)(ts & ~PMIC_TS_EN) };

		if (i2c_write(i2c, w, sizeof(w), PMIC_ADDR) == 0) {
			LOG_WRN("PMIC  TS monitor disabled -- charging with no "
				"battery temperature limit");
		}
	}

	return 0;
}

#if defined(CONFIG_RING_BENCH)
static void test_pmic(const struct device *i2c)
{
	uint8_t status, faults;
	int err;

	pmic_enable_i2c();

	err = reg_read(i2c, PMIC_ADDR, PMIC_REG_STATUS, &status, 1);
	if (err) {
		/* Report every access pattern with its real errno rather than
		 * a single opaque failure -- that habit cost two days once.
		 */
		uint8_t reg = PMIC_REG_STATUS, v = 0;
		int e_ptr = i2c_write(i2c, &reg, 1, PMIC_ADDR);
		int e_split = e_ptr ? -1 : i2c_read(i2c, &v, 1, PMIC_ADDR);
		uint8_t w[2] = { PMIC_REG_FAULTS, 0x00 };
		int e_write = i2c_write(i2c, w, sizeof(w), PMIC_ADDR);

		LOG_ERR("PMIC  BQ25120A @0x%02x: restart=%d ptr_write=%d "
			"split=%d(0x%02x) reg_write=%d", PMIC_ADDR, err,
			e_ptr, e_split, v, e_write);
		return;
	}

	(void)reg_read(i2c, PMIC_ADDR, PMIC_REG_FAULTS, &faults, 1);

	static const char *const stat[] = {
		"ready", "charging", "charge done", "FAULT",
	};

	LOG_INF("PMIC  BQ25120A @0x%02x: %s  (status 0x%02x, faults 0x%02x)",
		PMIC_ADDR, stat[status >> 6], status, faults);

	uint8_t chg = 0;

	if (reg_read(i2c, PMIC_ADDR, PMIC_REG_FASTCHG, &chg, 1) == 0) {
		/* ICHRG_RANGE (b7): 0 -> 5mA + code*1mA, 1 -> 40mA + code*10mA */
		uint8_t code = (chg >> 2) & 0x1f;
		int ichrg_ma = (chg & BIT(7)) ? (40 + code * 10) : (5 + code * 1);

		if (ichrg_ma != 20) {
			uint8_t w[2] = { PMIC_REG_FASTCHG, PMIC_FASTCHG_20MA };

			if (i2c_write(i2c, w, sizeof(w), PMIC_ADDR) == 0) {
				LOG_INF("PMIC  charge current %d mA -> 20 mA", ichrg_ma);
				chg = PMIC_FASTCHG_20MA;
				code = (chg >> 2) & 0x1f;
				ichrg_ma = 5 + code;
			}
		}

		LOG_INF("PMIC  charge current %d mA, charger %s, %s  (0x03 = 0x%02x)",
			ichrg_ma,
			(chg & BIT(1)) ? "DISABLED (CE=1)" : "enabled",
			(chg & BIT(0)) ? "HIGH-Z" : "not high-Z", chg);
	}

	/*
	 * TS (ball C3) is unconnected on this board, so the NTC monitor reads
	 * out of range and the charger reports a fault with charging
	 * suspended -- TS_FAULT = 01 in register 0x02. With no thermistor
	 * fitted the only correct action is to disable the TS function.
	 *
	 * This is a firmware workaround for a hardware omission. The next
	 * board spin should fit the divider; see HARDWARE_NOTES.md.
	 */
	uint8_t ts = 0;

	if (reg_read(i2c, PMIC_ADDR, PMIC_REG_TSCTRL, &ts, 1) == 0) {
		static const char *const ts_fault[] = {
			"normal",
			"out of range - CHARGING SUSPENDED",
			"cool - charge current halved",
			"warm - charge voltage reduced",
		};

		LOG_INF("PMIC  TS %s, fault: %s  (0x02 = 0x%02x)",
			(ts & PMIC_TS_EN) ? "enabled" : "disabled",
			ts_fault[(ts >> 5) & 0x03], ts);

		if (ts & PMIC_TS_EN) {
			uint8_t w[2] = { PMIC_REG_TSCTRL,
					 (uint8_t)(ts & ~PMIC_TS_EN) };

			if (i2c_write(i2c, w, sizeof(w), PMIC_ADDR) == 0) {
				LOG_INF("PMIC  TS monitor disabled (no NTC fitted)");
			}
		}
	}

	/*
	 * CD trade-off, datasheet Table 1: with VIN valid, CD high disables
	 * charging; with battery only, CD low means Hi-Z and no I2C. So park
	 * CD wherever the present situation needs it.
	 *
	 * VIN_UV (faults b6) set means no valid input, i.e. battery only.
	 */
	if (faults & BIT(6)) {
		LOG_INF("PMIC  no charger present -- CD held high for I2C");
	} else {
		const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

		gpio_pin_configure(gpio1, CD_PIN, GPIO_OUTPUT_INACTIVE);
		LOG_INF("PMIC  charger detected -- CD driven low to enable charging");

		/* Give the charger a moment, then report what it decided. */
		k_msleep(250);

		uint8_t again = 0;

		if (reg_read(i2c, PMIC_ADDR, PMIC_REG_STATUS, &again, 1) == 0) {
			LOG_INF("PMIC  after enable: %s  (status 0x%02x)",
				stat[again >> 6], again);
		}
	}

	if (faults & BIT(7)) {
		LOG_WRN("PMIC  VIN overvoltage");
	}
	if (faults & BIT(6)) {
		LOG_WRN("PMIC  VIN undervoltage");
	}
	if (faults & BIT(5)) {
		LOG_WRN("PMIC  battery UVLO");
	}
	if (faults & BIT(4)) {
		LOG_WRN("PMIC  battery overcurrent");
	}
}
#endif /* CONFIG_RING_BENCH */

int selftest_battery_mv(const struct device *i2c, uint8_t *percent_of_vbatreg)
{
	uint8_t vbatreg_raw = 0, mon = 0;
	uint8_t trigger[2] = { PMIC_REG_VBMON, PMIC_VBMON_READ };
	int err;

	pmic_enable_i2c();

	err = reg_read(i2c, PMIC_ADDR, PMIC_REG_VBATREG, &vbatreg_raw, 1);
	if (err) {
		return err;
	}

	/* VBATREG = 3.6 V + VBREG_CODE x 10 mV, code is bits [7:1] */
	int vbatreg_mv = 3600 + ((vbatreg_raw >> 1) * 10);

	/* Kick off a conversion, then let it settle. */
	err = i2c_write(i2c, trigger, sizeof(trigger), PMIC_ADDR);
	if (err) {
		return err;
	}
	k_msleep(5);

	err = reg_read(i2c, PMIC_ADDR, PMIC_REG_VBMON, &mon, 1);
	if (err) {
		return err;
	}

	/* VBMON_RANGE[6:5]: 00=60-70%, 01=70-80%, 10=80-90%, 11=90-100% */
	static const uint8_t range_base[4] = { 60, 70, 80, 90 };
	uint8_t base = range_base[(mon >> 5) & 0x03];

	/* VBMON_TH[4:2]: 001=+0%, 010=+2%, 011=+4%, 110=+6%, 111=+8% */
	uint8_t th = (mon >> 2) & 0x07;
	uint8_t offset;

	switch (th) {
	case 0x7: offset = 8; break;
	case 0x6: offset = 6; break;
	case 0x3: offset = 4; break;
	case 0x2: offset = 2; break;
	default:  offset = 0; break;
	}

	uint8_t pct = base + offset;

	if (percent_of_vbatreg) {
		*percent_of_vbatreg = pct;
	}

	return (vbatreg_mv * pct) / 100;
}

/*
 * Straight linear map from 3.30 V (empty-ish) to 4.20 V (full). Named a
 * gauge rather than a state of charge on purpose: a real LiPo curve is
 * strongly non-linear and load-dependent, and inventing one produces a
 * confident number that is simply wrong. Report millivolts; use this only
 * where a percentage is structurally required.
 */
uint8_t battery_gauge_pct(int mv)
{
	if (mv >= 4200) {
		return 100;
	}
	if (mv <= 3300) {
		return 0;
	}

	return (uint8_t)(((mv - 3300) * 100) / (4200 - 3300));
}

#if defined(CONFIG_RING_BENCH)
static void test_battery(const struct device *i2c)
{
	uint8_t pct_of_reg = 0;
	int mv = selftest_battery_mv(i2c, &pct_of_reg);

	if (mv < 0) {
		LOG_ERR("BATT  read failed (%d)", mv);
		return;
	}

	LOG_INF("BATT  %d mV  (%u%% of VBATREG, +/-42 mV monitor resolution)",
		mv, pct_of_reg);
}

static void test_imu(const struct device *i2c)
{
	uint8_t who = 0;
	uint8_t raw[6];
	int err;

	err = reg_read(i2c, IMU_ADDR, IMU_REG_WHO_AM_I, &who, 1);
	if (err) {
		LOG_ERR("IMU   LSM6DSV @0x%02x: no answer (%d)", IMU_ADDR, err);
		return;
	}

	const char *part;

	switch (who) {
	case IMU_WHO_LSM6DSV:
		part = "LSM6DSV";
		break;
	case IMU_WHO_LSM6DSV16BX:
		part = "LSM6DSV16BX";
		break;
	default:
		LOG_ERR("IMU   unknown WHO_AM_I 0x%02x at 0x%02x", who, IMU_ADDR);
		return;
	}

	uint8_t cfg[2] = { IMU_REG_CTRL1, IMU_CTRL1_120HZ_HP };

	err = i2c_write(i2c, cfg, sizeof(cfg), IMU_ADDR);
	if (err) {
		LOG_ERR("IMU   CTRL1 write failed (%d)", err);
		return;
	}

	k_msleep(50);

	err = reg_read(i2c, IMU_ADDR, IMU_REG_OUTX_L_A, raw, sizeof(raw));
	if (err) {
		LOG_ERR("IMU   accel read failed (%d)", err);
		return;
	}

	int16_t ax = (int16_t)(raw[0] | (raw[1] << 8));
	int16_t ay = (int16_t)(raw[2] | (raw[3] << 8));
	int16_t az = (int16_t)(raw[4] | (raw[5] << 8));

	/* default full scale is +/-2 g over a signed 16-bit range */
	LOG_INF("IMU   %s @0x%02x (WHO_AM_I 0x%02x): accel %d %d %d  -> "
		"%d %d %d mg", part, IMU_ADDR, who, ax, ay, az,
		(ax * 2000) / 32768, (ay * 2000) / 32768, (az * 2000) / 32768);
}
#endif /* CONFIG_RING_BENCH */

int gsr_stream_start(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	int err;

	if (!adc_is_ready_dt(&gsr_adc)) {
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&gsr_adc);
	if (err) {
		return err;
	}

	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_ACTIVE);

	/*
	 * The one settle for the whole stream. Measured at 0.5-1 s on
	 * hardware: the output starts near 800 mV and decays to V_REF as
	 * C14, C4 and C10 charge. Sampling before that returns the tail of
	 * a power-on transient, which would look exactly like a large skin
	 * conductance response at the start of every recording.
	 */
	k_msleep(800);

	/*
	 * Claim GSR_PWR so selftest_gsr_mv(), which slow_sense() calls every
	 * 30 s, stops powering the front end down when it finishes. Without
	 * this the stream gets a dip at 30 s intervals that is
	 * indistinguishable from a real event -- the same trap the bench
	 * monitor documented.
	 */
	gsr_pwr_owned = true;

	LOG_INF("GSR stream started (GSR_PWR held on, duty cycling suspended)");

	return 0;
}

void gsr_stream_stop(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	gsr_pwr_owned = false;
	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_INACTIVE);

	LOG_INF("GSR stream stopped");
}

int gsr_stream_raw(int16_t *raw)
{
	int16_t sample = 0;
	int err;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	if (raw == NULL) {
		return -EINVAL;
	}

	err = adc_sequence_init_dt(&gsr_adc, &seq);
	if (err == 0) {
		err = adc_read_dt(&gsr_adc, &seq);
	}
	if (err) {
		return err;
	}

	*raw = sample;

	return 0;
}

int selftest_gsr_mv(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	int16_t sample = 0;
	int32_t mv;
	int err;

	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	if (!adc_is_ready_dt(&gsr_adc)) {
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&gsr_adc);
	if (err) {
		return err;
	}

	/*
	 * Power the electrode divider only while measuring.
	 *
	 * The settle time is ~0.5-1 s, not 10 ms: measured on hardware, the
	 * output starts near 800 mV and decays to V_REF (500 mV) over several
	 * hundred ms as C14, C4 and C10 charge. The original 10 ms was far
	 * too short and would have read the tail of that transient -- masked
	 * until now by the vref-mv bug, which made every conversion 0 anyway.
	 */
	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_ACTIVE);
	if (!gsr_pwr_owned) {
		k_msleep(800);
	}

	err = adc_sequence_init_dt(&gsr_adc, &seq);
	if (err == 0) {
		err = adc_read_dt(&gsr_adc, &seq);
	}

	if (!gsr_pwr_owned) {
		gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_INACTIVE);
	}

	if (err) {
		return err;
	}

	mv = sample;
	err = adc_raw_to_millivolts_dt(&gsr_adc, &mv);
	if (err) {
		return err;
	}

	return (int)mv;
}

#if defined(CONFIG_RING_BENCH)
#if defined(CONFIG_RING_GSR_MONITOR)
/*
 * Skin conductance in tenths of a microsiemens.
 *
 * Conductance, not resistance, is the conventional EDA unit, and it is what
 * this topology produces directly. Returns 0 below the bias point, which is
 * where an open circuit and amplifier offset both land.
 *
 * Guarded on GSR_MONITOR rather than BENCH because selftest_gsr_monitor() is
 * its only caller. Under the wider guard, the perfectly legal combination
 * RING_BENCH=y + RING_GSR_MONITOR=n left it defined and unused, which is a
 * -Wunused-function warning -- and warnings are the only verification this
 * project has while there is no hardware.
 */
static int gsr_conductance_us_x10(int mv)
{
	if (mv <= GSR_VREF_MV) {
		return 0;
	}

	/* (dV/VREF)/R5 in uS, x10. Numerator peaks near 1.3e7: fits int32. */
	return ((mv - GSR_VREF_MV) * 10000) /
	       ((GSR_VREF_MV * (GSR_R5_OHMS / 100)) / 10);
}
#endif /* CONFIG_RING_GSR_MONITOR */

/* Sample AIN1 once, assuming GSR_PWR is already on and settled. */
static int gsr_sample_mv(void)
{
	int16_t sample = 0;
	int32_t mv;
	int err;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	err = adc_sequence_init_dt(&gsr_adc, &seq);
	if (err == 0) {
		err = adc_read_dt(&gsr_adc, &seq);
	}
	if (err) {
		return err;
	}

	mv = sample;
	err = adc_raw_to_millivolts_dt(&gsr_adc, &mv);

	return err ? err : (int)mv;
}

/*
 * How long does the GSR front end actually take to settle?
 *
 * selftest_gsr_mv() waits a fixed 10 ms. That is a guess, and the analog
 * path has two time constants that could be far longer: C4 sits across the
 * feedback resistor R5, and R6/C10 low-pass the output into AIN1. If the
 * reading is still climbing at 10 ms then every GSR number recorded so far
 * was taken before the amplifier had settled, and "0 mV" means nothing.
 *
 * Expected topology (from the netlist): U7B is a transimpedance stage with
 * IN+ held at V_REF_05 (R3/R4 divider off GSR_PWR, buffered by U7A), the
 * skin path from U8 to GND at U2, and R5 as feedback. So
 *
 *     V_OUT_GSR = V_REF x (1 + R5 / R_skin)
 *
 * With the electrodes OPEN the output should rest at V_REF -- roughly half
 * of GSR_PWR, so ~900 mV off the 1V8 rail, NOT 0 mV. A settled reading near
 * zero would mean the front end is unpowered or dead, not merely open.
 */
/*
 * Where does the GSR chain die?
 *
 * The settle sweep reads a hard 0 mV, but the front end should idle at
 * V_REF (~half of GSR_PWR) with the electrodes open. V_REF = 0 explains a
 * flat zero regardless of skin resistance, so the question is whether
 * GSR_PWR ever reaches U7 at all.
 *
 * Read the pins directly rather than trusting the drivers:
 *
 *   GSR_PWR (P0.11) driven high, read back through the input buffer.
 *     0 -> the net is loaded or shorted; U7 is unpowered, and nothing
 *          downstream can work
 *     1 -> the rail is fine and the fault is the divider, U7, or the
 *          ADC node
 *
 *   GSR_ADC (P0.03) floated with an internal pull-up, then a pull-down.
 *     1/0 -> node is free, so the 0 mV is the op-amp actively driving low
 *     0/0 -> the ADC node itself is held down (C10 shorted, or a bridge)
 *
 * nRF52833 P0: PIN_CNF[n] at 0x50000700 + 4n, OUT set/clr at 0x508/0x50C,
 * IN at 0x510.
 */
/*
 * Which analog input actually lands on P0.03?
 *
 * The front end is measured good -- V_OUT_GSR sits at V_REF (0.5 V) with the
 * electrodes open, exactly as designed -- and the ADC used to return 0.
 *
 * The first suspicion was an off-by-one, the devicetree constants being
 * zero-based (NRF_SAADC_AIN0 = 0) while the hardware PSELP field is
 * one-based with 0 meaning "not connected". Sweeping every input settles
 * that without more datasheet archaeology: whichever index reports ~500 mV
 * is the one wired to P0.03.
 *
 * Read the result against the mundane explanation, which is that the node
 * had no zephyr,vref-mv and adc_raw_to_millivolts_dt() therefore multiplied
 * every sample by zero. That alone accounts for every 0 mV reading in this
 * project's history, and it is already fixed.
 *
 * Raw counts scale as VDD/4 reference with gain 1/4, so full scale is
 * VDD = 1800 mV over 12 bits.
 */
static void gsr_ain_scan(void)
{
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc));
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	char line[224];
	int n = 0;

	if (!device_is_ready(adc)) {
		LOG_ERR("GSR   ADC not ready");
		return;
	}

	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_ACTIVE);
	k_msleep(50);

	for (uint8_t ain = 0; ain <= 7 && n < (int)sizeof(line) - 28; ain++) {
		struct adc_channel_cfg cfg = {
			.gain = ADC_GAIN_1_4,
			.reference = ADC_REF_VDD_1_4,
			.acquisition_time =
				ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
			.channel_id = 0,
			.input_positive = ain,
		};
		int16_t sample = 0;
		struct adc_sequence seq = {
			.channels = BIT(0),
			.buffer = &sample,
			.buffer_size = sizeof(sample),
			.resolution = 12,
		};

		if (adc_channel_setup(adc, &cfg) != 0 ||
		    adc_read(adc, &seq) != 0) {
			n += snprintk(&line[n], sizeof(line) - n, " %u=err",
				      ain);
			continue;
		}

		n += snprintk(&line[n], sizeof(line) - n, " %u=%dmV", ain,
			      (sample * 1800) / 4096);
	}

	LOG_INF("GSR   AIN scan (index=mV, expect ~500 on the P0.03 one):%s",
		line);

	/*
	 * The scan above reads this pin fine on a hand-built config, so the
	 * hardware and the AIN index are both right. Print what
	 * ADC_DT_SPEC_GET_BY_IDX actually produced rather than assuming it
	 * matches the devicetree; after the channel move ch_id=0 is what
	 * should show up.
	 */
	/*
	 * A/B the remaining variable: the channel index.
	 *
	 * The scan above reads AIN1 correctly on channel 0. The devicetree
	 * path was byte-for-byte identical except that it sat on channel 1,
	 * and it returned 0 -- but it also had no zephyr,vref-mv at the time,
	 * which by itself forces every conversion to 0 mV. So the index was
	 * never shown to be at fault; it was the last untested difference.
	 *
	 * Nothing in the driver supports the index theory either:
	 * adc_nrfx_saadc.c rejects a channel_id only when it is >=
	 * SAADC_CH_NUM, and treats channel 1 exactly like channel 0. The
	 * likeliest outcome of this sweep is therefore that every index reads
	 * about the same ~500 mV, which would mean the index was never the
	 * fault and vref-mv was the whole of it.
	 *
	 * THE MOVE HAS BEEN MADE AND IS HARMLESS: the devicetree node is now
	 * channel@0 with reg = <0> and io-channels = <&adc 0>, input still
	 * NRF_SAADC_AIN1. It is not being reverted. This sweep stays because
	 * it is what decides the question, and the line
	 *
	 *     GSR   channel A/B, input fixed at AIN1: ch0=<n>mV ch1=... ...
	 *
	 * is the log to read: ch0 and ch1 both near 500 mV means the index was
	 * irrelevant, ch0 near 500 mV with ch1 at 0 mV would be the surprise
	 * and would make the move the fix after all.
	 */
	n = 0;
	for (uint8_t ch = 0; ch < 4 && n < (int)sizeof(line) - 24; ch++) {
		struct adc_channel_cfg cfg = {
			.gain = ADC_GAIN_1_4,
			.reference = ADC_REF_VDD_1_4,
			.acquisition_time =
				ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
			.channel_id = ch,
			.input_positive = 1,	/* AIN1 = P0.03, fixed */
		};
		int16_t sample = 0;
		struct adc_sequence seq = {
			.channels = BIT(ch),
			.buffer = &sample,
			.buffer_size = sizeof(sample),
			.resolution = 12,
		};
		int rc_s = adc_channel_setup(adc, &cfg);
		int rc_r = rc_s ? rc_s : adc_read(adc, &seq);

		if (rc_r) {
			n += snprintk(&line[n], sizeof(line) - n, " ch%u=err%d",
				      ch, rc_r);
			continue;
		}
		n += snprintk(&line[n], sizeof(line) - n, " ch%u=%dmV", ch,
			      (sample * 1800) / 4096);
	}

	LOG_INF("GSR   channel A/B, input fixed at AIN1:%s", line);

	/*
	 * Same pin, same channel, same settings -- one path used to read it
	 * while the other returned 0. Run them back to back and print the
	 * sequence the DT helper builds, including the fields the spec dump
	 * does not show (oversampling, calibrate) and the raw counts before
	 * conversion.
	 *
	 * The hand-rolled config below tracks the devicetree, so it moved from
	 * channel 1 to channel 0 with it -- the point of this block is to hold
	 * everything equal and vary only the API, and the channel index itself
	 * is A/B'd by the sweep above. After the move both raw counts should
	 * be non-zero and within a few LSB of each other; a dt raw of 0 beside
	 * a non-zero hand raw would mean the move did not take.
	 */
	{
		int16_t s_dt = 0, s_hand = 0;
		struct adc_sequence q_dt = {
			.buffer = &s_dt,
			.buffer_size = sizeof(s_dt),
		};
		struct adc_channel_cfg c_hand = {
			.gain = ADC_GAIN_1_4,
			.reference = ADC_REF_VDD_1_4,
			.acquisition_time =
				ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
			.channel_id = 0,
			.input_positive = 1,
		};
		struct adc_sequence q_hand = {
			.channels = BIT(0),
			.buffer = &s_hand,
			.buffer_size = sizeof(s_hand),
			.resolution = 12,
		};
		int e1 = adc_channel_setup_dt(&gsr_adc);
		int e2 = adc_sequence_init_dt(&gsr_adc, &q_dt);
		int e3 = adc_read_dt(&gsr_adc, &q_dt);
		int e4 = adc_channel_setup(adc, &c_hand);
		int e5 = adc_read(adc, &q_hand);

		LOG_INF("GSR   path A/B: dt raw=%d err=%d/%d/%d "
			"seq{ch=0x%x res=%u ovs=%u cal=%u} || hand raw=%d "
			"err=%d/%d", s_dt, e1, e2, e3, q_dt.channels,
			q_dt.resolution, q_dt.oversampling, q_dt.calibrate,
			s_hand, e4, e5);
	}

	LOG_INF("GSR   dt spec: ch_id=%u input_p=%u input_n=%u diff=%u "
		"gain=%u ref=%u acq=%u res=%u",
		gsr_adc.channel_id, gsr_adc.channel_cfg.input_positive,
		gsr_adc.channel_cfg.input_negative,
		gsr_adc.channel_cfg.differential, gsr_adc.channel_cfg.gain,
		gsr_adc.channel_cfg.reference,
		gsr_adc.channel_cfg.acquisition_time, gsr_adc.resolution);
}

static void gsr_pin_probe(void)
{
	volatile uint32_t *const cnf = (volatile uint32_t *)0x50000700UL;
	volatile uint32_t *const outset = (volatile uint32_t *)0x50000508UL;
	volatile uint32_t *const outclr = (volatile uint32_t *)0x5000050CUL;
	volatile uint32_t *const in = (volatile uint32_t *)0x50000510UL;
	int pwr_hi, pwr_lo, adc_pu, adc_pd, adc_drv;

	/* GSR_PWR: output, input buffer connected so the readback is real. */
	cnf[GSR_PWR_PIN] = 0x00000001UL;

	*outset = BIT(GSR_PWR_PIN);
	k_msleep(5);
	pwr_hi = (*in >> GSR_PWR_PIN) & 1U;

	*outclr = BIT(GSR_PWR_PIN);
	k_msleep(5);
	pwr_lo = (*in >> GSR_PWR_PIN) & 1U;

	/* Put it back high so the front end is powered for the ADC probe. */
	*outset = BIT(GSR_PWR_PIN);
	k_msleep(20);

	/* GSR_ADC = P0.03 = AIN1. Float it and see what holds it. */
	cnf[GSR_ADC_PIN] = 0x0000000CUL;	/* input, pull-up */
	k_msleep(5);
	adc_pu = (*in >> GSR_ADC_PIN) & 1U;

	cnf[GSR_ADC_PIN] = 0x00000004UL;	/* input, pull-down */
	k_msleep(5);
	adc_pd = (*in >> GSR_ADC_PIN) & 1U;

	/*
	 * Short to GND, or the op-amp holding it? Both read 0 above.
	 *
	 * Drive the node high and see who wins. R6 sits between V_OUT_GSR and
	 * this pin, so an op-amp output at 0 V is reached through that series
	 * resistance and the MCU should overcome it. A solder bridge straight
	 * to GND has no such resistance and will hold the pin down.
	 *
	 *   reads 1 -> not a hard short; series impedance, i.e. the op-amp
	 *              side is what is at 0 V. Look at U7, R3/R4, V_REF.
	 *   reads 0 -> the GSR_ADC net itself is shorted. Look at U5.19, R6,
	 *              and C10 (a shorted C10 does exactly this).
	 *
	 * Standard drive (S0S1), 1 ms. The nRF52833 limits this to a few mA,
	 * so it is safe even if the node really is shorted to ground.
	 */
	cnf[GSR_ADC_PIN] = 0x00000001UL;	/* output, input buffer on */
	*outset = BIT(GSR_ADC_PIN);
	k_busy_wait(1000);
	adc_drv = (*in >> GSR_ADC_PIN) & 1U;
	*outclr = BIT(GSR_ADC_PIN);

	cnf[GSR_ADC_PIN] = 0x00000002UL;	/* back to analog: input buf off */

	LOG_INF("GSR   GSR_PWR(P0.11) driven hi=%d lo=%d -> %s", pwr_hi, pwr_lo,
		pwr_hi ? "rail reaches the pin"
		       : "STUCK LOW -- U7 never gets power");
	LOG_INF("GSR   GSR_ADC(P0.03) pull-up=%d pull-down=%d -> %s", adc_pu,
		adc_pd,
		(adc_pu && !adc_pd) ? "node free (op-amp not driving it)"
		: (!adc_pu && !adc_pd) ? "node HELD LOW -- bridge or C10 short"
				       : "driven high");
	LOG_INF("GSR   GSR_ADC forced high: reads %d -> %s", adc_drv,
		adc_drv ? "series impedance, not a short: fault is U7 / R3 / "
			  "R4 / V_REF side"
			: "HARD SHORT on the GSR_ADC net: U5.19, R6 or C10");
}

static void gsr_settle_sweep(void)
{
	static const uint32_t step_ms[] = { 1, 4, 5, 15, 25, 50, 150, 250,
					    500, 1000 };
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	uint32_t elapsed = 0;
	char line[176];
	int n = 0;

	/*
	 * Start genuinely cold. An earlier version of this sweep ran after
	 * gsr_pin_probe() had been driving P0.03 around, and read 0 at every
	 * point -- the diagnostic was measuring its own side effects. Drop
	 * GSR_PWR and let the front end fully discharge first.
	 */
	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_INACTIVE);
	k_msleep(300);

	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_ACTIVE);

	for (int i = 0; i < (int)ARRAY_SIZE(step_ms) && n < (int)sizeof(line) - 24;
	     i++) {
		int mv;

		k_msleep(step_ms[i]);
		elapsed += step_ms[i];

		/*
		 * Re-run channel setup before every read. gsr_sample_mv()
		 * relies on some earlier caller having configured the channel,
		 * and that assumption is exactly what is in question here.
		 */
		(void)adc_channel_setup_dt(&gsr_adc);

		mv = gsr_sample_mv();
		n += snprintk(&line[n], sizeof(line) - n, " %u=%d", elapsed,
			      mv);
	}

	/* Confirm GSR_PWR really is asserted for the whole sweep. */
	{
		uint32_t cnf = *(volatile uint32_t *)(0x50000700UL +
						      4UL * GSR_PWR_PIN);
		uint32_t out = *(volatile uint32_t *)0x50000504UL;

		LOG_INF("GSR   during sweep: GSR_PWR PIN_CNF=0x%08x OUT=%u",
			cnf, (out >> GSR_PWR_PIN) & 1U);
	}

	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_INACTIVE);

	LOG_INF("GSR   settle sweep (ms=mV):%s", line);
}

static void test_gsr(void)
{
	int mv = selftest_gsr_mv();

	if (mv < 0) {
		LOG_ERR("GSR   read failed (%d)", mv);
		return;
	}

	LOG_INF("GSR   AIN1 (P0.03) = %d mV with GSR_PWR on%s", mv,
		mv < 50 ? "  -- near zero; open electrodes should idle at "
			  "V_REF (~500 mV)" : "");

	gsr_settle_sweep();
	gsr_ain_scan();
	gsr_pin_probe();	/* last: it drives P0.03 */
}
#endif /* CONFIG_RING_BENCH */

#if defined(CONFIG_RING_GSR_MONITOR)
/*
 * Live GSR readout for bench work.
 *
 * slow_sense() only runs outside RING_CHARGING, so on a bench charger the
 * GSR is never re-read after boot -- you cannot watch a bridge take effect.
 * This samples in every state and holds GSR_PWR on between reads so the
 * front end stays settled, which the pulsed path in selftest_gsr_mv() does
 * not do.
 */
void selftest_gsr_monitor(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	static int64_t next_at;
	static int last_mv = -30000;
	int64_t now = k_uptime_get();
	int mv;

	if (now < next_at) {
		return;
	}
	next_at = now + 500;

	if (!adc_is_ready_dt(&gsr_adc) || adc_channel_setup_dt(&gsr_adc) != 0) {
		return;
	}

	gpio_pin_configure(gpio0, GSR_PWR_PIN, GPIO_OUTPUT_ACTIVE);
	gsr_pwr_owned = true;

	mv = gsr_sample_mv();
	if (mv < 0) {
		return;
	}

	/*
	 * Log on any real movement, and heartbeat every few seconds even when
	 * flat. Silence is ambiguous on the bench -- it reads the same as a
	 * dead monitor -- and the heartbeat also shows the noise floor, which
	 * is what tells you whether a small change is signal.
	 */
	{
		static int64_t next_beat;
		bool moved = (last_mv < -20000) || (mv - last_mv > 5) ||
			     (last_mv - mv > 5);

		if (moved || now >= next_beat) {
			int us_x10 = gsr_conductance_us_x10(mv);

			/*
			 * Below the bias point is not a small reading, it is an
			 * impossible one. A passive skin path can only pull
			 * current OUT of the summing node, which drives V_OUT
			 * up. Going down means current is being injected --
			 * a floating body with no ground-electrode contact, or
			 * an exposed conductor above V_REF near the pads.
			 * Reporting it as 0.0 uS hides that, so say it.
			 */
			if (mv < GSR_VREF_MV - 20) {
				LOG_WRN("GSR   %d mV  BELOW BIAS by %d mV -- "
					"current injected, not skin", mv,
					GSR_VREF_MV - mv);
			} else {
				LOG_INF("GSR   %d mV  %d.%d uS%s", mv,
					us_x10 / 10, us_x10 % 10,
					moved ? "" : "   (idle)");
			}
			last_mv = mv;
			next_beat = now + 3000;
		}
	}
}
#endif /* CONFIG_RING_GSR_MONITOR */

#if defined(CONFIG_RING_BENCH)
static void test_pins(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	int cd, acc_int;

	/* CD is left driven high by the PMIC test -- that is the correct
	 * battery-only operating state, so read it back rather than
	 * reconfiguring it as an input.
	 */
	gpio_pin_configure(gpio0, ACC_INT_PIN, GPIO_INPUT);

	cd = gpio_pin_get_raw(gpio1, CD_PIN);
	acc_int = gpio_pin_get_raw(gpio0, ACC_INT_PIN);

	LOG_INF("PINS  CD (P1.00) = %d [%s],  ACC_INT (P0.16) = %d",
		cd, cd ? "Active Battery, I2C enabled" : "Hi-Z, PMIC I2C OFF",
		acc_int);
}
#endif /* CONFIG_RING_BENCH */

/*
 * CD has to move with the charger, and the two states conflict:
 *
 *   battery only : CD low  -> Hi-Z, and the PMIC's I2C is switched off
 *   charger in   : CD high -> charging disabled
 *
 * Since CD high always keeps I2C alive, the poll is: raise CD, ask whether
 * VIN is valid, then park CD wherever that answer requires. Raising CD
 * briefly interrupts charging, which at this interval is negligible.
 */
int pmic_service(const struct device *i2c, bool *charging)
{
	const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	static int last_state = -1;
	uint8_t faults = 0;
	int err;

	if (charging) {
		*charging = false;
	}

	gpio_pin_configure(gpio1, CD_PIN, GPIO_OUTPUT_ACTIVE);
	k_msleep(5);

	err = reg_read(i2c, PMIC_ADDR, PMIC_REG_FAULTS, &faults, 1);
	if (err) {
		/*
		 * Leave CD high so I2C at least stays alive, and report the
		 * error rather than "not charging".
		 *
		 * These two used to be the same answer: this function returned
		 * bool, so a dead bus and a healthy battery-powered board were
		 * indistinguishable, and main.c set RING_FLAG_PMIC_OK either
		 * way. The app was told the PMIC was fine while I2C was down.
		 */
		if (last_state != -2) {
			LOG_WRN("PMIC  unreachable (%d)", err);
			last_state = -2;
		}
		return err;
	}

	bool charger = (faults & BIT(6)) == 0;

	if (charger) {
		gpio_pin_configure(gpio1, CD_PIN, GPIO_OUTPUT_INACTIVE);
	}

	if (last_state != (int)charger) {
		uint8_t status = 0;

		k_msleep(50);
		(void)reg_read(i2c, PMIC_ADDR, PMIC_REG_STATUS, &status, 1);
		LOG_INF("PMIC  %s (status 0x%02x)",
			charger ? "charger attached, charging enabled"
				: "on battery, CD high for I2C",
			status);
		last_state = (int)charger;
	}

	if (charging) {
		*charging = charger;
	}

	return 0;
}

#if defined(CONFIG_RING_BENCH)
void selftest_run(const struct device *i2c)
{
	LOG_INF("---- board self-test ----");
	test_pmic(i2c);
	test_battery(i2c);
	test_imu(i2c);
	test_gsr();
	test_pins();
	LOG_INF("---- self-test done ----");
}
#endif /* CONFIG_RING_BENCH */
