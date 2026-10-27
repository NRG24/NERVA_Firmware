#include "ble.h"
#include "history.h"
#include "ota.h"	/* an upload wants the fast interval too */
#include "maxm86161.h"	/* PPG_TUNE_* limits for opcode 0x10 */
#include "spot.h"	/* spot_config_valid() for opcode 0x12 */
#include "thermo.h"	/* THERMO_* limits for opcode 0x13 */

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/hrs.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_BT_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/*
 * Ring Service   f0a10000-1e5c-4a2b-8d3f-9c7b6e5a4d21
 *   status       f0a10001-...   read / notify
 *   ppg          f0a10002-...   notify
 *   imu          f0a10003-...   notify
 *   control      f0a10004-...   write
 *   activity     f0a10005-...   read / notify
 *   hrv          f0a10006-...   read / notify
 *   gsr stream   f0a10007-...   notify
 *   spo2         f0a10008-...   read / notify
 *   workout      f0a10009-...   read / notify
 *   history      f0a1000a-...   notify (sync replies, see opcode 0x0E)
 *   wellness     f0a1000b-...   read / notify
 */
#define RING_UUID_BASE(x)	BT_UUID_128_ENCODE(0xf0a10000 | (x), 0x1e5c, \
						   0x4a2b, 0x8d3f, \
						   0x9c7b6e5a4d21)

static const struct bt_uuid_128 uuid_svc =
	BT_UUID_INIT_128(RING_UUID_BASE(0));
static const struct bt_uuid_128 uuid_status =
	BT_UUID_INIT_128(RING_UUID_BASE(1));
static const struct bt_uuid_128 uuid_ppg =
	BT_UUID_INIT_128(RING_UUID_BASE(2));
static const struct bt_uuid_128 uuid_imu =
	BT_UUID_INIT_128(RING_UUID_BASE(3));
static const struct bt_uuid_128 uuid_control =
	BT_UUID_INIT_128(RING_UUID_BASE(4));
static const struct bt_uuid_128 uuid_activity =
	BT_UUID_INIT_128(RING_UUID_BASE(5));
static const struct bt_uuid_128 uuid_hrv =
	BT_UUID_INIT_128(RING_UUID_BASE(6));
static const struct bt_uuid_128 uuid_gsr =
	BT_UUID_INIT_128(RING_UUID_BASE(7));
static const struct bt_uuid_128 uuid_spo2 =
	BT_UUID_INIT_128(RING_UUID_BASE(8));
static const struct bt_uuid_128 uuid_workout =
	BT_UUID_INIT_128(RING_UUID_BASE(9));
static const struct bt_uuid_128 uuid_history =
	BT_UUID_INIT_128(RING_UUID_BASE(0xa));
static const struct bt_uuid_128 uuid_wellness =
	BT_UUID_INIT_128(RING_UUID_BASE(0xb));

static atomic_t connected_count;
static atomic_t status_subscribed;
static atomic_t ppg_subscribed;
static atomic_t imu_subscribed;
static atomic_t activity_subscribed;
static atomic_t hrv_subscribed;
static atomic_t gsr_subscribed;
static atomic_t spo2_subscribed;
static atomic_t workout_subscribed;
static atomic_t history_subscribed;
static atomic_t wellness_subscribed;
static atomic_t ppg_stream_on;
static atomic_t imu_stream_on;
static atomic_t gsr_stream_on;
static atomic_t spo2_mode_on;

/*
 * The one connection we allow (CONFIG_BT_MAX_CONN=1), held with a
 * reference so it cannot be freed while another thread is using it.
 * Written from the BT RX thread in connected()/disconnected(), read from
 * the main loop in ble_publish_ppg(), so every access goes through the
 * spinlock and hands back its own reference.
 */
static struct k_spinlock conn_lock;
static struct bt_conn *active_conn;

/*
 * latest_status is written whole by the main loop through
 * ble_publish_status() and read by the BT RX thread in read_status(). A
 * 20-byte struct is not copied atomically, so an unlocked GATT read could
 * return half of one sample and half of the next -- a packet the app has no
 * way to recognise as torn. Its own lock rather than conn_lock, because the
 * two protect unrelated things and sharing one would only widen both.
 *
 * Nothing calls into the BT stack while holding it: read_status() takes a
 * stack copy under the lock and hands bt_gatt_attr_read() the copy.
 */
static struct k_spinlock status_lock;
static struct ring_status latest_status;

/* Same tear-safety reasoning as latest_status, own lock for the same
 * reason: status and activity are unrelated and sharing one lock would
 * only widen both.
 */
static struct k_spinlock activity_lock;
static struct ring_activity latest_activity;

/* Same pattern again: own lock, tear-safe snapshot for the GATT read. */
static struct k_spinlock hrv_lock;
static struct ring_hrv latest_hrv;

static struct k_spinlock spo2_lock;
static struct ring_spo2 latest_spo2;

static struct k_spinlock workout_lock;
static struct ring_workout latest_workout;

static struct k_spinlock wellness_lock;
static struct ring_wellness latest_wellness = {
	.temp_c100 = INT16_MIN,
	.baseline_c100 = INT16_MIN,
	.skin_c100 = INT16_MIN,
};

static const struct ble_control_cbs *control_cbs;
static uint32_t ppg_seq;
static uint32_t gsr_seq;

/* --- advertising ------------------------------------------------------- */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HRS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/*
 * Both the initial start and the restart after a disconnect go through here,
 * so the two can never drift apart in the ad/sd they use.
 *
 * BT_LE_ADV_CONN_FAST_1 sets BT_LE_ADV_OPT_CONN, and the host stops the
 * advertiser as soon as a connection forms. Zephyr 4.4 has no auto-resume --
 * bt_le_adv_resume() no longer exists anywhere in the tree -- so without an
 * explicit restart the ring advertises exactly once per boot and is
 * invisible for ever after the first phone walks away.
 */
/*
 * Advertising power: fast for CONFIG_RING_ADV_SLOW_AFTER_S after a start,
 * then slow. See ble_service(), which does the switch on the main loop.
 */
#define ADV_IDLE	0
#define ADV_FAST	1
#define ADV_SLOW	2

static atomic_t adv_mode;
static atomic_t adv_fast_since_s;	/* uptime, seconds */

static int adv_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd,
				  ARRAY_SIZE(sd));

	if (!err) {
		atomic_set(&adv_fast_since_s, (atomic_val_t)(k_uptime_get() / 1000));
		atomic_set(&adv_mode, ADV_FAST);
	}

	return err;
}

/* --- GATT -------------------------------------------------------------- */

static ssize_t read_status(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	struct ring_status snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&status_lock);
	snapshot = latest_status;
	k_spin_unlock(&status_lock, key);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &snapshot,
				 sizeof(snapshot));
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	atomic_set(&status_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static void ppg_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	atomic_set(&ppg_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static void imu_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	atomic_set(&imu_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static ssize_t read_activity(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	struct ring_activity snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&activity_lock);
	snapshot = latest_activity;
	k_spin_unlock(&activity_lock, key);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &snapshot,
				 sizeof(snapshot));
}

static void activity_ccc_changed(const struct bt_gatt_attr *attr,
				 uint16_t value)
{
	atomic_set(&activity_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static ssize_t read_hrv(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	struct ring_hrv snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&hrv_lock);
	snapshot = latest_hrv;
	k_spin_unlock(&hrv_lock, key);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &snapshot,
				 sizeof(snapshot));
}

static void hrv_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	atomic_set(&hrv_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static void gsr_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	atomic_set(&gsr_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static ssize_t read_spo2(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	struct ring_spo2 snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&spo2_lock);
	snapshot = latest_spo2;
	k_spin_unlock(&spo2_lock, key);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &snapshot,
				 sizeof(snapshot));
}

static void spo2_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	atomic_set(&spo2_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static ssize_t read_workout(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	struct ring_workout snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&workout_lock);
	snapshot = latest_workout;
	k_spin_unlock(&workout_lock, key);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &snapshot,
				 sizeof(snapshot));
}

static void workout_ccc_changed(const struct bt_gatt_attr *attr,
				uint16_t value)
{
	atomic_set(&workout_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static void history_ccc_changed(const struct bt_gatt_attr *attr,
				uint16_t value)
{
	atomic_set(&history_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static ssize_t read_wellness(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	struct ring_wellness snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&wellness_lock);
	snapshot = latest_wellness;
	k_spin_unlock(&wellness_lock, key);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &snapshot,
				 sizeof(snapshot));
}

static void wellness_ccc_changed(const struct bt_gatt_attr *attr,
				 uint16_t value)
{
	atomic_set(&wellness_subscribed, value == BT_GATT_CCC_NOTIFY);
}

static void history_sync_request(uint32_t from_seq);

/*
 * Control opcodes, see APP_INTEGRATION.md:
 *   0x01 <u8 on>                     raw PPG streaming
 *   0x02 <u8 on>                     IMU streaming
 *   0x03                             start a measurement window now
 *   0x04 <u16 window_s> <u16 period_s>  set duty cycle
 *   0x05                             clear every bond
 *   0x06 <u16 weight_kg_x10>         set body weight for calorie estimate
 *   0x07                             reset steps/sleep/calories counters
 *   0x08 <u8 on>                     raw GSR streaming
 *   0x09 <u8 on>                     SpO2 mode (red+IR instead of green)
 *   0x0A <u8 age> <u8 sex>           age and sex for workout calories
 *   0x0B <u8 type>                   start a workout (0 = stop)
 *   0x0C                             power off (PMIC ship mode)
 *   0x0D <u32 unix_s>                set the wall clock
 *   0x0E <u32 from_seq>              sync history from this seq
 *   0x10 <u8 pa> <u8 led_rge> <u8 adc_rge> <u8 avg_log2>
 *                                    tune the heart-rate PPG front end
 *   0x11 <i8 dbm>                    radio TX power on this connection
 *   0x12 <u16 night_min> <u16 day_min> <u16 window_s>
 *                                    spot reading schedule
 *   0x13 <u8 mode> <u8 rise_c10>     hot-flash detection
 *   0x14                             spot reading now
 */
static ssize_t write_control(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	/*
	 * A zero-length write is a length error, not an offset error. Both used
	 * to come back as ATT 0x07, which sends an app author looking at an
	 * offset it never set.
	 */
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case 0x01:
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		atomic_set(&ppg_stream_on, p[1] != 0);
		if (control_cbs && control_cbs->stream_ppg) {
			control_cbs->stream_ppg(p[1] != 0);
		}
		LOG_INF("control: PPG streaming %s", p[1] ? "on" : "off");
		break;

	case 0x02:
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		atomic_set(&imu_stream_on, p[1] != 0);
		if (control_cbs && control_cbs->stream_imu) {
			control_cbs->stream_imu(p[1] != 0);
		}
		LOG_INF("control: IMU streaming %s", p[1] ? "on" : "off");
		break;

	case 0x03:
		if (control_cbs && control_cbs->measure_now) {
			control_cbs->measure_now();
		}
		LOG_INF("control: measure now");
		break;

	case 0x04: {
		if (len < 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		uint16_t window_s = p[1] | (p[2] << 8);
		uint16_t period_s = p[3] | (p[4] << 8);

		if (control_cbs && control_cbs->set_duty) {
			control_cbs->set_duty(window_s, period_s);
		}
		LOG_INF("control: duty %u s every %u s", window_s, period_s);
		break;
	}

	case 0x05: {
		/*
		 * Clear bonds, so a ring is not owned for ever by whichever
		 * phone reached it first. CONFIG_BT_MAX_PAIRED is 1 and
		 * CONFIG_BT_KEYS_OVERWRITE_OLDEST is deliberately left off --
		 * silently evicting the owner's keys for any stranger who
		 * pairs is a worse failure than refusing the stranger -- so
		 * without this opcode the only way back was an SWD erase.
		 *
		 * This characteristic is BT_GATT_PERM_WRITE_ENCRYPT, so the
		 * write can only arrive over an encrypted link, which on a
		 * ring that has exactly one bond means the bonded owner. An
		 * unpaired phone in range gets ATT 0x0F and nothing else.
		 *
		 * Disconnect afterwards: the keys backing this very link are
		 * gone, so the peer must come back and pair again rather than
		 * keep talking on a bond that no longer exists.
		 */
		int unpair_rc = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

		if (unpair_rc) {
			LOG_ERR("control: clear bonds failed (%d)", unpair_rc);
		} else {
			LOG_WRN("control: bonds cleared -- the next connection "
				"has to pair again");
		}

		(void)bt_conn_disconnect(conn,
					 BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		break;
	}

	case 0x06: {
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		uint16_t weight_kg_x10 = p[1] | (p[2] << 8);

		if (control_cbs && control_cbs->set_weight) {
			control_cbs->set_weight(weight_kg_x10);
		}
		LOG_INF("control: body weight %u.%u kg", weight_kg_x10 / 10U,
			weight_kg_x10 % 10U);
		break;
	}

	case 0x07:
		if (control_cbs && control_cbs->reset_activity) {
			control_cbs->reset_activity();
		}
		LOG_INF("control: activity counters reset");
		break;

	case 0x08:
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		atomic_set(&gsr_stream_on, p[1] != 0);
		if (control_cbs && control_cbs->stream_gsr) {
			control_cbs->stream_gsr(p[1] != 0);
		}
		LOG_INF("control: GSR streaming %s", p[1] ? "on" : "off");
		break;

	case 0x09:
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		atomic_set(&spo2_mode_on, p[1] != 0);
		if (control_cbs && control_cbs->spo2_mode) {
			control_cbs->spo2_mode(p[1] != 0);
		}
		LOG_INF("control: SpO2 mode %s (heart rate %s)",
			p[1] ? "on" : "off", p[1] ? "pauses" : "resumes");
		break;

	case 0x0A:
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (control_cbs && control_cbs->set_body) {
			control_cbs->set_body(p[1], p[2]);
		}
		LOG_INF("control: age %u, sex code %u", p[1], p[2]);
		break;

	case 0x0B:
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (control_cbs && control_cbs->workout) {
			control_cbs->workout(p[1]);
		}
		LOG_INF("control: workout %s (type %u)",
			p[1] ? "start" : "stop", p[1]);
		break;

	case 0x0C:
		/*
		 * Ship mode: the PMIC disconnects the battery and the ring is
		 * off until a charger is plugged in. Deferred to the main loop
		 * like every other opcode, because the I2C write must not run
		 * on the BT RX thread.
		 */
		if (control_cbs && control_cbs->ship_mode) {
			control_cbs->ship_mode();
		}
		LOG_WRN("control: ship mode requested");
		break;

	case 0x0D: {
		if (len < 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		uint32_t unix_s = sys_get_le32(&p[1]);

		/* A spinlock and two words; fine on the RX thread. */
		history_set_time(unix_s);
		LOG_INF("control: clock set, unix %u", unix_s);
		break;
	}

	case 0x0E: {
		if (len < 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		/*
		 * The reply arrives as notifications on the history
		 * characteristic, so a request with nobody subscribed would
		 * be answered into the void. Say so instead.
		 */
		if (!atomic_get(&history_subscribed)) {
			return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
		}

		/*
		 * A 32-byte record plus its 2-byte packet header does not fit
		 * a notification at the default 23-byte MTU. Started anyway,
		 * the sync would end before its first record and send
		 * nothing at all; refuse it so the app knows to ask for a
		 * bigger MTU first.
		 */
		if (bt_gatt_get_mtu(conn) <
		    3U + 2U + sizeof(struct hist_record)) {
			return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
		}

		uint32_t from_seq = sys_get_le32(&p[1]);

		history_sync_request(from_seq);
		LOG_INF("control: history sync from seq %u", from_seq);
		break;
	}

	case 0x10:
		/* Test builds only: see CONFIG_RING_DIAG_OPCODES. */
		if (!IS_ENABLED(CONFIG_RING_DIAG_OPCODES)) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		/*
		 * Tuning, not a user setting: RAM only, so a reboot always
		 * comes back to the defaults. The range check is here, at
		 * the edge, so a refused value comes back to the app as an
		 * ATT error instead of vanishing into a log line. The LED
		 * range stops at 62 mA: peak current comes off a small cell
		 * through the boost, and nothing above that has been tried.
		 */
		if (len < 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (p[1] == 0 || p[2] > PPG_TUNE_LED_RGE_MAX ||
		    p[3] > PPG_TUNE_ADC_RGE_MAX ||
		    p[4] > PPG_TUNE_AVG_LOG2_MAX) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (control_cbs && control_cbs->ppg_tune) {
			control_cbs->ppg_tune(p[1], p[2], p[3], p[4]);
		}
		LOG_INF("control: PPG tune PA 0x%02x, LED range %u, ADC range "
			"%u, avg log2 %u", p[1], p[2], p[3], p[4]);
		break;

	case 0x11:
		if (!IS_ENABLED(CONFIG_RING_DIAG_OPCODES)) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		/* The nRF52833 transmits from -40 to +8 dBm. */
		if ((int8_t)p[1] < -40 || (int8_t)p[1] > 8) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (control_cbs && control_cbs->tx_power) {
			control_cbs->tx_power((int8_t)p[1]);
		}
		LOG_INF("control: TX power %d dBm requested", (int8_t)p[1]);
		break;

	case 0x12: {
		/*
		 * Spot reading schedule. A setting, not a diagnostic, so in
		 * every build -- but RAM only like the clock: an app that
		 * wants something other than the build's defaults sends it
		 * on each connection. Zero turns a kind off.
		 */
		if (len < 7) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		struct spot_config cfg = {
			.night_min = sys_get_le16(&p[1]),
			.day_min = sys_get_le16(&p[3]),
			.window_s = sys_get_le16(&p[5]),
		};

		/* Pure arithmetic; safe on the RX thread. */
		if (!spot_config_valid(&cfg)) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (control_cbs && control_cbs->spot_config) {
			control_cbs->spot_config(cfg.night_min, cfg.day_min,
						 cfg.window_s);
		}
		LOG_INF("control: spot readings every %u min asleep, %u min "
			"awake, %u s each", cfg.night_min, cfg.day_min,
			cfg.window_s);
		break;
	}

	case 0x13:
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (p[1] > THERMO_MODE_ALWAYS || p[2] < THERMO_RISE_MIN_C10 ||
		    p[2] > THERMO_RISE_MAX_C10) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (control_cbs && control_cbs->hot_flash_config) {
			control_cbs->hot_flash_config(p[1], p[2]);
		}
		LOG_INF("control: hot-flash detection mode %u, rise %u.%u C",
			p[1], p[2] / 10U, p[2] % 10U);
		break;

	case 0x14: {
		uint8_t reason = (len >= 2) ? p[1] : SPOT_REASON_UNSPECIFIED;

		if (reason > SPOT_REASON_BREATHING) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (control_cbs && control_cbs->spot_now) {
			control_cbs->spot_now(reason);
		}
		LOG_INF("control: spot reading now (reason %u)", reason);
		break;
	}

	default:
		LOG_WRN("control: unknown opcode 0x%02x", p[0]);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	return len;
}

/*
 * Every Ring Service attribute demands an encrypted link. The control
 * characteristic is the one that mattered most: with a plain
 * BT_GATT_PERM_WRITE anyone in range could write opcodes and force
 * measurement windows on someone else's ring. The standard HRS and BAS
 * are left open so off-the-shelf HR apps keep working.
 */
BT_GATT_SERVICE_DEFINE(ring_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),

	BT_GATT_CHARACTERISTIC(&uuid_status.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_status, NULL,
			       NULL),
	BT_GATT_CCC(status_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_ppg.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ppg_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_imu.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(imu_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_control.uuid,
			       BT_GATT_CHRC_WRITE |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_control,
			       NULL),

	BT_GATT_CHARACTERISTIC(&uuid_activity.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_activity, NULL,
			       NULL),
	BT_GATT_CCC(activity_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_hrv.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_hrv, NULL, NULL),
	BT_GATT_CCC(hrv_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_gsr.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(gsr_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_spo2.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_spo2, NULL, NULL),
	BT_GATT_CCC(spo2_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_workout.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_workout, NULL,
			       NULL),
	BT_GATT_CCC(workout_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_history.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(history_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_wellness.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_wellness, NULL,
			       NULL),
	BT_GATT_CCC(wellness_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
);

/*
 * Attribute indices into the table above. BT_GATT_CHARACTERISTIC expands to
 * TWO attributes -- the Characteristic Declaration and then the Value -- and
 * BT_GATT_CCC to one more, so each notifiable characteristic occupies three
 * slots and these constants point at the first of each:
 *
 *   0  primary service
 *   1  status   decl   2 value   3 CCC
 *   4  ppg      decl   5 value   6 CCC
 *   7  imu      decl   8 value   9 CCC
 *  10  control  decl  11 value        (write-only, no CCC)
 *  12  activity decl  13 value  14 CCC
 *  15  hrv      decl  16 value  17 CCC
 *  18  gsr      decl  19 value  20 CCC
 *  21  spo2     decl  22 value  23 CCC
 *  24  workout  decl  25 value  26 CCC
 *  27  history  decl  28 value  29 CCC
 *  30  wellness decl  31 value  32 CCC
 *
 * Pointing at the declaration rather than the value is deliberate and is
 * what bt_gatt_notify() documents: "The attribute object on the parameters
 * can be the so called Characteristic Declaration [...] or the
 * Characteristic Value Declaration". It detects BT_UUID_GATT_CHRC, checks
 * the declaration carries BT_GATT_CHRC_NOTIFY, and resolves the value
 * handle itself. Any characteristic added here without BT_GATT_CHRC_NOTIFY
 * would therefore fail with -EINVAL rather than notify the wrong handle.
 */
#define ATTR_STATUS	(&ring_svc.attrs[1])
#define ATTR_PPG	(&ring_svc.attrs[4])
#define ATTR_IMU	(&ring_svc.attrs[7])
#define ATTR_ACTIVITY	(&ring_svc.attrs[12])
#define ATTR_HRV	(&ring_svc.attrs[15])
#define ATTR_GSR	(&ring_svc.attrs[18])
#define ATTR_SPO2	(&ring_svc.attrs[21])
#define ATTR_WORKOUT	(&ring_svc.attrs[24])
#define ATTR_HISTORY	(&ring_svc.attrs[27])
#define ATTR_WELLNESS	(&ring_svc.attrs[30])

/* --- connection tracking ----------------------------------------------- */

/*
 * Install @new as the active connection and hand back whatever was there,
 * with its reference, for the caller to release outside the lock.
 */
static struct bt_conn *conn_swap(struct bt_conn *new)
{
	k_spinlock_key_t key;
	struct bt_conn *old;

	key = k_spin_lock(&conn_lock);
	old = active_conn;
	active_conn = new;
	k_spin_unlock(&conn_lock, key);

	return old;
}

/*
 * A referenced handle on the active connection, or NULL when nobody is
 * connected. The caller must bt_conn_unref() it. Taking the reference
 * under the lock is what stops disconnected() freeing the object while a
 * caller is still holding the pointer.
 */
static struct bt_conn *conn_get(void)
{
	k_spinlock_key_t key;
	struct bt_conn *conn;

	key = k_spin_lock(&conn_lock);
	conn = active_conn ? bt_conn_ref(active_conn) : NULL;
	k_spin_unlock(&conn_lock, key);

	return conn;
}

/* --- notifications that never block ------------------------------------ */

/*
 * bt_gatt_notify() called from any thread but the system work queue waits
 * K_FOREVER for an ATT buffer -- see bt_att_chan_create_pdu() in att.c.
 * Every publisher in this file runs on the main loop, which is also the
 * only thing that feeds the watchdog. So a link that stopped draining -- a
 * weak antenna, a phone walking out of range -- parked the main loop inside
 * a notification until the watchdog reset the ring. Seen on hardware
 * 2026-10-02: with a PC streaming, reboots clustered on link drops.
 *
 * There are CONFIG_BT_ATT_TX_COUNT ATT buffers. Keep at most one fewer in
 * flight from here -- counted out before bt_gatt_notify_cb(), back in by its
 * sent callback -- and drop a notification instead of waiting for a buffer.
 * The spare is for the senders this file does not count: BAS, and ATT
 * responses from the RX thread.
 *
 * The stack does not call the sent callback for a PDU it discards on
 * disconnect or after a TX error (att_on_sent_cb()), so the count is reset
 * on every connect and disconnect rather than trusted to drain to zero.
 */
#define NOTIFY_INFLIGHT_MAX	(CONFIG_BT_ATT_TX_COUNT - 1)

static atomic_t notify_inflight;

static void notify_sent(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);

	/* Never below zero: a late callback can land after a reset. */
	for (;;) {
		atomic_val_t v = atomic_get(&notify_inflight);

		if (v <= 0 || atomic_cas(&notify_inflight, v, v - 1)) {
			return;
		}
	}
}

/*
 * Notify the active connection without ever blocking. -ENOBUFS means the
 * notification was dropped because the link is not keeping up; the stream
 * publishers still advance their seq, so a client sees the gap.
 */
static int notify_nowait(const struct bt_gatt_attr *attr, const void *data,
			 uint16_t len)
{
	struct bt_gatt_notify_params params = {
		.attr = attr,
		.data = data,
		.len = len,
		.func = notify_sent,
	};
	struct bt_conn *conn;
	int err;

	if (atomic_inc(&notify_inflight) >= NOTIFY_INFLIGHT_MAX) {
		notify_sent(NULL, NULL);
		return -ENOBUFS;
	}

	conn = conn_get();
	if (!conn) {
		notify_sent(NULL, NULL);
		return -ENOTCONN;
	}

	/* Not subscribed or not encrypted comes back as an error before any
	 * buffer is taken, so this releases the slot in every failure case.
	 */
	err = bt_gatt_notify_cb(conn, &params);
	bt_conn_unref(conn);

	if (err) {
		notify_sent(NULL, NULL);
	}

	return err;
}

/* --- history sync ------------------------------------------------------ */

/*
 * Replies to opcode 0x0E, as notifications on the history characteristic:
 *
 *   0x03 <u8 n> <n x 32-byte hist_record>    records, oldest first
 *   0x02 <20-byte summary>                    end of sync, sent last
 *
 * 0x01 carried v1's 16-byte records (v0.6-v0.7). The type changed with
 * the layout so an app that only knows 0x01 skips v2 records instead of
 * misreading them.
 *
 * Run from a work item rather than the main loop. The main loop sleeps up
 * to 200 ms a pass while idle, which would cap a full 19-day sync at
 * minutes; this sends as fast as the ATT buffers drain. notify_nowait()
 * makes it safe on the system work queue too: a full link reschedules the
 * work instead of blocking it.
 *
 * Unlike the live streams, a batch that finds no free buffer is NOT
 * dropped. It is kept and sent again, so a sync never has holes; the
 * records are in flash and nothing is lost by waiting for them.
 */
#define HIST_PKT_RECORDS	0x03
#define HIST_PKT_END		0x02
#define HIST_PKT_END_LEN	20
/* 2 + 7 x 32 = 226 bytes, inside the 244 a 247-byte MTU carries. */
#define HIST_BATCH_MAX		7
#define HIST_RETRY_MS		10
/* Packets per work run before yielding the queue to everyone else. */
#define HIST_BURST		4

static void history_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(history_work, history_work_fn);

static atomic_t history_request;	/* a new sync was asked for */
static atomic_t history_from;		/* its from_seq */
static atomic_t history_running;

/* Work item only. */
static struct hist_iter history_it;
static uint8_t history_pkt[2 + HIST_BATCH_MAX * sizeof(struct hist_record)];
static uint16_t history_pkt_len;

static void history_sync_request(uint32_t from_seq)
{
	atomic_set(&history_from, (atomic_val_t)from_seq);
	atomic_set(&history_request, 1);
	(void)k_work_reschedule(&history_work, K_NO_WAIT);
}

/* Records that fit one notification at the current MTU; 0 = no link. */
static uint8_t history_batch_max(void)
{
	struct bt_conn *conn = conn_get();
	uint16_t mtu;

	if (!conn) {
		return 0;
	}

	mtu = bt_gatt_get_mtu(conn);
	bt_conn_unref(conn);

	if (mtu < 3U + 2U + sizeof(struct hist_record)) {
		return 0;
	}

	return (uint8_t)MIN((mtu - 3U - 2U) / sizeof(struct hist_record),
			    HIST_BATCH_MAX);
}

/*
 * The end-of-sync summary. Everything a phone needs to put a wall-clock
 * time on records that were stamped with uptime because nobody had set
 * the clock yet -- see APP_INTEGRATION.md section 16.
 */
static void history_pkt_end(void)
{
	uint32_t unix_s = 0;

	(void)history_unix_now(&unix_s);	/* left at 0 when unknown */

	history_pkt[0] = HIST_PKT_END;
	sys_put_le32(history_it.oldest_seq, &history_pkt[1]);
	sys_put_le32(history_it.last_seq, &history_pkt[5]);
	sys_put_le32((uint32_t)(k_uptime_get() / 1000), &history_pkt[9]);
	sys_put_le32(unix_s, &history_pkt[13]);
	history_pkt[17] = history_boot_id();
	sys_put_le16(CONFIG_RING_HISTORY_EPOCH_S, &history_pkt[18]);
	history_pkt_len = HIST_PKT_END_LEN;
}

static void history_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_cas(&history_request, 1, 0)) {
		/* A new request restarts any sync already in progress. */
		history_iter_begin(&history_it,
				   (uint32_t)atomic_get(&history_from));
		history_pkt_len = 0;
		atomic_set(&history_running, 1);
	}

	if (!atomic_get(&history_running)) {
		return;
	}

	if (!ble_is_connected() || !atomic_get(&history_subscribed)) {
		atomic_set(&history_running, 0);
		return;
	}

	for (int i = 0; i < HIST_BURST; i++) {
		if (history_pkt_len == 0) {
			uint8_t max = history_batch_max();
			uint8_t n = 0;
			struct hist_record r;

			if (max == 0) {
				atomic_set(&history_running, 0);
				return;
			}

			while (n < max && history_iter_next(&history_it, &r)) {
				memcpy(&history_pkt[2 + n * sizeof(r)], &r,
				       sizeof(r));
				n++;
			}

			if (n > 0) {
				history_pkt[0] = HIST_PKT_RECORDS;
				history_pkt[1] = n;
				history_pkt_len = 2 + n * sizeof(r);
			} else {
				history_pkt_end();
			}
		}

		int err = notify_nowait(ATTR_HISTORY, history_pkt,
					history_pkt_len);

		if (err == -ENOBUFS) {
			/* Keep the batch; try again once a buffer drains. */
			(void)k_work_reschedule(&history_work,
						K_MSEC(HIST_RETRY_MS));
			return;
		}

		if (err) {
			LOG_WRN("history: sync stopped (%d)", err);
			atomic_set(&history_running, 0);
			return;
		}

		if (history_pkt[0] == HIST_PKT_END) {
			LOG_INF("history: sync done, oldest seq %u newest %u",
				history_it.oldest_seq, history_it.last_seq);
			history_pkt_len = 0;
			atomic_set(&history_running, 0);
			return;
		}

		history_pkt_len = 0;
	}

	(void)k_work_reschedule(&history_work, K_NO_WAIT);
}

/* --- connection parameters --------------------------------------------- */

/*
 * The ring used to accept whatever interval the central chose -- 15-30 ms
 * from a phone or a PC -- and keep it for the whole connection, waking the
 * radio 30-60 times a second at +8 dBm to exchange empty packets. With
 * CONFIG_RING_CONN_PARAM_MGMT, ble_service() asks instead for:
 *
 *   IDLE  75-105 ms, latency 4: the ring may sleep through four events
 *         with nothing to send, so it wakes about every half second. Live
 *         Status and heart rate (one notification a second) still go out
 *         at the next event, latency is skipped whenever data is queued.
 *   FAST  15-30 ms, no latency: while PPG, IMU or GSR streams, a
 *         history sync runs, or a firmware upload is under way (ota.h),
 *         which need the throughput.
 *
 * Both fit Apple's accessory rules: interval max at least 15 ms above min,
 * latency at most 30, max x (latency + 1) at most 2 s, and the 4 s
 * supervision timeout over three times that. The central can refuse;
 * le_param_updated() logs what it granted.
 */
#define CONN_IDLE_PARAM		BT_LE_CONN_PARAM(60, 84, 4, 400)
#define CONN_FAST_PARAM		BT_LE_CONN_PARAM(12, 24, 0, 400)
/* Let discovery and pairing run at the central's pace first. */
#define CONN_SETTLE_S		10
/* Do not re-ask more often than this, whatever changes. */
#define CONN_REQUEST_GAP_S	5

#define CONN_MODE_NONE		0
#define CONN_MODE_IDLE		1
#define CONN_MODE_FAST		2

static atomic_t conn_since_s;		/* uptime at connect, seconds */
static atomic_t conn_mode;		/* last mode requested */
static int64_t conn_last_request_ms;	/* main loop only */

static void conn_params_on_connect(void)
{
	atomic_set(&conn_since_s, (atomic_val_t)(k_uptime_get() / 1000));
	atomic_set(&conn_mode, CONN_MODE_NONE);
}

/* --- connection callbacks ---------------------------------------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct bt_conn *old;

	if (err) {
		LOG_ERR("connection failed (0x%02x)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	atomic_inc(&connected_count);
	/* The host stopped the advertiser to make this connection. */
	atomic_set(&adv_mode, ADV_IDLE);
	conn_params_on_connect();
	atomic_set(&notify_inflight, 0);	/* see notify_nowait() */

	old = conn_swap(bt_conn_ref(conn));

	if (old) {
		/* Should not happen with CONFIG_BT_MAX_CONN=1. */
		LOG_WRN("replacing a connection that never disconnected");
		bt_conn_unref(old);
	}

	LOG_INF("connected to %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn *old;

	old = conn_swap(NULL);
	atomic_dec(&connected_count);
	/* Discarded PDUs never call notify_sent(); see notify_nowait(). */
	atomic_set(&notify_inflight, 0);

	if (old) {
		bt_conn_unref(old);
	}

	/* Streaming is expensive; never leave it running for a gone phone.
	 * The GSR one also holds the analog front end powered, so leaving it
	 * on would defeat the idle power model indefinitely.
	 */
	atomic_set(&ppg_stream_on, 0);
	atomic_set(&imu_stream_on, 0);
	atomic_set(&gsr_stream_on, 0);

	/*
	 * SpO2 mode too. A phone that enables it and walks away would
	 * otherwise leave the ring running red and IR with no heart rate,
	 * no HRS notifications and no RMSSD accumulating, for the rest of
	 * the boot.
	 */
	atomic_set(&spo2_mode_on, 0);

	/* A sync in progress dies with the link; the phone resumes it from
	 * the last seq it stored.
	 */
	atomic_set(&history_request, 0);
	atomic_set(&history_running, 0);
	(void)k_work_cancel_delayable(&history_work);

	LOG_INF("disconnected (reason 0x%02x)", reason);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_WRN("security with %s failed at level %u (err %d)", addr,
			level, err);
	} else {
		LOG_INF("security with %s is level %u", addr, level);
	}
}

/*
 * The connection object has been freed, which is the point at which the host
 * will accept a new advertiser. disconnected() is too early: the object is
 * still alive there and bt_le_adv_start() can come back -ENOMEM. This is the
 * restart path the BT_LE_ADV_OPT_CONN documentation points at.
 */
static void recycled(void)
{
	int err = adv_start();

	if (err) {
		LOG_ERR("advertising failed to restart (%d) -- the ring is "
			"invisible to every phone until it resets", err);
		return;
	}

	LOG_INF("advertising restarted");
}

/* What the central actually granted, whichever side asked. */
static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);

	/* interval is in 1.25 ms units, timeout in 10 ms units */
	LOG_INF("connection interval %u.%02u ms, latency %u, timeout %u ms",
		(interval * 125U) / 100U, (interval * 125U) % 100U, latency,
		timeout * 10U);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.recycled = recycled,
	.le_param_updated = le_param_updated,
};

/* --- pairing ----------------------------------------------------------- */

/*
 * The ring has no display and no keypad, so the only association model
 * available is Just Works: no MITM protection, but the link is encrypted
 * and the bond survives a reboot. Registering neither passkey_display nor
 * passkey_entry is what tells the SMP layer our IO capability is
 * NoInputNoOutput -- see get_io_capa() in subsys/bluetooth/host/smp.c.
 */
static void auth_pairing_confirm(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("pairing requested by %s, accepting (Just Works)", addr);

	(void)bt_conn_auth_pairing_confirm(conn);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_WRN("pairing with %s cancelled", addr);
}

static const struct bt_conn_auth_cb auth_cb = {
	.pairing_confirm = auth_pairing_confirm,
	.cancel = auth_cancel,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("paired with %s, bonded %s", addr, bonded ? "yes" : "no");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_WRN("pairing with %s failed (reason %d)", addr, reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* --- API --------------------------------------------------------------- */

bool ble_is_connected(void)
{
	return atomic_get(&connected_count) > 0;
}

bool ble_ppg_streaming(void)
{
	return atomic_get(&ppg_stream_on) && atomic_get(&ppg_subscribed);
}

bool ble_imu_streaming(void)
{
	return atomic_get(&imu_stream_on) && atomic_get(&imu_subscribed);
}

bool ble_gsr_streaming(void)
{
	return atomic_get(&gsr_stream_on) && atomic_get(&gsr_subscribed);
}

bool ble_gsr_stream_requested(void)
{
	return atomic_get(&gsr_stream_on) != 0;
}

bool ble_spo2_mode(void)
{
	return atomic_get(&spo2_mode_on) != 0;
}

void ble_set_control_cbs(const struct ble_control_cbs *cbs)
{
	control_cbs = cbs;
}

int ble_start(void)
{
	int err;

	err = bt_conn_auth_cb_register(&auth_cb);
	if (err) {
		LOG_ERR("auth callbacks rejected (%d)", err);
		return err;
	}

	err = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (err) {
		LOG_ERR("auth info callbacks rejected (%d)", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

#if defined(CONFIG_BT_SETTINGS)
	/*
	 * Bonds live in the settings partition. This has to run after
	 * bt_enable() and before advertising, or a phone that is already
	 * bonded reconnects to a ring that has forgotten its keys.
	 *
	 * It is load-bearing for far more than the bonds. With
	 * CONFIG_BT_SETTINGS the stack is NOT finished when bt_enable()
	 * returns 0 -- hci_core.c logs "No ID address. App must call
	 * settings_load()" and defers the rest to the settings commit
	 * handler. A failure here therefore leaves no identity address, and
	 * bt_le_adv_start() below fails too. Treating it as a warning meant
	 * a ring that logged one line about lost bonds and then never
	 * advertised again, with nothing saying why.
	 */
	err = settings_load();
	if (err) {
		LOG_ERR("settings_load failed (%d) -- BLE stack not finalised, "
			"no advertising, bonds lost", err);
		return err;
	}
#endif

	err = adv_start();
	if (err) {
		LOG_ERR("advertising failed to start (%d)", err);
		return err;
	}

	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	char addr_str[BT_ADDR_LE_STR_LEN] = "?";

	bt_id_get(addrs, &count);
	if (count > 0) {
		bt_addr_le_to_str(&addrs[0], addr_str, sizeof(addr_str));
	}

	LOG_INF("firmware revision %s", CONFIG_BT_DIS_FW_REV_STR);
	LOG_INF("advertising as %s, addr %s", CONFIG_BT_DEVICE_NAME, addr_str);
	LOG_INF("services: Heart Rate, Battery, Device Information, Ring");
	LOG_INF("Ring Service requires an encrypted link (Just Works pairing)");

	return 0;
}

void ble_service(int64_t now_ms)
{
	/* --- advertising: fast, then slow --- */
	if (CONFIG_RING_ADV_SLOW_AFTER_S > 0 && !ble_is_connected() &&
	    atomic_get(&adv_mode) == ADV_FAST &&
	    (now_ms / 1000) - atomic_get(&adv_fast_since_s) >=
	    CONFIG_RING_ADV_SLOW_AFTER_S) {
		/*
		 * Stop, then restart slow. A central can connect in the gap;
		 * then the slow start fails harmlessly and recycled() restarts
		 * fast when that connection ends.
		 */
		int err = bt_le_adv_stop();

		if (!err) {
			err = bt_le_adv_start(
				BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
						BT_GAP_ADV_SLOW_INT_MIN,
						BT_GAP_ADV_SLOW_INT_MAX, NULL),
				ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
		}

		if (!err) {
			atomic_set(&adv_mode, ADV_SLOW);
			LOG_INF("advertising slowed to 1-1.2 s");
		} else if (!ble_is_connected()) {
			LOG_WRN("slow advertising failed (%d), staying fast", err);
			if (adv_start() != 0) {
				atomic_set(&adv_mode, ADV_IDLE);
				LOG_ERR("advertising failed to restart");
			}
		} else {
			atomic_set(&adv_mode, ADV_IDLE);
		}
	}

	/* --- connection interval: idle or fast --- */
	if (!IS_ENABLED(CONFIG_RING_CONN_PARAM_MGMT) || !ble_is_connected()) {
		return;
	}

	bool want_fast = ble_ppg_streaming() || ble_imu_streaming() ||
			 ble_gsr_streaming() ||
			 atomic_get(&history_running) ||
			 atomic_get(&history_request) || ota_active();
	atomic_val_t want = want_fast ? CONN_MODE_FAST : CONN_MODE_IDLE;

	if (want == atomic_get(&conn_mode)) {
		return;
	}
	/* Idle waits for discovery to settle; fast never waits. */
	if (!want_fast &&
	    (now_ms / 1000) - atomic_get(&conn_since_s) < CONN_SETTLE_S) {
		return;
	}
	if (conn_last_request_ms != 0 &&
	    now_ms - conn_last_request_ms < CONN_REQUEST_GAP_S * 1000 &&
	    !want_fast) {
		return;
	}

	struct bt_conn *conn = conn_get();

	if (!conn) {
		return;
	}

	int err = bt_conn_le_param_update(conn, want_fast ? CONN_FAST_PARAM
							  : CONN_IDLE_PARAM);

	bt_conn_unref(conn);
	conn_last_request_ms = now_ms;

	if (err) {
		LOG_WRN("connection parameter request failed (%d)", err);
		return;
	}

	atomic_set(&conn_mode, want);
	LOG_INF("asked for %s connection parameters",
		want_fast ? "fast" : "idle");
}

int ble_set_tx_power(int8_t dbm, int8_t *selected)
{
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	struct bt_conn *conn;
	uint16_t handle;
	int err;

	conn = conn_get();
	if (!conn) {
		return -ENOTCONN;
	}

	err = bt_hci_get_conn_handle(conn, &handle);
	bt_conn_unref(conn);
	if (err) {
		return err;
	}

	buf = bt_hci_cmd_alloc(K_SECONDS(1));
	if (!buf) {
		return -ENOBUFS;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle_type = BT_HCI_VS_LL_HANDLE_TYPE_CONN;
	cp->handle = sys_cpu_to_le16(handle);
	cp->tx_power_level = dbm;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err) {
		return err;
	}

	rp = (void *)rsp->data;
	*selected = rp->selected_tx_power;
	net_buf_unref(rsp);

	return 0;
}

void ble_notify_hr(uint16_t bpm, const uint16_t *rr_ms, uint8_t n)
{
	/*
	 * What bt_hrs_notify() sends -- flags 0x06 (8-bit rate, sensor
	 * contact supported and detected), then the rate -- plus RR
	 * intervals when there are any (flag 0x10), and through
	 * notify_nowait(): bt_hrs_notify() calls bt_gatt_notify(), which can
	 * block the main loop like any other notification.
	 */
	static const struct bt_gatt_attr *hrm_attr;
	uint8_t hrm[2 + 2 * BLE_HRM_RR_MAX];
	size_t len = 2;

	n = MIN(n, BLE_HRM_RR_MAX);
	hrm[0] = n ? 0x16 : 0x06;
	hrm[1] = (uint8_t)MIN(bpm, UINT8_MAX);
	for (uint8_t i = 0; i < n; i++) {
		/* The SIG unit is 1/1024 s. */
		sys_put_le16((uint16_t)(((uint32_t)rr_ms[i] * 1024U + 500U) /
					1000U), &hrm[len]);
		len += 2;
	}

	if (!ble_is_connected()) {
		return;
	}

	if (!hrm_attr) {
		hrm_attr = bt_gatt_find_by_uuid(NULL, 0,
						BT_UUID_HRS_MEASUREMENT);
		if (!hrm_attr) {
			return;
		}
	}

	(void)notify_nowait(hrm_attr, hrm, len);
}

void ble_notify_battery(uint8_t percent)
{
	if (percent > 100) {
		percent = 100;
	}

	/*
	 * bt_bas_set_battery_level() notifies through bt_gatt_notify(), so it
	 * can block. It is the one sender notify_nowait() does not count, so
	 * call it only while a buffer is certainly free; a skipped update is
	 * retried by the next slow_sense() pass. Off a connection the count
	 * is 0 and this only stores the value for reads.
	 */
	if (atomic_get(&notify_inflight) >= NOTIFY_INFLIGHT_MAX) {
		return;
	}

	(void)bt_bas_set_battery_level(percent);
}

void ble_publish_status(const struct ring_status *status)
{
	struct ring_status snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&status_lock);
	latest_status = *status;
	snapshot = latest_status;
	k_spin_unlock(&status_lock, key);

	if (!ble_is_connected() || !atomic_get(&status_subscribed)) {
		return;
	}

	/*
	 * Notify from the snapshot, not from latest_status: the lock is
	 * released before this call because the BT stack must never be
	 * entered with a spinlock held.
	 */
	(void)notify_nowait(ATTR_STATUS, &snapshot, sizeof(snapshot));
}

/*
 * Samples per PPG notification once the MTU allows it. The main loop drains
 * the FIFO every 20 ms, two samples at a time, and one notification per
 * drain is 50 a second -- more than three ATT buffers carry over a marginal
 * link, so most were dropped (or, before notify_nowait(), waited on). Ten
 * is 100 ms of latency for a fifth of the packets.
 */
#define PPG_BATCH_SAMPLES	10

/* Main loop only, like every caller of ble_publish_ppg(), so no lock. */
static uint32_t ppg_pending[40];
static uint8_t ppg_pending_n;

void ble_publish_ppg(const uint32_t *samples, uint8_t count)
{
	/*
	 * seq (u32) + count (u8) + samples. Sized for a 247-byte MTU; the
	 * phone should request one or it will be capped at 4 samples per
	 * notification by the 23-byte default.
	 */
	uint8_t buf[5 + ARRAY_SIZE(ppg_pending) * sizeof(uint32_t)];
	struct bt_conn *conn;
	uint16_t mtu;
	uint16_t mtu_payload;
	uint8_t max_samples;
	uint8_t n;

	if (!ble_ppg_streaming()) {
		ppg_pending_n = 0;
		return;
	}

	/*
	 * bt_gatt_get_mtu(NULL) is not a "use the default" shorthand: it
	 * reaches att_get(), which dereferences conn->state and faults. There
	 * is no MTU to ask about when nobody is connected, so leave.
	 */
	conn = conn_get();
	if (!conn) {
		ppg_pending_n = 0;
		return;
	}

	mtu = bt_gatt_get_mtu(conn);
	bt_conn_unref(conn);

	if (mtu < 3) {
		return;
	}

	mtu_payload = mtu - 3;
	max_samples = (mtu_payload > 5) ? (mtu_payload - 5) / sizeof(uint32_t)
				        : 0;
	if (max_samples > ARRAY_SIZE(ppg_pending)) {
		max_samples = ARRAY_SIZE(ppg_pending);
	}
	if (max_samples == 0) {
		return;
	}

	/* Queue the new samples, dropping the oldest if a burst overflows. */
	for (uint8_t i = 0; i < count; i++) {
		if (ppg_pending_n == ARRAY_SIZE(ppg_pending)) {
			memmove(&ppg_pending[0], &ppg_pending[1],
				(ARRAY_SIZE(ppg_pending) - 1) *
				sizeof(ppg_pending[0]));
			ppg_pending_n--;
		}
		ppg_pending[ppg_pending_n++] = samples[i];
	}

	if (ppg_pending_n < MIN(PPG_BATCH_SAMPLES, max_samples)) {
		return;
	}

	n = MIN(ppg_pending_n, max_samples);

	/*
	 * seq advances whether or not the notification goes out, so a batch
	 * notify_nowait() had to drop shows the client a gap in seq.
	 */
	sys_put_le32(ppg_seq++, &buf[0]);
	buf[4] = n;
	for (uint8_t i = 0; i < n; i++) {
		sys_put_le32(ppg_pending[i], &buf[5 + i * sizeof(uint32_t)]);
	}

	ppg_pending_n -= n;
	memmove(&ppg_pending[0], &ppg_pending[n],
		ppg_pending_n * sizeof(ppg_pending[0]));

	(void)notify_nowait(ATTR_PPG, buf, 5 + n * sizeof(uint32_t));
}

void ble_publish_activity(const struct ring_activity *activity)
{
	struct ring_activity snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&activity_lock);
	latest_activity = *activity;
	snapshot = latest_activity;
	k_spin_unlock(&activity_lock, key);

	if (!ble_is_connected() || !atomic_get(&activity_subscribed)) {
		return;
	}

	(void)notify_nowait(ATTR_ACTIVITY, &snapshot, sizeof(snapshot));
}

void ble_publish_gsr(const int16_t *samples, uint8_t count)
{
	/*
	 * seq (u32) + count (u8) + samples, the same shape as the PPG stream
	 * so an app can reuse its gap-detection logic. Sized for a 247-byte
	 * MTU; at the 23-byte default it carries GSR_MAX_SAMPLES_DEFAULT_MTU.
	 */
	uint8_t buf[5 + 40 * sizeof(int16_t)];
	struct bt_conn *conn;
	uint16_t mtu;
	uint16_t mtu_payload;
	uint8_t max_samples;
	uint8_t n;

	if (!ble_gsr_streaming()) {
		return;
	}

	/* bt_gatt_get_mtu(NULL) dereferences conn->state and faults; there
	 * is no MTU to ask about with nobody connected.
	 */
	conn = conn_get();
	if (!conn) {
		return;
	}

	mtu = bt_gatt_get_mtu(conn);
	bt_conn_unref(conn);

	if (mtu < 3) {
		return;
	}

	mtu_payload = mtu - 3;
	max_samples = (mtu_payload > 5) ? (mtu_payload - 5) / sizeof(int16_t)
					: 0;
	if (max_samples > 40) {
		max_samples = 40;
	}

	n = MIN(count, max_samples);
	if (n == 0) {
		return;
	}

	sys_put_le32(gsr_seq++, &buf[0]);
	buf[4] = n;
	for (uint8_t i = 0; i < n; i++) {
		sys_put_le16((uint16_t)samples[i], &buf[5 + i * sizeof(int16_t)]);
	}

	(void)notify_nowait(ATTR_GSR, buf, 5 + n * sizeof(int16_t));
}

void ble_publish_spo2(const struct ring_spo2 *spo2)
{
	struct ring_spo2 snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&spo2_lock);
	latest_spo2 = *spo2;
	snapshot = latest_spo2;
	k_spin_unlock(&spo2_lock, key);

	if (!ble_is_connected() || !atomic_get(&spo2_subscribed)) {
		return;
	}

	(void)notify_nowait(ATTR_SPO2, &snapshot, sizeof(snapshot));
}

void ble_publish_workout(const struct ring_workout *workout)
{
	struct ring_workout snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&workout_lock);
	latest_workout = *workout;
	snapshot = latest_workout;
	k_spin_unlock(&workout_lock, key);

	if (!ble_is_connected() || !atomic_get(&workout_subscribed)) {
		return;
	}

	(void)notify_nowait(ATTR_WORKOUT, &snapshot, sizeof(snapshot));
}

void ble_publish_wellness(const struct ring_wellness *wellness)
{
	struct ring_wellness snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&wellness_lock);
	latest_wellness = *wellness;
	snapshot = latest_wellness;
	k_spin_unlock(&wellness_lock, key);

	if (!ble_is_connected() || !atomic_get(&wellness_subscribed)) {
		return;
	}

	(void)notify_nowait(ATTR_WELLNESS, &snapshot, sizeof(snapshot));
}

void ble_publish_hrv(const struct ring_hrv *hrv)
{
	struct ring_hrv snapshot;
	k_spinlock_key_t key;

	key = k_spin_lock(&hrv_lock);
	latest_hrv = *hrv;
	snapshot = latest_hrv;
	k_spin_unlock(&hrv_lock, key);

	if (!ble_is_connected() || !atomic_get(&hrv_subscribed)) {
		return;
	}

	(void)notify_nowait(ATTR_HRV, &snapshot, sizeof(snapshot));
}

void ble_publish_imu(int16_t x, int16_t y, int16_t z)
{
	uint8_t buf[6];

	if (!ble_imu_streaming()) {
		return;
	}

	sys_put_le16((uint16_t)x, &buf[0]);
	sys_put_le16((uint16_t)y, &buf[2]);
	sys_put_le16((uint16_t)z, &buf[4]);

	(void)notify_nowait(ATTR_IMU, buf, sizeof(buf));
}
