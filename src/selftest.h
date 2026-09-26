/*
 * Board self-test and PMIC configuration.
 *
 * Two different things live here and the distinction matters:
 *
 *   pmic_configure() / pmic_service()  load-bearing. The board will not
 *     charge without them. Always called.
 *
 *   selftest_run() / selftest_gsr_monitor()  diagnostics. Bench only, and
 *     compiled out entirely otherwise.
 */

#ifndef SELFTEST_H_
#define SELFTEST_H_

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

/*
 * Writes everything the BQ25120A needs before it will charge: 20 mA charge
 * current, and TS monitoring off because ball C3 is unconnected on this
 * board. Call once at boot, before pmic_service().
 *
 * Returns 0, or a negative errno if the PMIC could not be reached.
 */
int pmic_configure(const struct device *i2c);

/*
 * Re-evaluate the charger situation and park CD correctly:
 * charger present -> CD low so it charges; battery only -> CD high so the
 * PMIC stays out of Hi-Z and its I2C keeps working. Call periodically.
 *
 * Returns 0 with *charging set, or a negative errno if the PMIC did not
 * answer. Do NOT collapse those two into one boolean -- that is what made a
 * dead I2C bus look like a healthy board running on battery.
 */
int pmic_service(const struct device *i2c, bool *charging);

/* Single GSR reading in millivolts, or a negative errno. */
int selftest_gsr_mv(void);

/*
 * Continuous GSR sampling, for the raw stream over BLE.
 *
 * Skin conductance responses peak roughly 1.4 s after onset, so the 1 Hz
 * gsr_mv in the status packet lands about one sample on the rise and
 * cannot resolve the shape at all. These exist to sample fast enough that
 * it can.
 *
 * gsr_stream_start() pays the front end's ~800 ms settle ONCE and then
 * holds GSR_PWR on, so every later sample is already settled. That is the
 * whole point, and also the cost: the analog stage stays powered for as
 * long as the stream runs, which defeats the duty cycling the rest of the
 * firmware is built around. It is opt-in and off by default for that
 * reason.
 *
 * BLOCKING: start() sleeps for the settle. Call it from the main loop,
 * never from a BLE callback -- those run on the BT RX thread.
 *
 * While a stream is running, selftest_gsr_mv() leaves GSR_PWR alone rather
 * than powering it down when it finishes, so the 30 s status reading does
 * not punch a hole in the trace.
 */
int gsr_stream_start(void);
void gsr_stream_stop(void);

/*
 * One conversion, in raw ADC counts -- deliberately not millivolts. The
 * conversion to volts, and from there to conductance, needs constants
 * (V_REF, R5) that are board-specific and, on this revision, not yet
 * trusted. Sending counts keeps that decision on the phone where it can be
 * changed without a firmware flash.
 *
 * Returns 0 and writes *raw, or a negative errno.
 */
int gsr_stream_raw(int16_t *raw);

/*
 * Battery via the BQ25120A voltage monitor. Returns millivolts, or a
 * negative errno. *percent_of_vbatreg, when non-NULL, gets the raw monitor
 * result as a percentage of the regulation voltage -- which is NOT state of
 * charge.
 */
int selftest_battery_mv(const struct device *i2c, uint8_t *percent_of_vbatreg);

/*
 * Linear 3.30-4.20 V gauge, 0-100. This is a VOLTAGE BAR, not state of
 * charge -- a LiPo's discharge curve is nowhere near linear, and terminal
 * voltage sags under load. It exists only because BLE's Battery Service
 * carries a percentage and nothing else. Trust the millivolts.
 */
uint8_t battery_gauge_pct(int mv);

#if defined(CONFIG_RING_BENCH)
/* Runs every diagnostic and logs a pass/fail line per subsystem. */
void selftest_run(const struct device *i2c);
#else
static inline void selftest_run(const struct device *i2c) { ARG_UNUSED(i2c); }
#endif

#if defined(CONFIG_RING_GSR_MONITOR)
/*
 * Bench-only live GSR readout. Samples in every state and holds GSR_PWR on
 * so the front end stays settled; logs only when the value moves.
 *
 * Holding GSR_PWR on defeats the duty cycling, which is why this is not
 * merely verbose but genuinely unshippable.
 */
void selftest_gsr_monitor(void);
#else
static inline void selftest_gsr_monitor(void) { }
#endif

#endif /* SELFTEST_H_ */
