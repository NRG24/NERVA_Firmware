/*
 * SpO2 by ratio-of-ratios on the red and IR channels.
 *
 * Haemoglobin and oxyhaemoglobin absorb red (660 nm) and infrared (880 nm)
 * differently, so the ratio of each channel's pulsatile component to its
 * steady component tracks saturation:
 *
 *     R = (AC_red / DC_red) / (AC_ir / DC_ir)
 *
 * R is a real measurement. It falls out of the optics and the arithmetic,
 * needs no calibration, and is the number to record when comparing this
 * ring against a reference oximeter.
 *
 * SpO2 IS NOT. Turning R into a percentage takes an empirical curve that
 * every manufacturer derives by desaturating volunteers under a reference
 * instrument and fitting the result. This firmware has never been through
 * that, so the curve here is a literature default and the percentage it
 * produces is an illustration, not a measurement.
 *
 * That is why spo2_result() reports the percentage with SPO2_FLAG_
 * UNCALIBRATED always set, and why R is published alongside it. An app
 * must not present the percentage as a reading while that flag is set --
 * and it is set unconditionally, because nothing in the firmware can
 * clear it. Only a calibration campaign can, by replacing the constants
 * in spo2.c and the flag with them.
 */

#ifndef SPO2_H_
#define SPO2_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The percentage comes from an uncalibrated curve. Always set in this
 * firmware. An app showing the number anyway must label it as such.
 */
#define SPO2_FLAG_UNCALIBRATED	(1U << 0)

/* The window held enough clean pulsatile signal for R to mean anything. */
#define SPO2_FLAG_VALID		(1U << 1)

struct spo2_result {
	uint16_t ratio_x1000;	/* R x1000; 0 when not measured */
	uint8_t percent;	/* 0 when not measured */
	uint8_t flags;
};

void spo2_init(void);

/*
 * Feed one red/IR sample pair, both raw 19-bit counts from the same frame.
 *
 * The caller demultiplexes the FIFO by tag and pairs them up; a pair from
 * two different frames is 10 ms apart, which is harmless, but a pair where
 * one channel is stale is not -- that biases R directly.
 */
void spo2_feed(uint32_t red, uint32_t ir);

/*
 * True when both channels see enough light to be on a finger.
 *
 * The green channel's equivalent (hr_finger_present) is not usable in SpO2
 * mode, because the green LED is not lit -- so without this the
 * measurement window would decide no finger was present and abandon
 * itself after a few seconds.
 */
bool spo2_finger_present(void);

/* Current estimate. Check flags before using either number. */
void spo2_result(struct spo2_result *out);

/* Forget the window. Call when the finger leaves or a new window opens. */
void spo2_reset(void);

#endif /* SPO2_H_ */
