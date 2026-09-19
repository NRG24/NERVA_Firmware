/* Syntax-check stub. See tests/README.md -- this is NOT the Zephyr API. */
#ifndef ZSTUB_BT_UUID_H_
#define ZSTUB_BT_UUID_H_

#include <stdint.h>
#include <zephyr/sys/util.h>

enum { BT_UUID_TYPE_16, BT_UUID_TYPE_32, BT_UUID_TYPE_128 };

struct bt_uuid { uint8_t type; };
struct bt_uuid_16 { struct bt_uuid uuid; uint16_t val; };
struct bt_uuid_128 { struct bt_uuid uuid; uint8_t val[16]; };

#define ZSTUB_B(v, n)	((uint8_t)(((uint64_t)(v) >> (8 * (n))) & 0xff))

#define BT_UUID_16_ENCODE(w16)	ZSTUB_B(w16, 0), ZSTUB_B(w16, 1)

#define BT_UUID_128_ENCODE(w32, w1, w2, w3, w48)			\
	ZSTUB_B(w48, 0), ZSTUB_B(w48, 1), ZSTUB_B(w48, 2),		\
	ZSTUB_B(w48, 3), ZSTUB_B(w48, 4), ZSTUB_B(w48, 5),		\
	ZSTUB_B(w3, 0), ZSTUB_B(w3, 1),					\
	ZSTUB_B(w2, 0), ZSTUB_B(w2, 1),					\
	ZSTUB_B(w1, 0), ZSTUB_B(w1, 1),					\
	ZSTUB_B(w32, 0), ZSTUB_B(w32, 1), ZSTUB_B(w32, 2), ZSTUB_B(w32, 3)

#define BT_UUID_INIT_128(...)						\
	{ .uuid = { BT_UUID_TYPE_128 }, .val = { __VA_ARGS__ } }

#define BT_UUID_DECLARE_16(v)	((const struct bt_uuid *)0)

#define BT_UUID_HRS_VAL		0x180d
#define BT_UUID_BAS_VAL		0x180f
#define BT_UUID_GATT_CHRC_VAL	0x2803
#define BT_UUID_GATT_CCC_VAL	0x2902

extern const struct bt_uuid *BT_UUID_GATT_CHRC;
extern const struct bt_uuid *BT_UUID_GATT_CCC;

int bt_uuid_cmp(const struct bt_uuid *u1, const struct bt_uuid *u2);

#endif
