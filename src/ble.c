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
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/*
 * Ring Service   f0a10000-1e5c-4a2b-8d3f-9c7b6e5a4d21
 *   status       f0a10001-...   read / notify
 *   ppg          f0a10002-...   notify
 *   imu          f0a10003-...   notify
 *   control      f0a10004-...   write
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

static atomic_t connected_count;
static atomic_t status_subscribed;
static atomic_t ppg_subscribed;
static atomic_t imu_subscribed;
static atomic_t ppg_stream_on;
static atomic_t imu_stream_on;

static struct ring_status latest_status;
static const struct ble_control_cbs *control_cbs;
static uint32_t ppg_seq;

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

/* --- GATT -------------------------------------------------------------- */

static ssize_t read_status(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &latest_status,
				 sizeof(latest_status));
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

/*
 * Control opcodes, see APP_INTEGRATION.md:
 *   0x01 <u8 on>                     raw PPG streaming
 *   0x02 <u8 on>                     IMU streaming
 *   0x03                             start a measurement window now
 *   0x04 <u16 window_s> <u16 period_s>  set duty cycle
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

	default:
		LOG_WRN("control: unknown opcode 0x%02x", p[0]);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(ring_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),

	BT_GATT_CHARACTERISTIC(&uuid_status.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_status, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&uuid_ppg.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ppg_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&uuid_imu.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(imu_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&uuid_control.uuid,
			       BT_GATT_CHRC_WRITE |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_control, NULL),
);

/* Attribute indices: 1 = status value, 4 = ppg value, 7 = imu value. */
#define ATTR_STATUS	(&ring_svc.attrs[1])
#define ATTR_PPG	(&ring_svc.attrs[4])
#define ATTR_IMU	(&ring_svc.attrs[7])

/* --- connection callbacks ---------------------------------------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("connection failed (0x%02x)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	atomic_inc(&connected_count);
	LOG_INF("connected to %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	atomic_dec(&connected_count);

	/* Streaming is expensive; never leave it running for a gone phone. */
	atomic_set(&ppg_stream_on, 0);
	atomic_set(&imu_stream_on, 0);

	LOG_INF("disconnected (reason 0x%02x)", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
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

void ble_set_control_cbs(const struct ble_control_cbs *cbs)
{
	control_cbs = cbs;
}

int ble_start(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
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

	LOG_INF("advertising as %s, addr %s", CONFIG_BT_DEVICE_NAME, addr_str);
	LOG_INF("services: Heart Rate, Battery, Ring");

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
	latest_status = *status;

	if (!ble_is_connected() || !atomic_get(&status_subscribed)) {
		return;
	}

	(void)bt_gatt_notify(NULL, ATTR_STATUS, &latest_status,
			     sizeof(latest_status));
}

void ble_publish_ppg(const uint32_t *samples, uint8_t count)
{
	/*
	 * seq (u32) + count (u8) + samples. Sized for a 247-byte MTU; the
	 * phone should request one or it will be capped at 4 samples per
	 * notification by the 23-byte default.
	 */
	uint8_t buf[5 + 40 * sizeof(uint32_t)];
	uint16_t mtu_payload;
	uint8_t max_samples;
	uint8_t n;

	if (!ble_ppg_streaming()) {
		return;
	}

	mtu_payload = bt_gatt_get_mtu(NULL) - 3;
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
