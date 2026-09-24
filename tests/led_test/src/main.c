/*
 * MAXM86161 LED bring-up test, 2026-09-23 board.
 *
 * Standalone and deliberately dumb: no driver, no state machine, no BLE.
 * Every register write is read back and every errno is logged, because the
 * bench image writes the LED registers with (void) and so cannot tell a
 * failed write from a dark LED.
 *
 * Pointer-first I2C only -- never a bare address read (POSTMORTEM.md).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ledtest, LOG_LEVEL_INF);

#define MAX_ADDR	0x62
#define PMIC_ADDR	0x6a

#define BOOST_EN_PIN	12	/* U5.15 = P0.12 */
#define CD_PIN		0	/* U5.16 = P1.00 on the 2026-09-23 board (was P1.09) */

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

static int rd(uint8_t addr, uint8_t reg, uint8_t *v)
{
	return i2c_write_read(i2c, addr, &reg, 1, v, 1);
}

static int wr(uint8_t addr, uint8_t reg, uint8_t v)
{
	uint8_t b[2] = { reg, v };

	return i2c_write(i2c, b, sizeof(b), addr);
}

/* Write, read back, log both errnos. Returns 1 on any failure. */
static int wv(uint8_t reg, uint8_t val, const char *name)
{
	uint8_t back = 0xEE;
	int ew = wr(MAX_ADDR, reg, val);
	int er = rd(MAX_ADDR, reg, &back);

	if (ew == 0 && er == 0 && back == val) {
		LOG_INF("  %-13s 0x%02x = 0x%02x  ok", name, reg, val);
		return 0;
	}

	LOG_ERR("  %-13s 0x%02x wrote 0x%02x (err %d) read back 0x%02x "
		"(err %d)  MISMATCH", name, reg, val, ew, back, er);
	return 1;
}

static void show_activity(void)
{
	uint8_t wp = 0, ovf = 0, cnt = 0, s1 = 0, f[3] = { 0 };
	uint8_t reg = 0x08;
	int e1 = rd(MAX_ADDR, 0x04, &wp);
	int e2 = rd(MAX_ADDR, 0x06, &ovf);
	int e3 = rd(MAX_ADDR, 0x07, &cnt);
	int e4 = rd(MAX_ADDR, 0x00, &s1);
	int e5 = i2c_write_read(i2c, MAX_ADDR, &reg, 1, f, 3);
	uint32_t raw = ((uint32_t)f[0] << 16) | ((uint32_t)f[1] << 8) | f[2];

	/*
	 * LED_COMPB (INT_STATUS_1 bit 3) latches at the end of any sample in
	 * which the LED driver could not deliver its programmed current --
	 * an open or dead LED, a missing PGND joint, or no headroom on VLED.
	 * It only latches because LED_COMPB_EN is set below. This is the
	 * chip's own answer to "is current flowing through the LED".
	 */
	LOG_INF("    sampling: WR_PTR %3u  count %3u  OVF %3u  INT1 0x%02x  "
		"LED_COMPB=%d %s  sample tag %u value %u  (errs %d %d %d %d %d)",
		wp, cnt, ovf, s1, (s1 >> 3) & 1,
		(s1 & BIT(3)) ? "<-- LED DRIVER NOT COMPLIANT: no LED current"
			      : "(LED current OK)",
		raw >> 19, raw & 0x7FFFF, e1, e2, e3, e4, e5);
}

int main(void)
{
	volatile uint32_t *const resetreas = (volatile uint32_t *)0x40000400UL;
	uint32_t rr = *resetreas;

	*resetreas = rr;	/* write-1-to-clear */

	LOG_INF("==== MAXM86161 LED test ====");
	LOG_INF("RESETREAS 0x%08x  (bit0 pin, 1 watchdog, 2 soft, 3 lockup, "
		"16 system-off wake, 18 debug)", rr);

	if (!device_is_ready(i2c) || !device_is_ready(gpio0) ||
	    !device_is_ready(gpio1)) {
		LOG_ERR("i2c/gpio not ready");
		return 0;
	}

	/* CD high keeps the PMIC out of Hi-Z so its I2C answers. */
	gpio_pin_configure(gpio1, CD_PIN, GPIO_OUTPUT_HIGH);
	k_msleep(10);

	uint8_t st = 0, fl = 0;
	int ep1 = rd(PMIC_ADDR, 0x00, &st);
	int ep2 = rd(PMIC_ADDR, 0x01, &fl);

	LOG_INF("PMIC  CD=P1.00 high: status 0x%02x (err %d)  faults 0x%02x "
		"(err %d)", st, ep1, fl, ep2);

	/* Boost on, input buffer connected so the readback is the real pin. */
	gpio_pin_configure(gpio0, BOOST_EN_PIN, GPIO_OUTPUT_HIGH | GPIO_INPUT);
	k_msleep(50);
	LOG_INF("BOOST_EN P0.12 driven high, pin reads %d",
		gpio_pin_get_raw(gpio0, BOOST_EN_PIN));

	uint8_t id = 0, rev = 0;
	int e = -1;

	for (int i = 1; i <= 10; i++) {
		e = rd(MAX_ADDR, 0xFF, &id);
		if (e == 0 && id == 0x36) {
			break;
		}
		LOG_WRN("PART_ID try %d: err %d id 0x%02x", i, e, id);
		k_msleep(100);
	}
	if (e != 0 || id != 0x36) {
		LOG_ERR("MAXM86161 not found -- stopping");
		return 0;
	}
	(void)rd(MAX_ADDR, 0xFE, &rev);
	LOG_INF("MAXM86161 at 0x62: PART_ID 0x%02x REV 0x%02x", id, rev);

	int er = wr(MAX_ADDR, 0x0D, 0x01);	/* RESET, self-clears */
	uint8_t s1 = 0;

	k_msleep(10);
	(void)rd(MAX_ADDR, 0x00, &s1);
	LOG_INF("soft reset (err %d), INT_STATUS_1 0x%02x (bit0 = PWR_RDY)",
		er, s1);

	static const struct {
		uint8_t ledc;
		uint8_t pa_reg;
		const char *name;
	} leds[] = {
		{ 1, 0x23, "LED1 GREEN" },
		{ 3, 0x25, "LED3 RED" },
		{ 2, 0x24, "LED2 IR (invisible; a phone camera shows it)" },
		/* LED1 slot at 0 mA: the dark baseline. With a finger on
		 * the sensor, a working LED reads far higher than this.
		 */
		{ 1, 0x00, "DARK baseline (LED1 slot, 0 mA)" },
	};

	for (;;) {
		for (size_t n = 0; n < ARRAY_SIZE(leds); n++) {
			int bad = 0;

			LOG_INF("---- %s for 4 s ----", leds[n].name);
			bad |= wv(0x0D, 0x02, "SYS_CTRL shdn");
			bad |= wv(0x11, (2 << 2) | 3, "PPG_CONFIG_1");
			bad |= wv(0x12, (0x11 << 3), "PPG_CONFIG_2");
			bad |= wv(0x13, (1 << 6), "PPG_CONFIG_3");
			bad |= wv(0x15, 0x01, "PD_BIAS");
			/* LED_COMPB_EN only -- lets the compliance flag latch */
			bad |= wv(0x02, 0x08, "INT_ENABLE_1");
			/* 124 mA range on all three drivers */
			bad |= wv(0x2A, 0x3F, "LED_RANGE_1");
			/* 0x80 x 0.48 mA = ~61 mA peak, ~7 mA average */
			bad |= wv(0x23, leds[n].pa_reg == 0x23 ? 0x80 : 0, "LED1_PA");
			bad |= wv(0x24, leds[n].pa_reg == 0x24 ? 0x80 : 0, "LED2_PA");
			bad |= wv(0x25, leds[n].pa_reg == 0x25 ? 0x80 : 0, "LED3_PA");
			bad |= wv(0x09, 0x0F, "FIFO_CONFIG_1");
			/* STAT_CLR | A_FULL_TYPE | FIFO_RO, then flush */
			bad |= wv(0x0A, 0x0E, "FIFO_CONFIG_2");
			(void)wr(MAX_ADDR, 0x0A, 0x1E);
			bad |= wv(0x20, leds[n].ledc, "LED_SEQ_1");
			bad |= wv(0x21, 0x00, "LED_SEQ_2");
			bad |= wv(0x22, 0x00, "LED_SEQ_3");
			bad |= wv(0x0D, 0x00, "SYS_CTRL run");

			if (bad) {
				LOG_ERR("  configuration had FAILURES, see above");
			} else {
				LOG_INF("  every register verified -- %s",
					leds[n].pa_reg ? "LED should be ON now"
						       : "all LEDs at 0 mA");
			}

			for (int t = 0; t < 4; t++) {
				k_msleep(1000);
				show_activity();
			}

			(void)wr(MAX_ADDR, 0x0D, 0x02);
			k_msleep(500);
		}
	}

	return 0;
}
