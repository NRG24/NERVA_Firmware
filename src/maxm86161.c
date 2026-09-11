#include "maxm86161.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(maxm86161, LOG_LEVEL_INF);

/*
 * Refuse to touch the bus until a probe has succeeded.
 *
 * maxm86161_probe() sets dev->addr = 0 when it finds nothing, and 0x00 is
 * the I2C GENERAL CALL address, not a harmless no-op: a write there is
 * broadcast to every device on the bus and the second byte is a command,
 * 0x06 being "reset and reload". ppg_off() runs unconditionally at boot, so
 * a board whose PPG did not answer was quietly general-calling the bus that
 * the IMU and the PMIC share.
 */
static inline bool addr_valid(const struct maxm86161 *dev)
{
	return dev->addr != 0U;
}

int maxm86161_read_reg(struct maxm86161 *dev, uint8_t reg, uint8_t *val)
{
	if (!addr_valid(dev)) {
		return -ENODEV;
	}

	return i2c_write_read(dev->i2c, dev->addr, &reg, 1, val, 1);
}

int maxm86161_write_reg(struct maxm86161 *dev, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };

	if (!addr_valid(dev)) {
		return -ENODEV;
	}

	return i2c_write(dev->i2c, buf, sizeof(buf), dev->addr);
}

/*
 * Probe one address three different ways and report each, because they do
 * not fail together:
 *
 *   bare   - i2c_read(): address phase only, no register pointer
 *   split  - write the pointer, STOP, then a separate read transaction
 *   restart- i2c_write_read(): pointer then repeated START (what the
 *            datasheet documents in Figures 14/15)
 *
 * A part that ACKs "bare" but rejects "restart" is answering its address
 * while its transfer state machine is wedged.
 */
static int try_addr(struct maxm86161 *dev, uint8_t addr, uint8_t *part_id)
{
	uint8_t reg = REG_PART_ID;
	uint8_t dummy = 0, id_split = 0, id_restart = 0;
	int e_bare, e_ptr, e_split = -1, e_restart;

	dev->addr = addr;

	e_bare = i2c_read(dev->i2c, &dummy, 1, addr);

	e_ptr = i2c_write(dev->i2c, &reg, 1, addr);
	if (e_ptr == 0) {
		e_split = i2c_read(dev->i2c, &id_split, 1, addr);
	}

	e_restart = i2c_write_read(dev->i2c, addr, &reg, 1, &id_restart, 1);

	LOG_INF("0x%02x  bare=%d  ptr_write=%d  split=%d(0x%02x)  restart=%d(0x%02x)",
		addr, e_bare, e_ptr, e_split, id_split, e_restart, id_restart);

	/* Take whichever access pattern actually returned the part ID. */
	if (e_restart == 0) {
		*part_id = id_restart;
		return 0;
	}
	if (e_split == 0) {
		*part_id = id_split;
		return 0;
	}

	return (e_restart != 0) ? e_restart : e_bare;
}

int maxm86161_probe(struct maxm86161 *dev, const struct device *i2c)
{
	static const uint8_t candidates[] = { MAXM86161_ADDR, MAXM86161_ADDR_ALT };
	uint8_t part_id = 0;

	dev->i2c = i2c;

	for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (try_addr(dev, candidates[i], &part_id) != 0) {
			LOG_DBG("no ACK at 0x%02x", candidates[i]);
			continue;
		}

		if (part_id != MAXM86161_PART_ID_VAL) {
			LOG_WRN("0x%02x answered but PART_ID is 0x%02x, expected 0x%02x",
				candidates[i], part_id, MAXM86161_PART_ID_VAL);
			continue;
		}

		uint8_t rev = 0;

		(void)maxm86161_read_reg(dev, REG_REVISION_ID, &rev);
		LOG_INF("MAXM86161 found at 0x%02x (PART_ID 0x%02x, REV 0x%02x)",
			dev->addr, part_id, rev);
		return 0;
	}

	dev->addr = 0;
	return -ENODEV;
}

int maxm86161_start_ppg(struct maxm86161 *dev, uint8_t ledc, uint8_t pa)
{
	uint8_t status;
	int err;

	/*
	 * LDO_EN (U3.3) is strapped to 1V8 on this board, so the part is
	 * always powered and the only way to get it to a known state is the
	 * RESET bit. It self-clears once the sequence finishes.
	 */
	err = maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, SYS_RESET);
	if (err) {
		return err;
	}
	k_msleep(10);

	/* Reading status 1 clears PWR_RDY left over from power-up. */
	(void)maxm86161_read_reg(dev, REG_INT_STATUS_1, &status);

	/* Configure while shut down, then release. */
	err = maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, SYS_SHDN);
	if (err) {
		return err;
	}

	struct {
		uint8_t reg;
		uint8_t val;
	} const init[] = {
		/* INTB (U3.14) is not routed to the MCU on this board, so no
		 * interrupts are enabled -- the FIFO is polled instead.
		 */
		{ REG_INT_ENABLE_1, 0x00 },
		{ REG_INT_ENABLE_2, 0x00 },

		/* ALC on, 16 uA full scale, 117.3 us integration time. */
		{ REG_PPG_CONFIG_1, (PPG_ADC_RGE_16uA << 2) | PPG_TINT_117US },

		/* ~100 sps, no on-chip averaging. */
		{ REG_PPG_CONFIG_2, (PPG_SR_100HZ << 3) | PPG_SMP_AVE_1 },

		/* 6 us LED settling, burst mode off. */
		{ REG_PPG_CONFIG_3, (LED_SETLNG_6US << 6) },

		/* Internal photodiode. */
		{ REG_PHOTO_DIODE_BIAS, PDBIAS1_0_65PF },

		/* One exposure per sample, on the selected driver only. */
		{ REG_LED_SEQ_1, (LEDC_NONE << 4) | ledc },
		{ REG_LED_SEQ_2, 0x00 },
		{ REG_LED_SEQ_3, 0x00 },

		/* 31 mA full-scale range, 0.12 mA per LSB. */
		{ REG_LED_RANGE_1, (LED_RGE_31MA << 4) | (LED_RGE_31MA << 2) |
				   LED_RGE_31MA },
		{ REG_LED1_PA, (ledc == LEDC_LED1) ? pa : 0x00 },
		{ REG_LED2_PA, (ledc == LEDC_LED2) ? pa : 0x00 },
		{ REG_LED3_PA, (ledc == LEDC_LED3) ? pa : 0x00 },

		/* Roll over on full so a slow poll loses old samples, not new
		 * ones, and let a FIFO read clear the status bits.
		 */
		{ REG_FIFO_CONFIG_1, 0x0F },
		{ REG_FIFO_CONFIG_2, FIFO_CFG2_STAT_CLR | FIFO_CFG2_A_FULL_TYPE |
				     FIFO_CFG2_FIFO_RO },
		{ REG_FIFO_CONFIG_2, FIFO_CFG2_STAT_CLR | FIFO_CFG2_A_FULL_TYPE |
				     FIFO_CFG2_FIFO_RO | FIFO_CFG2_FLUSH_FIFO },
	};

	for (size_t i = 0; i < ARRAY_SIZE(init); i++) {
		err = maxm86161_write_reg(dev, init[i].reg, init[i].val);
		if (err) {
			LOG_ERR("write 0x%02x = 0x%02x failed (%d)",
				init[i].reg, init[i].val, err);
			return err;
		}
	}

	err = maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, 0x00);
	if (err) {
		return err;
	}

	static const char *const names[] = {
		[LEDC_LED1] = "LED1 green 530nm",
		[LEDC_LED2] = "LED2 IR 880nm",
		[LEDC_LED3] = "LED3 red 660nm",
	};

	LOG_INF("PPG running: %s, PA 0x%02x (~%u.%02u mA), 100 sps",
		names[ledc], pa, (pa * 12U) / 100U, (pa * 12U) % 100U);

	return 0;
}

int maxm86161_leds_off(struct maxm86161 *dev)
{
	int err;

	err = maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, SYS_SHDN);
	if (err) {
		return err;
	}

	(void)maxm86161_write_reg(dev, REG_LED_SEQ_1, LEDC_NONE);
	(void)maxm86161_write_reg(dev, REG_LED_SEQ_2, 0x00);
	(void)maxm86161_write_reg(dev, REG_LED_SEQ_3, 0x00);
	(void)maxm86161_write_reg(dev, REG_LED1_PA, 0x00);
	(void)maxm86161_write_reg(dev, REG_LED2_PA, 0x00);
	(void)maxm86161_write_reg(dev, REG_LED3_PA, 0x00);

	return 0;
}

int maxm86161_led_solid(struct maxm86161 *dev, uint8_t ledc, uint8_t pa)
{
	int err;

	err = maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, SYS_SHDN);
	if (err) {
		return err;
	}

	/* Widest pulse for maximum on-time, so the eye sees it as steady. */
	(void)maxm86161_write_reg(dev, REG_PPG_CONFIG_1,
				  (PPG_ADC_RGE_16uA << 2) | PPG_TINT_117US);
	(void)maxm86161_write_reg(dev, REG_PPG_CONFIG_2,
				  (0x11 << 3) | PPG_SMP_AVE_1);

	/* 31 mA range: this stays on indefinitely, so keep the current low. */
	(void)maxm86161_write_reg(dev, REG_LED_RANGE_1,
				  (LED_RGE_31MA << 4) | (LED_RGE_31MA << 2) |
				  LED_RGE_31MA);

	(void)maxm86161_write_reg(dev, REG_LED1_PA,
				  (ledc == LEDC_LED1) ? pa : 0x00);
	(void)maxm86161_write_reg(dev, REG_LED2_PA,
				  (ledc == LEDC_LED2) ? pa : 0x00);
	(void)maxm86161_write_reg(dev, REG_LED3_PA,
				  (ledc == LEDC_LED3) ? pa : 0x00);

	(void)maxm86161_write_reg(dev, REG_LED_SEQ_1, (LEDC_NONE << 4) | ledc);
	(void)maxm86161_write_reg(dev, REG_LED_SEQ_2, 0x00);
	(void)maxm86161_write_reg(dev, REG_LED_SEQ_3, 0x00);

	/* No sleep, no leds_off: it stays lit until something else changes it. */
	return maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, 0x00);
}

int maxm86161_indicate(struct maxm86161 *dev, uint8_t ledc, uint32_t ms)
{
	int err;

	err = maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, SYS_SHDN);
	if (err) {
		return err;
	}

	/* Widest pulse (117.3 us integration) for maximum on-time. */
	(void)maxm86161_write_reg(dev, REG_PPG_CONFIG_1,
				  (PPG_ADC_RGE_16uA << 2) | PPG_TINT_117US);

	/*
	 * 1024 sps is the fastest the datasheet allows with one exposure at
	 * this integration time (p.49). 1024 x 123.8 us = ~12.7% duty.
	 */
	(void)maxm86161_write_reg(dev, REG_PPG_CONFIG_2,
				  (0x11 << 3) | PPG_SMP_AVE_1);

	/* Full 124 mA range on every driver, then pick one. */
	(void)maxm86161_write_reg(dev, REG_LED_RANGE_1,
				  (LED_RGE_124MA << 4) | (LED_RGE_124MA << 2) |
				  LED_RGE_124MA);

	(void)maxm86161_write_reg(dev, REG_LED1_PA,
				  (ledc == LEDC_LED1) ? 0xff : 0x00);
	(void)maxm86161_write_reg(dev, REG_LED2_PA,
				  (ledc == LEDC_LED2) ? 0xff : 0x00);
	(void)maxm86161_write_reg(dev, REG_LED3_PA,
				  (ledc == LEDC_LED3) ? 0xff : 0x00);

	(void)maxm86161_write_reg(dev, REG_LED_SEQ_1, (LEDC_NONE << 4) | ledc);
	(void)maxm86161_write_reg(dev, REG_LED_SEQ_2, 0x00);
	(void)maxm86161_write_reg(dev, REG_LED_SEQ_3, 0x00);

	err = maxm86161_write_reg(dev, REG_SYSTEM_CONTROL, 0x00);
	if (err) {
		return err;
	}

	k_msleep(ms);

	return maxm86161_leds_off(dev);
}

int maxm86161_fifo_count(struct maxm86161 *dev)
{
	uint8_t count;
	int err;

	err = maxm86161_read_reg(dev, REG_FIFO_DATA_COUNT, &count);
	if (err) {
		return err;
	}

	return MIN(count, FIFO_DEPTH);
}

int maxm86161_fifo_read(struct maxm86161 *dev, uint32_t *out, uint8_t max_samples)
{
	uint8_t buf[FIFO_DEPTH * FIFO_SAMPLE_BYTES];
	uint8_t reg = REG_FIFO_DATA;
	int count;
	int err;

	if (!addr_valid(dev)) {
		return -ENODEV;
	}

	count = maxm86161_fifo_count(dev);
	if (count <= 0) {
		return count;
	}

	count = MIN(count, (int)max_samples);

	/*
	 * The address pointer does not auto-increment on FIFO_DATA, so a
	 * single burst read of count * 3 bytes walks the FIFO.
	 */
	err = i2c_write_read(dev->i2c, dev->addr, &reg, 1, buf,
			     count * FIFO_SAMPLE_BYTES);
	if (err) {
		return err;
	}

	for (int i = 0; i < count; i++) {
		const uint8_t *s = &buf[i * FIFO_SAMPLE_BYTES];

		out[i] = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
	}

	return count;
}
