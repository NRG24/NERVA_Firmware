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

#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define BIT(n)		(1U << (n))

#endif /* TEST_STUB_ZEPHYR_SYS_UTIL_H_ */
