/* Syntax-check stub. */
#ifndef ZSTUB_HWINFO_H_
#define ZSTUB_HWINFO_H_
#include <stdint.h>
#include <zephyr/sys/util.h>
#define RESET_PIN		(1 << 0)
#define RESET_SOFTWARE		(1 << 1)
#define RESET_BROWNOUT		(1 << 2)
#define RESET_POR		(1 << 3)
#define RESET_WATCHDOG		(1 << 4)
#define RESET_DEBUG		(1 << 5)
#define RESET_SECURITY		(1 << 6)
#define RESET_LOW_POWER_WAKE	(1 << 7)
int hwinfo_get_reset_cause(uint32_t *cause);
int hwinfo_clear_reset_cause(void);
#endif
