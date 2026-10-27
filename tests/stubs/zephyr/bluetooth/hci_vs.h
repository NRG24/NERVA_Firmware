/* Syntax-check stub: the one vendor command ble.c sends. */
#ifndef ZSTUB_BT_HCI_VS_H_
#define ZSTUB_BT_HCI_VS_H_
#include <stdint.h>
#include <zephyr/sys/util.h>
#define BT_HCI_VS_LL_HANDLE_TYPE_CONN		0x02
#define BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL	0xfc0e
struct bt_hci_cp_vs_write_tx_power_level {
	uint8_t handle_type;
	uint16_t handle;
	int8_t tx_power_level;
} __packed;
struct bt_hci_rp_vs_write_tx_power_level {
	uint8_t status;
	uint8_t handle_type;
	uint16_t handle;
	int8_t selected_tx_power;
} __packed;
#endif
