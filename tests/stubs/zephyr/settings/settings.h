/*
 * Syntax-check stub, and just enough of the real API for test_profile.c
 * to drive profile.c's handler the way settings_load() would.
 */
#ifndef ZSTUB_SETTINGS_H_
#define ZSTUB_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>		/* ssize_t, which Zephyr's header pulls in */

typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

int settings_load(void);
int settings_subsys_init(void);
int settings_save_one(const char *name, const void *value, size_t val_len);
int settings_name_steq(const char *name, const char *key, const char **next);

/*
 * The real macro places a struct in an iterable section that
 * settings_load() walks. Here it leaves the set handler somewhere a test
 * can find it, under a name derived from the handler's.
 */
#define SETTINGS_STATIC_HANDLER_DEFINE(_hname, _tree, _get, _set, _commit, \
				       _export)				   \
	int (*const settings_stub_set_##_hname)(const char *, size_t,	   \
						settings_read_cb, void *) = _set

#endif
