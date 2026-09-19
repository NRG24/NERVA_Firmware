/* Syntax-check stub. */
#ifndef ZSTUB_BYTEORDER_H_
#define ZSTUB_BYTEORDER_H_
#include <stdint.h>
void sys_put_le16(uint16_t val, uint8_t dst[2]);
void sys_put_le32(uint32_t val, uint8_t dst[4]);
uint16_t sys_get_le16(const uint8_t src[2]);
uint32_t sys_get_le32(const uint8_t src[4]);
#endif
