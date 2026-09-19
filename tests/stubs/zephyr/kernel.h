/* Syntax-check stub. */
#ifndef ZSTUB_KERNEL_H_
#define ZSTUB_KERNEL_H_
#include <errno.h>		/* Zephyr's kernel.h pulls this in too */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
int32_t k_msleep(int32_t ms);
int64_t k_uptime_get(void);
void k_busy_wait(uint32_t usec);
/* Zephyr's kernel.h exposes the printk family too. */
int snprintk(char *str, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
struct k_spinlock { int dummy; };
typedef int k_spinlock_key_t;
k_spinlock_key_t k_spin_lock(struct k_spinlock *l);
void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key);
#endif
