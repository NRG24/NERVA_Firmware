/*
 * BLE peripheral.
 *
 * Two layers, deliberately:
 *
 *  - Standard Heart Rate Service (0x180D) and Battery Service (0x180F), so
 *    any off-the-shelf HR app works with no special support.
 *  - A custom Ring Service carrying everything the standard services cannot
 *    express: signal quality, raw PPG, IMU, GSR, and control.
 *
 * See APP_INTEGRATION.md for the wire format and UUIDs.
 */

#ifndef BLE_H_
#define BLE_H_

#include <stdbool.h>
#include <stdint.h>

/* Values for ring_status.state */
#define RING_STATE_IDLE		0
#define RING_STATE_MEASURING	1
#define RING_STATE_CHARGING	2

/* Bits in ring_status.flags */
#define RING_FLAG_FINGER	BIT(0)
#define RING_FLAG_CHARGER	BIT(1)
#define RING_FLAG_PPG_OK	BIT(2)
#define RING_FLAG_IMU_OK	BIT(3)
#define RING_FLAG_PMIC_OK	BIT(4)

/*
 * Status payload, little-endian, 20 bytes so it fits a default 23-byte ATT
 * MTU with no fragmentation.
 */
struct ring_status {
	uint8_t state;
	uint8_t flags;
	uint16_t hr_x10;	/* 0 when there is no valid reading */
	uint16_t battery_mv;
	int16_t gsr_mv;		/* -1 when not measured */
	uint32_t ppg_dc;
	uint16_t ppg_ac;
	uint16_t perfusion_x10;	/* tenths of a percent */
	uint32_t uptime_s;
} __packed;

/* Callbacks the phone can trigger by writing to the control characteristic. */
struct ble_control_cbs {
	void (*stream_ppg)(bool on);
	void (*stream_imu)(bool on);
	void (*measure_now)(void);
	void (*set_duty)(uint16_t window_s, uint16_t period_s);
};

int ble_start(void);
void ble_set_control_cbs(const struct ble_control_cbs *cbs);

bool ble_is_connected(void);

/* Standard services. */
void ble_notify_hr(uint16_t bpm);
void ble_notify_battery(uint8_t percent);

/* Custom service. All are no-ops when nobody has subscribed. */
void ble_publish_status(const struct ring_status *status);
void ble_publish_ppg(const uint32_t *samples, uint8_t count);
void ble_publish_imu(int16_t x, int16_t y, int16_t z);

bool ble_ppg_streaming(void);
bool ble_imu_streaming(void);

#endif /* BLE_H_ */
