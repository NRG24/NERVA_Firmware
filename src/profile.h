/*
 * The wearer's profile, kept in flash so a reboot does not forget it.
 *
 * Today that is body weight and nothing else: the one input the calorie
 * estimate needs that the ring cannot measure. It used to live only in
 * calories.c's RAM, so every reboot -- a watchdog reset, a flat battery,
 * a reflash -- quietly put the wearer back at the 70 kg default, and the
 * estimate stayed wrong until the app happened to notice the reboot and
 * send the weight again. Nothing on the wire says which weight is in use,
 * so an app that missed the reboot had no way to find out.
 *
 * Stored through Zephyr's settings subsystem under "ring/weight", in the
 * same NVS partition that already holds the BLE bonds (prj.conf). No new
 * partition, no new Kconfig.
 *
 * Kept out of calories.c on purpose. That module is pure arithmetic and
 * is tested on a host with no Zephyr at all; flash belongs here.
 *
 * Main-thread only, like the activity modules: the BLE control callback
 * hands the weight over through an atomic in main.c, and only the main
 * loop calls in. settings_load() -- which is what delivers the saved value
 * -- also runs on the main thread, from ble_start().
 */

#ifndef PROFILE_H_
#define PROFILE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Minimum spacing between two flash writes.
 *
 * A bonded app can write the weight opcode as fast as the link allows,
 * and nothing about that is malicious -- a slider in a settings screen
 * does it. Each write costs flash wear and, when NVS has to garbage
 * collect, a page erase with the radio running. So the value in RAM
 * changes at once (the estimate never waits on this), and the value in
 * flash follows at most this often. The last write of a burst always
 * lands; it is only delayed.
 */
#define PROFILE_SAVE_MIN_INTERVAL_MS	60000

/*
 * The weight read back from flash at boot, in kg x10.
 *
 * Returns false when there is none: first boot, an erased partition, a
 * record of the wrong size, or a build without CONFIG_SETTINGS. The value
 * is returned as stored; calories_set_weight() clamps it like any other.
 *
 * Only meaningful after settings_load(), which ble_start() calls.
 */
bool profile_saved_weight(uint16_t *kg_x10);

/*
 * Record the weight now in use. Cheap, never touches flash: it only marks
 * the value to be written by profile_service(). A value equal to what is
 * already stored is not rewritten, so an app that resends the weight on
 * every connection costs nothing.
 */
void profile_note_weight(uint16_t kg_x10);

/*
 * Write a pending weight if one is due. Call every pass of the main loop.
 *
 * A failed write is logged and retried after the same interval rather
 * than on every pass: a partition that will not take a write now will not
 * take one 20 ms from now either.
 */
void profile_service(int64_t now_ms);

#endif /* PROFILE_H_ */
