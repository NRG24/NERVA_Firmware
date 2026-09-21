#include "ble.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
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

static atomic_t connected_count;
static atomic_t status_subscribed;
static atomic_t ppg_subscribed;
static atomic_t imu_subscribed;
static atomic_t activity_subscribed;
static atomic_t hrv_subscribed;
static atomic_t gsr_subscribed;
static atomic_t ppg_stream_on;
static atomic_t imu_stream_on;
static atomic_t gsr_stream_on;

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
static int adv_start(void)
{
	return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd,
			       ARRAY_SIZE(sd));
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
 */
static ssize_t write_control(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0 || len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
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

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.recycled = recycled,
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

void ble_notify_hr(uint16_t bpm)
{
	if (!ble_is_connected()) {
		return;
	}

	(void)bt_hrs_notify(bpm);
}

void ble_notify_battery(uint8_t percent)
{
	if (percent > 100) {
		percent = 100;
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
	(void)bt_gatt_notify(NULL, ATTR_STATUS, &snapshot, sizeof(snapshot));
}

void ble_publish_ppg(const uint32_t *samples, uint8_t count)
{
	/*
	 * seq (u32) + count (u8) + samples. Sized for a 247-byte MTU; the
	 * phone should request one or it will be capped at 4 samples per
	 * notification by the 23-byte default.
	 */
	uint8_t buf[5 + 40 * sizeof(uint32_t)];
	struct bt_conn *conn;
	uint16_t mtu;
	uint16_t mtu_payload;
	uint8_t max_samples;
	uint8_t n;

	if (!ble_ppg_streaming()) {
		return;
	}

	/*
	 * bt_gatt_get_mtu(NULL) is not a "use the default" shorthand: it
	 * reaches att_get(), which dereferences conn->state and faults. There
	 * is no MTU to ask about when nobody is connected, so leave.
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
	max_samples = (mtu_payload > 5) ? (mtu_payload - 5) / sizeof(uint32_t)
				        : 0;
	if (max_samples > 40) {
		max_samples = 40;
	}

	n = MIN(count, max_samples);
	if (n == 0) {
		return;
	}

	sys_put_le32(ppg_seq++, &buf[0]);
	buf[4] = n;
	for (uint8_t i = 0; i < n; i++) {
		sys_put_le32(samples[i], &buf[5 + i * sizeof(uint32_t)]);
	}

	(void)bt_gatt_notify(NULL, ATTR_PPG, buf, 5 + n * sizeof(uint32_t));
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

	(void)bt_gatt_notify(NULL, ATTR_ACTIVITY, &snapshot, sizeof(snapshot));
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

	(void)bt_gatt_notify(NULL, ATTR_GSR, buf, 5 + n * sizeof(int16_t));
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

	(void)bt_gatt_notify(NULL, ATTR_HRV, &snapshot, sizeof(snapshot));
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

	(void)bt_gatt_notify(NULL, ATTR_IMU, buf, sizeof(buf));
}
