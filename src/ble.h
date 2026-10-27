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

/*
 * SpO2 payload.
 *
 * `ratio_x1000` is the real measurement: it comes out of the optics with
 * no calibration and is what a comparison against a reference oximeter
 * would be fitted to. `percent` is that ratio put through a literature
 * curve that has never been calibrated for this hardware, which is why
 * SPO2_FLAG_UNCALIBRATED is always set. See spo2.h.
 */
#define SPO2_FLAG_UNCALIBRATED	(1U << 0)
#define SPO2_FLAG_VALID		(1U << 1)

struct ring_spo2 {
	uint16_t ratio_x1000;	/* R x1000; 0 = not measured */
	uint8_t percent;	/* 0 = not measured */
	uint8_t flags;
} __packed;

/*
 * Workout payload: the figures for the workout the app started with
 * opcode 0x0B, or the last one once it has stopped. Its own
 * characteristic for the same reason as HRV -- Activity is full.
 *
 * The three minute counts say how each minute was priced (see
 * calories.h): from heart rate, from steps because the heart rate was
 * low, or from the activity's typical MET because there was no usable
 * heart rate. They add up to `minutes`. A workout that is mostly
 * fallback minutes is mostly a guess, and an app should say so.
 */
struct ring_workout {
	uint8_t active;			/* 1 while running */
	uint8_t type;			/* WORKOUT_* from calories.h */
	uint16_t minutes;
	uint16_t hr_minutes;
	uint16_t rest_minutes;
	uint16_t fallback_minutes;
	uint16_t avg_hr_x10;		/* over hr_minutes; 0 = none */
	uint32_t kcal_x1000;		/* this workout only */
} __packed;

/* Bits in ring_wellness.flags */
#define WELLNESS_FLAG_THERMO_READY	BIT(0)	/* baseline settled, see thermo.h */
#define WELLNESS_FLAG_HOT_FLASH		BIT(1)	/* a hot flash is in progress */
#define WELLNESS_FLAG_SPOT_RUNNING	BIT(2)	/* a spot reading is under way */
#define WELLNESS_FLAG_SPOT_VALID	BIT(3)	/* the spot_* fields passed */

/*
 * Wellness payload: temperature, hot flashes and the last spot reading
 * (thermo.h, spot.h). Notified when a hot flash starts or ends, when a
 * spot reading finishes, and once a minute; read at any time.
 *
 * Temperatures are 0.01 C, INT16_MIN for "none". spot_kind is SPOT_* from
 * spot.h (4 = a manual reading, opcode 0x14), 0 until the first reading this boot. spot_uptime_s is when it
 * finished, on the same clock as ring_status.uptime_s.
 *
 * 16 bytes, every field on its natural alignment.
 */
struct ring_wellness {
	int16_t temp_c100;	/* ring (nRF die), smoothed */
	int16_t baseline_c100;	/* settled baseline */
	int16_t skin_c100;	/* PPG sensor face, last window */
	uint8_t flags;		/* WELLNESS_FLAG_* */
	uint8_t hot_flashes;	/* since boot, wraps at 256 */
	uint8_t spot_kind;
	uint8_t spot_hr_bpm;
	uint8_t spot_rmssd_ms;
	uint8_t spot_beats;
	uint32_t spot_uptime_s;
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
	void (*spo2_mode)(bool on);
	void (*set_body)(uint8_t age_years, uint8_t sex);
	void (*workout)(uint8_t type);	/* 0 = stop */
	void (*ship_mode)(void);	/* power off until a charger wakes it */
	/* Heart-rate PPG front end, already range-checked (opcode 0x10). */
	void (*ppg_tune)(uint8_t led_pa, uint8_t led_rge, uint8_t adc_rge,
			 uint8_t avg_log2);
	void (*tx_power)(int8_t dbm);	/* opcode 0x11 */
	/* Spot reading schedule, already range-checked (opcode 0x12). */
	void (*spot_config)(uint16_t night_min, uint16_t day_min,
			    uint16_t window_s);
	/* Hot-flash detection, already range-checked (opcode 0x13). */
	void (*hot_flash_config)(uint8_t mode, uint8_t rise_c10);
	void (*spot_now)(void);		/* opcode 0x14 */
};

/*
 * Radio housekeeping, every pass of the main loop: slows advertising after
 * CONFIG_RING_ADV_SLOW_AFTER_S, and with CONFIG_RING_CONN_PARAM_MGMT asks
 * the central for an idle or a fast connection interval to suit what is
 * being sent. Main loop only: both send HCI commands and wait.
 */
void ble_service(int64_t now_ms);

/*
 * Set the radio's transmit power on the current connection. Sends an HCI
 * command and waits for the reply, so main loop only -- never the BT RX
 * thread. *selected is what the controller actually chose (it rounds to the
 * nearest supported level). -ENOTCONN with nobody connected.
 */
int ble_set_tx_power(int8_t dbm, int8_t *selected);

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
void ble_publish_spo2(const struct ring_spo2 *spo2);
void ble_publish_workout(const struct ring_workout *workout);
void ble_publish_wellness(const struct ring_wellness *wellness);

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

/* True while the app has asked for SpO2 (red+IR) measurement windows. */
bool ble_spo2_mode(void);

#endif /* BLE_H_ */
