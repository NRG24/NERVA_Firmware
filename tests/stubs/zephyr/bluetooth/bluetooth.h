/* Syntax-check stub. See tests/README.md -- this is NOT the Zephyr API. */
#ifndef ZSTUB_BT_BLUETOOTH_H_
#define ZSTUB_BT_BLUETOOTH_H_
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#define BT_ADDR_LE_STR_LEN	30
#define BT_ID_DEFAULT		0

typedef struct { uint8_t type; uint8_t a[6]; } bt_addr_le_t;
extern const bt_addr_le_t *BT_ADDR_LE_ANY;

#define BT_LE_AD_GENERAL	BIT(1)
#define BT_LE_AD_NO_BREDR	BIT(2)

#define BT_DATA_FLAGS		0x01
#define BT_DATA_UUID16_ALL	0x03
#define BT_DATA_NAME_COMPLETE	0x09

struct bt_data {
	uint8_t type;
	uint8_t data_len;
	const uint8_t *data;
};

#define BT_DATA(_type, _data, _data_len)				\
	{ .type = (_type), .data_len = (_data_len),			\
	  .data = (const uint8_t *)(_data) }

#define BT_DATA_BYTES(_type, _bytes...)					\
	BT_DATA(_type, ((const uint8_t []){ _bytes }),			\
		sizeof((const uint8_t []){ _bytes }))

struct bt_le_adv_param { uint32_t options; };
#define BT_LE_ADV_OPT_CONN	BIT(0)
extern const struct bt_le_adv_param *BT_LE_ADV_CONN_FAST_1;

int bt_enable(void (*cb)(int err));
int bt_le_adv_start(const struct bt_le_adv_param *param,
		    const struct bt_data *ad, size_t ad_len,
		    const struct bt_data *sd, size_t sd_len);
int bt_unpair(uint8_t id, const bt_addr_le_t *addr);
void bt_id_get(bt_addr_le_t *addrs, size_t *count);
int bt_addr_le_to_str(const bt_addr_le_t *addr, char *str, size_t len);

#endif
