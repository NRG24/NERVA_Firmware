/* Syntax-check stub. See tests/README.md -- this is NOT the Zephyr API. */
#ifndef ZSTUB_WATCHDOG_H_
#define ZSTUB_WATCHDOG_H_
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#define WDT_FLAG_RESET_NONE		0
#define WDT_FLAG_RESET_CPU_CORE		1
#define WDT_FLAG_RESET_SOC		2

#define WDT_OPT_PAUSE_IN_SLEEP		BIT(0)
#define WDT_OPT_PAUSE_HALTED_BY_DBG	BIT(1)

struct wdt_window { uint32_t min; uint32_t max; };

typedef void (*wdt_callback_t)(const struct device *dev, int channel_id);

struct wdt_timeout_cfg {
	struct wdt_window window;
	wdt_callback_t callback;
	uint8_t flags;
};

int wdt_install_timeout(const struct device *dev,
			const struct wdt_timeout_cfg *cfg);
int wdt_setup(const struct device *dev, uint8_t options);
int wdt_feed(const struct device *dev, int channel_id);
int wdt_disable(const struct device *dev);
#endif
