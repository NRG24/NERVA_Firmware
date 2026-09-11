/*
 * LSM6DSV16BX accelerometer: motion readout plus wake-on-motion so the
 * firmware can sleep the optical front end while the ring is off a finger.
 *
 * NOTE: the part on this board answers WHO_AM_I = 0x71 (LSM6DSV16BX). The
 * datasheet filed with this project is the LSM6DSV (0x70). Basic
 * accelerometer and the wake-up/interrupt block are register-compatible;
 * the embedded-function maps are not. See BRINGUP_RESULTS.md.
 */

#ifndef IMU_H_
#define IMU_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

struct imu_accel {
	int16_t x, y, z;	/* raw, +/-2 g full scale */
};

/* Verifies WHO_AM_I and starts the accelerometer. */
int imu_init(const struct device *i2c);

int imu_read_accel(struct imu_accel *out);

/* Milli-g magnitude, handy for wear/motion heuristics. */
int imu_magnitude_mg(void);

/*
 * Route a wake-up (slope) event to INT1 -> P0.16 and enable the GPIO
 * interrupt. threshold_mg is approximate; resolution is ~31 mg at +/-2 g.
 */
int imu_arm_wake(uint16_t threshold_mg);

int imu_disarm_wake(void);

/* True if INT1 has fired since the last call. Clears the flag. */
bool imu_take_wake_event(void);

/* Reads and clears the IMU's own latched interrupt source. */
void imu_clear_int(void);

/*
 * Logs the INT1 pin level alongside the IMU's own latched wake source,
 * to separate "the IMU never fired" from "the IMU fired and the pin did
 * not move". Compiled to a no-op when IMU_WAKE_DIAG is 0 in imu.c.
 */
void imu_wake_diag(void);

#endif /* IMU_H_ */
