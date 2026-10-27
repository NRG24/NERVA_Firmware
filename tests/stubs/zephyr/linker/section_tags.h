/* Syntax-check stub. On a host there is no section that survives a reset,
 * so __noinit is plain static storage: zeroed, which history.c's check
 * word rejects exactly as it rejects garbage after a power-on reset.
 */
#ifndef ZSTUB_SECTION_TAGS_H_
#define ZSTUB_SECTION_TAGS_H_
#define __noinit
#endif
