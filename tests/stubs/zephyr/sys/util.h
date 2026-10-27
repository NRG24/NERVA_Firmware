/*
 * Just enough of Zephyr's sys/util.h to build the activity modules on a
 * host compiler.
 *
 * These are the only Zephyr symbols steps.c, sleep.c and calories.c use --
 * that is deliberate, and it is what makes them testable off-target. If a
 * change to those files needs more than this, think hard about whether it
 * belongs there or in main.c: the modules are pure logic on purpose.
 */

#ifndef TEST_STUB_ZEPHYR_SYS_UTIL_H_
#define TEST_STUB_ZEPHYR_SYS_UTIL_H_

/*
 * ARG_UNUSED, __packed and BUILD_ASSERT really come from Zephyr's toolchain headers,
 * which util.h pulls in. They are here so one -Istubs covers both the
 * behavioural tests and the syntax check.
 */
#ifndef ARG_UNUSED
#define ARG_UNUSED(x)	(void)(x)
#endif
#ifndef __packed
#define __packed	__attribute__((packed))
#endif
#ifndef BUILD_ASSERT
#define BUILD_ASSERT(expr, ...)	_Static_assert(expr, "" __VA_ARGS__)
#endif

/*
 * Zephyr's IS_ENABLED(): 1 when the macro is defined to 1, else 0, usable
 * in an ordinary if(). The same preprocessor trick as util_macro.h.
 */
#define IS_ENABLED(config_macro)	Z_IS_ENABLED1(config_macro)
#define Z_IS_ENABLED1(config_macro)	Z_IS_ENABLED2(_XXXX##config_macro)
#define _XXXX1				_YYYY,
#define Z_IS_ENABLED2(one_or_two_args)	Z_IS_ENABLED3(one_or_two_args 1, 0)
#define Z_IS_ENABLED3(ignore_this, val, ...)	val

#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define BIT(n)		(1U << (n))
#define CLAMP(v, lo, hi)	(((v) < (lo)) ? (lo) : (((v) > (hi)) ? (hi) : (v)))

#endif /* TEST_STUB_ZEPHYR_SYS_UTIL_H_ */
