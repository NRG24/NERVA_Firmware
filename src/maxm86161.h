/*
 * MAXM86161 register definitions and a minimal bring-up driver.
 *
 * Datasheet: MAXM86161 Single-Supply Integrated Optical Module for HR
 * and SpO2 Measurement, 19-100523 Rev 0.
 */

#ifndef MAXM86161_H_
#define MAXM86161_H_

#include <zephyr/device.h>
#include <stdint.h>

/*
 * The prose in the datasheet (p.30) and the transaction figures (p.32-34)
 * disagree about the slave address: the text gives 0b1100010x (7-bit 0x62,
 * write byte 0xC4) while the figures draw 1100011 (7-bit 0x63). 0x62 is the
 * one Maxim's own driver uses and the one real parts answer on; 0x63 is kept
 * here so the probe can fall back to it.
 */
#define MAXM86161_ADDR		0x62
#define MAXM86161_ADDR_ALT	0x63

#define MAXM86161_PART_ID_VAL	0x36

/* Status */
#define REG_INT_STATUS_1	0x00
#define REG_INT_STATUS_2	0x01
#define REG_INT_ENABLE_1	0x02
#define REG_INT_ENABLE_2	0x03

#define INT1_A_FULL		BIT(7)
#define INT1_DATA_RDY		BIT(6)
#define INT1_ALC_OVF		BIT(5)
#define INT1_PROX_INT		BIT(4)
#define INT1_LED_COMPB		BIT(3)
#define INT1_DIE_TEMP_RDY	BIT(2)
#define INT1_PWR_RDY		BIT(0)

/* FIFO */
#define REG_FIFO_WR_PTR		0x04
#define REG_FIFO_RD_PTR		0x05
#define REG_OVF_COUNTER		0x06
#define REG_FIFO_DATA_COUNT	0x07
#define REG_FIFO_DATA		0x08
#define REG_FIFO_CONFIG_1	0x09
#define REG_FIFO_CONFIG_2	0x0A

#define FIFO_CFG2_FLUSH_FIFO	BIT(4)
#define FIFO_CFG2_STAT_CLR	BIT(3)
#define FIFO_CFG2_A_FULL_TYPE	BIT(2)
#define FIFO_CFG2_FIFO_RO	BIT(1)

/* System */
#define REG_SYSTEM_CONTROL	0x0D

#define SYS_SINGLE_PPG		BIT(3)
#define SYS_LP_MODE		BIT(2)
#define SYS_SHDN		BIT(1)
#define SYS_RESET		BIT(0)

/* PPG configuration */
#define REG_PPG_SYNC_CTRL	0x10
#define REG_PPG_CONFIG_1	0x11
#define REG_PPG_CONFIG_2	0x12
#define REG_PPG_CONFIG_3	0x13
#define REG_PROX_INT_THRESH	0x14
#define REG_PHOTO_DIODE_BIAS	0x15
#define REG_PICKET_FENCE	0x16

/* PPG_CONFIG_1: ADC full scale, bits [3:2] */
#define PPG_ADC_RGE_4uA		0
#define PPG_ADC_RGE_8uA		1
#define PPG_ADC_RGE_16uA	2
#define PPG_ADC_RGE_32uA	3

/* PPG_CONFIG_1: integration time / LED pulse width, bits [1:0] */
#define PPG_TINT_14US		0
#define PPG_TINT_29US		1
#define PPG_TINT_59US		2
#define PPG_TINT_117US		3

/* PPG_CONFIG_2: sample rate, bits [7:3] (N = 1 pulse per sample) */
#define PPG_SR_25HZ		0x00
#define PPG_SR_50HZ		0x01
#define PPG_SR_84HZ		0x02
#define PPG_SR_100HZ		0x03
#define PPG_SR_200HZ		0x04
#define PPG_SR_400HZ		0x05

/* PPG_CONFIG_2: on-chip averaging, bits [2:0] */
#define PPG_SMP_AVE_1		0

/* PPG_CONFIG_3: LED settling time, bits [7:6] */
#define LED_SETLNG_6US		1

/* Photodiode bias for the module's internal PD (0-65 pF) */
#define PDBIAS1_0_65PF		0x01

/* LED sequence */
#define REG_LED_SEQ_1		0x20
#define REG_LED_SEQ_2		0x21
#define REG_LED_SEQ_3		0x22

#define LEDC_NONE		0x0
#define LEDC_LED1		0x1	/* green, 530 nm */
#define LEDC_LED2		0x2	/* IR,    880 nm */
#define LEDC_LED3		0x3	/* red,   660 nm */
#define LEDC_PILOT_LED1		0x8
#define LEDC_DIRECT_AMBIENT	0x9

/* LED drive */
#define REG_LED1_PA		0x23
#define REG_LED2_PA		0x24
#define REG_LED3_PA		0x25
#define REG_LED_PILOT_PA	0x29
#define REG_LED_RANGE_1		0x2A

#define LED_RGE_31MA		0
#define LED_RGE_62MA		1
#define LED_RGE_93MA		2
#define LED_RGE_124MA		3

/* Part ID */
#define REG_REVISION_ID		0xFE
#define REG_PART_ID		0xFF

/* FIFO sample: 24 bits, tag in [23:19], data in [18:0] */
#define FIFO_SAMPLE_BYTES	3
#define FIFO_DEPTH		128

#define FIFO_TAG(raw)		((uint8_t)((raw) >> 19))
#define FIFO_DATA(raw)		((raw) & 0x7FFFFU)

#define TAG_PPG1_LEDC1		0x01
#define TAG_PPG1_LEDC2		0x02
#define TAG_PPG1_LEDC3		0x03
#define TAG_INVALID		0x1F

/*
 * IMPORTANT: never probe this part with an address-only read
 * (i2c_read() with no register pointer). It returns -EIO and leaves the
 * device unable to service subsequent register reads until the bus goes
 * idle. The datasheet only ever documents pointer-first transactions
 * (Figures 12-15). See POSTMORTEM.md.
 */
struct maxm86161 {
	const struct device *i2c;
	uint8_t addr;
};

/* Probe the bus, verify PART_ID, latch the address that answered. */
int maxm86161_probe(struct maxm86161 *dev, const struct device *i2c);

/*
 * Soft reset + configure single-channel PPG at 100 sps.
 *
 * ledc picks which driver runs the exposure: LEDC_LED1 (green 530nm),
 * LEDC_LED2 (IR 880nm) or LEDC_LED3 (red 660nm). pa is that driver's
 * current DAC code in the 31mA range, 0.12mA per LSB.
 */
int maxm86161_start_ppg(struct maxm86161 *dev, uint8_t ledc, uint8_t pa);

/*
 * Configure a two-slot sequence for SpO2: IR in slot 1, red in slot 2, at
 * the same 100 sps. Each frame therefore produces TWO FIFO samples, so the
 * FIFO fills twice as fast -- 128 entries is 0.64 s of headroom rather
 * than 1.28 s, still comfortable at the 20 ms poll.
 *
 * Demultiplex on the tag: IR is TAG_PPG1_LEDC1 and red is TAG_PPG1_LEDC2,
 * because the part tags by slot and not by which LED is in it.
 *
 * This does NOT drive the green LED, so nothing that depends on the green
 * channel -- which is every heart-rate number this firmware produces --
 * works while it is running.
 */
int maxm86161_start_spo2(struct maxm86161 *dev, uint8_t ir_pa, uint8_t red_pa);

/*
 * Drive one LED hard enough to see, for ms milliseconds, then stop.
 *
 * At the normal 100 sps the LED is lit for 123.8 us per 10 ms sample --
 * about 1.2% duty, which is invisible for the red die. This runs 1024 sps
 * at the widest pulse and full current range for roughly 12.7% duty.
 */
/*
 * Turn one LED on and leave it on. Non-blocking, unlike maxm86161_indicate(),
 * which sleeps and then switches the LEDs off. pa is in the 31 mA range.
 */
int maxm86161_led_solid(struct maxm86161 *dev, uint8_t ledc, uint8_t pa);

int maxm86161_indicate(struct maxm86161 *dev, uint8_t ledc, uint32_t ms);

/* All LED drivers off and the part in shutdown. */
int maxm86161_leds_off(struct maxm86161 *dev);

/* Number of samples waiting in the FIFO. */
int maxm86161_fifo_count(struct maxm86161 *dev);

/* Pop up to max_samples 24-bit words. Returns the count read. */
int maxm86161_fifo_read(struct maxm86161 *dev, uint32_t *out, uint8_t max_samples);

int maxm86161_read_reg(struct maxm86161 *dev, uint8_t reg, uint8_t *val);
int maxm86161_write_reg(struct maxm86161 *dev, uint8_t reg, uint8_t val);

#endif /* MAXM86161_H_ */
