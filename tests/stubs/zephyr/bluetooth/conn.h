/* Syntax-check stub. See tests/README.md -- this is NOT the Zephyr API. */
#ifndef ZSTUB_BT_CONN_H_
#define ZSTUB_BT_CONN_H_
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/bluetooth.h>

struct bt_conn;

typedef enum { BT_SECURITY_L0, BT_SECURITY_L1, BT_SECURITY_L2,
	       BT_SECURITY_L3, BT_SECURITY_L4 } bt_security_t;

enum bt_security_err {
	BT_SECURITY_ERR_SUCCESS,
	BT_SECURITY_ERR_AUTH_FAIL,
	BT_SECURITY_ERR_PIN_OR_KEY_MISSING,
	BT_SECURITY_ERR_UNSPECIFIED,
};

struct bt_conn_cb {
	void (*connected)(struct bt_conn *conn, uint8_t err);
	void (*disconnected)(struct bt_conn *conn, uint8_t reason);
	void (*security_changed)(struct bt_conn *conn, bt_security_t level,
				 enum bt_security_err err);
	void (*recycled)(void);
	void (*le_param_updated)(struct bt_conn *conn, uint16_t interval,
				 uint16_t latency, uint16_t timeout);
};

struct bt_le_conn_param {
	uint16_t interval_min;
	uint16_t interval_max;
	uint16_t latency;
	uint16_t timeout;
};
#define BT_LE_CONN_PARAM(int_min, int_max, lat, to)			\
	((struct bt_le_conn_param[]) { {				\
		.interval_min = (int_min), .interval_max = (int_max),	\
		.latency = (lat), .timeout = (to) } })
int bt_conn_le_param_update(struct bt_conn *conn,
			    const struct bt_le_conn_param *param);

struct bt_conn_auth_cb {
	void (*passkey_display)(struct bt_conn *conn, unsigned int passkey);
	void (*passkey_entry)(struct bt_conn *conn);
	void (*pairing_confirm)(struct bt_conn *conn);
	void (*cancel)(struct bt_conn *conn);
};

struct bt_conn_auth_info_cb {
	void (*pairing_complete)(struct bt_conn *conn, bool bonded);
	void (*pairing_failed)(struct bt_conn *conn, enum bt_security_err reason);
};

struct bt_conn *bt_conn_ref(struct bt_conn *conn);
void bt_conn_unref(struct bt_conn *conn);
const bt_addr_le_t *bt_conn_get_dst(const struct bt_conn *conn);
int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason);
int bt_conn_auth_cb_register(const struct bt_conn_auth_cb *cb);
int bt_conn_auth_info_cb_register(struct bt_conn_auth_info_cb *cb);
int bt_conn_auth_pairing_confirm(struct bt_conn *conn);

/* The real macro places it in an iterable section, so it is never
 * "unused"; say so, or Clang's -Wunused-variable fires where GCC's did not.
 */
#define BT_CONN_CB_DEFINE(_name)	\
	static struct bt_conn_cb _name __attribute__((used))

#endif
