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

/* Values for ring_activity.sleep_state */
#define SLEEP_STATE_AWAKE	0
#define SLEEP_STATE_ASLEEP	1

/*
 * Activity payload: steps, calories, sleep. Everything here is derived
 * from the accelerometer only (steps.c/sleep.c/calories.c) -- see those
 * files for the "unvalidated" caveats that apply to every field.
 *
 * Field order puts every member on its natural alignment inside the packed
 * struct -- u32s, then u16s, then the single u8. Packed access to a
 * misaligned word costs a byte-wise fixup on every read and write, and
 * there is no reason to pay it.
 */
struct ring_activity {
	uint32_t steps;
	uint32_t kcal_x1000;
	uint16_t cadence_spm;
	uint16_t sleep_session_min;	/* minutes into the current session */
	uint16_t sleep_total_min;	/* minutes asleep since boot/reset */
	uint16_t restless_min;
	uint8_t sleep_state;		/* see SLEEP_STATE_* above */
} __packed;

/*
 * HRV payload. Separate characteristic rather than new fields on Status or
 * Activity: both of those are already at the 20-byte payload a
 * notification carries at the default 23-byte ATT MTU, and an app is being
 * written against their current layout.
 */
struct ring_hrv {
	uint16_t rmssd_x10;	/* milliseconds x10; 0 = not enough clean beats */
	uint8_t rmssd_beats;	/* successive differences behind the value */
} __packed;

/* Callbacks the phone can trigger by writing to the control characteristic. */
struct ble_control_cbs {
	void (*stream_ppg)(bool on);
	void (*stream_imu)(bool on);
	void (*measure_now)(void);
	void (*set_duty)(uint16_t window_s, uint16_t period_s);
	void (*set_weight)(uint16_t weight_kg_x10);
	void (*reset_activity)(void);
	void (*stream_gsr)(bool on);
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
void ble_publish_activity(const struct ring_activity *activity);
void ble_publish_hrv(const struct ring_hrv *hrv);

/*
 * Raw GSR, in ADC counts. Sized by the caller to the negotiated MTU --
 * ble_publish_gsr() caps `count` itself and never fragments.
 */
void ble_publish_gsr(const int16_t *samples, uint8_t count);

/* Largest batch that fits an unnegotiated 23-byte MTU: seq(4) + count(1)
 * + 7 samples x 2 bytes = 19 of the 20 payload bytes.
 */
#define GSR_MAX_SAMPLES_DEFAULT_MTU	7

bool ble_ppg_streaming(void);
bool ble_imu_streaming(void);
bool ble_gsr_streaming(void);

/*
 * Whether the app has asked for the GSR stream, regardless of whether it
 * has subscribed to the characteristic yet.
 *
 * The distinction matters here and nowhere else: enabling this stream
 * powers the analog front end, so the firmware needs to know when to tear
 * that down. An app that sends the opcode before subscribing would
 * otherwise have the stream powered up and immediately powered back down.
 * Cleared on disconnect, so this also catches a phone that walks away.
 */
bool ble_gsr_stream_requested(void);

#endif /* BLE_H_ */
