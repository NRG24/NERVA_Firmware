/*
 * Pins the exact register writes the MAXM86161 driver makes.
 *
 * This exists because of a refactor. Adding the two-slot SpO2 sequence
 * meant routing the single-LED heart-rate path through the same
 * slot-driven configure, and the green channel at 100 sps is the ONLY
 * configuration this project has ever seen work on hardware -- the RTT log
 * from v0.1-bench is the whole of the evidence base. A refactor that
 * quietly changed one register value there would cost the next bring-up
 * days, and nothing else in the suite would notice.
 *
 * So: stub the I2C layer, record every write, and compare against the
 * values the proven build produced. The expected table below is not
 * derived from the current code -- it is transcribed from the driver as it
 * stood before the refactor, which is what makes it a check rather than a
 * tautology.
 */

#include "sim.h"

#include "maxm86161.h"

#include <zephyr/sys/util.h>	/* the stub under tests/stubs */

#include <stdio.h>
#include <string.h>

/* --- I2C capture ------------------------------------------------------- */

struct write_rec {
	uint8_t reg;
	uint8_t val;
};

static struct write_rec writes[64];
static size_t n_writes;

int i2c_write(const struct device *dev, const uint8_t *buf, uint32_t num_bytes,
	      uint16_t addr)
{
	(void)dev;
	(void)addr;

	if (num_bytes == 2 && n_writes < ARRAY_SIZE(writes)) {
		writes[n_writes].reg = buf[0];
		writes[n_writes].val = buf[1];
		n_writes++;
	}

	return 0;
}

int i2c_write_read(const struct device *dev, uint16_t addr,
		   const void *write_buf, size_t num_write, void *read_buf,
		   size_t num_read)
{
	const uint8_t *reg = write_buf;
	uint8_t *out = read_buf;

	(void)dev;
	(void)addr;
	(void)num_write;

	/* Answer the part-ID probe so start-up proceeds; everything else
	 * reads back as zero, which the driver does not act on.
	 */
	if (num_read >= 1) {
		out[0] = (*reg == REG_PART_ID) ? MAXM86161_PART_ID_VAL : 0;
	}

	return 0;
}

int i2c_read(const struct device *dev, uint8_t *buf, uint32_t num_bytes,
	     uint16_t addr)
{
	(void)dev;
	(void)buf;
	(void)num_bytes;
	(void)addr;

	return 0;
}

int32_t k_msleep(int32_t ms)
{
	return ms;
}

void zstub_log(const char *fmt, ...)
{
	(void)fmt;
}

/* --- helpers ----------------------------------------------------------- */

/* Value of the LAST write to `reg`, or -1 if it was never written. */
static int last_write(uint8_t reg)
{
	int val = -1;

	for (size_t i = 0; i < n_writes; i++) {
		if (writes[i].reg == reg) {
			val = writes[i].val;
		}
	}

	return val;
}

static void check_reg(uint8_t reg, int want, const char *name)
{
	int got = last_write(reg);

	sim_check(got == want, __FILE__, __LINE__,
		  "%s (0x%02x): wrote 0x%02x, expected 0x%02x", name, reg,
		  got, want);
}

/*
 * Transcribed from the driver as it stood at `prod`, before the two-slot
 * refactor. Green LED1 at PA 0x80, 100 sps, single slot.
 */
static void test_green_path_is_byte_identical(void)
{
	struct maxm86161 dev = { .i2c = NULL, .addr = MAXM86161_ADDR };

	sim_section("the proven green configuration is unchanged");

	n_writes = 0;
	maxm86161_start_ppg(&dev, LEDC_LED1, 0x80);

	CHECK(n_writes > 10, "only %zu register writes recorded", n_writes);

	/* ALC on, 16 uA full scale, 117.3 us integration. */
	check_reg(REG_PPG_CONFIG_1, (PPG_ADC_RGE_16uA << 2) | PPG_TINT_117US,
		  "PPG_CONFIG_1");

	/*
	 * The one the refactor could most easily have broken: the sample
	 * rate code now depends on how many slots are populated, and the
	 * single-slot case must still be the one-pulse 100 sps code.
	 */
	check_reg(REG_PPG_CONFIG_2, (0x03 << 3) | PPG_SMP_AVE_1,
		  "PPG_CONFIG_2 (one pulse per sample)");

	check_reg(REG_PPG_CONFIG_3, LED_SETLNG_6US << 6, "PPG_CONFIG_3");
	check_reg(REG_PHOTO_DIODE_BIAS, PDBIAS1_0_65PF, "PHOTO_DIODE_BIAS");

	/* Slot 1 = green, slot 2 empty. */
	check_reg(REG_LED_SEQ_1, (LEDC_NONE << 4) | LEDC_LED1, "LED_SEQ_1");
	check_reg(REG_LED_SEQ_2, 0x00, "LED_SEQ_2");
	check_reg(REG_LED_SEQ_3, 0x00, "LED_SEQ_3");

	/* Only the green driver carries current. */
	check_reg(REG_LED1_PA, 0x80, "LED1_PA (green)");
	check_reg(REG_LED2_PA, 0x00, "LED2_PA (IR, must be dark)");
	check_reg(REG_LED3_PA, 0x00, "LED3_PA (red, must be dark)");
}

/*
 * The SpO2 sequence. The sample-rate code is the part worth pinning: the
 * field encodes pulses-per-sample as well as rate, and writing the
 * one-pulse code with two slots populated is accepted by the part and then
 * simply never runs slot 2 -- no red samples, no error.
 */
static void test_spo2_sequence(void)
{
	struct maxm86161 dev = { .i2c = NULL, .addr = MAXM86161_ADDR };

	sim_section("the SpO2 two-slot sequence");

	n_writes = 0;
	maxm86161_start_spo2(&dev, 0x80, 0x60);

	check_reg(REG_PPG_CONFIG_2, (PPG_SR_P2_100HZ << 3) | PPG_SMP_AVE_1,
		  "PPG_CONFIG_2 (two pulses per sample)");

	/*
	 * Slot 1 IR in the low nibble, slot 2 red in the high nibble. The
	 * order is load-bearing: the FIFO tags by slot, so swapping these
	 * inverts the ratio of ratios with no error anywhere.
	 */
	check_reg(REG_LED_SEQ_1, (LEDC_LED3 << 4) | LEDC_LED2, "LED_SEQ_1");
	check_reg(REG_LED_SEQ_2, 0x00, "LED_SEQ_2");

	check_reg(REG_LED1_PA, 0x00, "LED1_PA (green, must be dark)");
	check_reg(REG_LED2_PA, 0x80, "LED2_PA (IR)");
	check_reg(REG_LED3_PA, 0x60, "LED3_PA (red)");
}

/* The two modes must not be able to light green and red/IR together. */
static void test_modes_are_exclusive(void)
{
	struct maxm86161 dev = { .i2c = NULL, .addr = MAXM86161_ADDR };

	sim_section("green and red/IR are never lit together");

	n_writes = 0;
	maxm86161_start_ppg(&dev, LEDC_LED1, 0x80);
	CHECK(last_write(REG_LED2_PA) == 0 && last_write(REG_LED3_PA) == 0,
	      "heart-rate mode left red or IR powered");

	n_writes = 0;
	maxm86161_start_spo2(&dev, 0x80, 0x80);
	CHECK(last_write(REG_LED1_PA) == 0,
	      "SpO2 mode left the green driver powered");
}

int main(void)
{
	printf("MAXM86161 driver register tests\n");

	test_green_path_is_byte_identical();
	test_spo2_sequence();
	test_modes_are_exclusive();

	return sim_report("test_driver");
}
