/* Syntax-check stub. */
#ifndef ZSTUB_BT_HCI_H_
#define ZSTUB_BT_HCI_H_
#include <stdint.h>
#include <zephyr/kernel.h>
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN	0x13
#define BT_ATT_ERR_INVALID_OFFSET		0x07
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN	0x0d
#define BT_ATT_ERR_VALUE_NOT_ALLOWED		0x13
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES	0x11
#define BT_ATT_ERR_CCC_IMPROPER_CONF		0xfd

/* ble_set_tx_power(): one vendor command and its reply. */
struct net_buf {
	uint8_t *data;
	uint16_t len;
};
struct bt_conn;
void *net_buf_add(struct net_buf *buf, size_t len);
void net_buf_unref(struct net_buf *buf);
struct net_buf *bt_hci_cmd_alloc(k_timeout_t timeout);
int bt_hci_cmd_send_sync(uint16_t opcode, struct net_buf *buf,
			 struct net_buf **rsp);
int bt_hci_get_conn_handle(const struct bt_conn *conn, uint16_t *conn_handle);
#endif
