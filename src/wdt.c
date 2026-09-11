#include "wdt.h"

/*
 * The whole file is conditional.
 *
 * wdt.h supplies static inline no-ops when the watchdog is compiled out, so
 * without this guard those collide with the real definitions here and the
 * build fails with "redefinition of ring_wdt_start" -- plus
 * CONFIG_RING_WATCHDOG_TIMEOUT_MS does not exist to size the timeout with.
 * Caught by building with CONFIG_RING_WATCHDOG=n, which is worth doing
 * whenever a Kconfig option gains an implementation file.
 */
#if defined(CONFIG_RING_WATCHDOG)

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wdt, LOG_LEVEL_INF);

static const struct device *const wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));

/* Negative until the watchdog is running, so feeding early is harmless. */
static int wdt_channel = -1;

int ring_wdt_start(void)
{
	/*
	 * window.min must be 0 and flags must be exactly WDT_FLAG_RESET_SOC:
	 * wdt_nrfx.c rejects anything else with -EINVAL / -ENOTSUP.
	 *
	 * No callback. The nRF watchdog fires its TIMEOUT event only about
	 * two 32.768 kHz cycles before the reset lands -- roughly 61 us,
	 * which is not enough to reliably get a line out over RTT. The reset
	 * cause is recovered on the next boot from hwinfo instead, which is
	 * both reliable and readable after the fact. See main.c.
	 */
	struct wdt_timeout_cfg cfg = {
		.flags = WDT_FLAG_RESET_SOC,
		.window = {
			.min = 0U,
			.max = CONFIG_RING_WATCHDOG_TIMEOUT_MS,
		},
		.callback = NULL,
	};
	int rc;

	if (!device_is_ready(wdt_dev)) {
		LOG_ERR("watchdog device not ready -- running unprotected");
		return -ENODEV;
	}

	rc = wdt_install_timeout(wdt_dev, &cfg);
	if (rc < 0) {
		LOG_ERR("wdt_install_timeout failed (%d) -- running unprotected",
			rc);
		return rc;
	}

	wdt_channel = rc;

	/*
	 * Option flags read backwards, so spell it out:
	 *
	 *   WDT_OPT_PAUSE_HALTED_BY_DBG  set -> PAUSES while the debugger has
	 *     the CPU halted. Without it the driver sets RUN_HALT and every
	 *     breakpoint reboots the board. This board is debugged over SWD,
	 *     so it is set.
	 *
	 *   WDT_OPT_PAUSE_IN_SLEEP  deliberately NOT set, so the watchdog
	 *     keeps counting while the CPU sleeps. The idle path sleeps for
	 *     200 ms at a time, so a hang that parks in sleep is exactly the
	 *     hang worth catching.
	 */
	rc = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (rc < 0) {
		wdt_channel = -1;
		LOG_ERR("wdt_setup failed (%d) -- running unprotected", rc);
		return rc;
	}

	/* Start from a full window rather than wherever the counter sat. */
	(void)wdt_feed(wdt_dev, wdt_channel);

	LOG_INF("watchdog armed: %d ms, resets the SoC, pauses under debugger",
		CONFIG_RING_WATCHDOG_TIMEOUT_MS);

	return 0;
}

void ring_wdt_feed(void)
{
	if (wdt_channel < 0) {
		return;
	}

	(void)wdt_feed(wdt_dev, wdt_channel);
}

#endif /* CONFIG_RING_WATCHDOG */
