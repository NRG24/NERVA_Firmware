/*
 * Board self-test: everything on the ring board except the PPG channel,
 * which main.c exercises continuously.
 */

#ifndef SELFTEST_H_
#define SELFTEST_H_

#include <stdbool.h>

#include <zephyr/device.h>

/* Runs every check and logs a pass/fail line per subsystem. */
void selftest_run(const struct device *i2c);

/*
 * Re-evaluate the charger situation and park CD correctly:
 * charger present -> CD low so it charges; battery only -> CD high so the
 * PMIC stays out of Hi-Z and its I2C keeps working. Call periodically.
 */
bool pmic_service(const struct device *i2c);

/* Single GSR reading in millivolts, or a negative errno. */
int selftest_gsr_mv(void);

/*
 * Bench-only live GSR readout. Samples in every state and holds GSR_PWR on
 * so the front end stays settled; logs only when the value moves.
 */
void selftest_gsr_monitor(void);

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

#endif /* SELFTEST_H_ */
