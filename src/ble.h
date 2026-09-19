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
 * Values for ring_activity.wear_state: whether the ring was confirmed to
 * be on a hand during the session sleep_state describes.
 *
 * Stillness cannot tell a sleeping hand from a nightstand, so the ring
 * samples the PPG DC level -- the only wear signal this board has -- a
 * few times per session and reports what it found. UNKNOWN is a real
 * answer and the honest default: it is what an absent or dead PPG
 * produces, and what every session reports until enough checks have
 * landed. It does NOT mean "not worn".
 *
 * These are advisory. A NOT_WORN session is still reported as sleep, with
 * its minutes still in sleep_total_min, because the threshold behind the
 * verdict has never been measured on a loosely worn ring -- see
 * APP_INTEGRATION.md section 7. Filtering is the app's call, made with
 * wear_checks/wear_confirmed in hand.
 */
#define RING_WEAR_UNKNOWN	0
#define RING_WEAR_WORN		1
#define RING_WEAR_NOT_WORN	2

/*
 * Activity payload: steps, calories, sleep. Everything here is derived
 * from the accelerometer only (steps.c/sleep.c/calories.c), except the
 * three wear fields, which come from the PPG -- see those files for the
 * "unvalidated" caveats that apply to every field.
 *
 * Field order puts every member on its natural alignment inside the packed
 * struct -- u32s, then u16s, then the u8s. Packed access to a misaligned
 * word costs a byte-wise fixup on every read and write, and there is no
 * reason to pay it.
 *
 * 20 bytes, which is the whole of a default 23-byte ATT MTU once ATT's
 * 3-byte header is taken out -- the same ceiling ring_status sits on. Any
 * further field here starts fragmenting the notification, so it needs a
 * reason.
 */
struct ring_activity {
	uint32_t steps;
	uint32_t kcal_x1000;
	uint16_t cadence_spm;
	uint16_t sleep_session_min;	/* minutes into the current session */
	uint16_t sleep_total_min;	/* minutes asleep since boot/reset */
	uint16_t restless_min;
	uint8_t sleep_state;		/* see SLEEP_STATE_* above */
	uint8_t wear_state;		/* see RING_WEAR_* above */
	uint8_t wear_checks;		/* wear checks taken this session */
	uint8_t wear_confirmed;		/* how many of those found a hand */
} __packed;

/* Callbacks the phone can trigger by writing to the control characteristic. */
struct ble_control_cbs {
	void (*stream_ppg)(bool on);
	void (*stream_imu)(bool on);
	void (*measure_now)(void);
	void (*set_duty)(uint16_t window_s, uint16_t period_s);
	void (*set_weight)(uint16_t weight_kg_x10);
	void (*reset_activity)(void);
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

bool ble_ppg_streaming(void);
bool ble_imu_streaming(void);

#endif /* BLE_H_ */
