/* Syntax-check stub, and the interface test_history.c fakes. */
#ifndef ZSTUB_FLASH_MAP_H_
#define ZSTUB_FLASH_MAP_H_
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
struct flash_area {
	uint8_t fa_id;
	uint8_t fa_device_id;
	uint16_t pad16;
	off_t fa_off;
	size_t fa_size;
	const void *fa_dev;
};
/* One partition is all the firmware asks for by name. */
#define PARTITION_ID(label)	0
int flash_area_open(uint8_t id, const struct flash_area **fa);
int flash_area_read(const struct flash_area *fa, off_t off, void *dst,
		    size_t len);
int flash_area_write(const struct flash_area *fa, off_t off, const void *src,
		     size_t len);
int flash_area_erase(const struct flash_area *fa, off_t off, size_t len);
#endif
