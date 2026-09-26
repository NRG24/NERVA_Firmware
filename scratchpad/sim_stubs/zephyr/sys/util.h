/*
 * Stub of Zephyr's <zephyr/sys/util.h>, just enough to compile the pure
 * logic modules (sleep.c, steps.c, calories.c) on a host with gcc.
 *
 * There is no Zephyr SDK in the environment these simulations run in, and
 * there is no board to run the real thing on either, so this is how the
 * activity modules get exercised at all. It deliberately provides ONLY the
 * three macros those files use: anything that needs more of Zephyr than
 * this is not a pure logic module and does not belong in a simulation.
 */

#ifndef SIM_STUB_ZEPHYR_SYS_UTIL_H_
#define SIM_STUB_ZEPHYR_SYS_UTIL_H_

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#endif /* SIM_STUB_ZEPHYR_SYS_UTIL_H_ */
