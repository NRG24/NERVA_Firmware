/* Syntax-check stub. See tests/README.md -- this is NOT the Zephyr API. */
#ifndef ZSTUB_ADC_H_
#define ZSTUB_ADC_H_
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#define ADC_ACQ_TIME_MICROSECONDS	1
#define ADC_ACQ_TIME(unit, value)	((uint16_t)(((unit) << 14) | ((value) & 0x3FFF)))
#define ADC_GAIN_1_4			3
#define ADC_REF_VDD_1_4			4

struct adc_channel_cfg {
	uint8_t gain;
	uint8_t reference;
	uint16_t acquisition_time;
	uint8_t channel_id;
	bool differential;
	uint8_t input_positive;
	uint8_t input_negative;
};

struct adc_sequence_options;

struct adc_sequence {
	const struct adc_sequence_options *options;
	uint32_t channels;
	void *buffer;
	size_t buffer_size;
	uint8_t resolution;
	uint8_t oversampling;
	bool calibrate;
};

struct adc_dt_spec {
	const struct device *dev;
	uint8_t channel_id;
	bool channel_cfg_dt_node_exists;
	struct adc_channel_cfg channel_cfg;
	uint8_t vref_mv;
	uint8_t resolution;
	uint8_t oversampling;
};

#define ADC_DT_SPEC_GET_BY_IDX(node, idx)	((struct adc_dt_spec){ 0 })

bool adc_is_ready_dt(const struct adc_dt_spec *spec);
int adc_channel_setup(const struct device *dev, const struct adc_channel_cfg *cfg);
int adc_channel_setup_dt(const struct adc_dt_spec *spec);
int adc_read(const struct device *dev, const struct adc_sequence *sequence);
int adc_read_dt(const struct adc_dt_spec *spec, const struct adc_sequence *sequence);
int adc_sequence_init_dt(const struct adc_dt_spec *spec, struct adc_sequence *seq);
int adc_raw_to_millivolts_dt(const struct adc_dt_spec *spec, int32_t *valp);
#endif
