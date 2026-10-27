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
/* Timeouts, mutexes and delayable work: history.c and the BLE sync. */
typedef struct { int64_t ms; } k_timeout_t;
#define K_NO_WAIT	((k_timeout_t){ 0 })
#define K_FOREVER	((k_timeout_t){ -1 })
#define K_MSEC(m)	((k_timeout_t){ (m) })
#define K_SECONDS(s)	((k_timeout_t){ (s) * 1000 })
struct k_mutex { int dummy; };
#define K_MUTEX_DEFINE(name)	struct k_mutex name = { 0 }
int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);
int k_mutex_unlock(struct k_mutex *mutex);
struct k_work { int dummy; };
typedef void (*k_work_handler_t)(struct k_work *work);
struct k_work_delayable { struct k_work work; k_work_handler_t handler; };
#define K_WORK_DELAYABLE_DEFINE(name, fn) \
	struct k_work_delayable name = { .handler = (fn) }
int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay);
int k_work_cancel_delayable(struct k_work_delayable *dwork);
#endif
