/* Syntax-check stub. See tests/README.md -- this is NOT the Zephyr API. */
#ifndef ZSTUB_DEVICE_H_
#define ZSTUB_DEVICE_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct device { const char *name; void *config; void *data; };
bool device_is_ready(const struct device *dev);
#define DT_NODELABEL(node)	node
#define DEVICE_DT_GET(node)	((const struct device *)0)
#endif
