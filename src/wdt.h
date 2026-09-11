/*
 * Hardware watchdog.
 *
 * The main loop must call ring_wdt_feed() often enough that the gap between
 * two feeds never approaches CONFIG_RING_WATCHDOG_TIMEOUT_MS. Feeding from
 * anywhere else -- a timer, a dedicated thread -- would defeat the point,
 * because the thing being checked is that the main loop is still running.
 *
 * Compiles to nothing when CONFIG_RING_WATCHDOG is off.
 */

#ifndef WDT_H_
#define WDT_H_

#include <zephyr/kernel.h>

#if defined(CONFIG_RING_WATCHDOG)

/*
 * Installs and starts the watchdog. Call LAST, after every subsystem has
 * initialised: on the nRF52833 the watchdog cannot be stopped once started
 * (there is no STOP task on this part), so a hang before this point leaves
 * a board that can still be flashed, while starting it early would turn a
 * slow or failing init into a reboot loop.
 *
 * Returns 0, or a negative errno with nothing started.
 */
int ring_wdt_start(void);

/* Feeds the watchdog. Safe to call before ring_wdt_start(); does nothing. */
void ring_wdt_feed(void);

#else

static inline int ring_wdt_start(void) { return 0; }
static inline void ring_wdt_feed(void) { }

#endif /* CONFIG_RING_WATCHDOG */

#endif /* WDT_H_ */
