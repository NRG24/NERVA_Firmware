/* Syntax-check stub.
 *
 * LOG_* route to a printf-format-checked function on purpose: a mismatched
 * specifier is a real bug this pass can catch, and Zephyr's own logging
 * checks the same way.
 */
#ifndef ZSTUB_LOG_H_
#define ZSTUB_LOG_H_
#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_ERR  1
#define LOG_LEVEL_WRN  2
#define LOG_LEVEL_INF  3
#define LOG_LEVEL_DBG  4
void zstub_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG_MODULE_REGISTER(...)	struct zstub_log_unused_##__LINE__;
#define LOG_ERR(...)	zstub_log(__VA_ARGS__)
#define LOG_WRN(...)	zstub_log(__VA_ARGS__)
#define LOG_INF(...)	zstub_log(__VA_ARGS__)
#define LOG_DBG(...)	zstub_log(__VA_ARGS__)
#define LOG_PANIC()	do { } while (0)
#endif
