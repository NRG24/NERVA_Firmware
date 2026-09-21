/* Syntax-check stub. */
#ifndef ZSTUB_ATOMIC_H_
#define ZSTUB_ATOMIC_H_
#include <stdbool.h>
typedef long atomic_t;
/* Zephyr defines this as a plain initialiser, not a call. */
#define ATOMIC_INIT(i)	(i)
typedef long atomic_val_t;
atomic_val_t atomic_set(atomic_t *target, atomic_val_t value);
atomic_val_t atomic_get(const atomic_t *target);
bool atomic_cas(atomic_t *target, atomic_val_t old_value, atomic_val_t new_value);
atomic_val_t atomic_inc(atomic_t *target);
atomic_val_t atomic_dec(atomic_t *target);
#endif
