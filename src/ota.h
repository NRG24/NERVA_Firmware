/*
 * Over-the-air updates: the app's side of MCUboot and the SMP server.
 *
 * Nordic's MCUmgr does the transfer -- a phone uploads a signed,
 * LZMA2-compressed image over BLE into MCUboot's slot 1, marks it, and
 * resets the ring, and MCUboot checks the signature, decompresses it over
 * slot 0 and boots it. OTA.md has the whole flow, for the app and for
 * tools/ota_update.py.
 *
 * What this module adds is the one thing that is particular to this ring:
 * slot 1 IS the history partition (see the board DTS for why), so an
 * upload and the log cannot both own it.
 *
 *   - The first chunk of an upload (offset 0), or an image erase command,
 *     suspends the history first (history_suspend()), which saves the next
 *     seq to settings so the numbering carries on after the update.
 *   - Below OTA_MIN_BATTERY_MV a new upload is refused: transfer plus the
 *     bootloader's copy is minutes of radio and flash work, and a flat
 *     battery halfway through MCUboot's copy is a ring that needs the jig.
 *   - An upload abandoned for OTA_ABANDON_MS hands the partition back:
 *     history_resume() formats it and the log carries on. A later chunk
 *     of that upload is then refused, so the phone restarts it from zero
 *     instead of writing into the log.
 *   - An image that finished uploading and was marked for install, but
 *     never followed by a reset, is installed anyway: after
 *     OTA_INSTALL_WAIT_MS the ring reboots itself.
 *
 * The running image is marked confirmed at boot. In overwrite-only mode
 * nothing reverts, so this only makes the image list read honestly.
 *
 * The decisions are plain functions of their inputs, so tests/test_ota.c
 * runs them on a host; the MCUmgr glue around them needs the real stack.
 */

#ifndef OTA_H_
#define OTA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_MIN_BATTERY_MV	3600
#define OTA_ABANDON_MS		(10LL * 60 * 1000)
#define OTA_INSTALL_WAIT_MS	(60LL * 1000)

/*
 * Register the MCUmgr hooks and confirm the running image. Call before
 * ble_start(), so no SMP command can arrive unhooked.
 */
void ota_init(void);

/* Resume the history or reboot to install, when due. Every main-loop pass. */
void ota_service(int64_t now_ms);

/* An upload owns slot 1 (or did, and its outcome is still pending). */
bool ota_active(void);

/* Latest battery reading; 0 = unknown, which does not block an upload. */
void ota_set_battery_mv(uint16_t mv);

/* --- the decisions, exposed for tests/test_ota.c ------------------------- */

enum ota_verdict {
	OTA_ACCEPT,
	OTA_REJECT_RESTART,	/* history took slot 1 back: start from 0 */
	OTA_REJECT_BATTERY,	/* below OTA_MIN_BATTERY_MV */
};

enum ota_action {
	OTA_NONE,
	OTA_RESUME_HISTORY,
	OTA_REBOOT,
};

/* An upload chunk at byte `off` is about to be written. On OTA_ACCEPT
 * with off == 0 the caller suspends the history before the write.
 */
enum ota_verdict ota_on_chunk(size_t off, int64_t now_ms);

/* An image erase command is about to run (history already suspended). */
void ota_on_erase(int64_t now_ms);

/* The last byte of an upload has been written. */
void ota_on_uploaded(int64_t now_ms);

/* Any other image-management traffic: keeps an upload from timing out. */
void ota_on_activity(int64_t now_ms);

/* What ota_service() should do now. `upgrade_requested`: slot 1's trailer
 * says MCUboot will install it at the next boot.
 */
enum ota_action ota_decide(int64_t now_ms, bool upgrade_requested);

#endif /* OTA_H_ */
