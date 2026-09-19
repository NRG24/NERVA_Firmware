/* Syntax-check stub. */
#ifndef ZSTUB_REGULATOR_H_
#define ZSTUB_REGULATOR_H_
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
int regulator_enable(const struct device *dev);
int regulator_disable(const struct device *dev);
#endif
