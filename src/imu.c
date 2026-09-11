#include "imu.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#define IMU_ADDR		0x6b

#define REG_FUNC_CFG_ACCESS	0x01
#define REG_IF_CFG		0x03
#define REG_INT1_CTRL		0x0d
#define REG_WHO_AM_I		0x0f
#define REG_CTRL1		0x10
#define REG_CTRL3		0x12
#define REG_ALL_INT_SRC		0x1d
/*
 * WAKE_UP_SRC is at 45h on the LSM6DSV family. 1Bh is the LSM6DSO/DSL
 * address and lands in the OIS/reserved block here -- it reads without
 * error and tells you nothing, so the bug is silent. Verified against
 * LSM6DSV DS13476 Rev 2 section 9.43.
 */
#define REG_WAKE_UP_SRC		0x45
#define REG_OUTX_L_A		0x28
#define REG_FUNCTIONS_ENABLE	0x50
#define REG_INACTIVITY_DUR	0x54
#define REG_TAP_CFG0		0x56
#define REG_WAKE_UP_THS		0x5b
#define REG_WAKE_UP_DUR		0x5c
#define REG_MD1_CFG		0x5e

#define WHO_LSM6DSV		0x70
#define WHO_LSM6DSV16BX		0x71

/* CTRL1: OP_MODE_XL 000 (high performance) | ODR_XL 0110 (120 Hz) */
#define CTRL1_120HZ_HP		0x06

#define FUNC_INTERRUPTS_ENABLE	BIT(7)
#define IF_CFG_H_LACTIVE	BIT(5)
#define IF_CFG_PP_OD		BIT(4)
#define WAKE_UP_SRC_WU_IA	BIT(3)
#define TAP_CFG0_LIR		BIT(0)
#define MD1_CFG_INT1_WU		BIT(5)

/*
 * Wake-path diagnostic. Reads the latched source registers and pulses
 * INT1, so it perturbs the very interrupt path the firmware depends on.
 * See POSTMORTEM.md rule 3.
 *
 * Controlled from Kconfig (CONFIG_RING_IMU_WAKE_DIAG, under RING_BENCH)
 * rather than by editing this line. It used to be a hand-edited 1/0 here
 * while BENCH_POWER_LED was hand-edited in main.c, and keeping two files
 * in step by hand is how a bench instrument ends up in a shipped image.
 */
#if defined(CONFIG_RING_IMU_WAKE_DIAG)
#define IMU_WAKE_DIAG		1
#else
#define IMU_WAKE_DIAG		0
#endif

/* ACC_INT = U5.36 = module GPIO_36 = P0.16 */
#define ACC_INT_PIN		16

/*
 * WAKE_UP_THS resolution is set by WU_INACT_THS_W in INACTIVITY_DUR (54h),
 * NOT by the accelerometer full scale. Default 000 is 7.8125 mg/LSB. This
 * code writes that value explicitly so the constant below cannot drift out
 * of sync with the register.
 */
#define REG_INACTIVITY_DUR_VAL	0x00
#define WK_THS_LSB_MG		8

static const struct device *imu_i2c;
static struct gpio_callback int1_cb;
static atomic_t wake_flag;

static int rd(uint8_t reg, uint8_t *buf, size_t len)
{
	/* Pointer-first always -- see POSTMORTEM.md */
	return i2c_write_read(imu_i2c, IMU_ADDR, &reg, 1, buf, len);
}

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };

	return i2c_write(imu_i2c, buf, sizeof(buf), IMU_ADDR);
}

static void int1_handler(const struct device *port, struct gpio_callback *cb,
			 gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	atomic_set(&wake_flag, 1);
}

int imu_init(const struct device *i2c)
{
	uint8_t who = 0;
	int err;

	imu_i2c = i2c;

	err = rd(REG_WHO_AM_I, &who, 1);
	if (err) {
		LOG_ERR("no answer at 0x%02x (%d)", IMU_ADDR, err);
		return err;
	}

	if (who != WHO_LSM6DSV && who != WHO_LSM6DSV16BX) {
		LOG_ERR("unexpected WHO_AM_I 0x%02x", who);
		return -ENODEV;
	}

	LOG_INF("%s at 0x%02x", (who == WHO_LSM6DSV16BX) ? "LSM6DSV16BX"
							 : "LSM6DSV", IMU_ADDR);

	/*
	 * CTRL3 = BOOT | BDU | 0 | 0 | 0 | IF_INC | 0 | SW_RESET
	 *
	 * BDU stops a read straddling an update. IF_INC must stay set or
	 * multi-byte reads return the same register repeatedly -- writing
	 * BDU alone silently breaks every burst read in this file.
	 */
	(void)wr(REG_CTRL3, BIT(6) | BIT(2));

	return wr(REG_CTRL1, CTRL1_120HZ_HP);
}

int imu_read_accel(struct imu_accel *out)
{
	uint8_t raw[6];
	int err = rd(REG_OUTX_L_A, raw, sizeof(raw));

	if (err) {
		return err;
	}

	out->x = (int16_t)(raw[0] | (raw[1] << 8));
	out->y = (int16_t)(raw[2] | (raw[3] << 8));
	out->z = (int16_t)(raw[4] | (raw[5] << 8));

	return 0;
}

int imu_magnitude_mg(void)
{
	struct imu_accel a;

	if (imu_read_accel(&a) != 0) {
		return -EIO;
	}

	/* |a| without floating point: sum of squares in mg, then isqrt. */
	int32_t x = (a.x * 2000) / 32768;
	int32_t y = (a.y * 2000) / 32768;
	int32_t z = (a.z * 2000) / 32768;
	uint32_t sq = (uint32_t)(x * x + y * y + z * z);
	uint32_t root = 0;
	uint32_t bit = 1u << 30;

	while (bit > sq) {
		bit >>= 2;
	}
	while (bit) {
		if (sq >= root + bit) {
			sq -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}

	return (int)root;
}

int imu_arm_wake(uint16_t threshold_mg)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	uint8_t ths = threshold_mg / WK_THS_LSB_MG;
	int err;

	if (ths == 0) {
		ths = 1;
	}
	if (ths > 0x3f) {
		ths = 0x3f;
	}

	/* Slope filter (not HPF) feeding wake-up, and latch the interrupt. */
	err = wr(REG_TAP_CFG0, TAP_CFG0_LIR);
	if (err) {
		return err;
	}

	/* Pin down the threshold weight rather than trusting the reset value. */
	(void)wr(REG_INACTIVITY_DUR, REG_INACTIVITY_DUR_VAL);

	(void)wr(REG_WAKE_UP_THS, ths);		/* WK_THS[5:0] */
	(void)wr(REG_WAKE_UP_DUR, 0x00);	/* fire on the first sample over */
	(void)wr(REG_MD1_CFG, MD1_CFG_INT1_WU);	/* route wake-up to INT1 */

	err = wr(REG_FUNCTIONS_ENABLE, FUNC_INTERRUPTS_ENABLE);
	if (err) {
		return err;
	}

	/*
	 * Read the configuration back. Every write above is fire-and-forget,
	 * and a silently dropped one looks exactly like a dead interrupt --
	 * which is precisely the failure this is here to diagnose.
	 */
	uint8_t bank = 0xff, ifcfg = 0xff;
	uint8_t v[7] = { 0 };

	/*
	 * FUNC_CFG_ACCESS first. Every other readback here is self-consistent
	 * if the part is parked in the embedded-function bank -- the writes
	 * and the reads would both go to the wrong page and agree with each
	 * other. This is the one register that can tell you that.
	 */
	(void)rd(REG_FUNC_CFG_ACCESS, &bank, 1);
	(void)rd(REG_IF_CFG, &ifcfg, 1);

	(void)rd(REG_CTRL1, &v[0], 1);
	(void)rd(REG_TAP_CFG0, &v[1], 1);
	(void)rd(REG_INACTIVITY_DUR, &v[2], 1);
	(void)rd(REG_WAKE_UP_THS, &v[3], 1);
	(void)rd(REG_WAKE_UP_DUR, &v[4], 1);
	(void)rd(REG_MD1_CFG, &v[5], 1);
	(void)rd(REG_FUNCTIONS_ENABLE, &v[6], 1);

	LOG_INF("readback BANK=0x%02x IF_CFG=0x%02x CTRL1=0x%02x "
		"TAP_CFG0=0x%02x INACT_DUR=0x%02x", bank, ifcfg, v[0], v[1],
		v[2]);
	LOG_INF("readback WAKE_THS=0x%02x WAKE_DUR=0x%02x MD1_CFG=0x%02x "
		"FUNC_EN=0x%02x", v[3], v[4], v[5], v[6]);

	if (bank != 0x00) {
		LOG_ERR("not in the main register bank (FUNC_CFG_ACCESS=0x%02x)"
			" -- every write above went to the wrong page", bank);
	}

	/*
	 * INT1 is push-pull active-high by default (IF_CFG defaults: H_LACTIVE
	 * = 0, PP_OD = 0), which is what the GPIO_INPUT / EDGE_RISING setup
	 * below assumes. Open-drain with no pull-up on ACC_INT would look
	 * exactly like a dead interrupt, so say so rather than assume.
	 */
	if (ifcfg & IF_CFG_PP_OD) {
		LOG_ERR("INT1 is open-drain (IF_CFG=0x%02x) and ACC_INT has no "
			"pull-up -- it can never drive high", ifcfg);
	}
	if (ifcfg & IF_CFG_H_LACTIVE) {
		LOG_ERR("INT1 is active-low (IF_CFG=0x%02x) but the GPIO is "
			"armed for a rising edge", ifcfg);
	}

	if ((v[5] & MD1_CFG_INT1_WU) == 0 ||
	    (v[6] & FUNC_INTERRUPTS_ENABLE) == 0) {
		LOG_ERR("wake config did not stick");
		return -EIO;
	}

	if (!device_is_ready(gpio0)) {
		return -ENODEV;
	}

	gpio_pin_configure(gpio0, ACC_INT_PIN, GPIO_INPUT);
	gpio_init_callback(&int1_cb, int1_handler, BIT(ACC_INT_PIN));
	gpio_add_callback(gpio0, &int1_cb);
	gpio_pin_interrupt_configure(gpio0, ACC_INT_PIN, GPIO_INT_EDGE_RISING);

	atomic_set(&wake_flag, 0);
	imu_clear_int();

#if IMU_WAKE_DIAG
	/*
	 * Is the ACC_INT net actually connected?
	 *
	 * INT1 is push-pull (IF_CFG PP_OD = 0, confirmed by readback above),
	 * so when idle the IMU actively drives the net LOW. An nRF internal
	 * pull-up is ~13 kOhm and loses to that driver. So:
	 *
	 *   pull-up 1, pull-down 0  nothing is driving it -- the net is OPEN
	 *                           (bad joint on U4.4 or module pad 36)
	 *   pull-up 0, pull-down 0  driven low: net intact, IMU not asserting
	 *   pull-up 1, pull-down 1  driven high: asserted right now
	 *
	 * This distinguishes a broken net from a silent IMU without a meter,
	 * which is otherwise the one thing the register readback cannot tell
	 * you.
	 */
	int pu, pd;

	gpio_pin_configure(gpio0, ACC_INT_PIN, GPIO_INPUT | GPIO_PULL_UP);
	k_busy_wait(200);
	pu = gpio_pin_get_raw(gpio0, ACC_INT_PIN);

	gpio_pin_configure(gpio0, ACC_INT_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
	k_busy_wait(200);
	pd = gpio_pin_get_raw(gpio0, ACC_INT_PIN);

	gpio_pin_configure(gpio0, ACC_INT_PIN, GPIO_INPUT);

	LOG_INF("ACC_INT net test: pull-up=%d pull-down=%d  -> %s", pu, pd,
		(pu == 1 && pd == 0) ? "OPEN, nothing driving P0.16"
		: (pu == 0 && pd == 0) ? "driven low (net intact)"
		: (pu == 1 && pd == 1) ? "driven high (asserted)"
		: "inconsistent");

	/*
	 * Can the INT1 pin driver drive high at all?
	 *
	 * The wake event is provably detected (WAKE_UP_SRC shows WU_IA) and
	 * the net is provably intact, yet the pin never leaves 0. That splits
	 * into two very different faults, and data-ready tells them apart:
	 * INT1_DRDY_XL is OR'd into the same pin as MD1_CFG and fires every
	 * accelerometer sample, so at 120 Hz the pin must toggle within a few
	 * ms if the output stage works at all.
	 *
	 *   toggles  the pin driver and the net are fine -- the fault is
	 *            specifically the wake-event -> INT1 routing, i.e. an
	 *            LSM6DSV16BX difference from the LSM6DSV register map
	 *   stuck 0  the INT1 output stage itself is disabled or unbonded,
	 *            and no amount of MD1_CFG will help
	 *
	 * INT1_CTRL is restored to 0 afterwards so this leaves no trace.
	 */
	int highs = 0;

	(void)wr(REG_INT1_CTRL, 0x01);		/* INT1_DRDY_XL */
	for (int i = 0; i < 400; i++) {
		if (gpio_pin_get_raw(gpio0, ACC_INT_PIN) == 1) {
			highs++;
		}
		k_busy_wait(100);		/* 400 x 100us = 40 ms > 4 samples */
	}
	(void)wr(REG_INT1_CTRL, 0x00);

	LOG_INF("INT1 drive test (DRDY_XL, 40 ms): %d/400 samples high -> %s",
		highs, highs ? "pin CAN drive high; wake routing is the fault"
			     : "pin STUCK LOW; INT1 output stage is the fault");

	/*
	 * Short-to-ground, or an IMU that simply never drives high?
	 *
	 * The pull-up/pull-down test above cannot tell those apart -- a net
	 * shorted to GND and a push-pull driver holding low both read 0/0.
	 * Open-drain active-low separates them: in that mode the IMU RELEASES
	 * the line when idle, so an internal pull-up is the only thing on it.
	 *
	 *   reads 1  the net is clean and the IMU can let go -- so the low was
	 *            the push-pull idle level, and the fault is that INT1 is
	 *            never asserted
	 *   reads 0  something else is holding ACC_INT down: a solder bridge
	 *            to GND, or an INT1 pad stuck low inside the part
	 *
	 * Open-drain plus a pull-up is the combination open-drain exists for,
	 * so there is no drive contention here.
	 */
	int od;

	(void)wr(REG_IF_CFG, IF_CFG_H_LACTIVE | IF_CFG_PP_OD);
	gpio_pin_configure(gpio0, ACC_INT_PIN, GPIO_INPUT | GPIO_PULL_UP);
	k_msleep(2);
	od = gpio_pin_get_raw(gpio0, ACC_INT_PIN);

	/*
	 * Prove the pull-up is actually on before blaming the board.
	 *
	 * Every "held low" conclusion above assumes Zephyr really enabled the
	 * pull-up; if it silently did not, an open net reads 0 too and looks
	 * exactly like a short. nRF52833 P0.PIN_CNF[16] is at 0x50000700 +
	 * 4*16. PULL is bits [3:2]: 0 disabled, 1 pulldown, 3 pullup.
	 * See POSTMORTEM.md rule 3 -- the diagnostic is a suspect too.
	 */
	uint32_t cnf = *(volatile uint32_t *)(0x50000700UL + 4UL * ACC_INT_PIN);

	LOG_INF("P0.16 PIN_CNF=0x%08x  DIR=%u INPUT=%u PULL=%u (3 = pull-up)",
		cnf, cnf & 1U, (cnf >> 1) & 1U, (cnf >> 2) & 3U);

	(void)wr(REG_IF_CFG, 0x00);
	gpio_pin_configure(gpio0, ACC_INT_PIN, GPIO_INPUT);

	LOG_INF("INT1 release test (open-drain + pull-up): pin=%d -> %s", od,
		od ? "net clean, IMU releases it"
		   : "ACC_INT HELD LOW externally -- short to GND or dead pad");

	/*
	 * The drive test above pulses INT1 several hundred times, and every
	 * rising edge trips the ISR -- leaving wake_flag set before the ring
	 * has moved, which would fire a spurious measurement window on the
	 * first idle pass. Clear what the diagnostic itself caused.
	 */
	atomic_set(&wake_flag, 0);
	imu_clear_int();
#endif

	LOG_INF("wake-on-motion armed: %u mg (WK_THS=%u) -> INT1 -> P0.%d",
		threshold_mg, ths, ACC_INT_PIN);

	return 0;
}

int imu_disarm_wake(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	gpio_pin_interrupt_configure(gpio0, ACC_INT_PIN, GPIO_INT_DISABLE);
	gpio_remove_callback(gpio0, &int1_cb);

	(void)wr(REG_MD1_CFG, 0x00);
	return wr(REG_FUNCTIONS_ENABLE, 0x00);
}

bool imu_take_wake_event(void)
{
	return atomic_cas(&wake_flag, 1, 0);
}

void imu_clear_int(void)
{
	uint8_t sink;

	/* Reading these clears the latched sources. */
	(void)rd(REG_ALL_INT_SRC, &sink, 1);
	(void)rd(REG_WAKE_UP_SRC, &sink, 1);
}

#if IMU_WAKE_DIAG
/*
 * Answers the one question the arm-time readback cannot: when the ring is
 * shaken, does the IMU detect the event at all, and if it does, does the
 * pin actually move?
 *
 *   pin high + WU_IA set  the interrupt works -- the GPIO edge/callback
 *                         path is what is dropping it
 *   pin low  + WU_IA set  detection works, the pin does not: IF_CFG, or a
 *                         broken ACC_INT net between U4 and P0.16
 *   pin low  + WU_IA clr  the detection chain is not triggering at all
 *
 * The pin is sampled before the source registers, because reading those
 * clears the latch and deasserts the pin.
 */
void imu_wake_diag(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	static int last_pin = -1;
	static uint8_t last_src = 0xff;
	uint8_t src = 0, all = 0;
	int pin;

	if (!device_is_ready(gpio0)) {
		return;
	}

	pin = gpio_pin_get_raw(gpio0, ACC_INT_PIN);

	(void)rd(REG_WAKE_UP_SRC, &src, 1);
	(void)rd(REG_ALL_INT_SRC, &all, 1);

	if (pin == last_pin && src == last_src) {
		return;
	}
	last_pin = pin;
	last_src = src;

	/*
	 * Peek the ISR flag without consuming it -- main.c only calls
	 * imu_take_wake_event() in RING_IDLE, so on a bench charger the whole
	 * GPIOTE -> callback path is otherwise unobservable.
	 */
	LOG_INF("wake diag: INT1 pin=%d WAKE_UP_SRC=0x%02x ALL_INT_SRC=0x%02x "
		"isr_flag=%ld%s", pin, src, all, (long)atomic_get(&wake_flag),
		(src & WAKE_UP_SRC_WU_IA) ? "  <-- WU_IA" : "");
}
#else
void imu_wake_diag(void)
{
}
#endif /* IMU_WAKE_DIAG */
