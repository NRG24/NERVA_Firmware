/* Syntax-check stub. */
#ifndef ZSTUB_I2C_H_
#define ZSTUB_I2C_H_
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
int i2c_write(const struct device *dev, const uint8_t *buf, uint32_t num_bytes, uint16_t addr);
int i2c_read(const struct device *dev, uint8_t *buf, uint32_t num_bytes, uint16_t addr);
int i2c_write_read(const struct device *dev, uint16_t addr, const void *write_buf,
		   size_t num_write, void *read_buf, size_t num_read);
int i2c_recover_bus(const struct device *dev);
#endif
