/*
 * Syntax-check stub. See tests/README.md -- this is NOT the Zephyr API.
 *
 * One thing here is load-bearing rather than merely syntactic:
 * BT_GATT_CHARACTERISTIC expands to TWO array entries and BT_GATT_CCC to
 * one, exactly as the real macros do. That makes the attribute count of a
 * BT_GATT_SERVICE_DEFINE come out right, which is what
 * test_gatt_layout.c asserts against -- so an added or reordered
 * characteristic trips a compile error rather than silently shifting the
 * ATTR_* indices in ble.c onto the wrong attribute.
 */
#ifndef ZSTUB_BT_GATT_H_
#define ZSTUB_BT_GATT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/util.h>

/* Permissions */
#define BT_GATT_PERM_NONE		0
#define BT_GATT_PERM_READ		BIT(0)
#define BT_GATT_PERM_WRITE		BIT(1)
#define BT_GATT_PERM_READ_ENCRYPT	BIT(2)
#define BT_GATT_PERM_WRITE_ENCRYPT	BIT(3)

/* Characteristic properties */
#define BT_GATT_CHRC_READ		BIT(1)
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP	BIT(2)
#define BT_GATT_CHRC_WRITE		BIT(3)
#define BT_GATT_CHRC_NOTIFY		BIT(4)
#define BT_GATT_CHRC_INDICATE		BIT(5)

/* CCC values */
#define BT_GATT_CCC_NOTIFY		0x0001
#define BT_GATT_CCC_INDICATE		0x0002

struct bt_gatt_attr;

typedef ssize_t (*bt_gatt_attr_read_func_t)(struct bt_conn *conn,
					    const struct bt_gatt_attr *attr,
					    void *buf, uint16_t len,
					    uint16_t offset);
typedef ssize_t (*bt_gatt_attr_write_func_t)(struct bt_conn *conn,
					     const struct bt_gatt_attr *attr,
					     const void *buf, uint16_t len,
					     uint16_t offset, uint8_t flags);

struct bt_gatt_attr {
	const struct bt_uuid *uuid;
	bt_gatt_attr_read_func_t read;
	bt_gatt_attr_write_func_t write;
	void *user_data;
	uint16_t handle;
	uint16_t perm;
};

struct bt_gatt_chrc {
	const struct bt_uuid *uuid;
	uint16_t value_handle;
	uint8_t properties;
};

struct bt_gatt_service_static {
	const struct bt_gatt_attr *attrs;
	size_t attr_count;
};

ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t buf_len, uint16_t offset,
			  const void *value, uint16_t value_len);
ssize_t bt_gatt_attr_read_chrc(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset);
uint16_t bt_gatt_get_mtu(struct bt_conn *conn);
int bt_gatt_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		   const void *data, uint16_t len);

#define BT_GATT_ERR(_att_err)	(-(_att_err))

#define BT_GATT_ATTRIBUTE(_uuid, _perm, _read, _write, _user_data)	\
	{								\
		.uuid = (const struct bt_uuid *)(_uuid),		\
		.read = (_read),					\
		.write = (_write),					\
		.user_data = (void *)(_user_data),			\
		.perm = (_perm),					\
	}

#define BT_GATT_PRIMARY_SERVICE(_service)				\
	BT_GATT_ATTRIBUTE(0, BT_GATT_PERM_READ, NULL, NULL, (void *)(_service))

/* Two attributes, as in Zephyr: the declaration then the value. */
#define BT_GATT_CHARACTERISTIC(_uuid, _props, _perm, _read, _write, _user_data) \
	BT_GATT_ATTRIBUTE(0, BT_GATT_PERM_READ, bt_gatt_attr_read_chrc, NULL, NULL), \
	BT_GATT_ATTRIBUTE(_uuid, _perm, _read, _write, _user_data)

typedef void (*bt_gatt_ccc_cfg_changed_t)(const struct bt_gatt_attr *attr,
					  uint16_t value);

#define BT_GATT_CCC(_changed, _perm)					\
	BT_GATT_ATTRIBUTE(0, _perm, NULL, NULL, (void *)(_changed))

/*
 * The real macro builds a static array plus a section entry. The array and
 * an `attrs` pointer into it are what ble.c's ATTR_* indexing needs.
 */
#define BT_GATT_SERVICE_DEFINE(_name, ...)				\
	static const struct bt_gatt_attr _name##_attrs[] = { __VA_ARGS__ }; \
	static const struct bt_gatt_service_static _name = {		\
		.attrs = _name##_attrs,					\
		.attr_count = ARRAY_SIZE(_name##_attrs),		\
	}

#endif
