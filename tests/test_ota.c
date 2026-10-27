/*
 * Over-the-air update decisions (ota.c) on a host.
 *
 * The history partition is MCUboot's slot 1, so these decide when an
 * upload may write into it, when the log takes it back, and when the ring
 * reboots itself to install. What this proves: a chunk is never accepted
 * into a partition the log has taken back; a low battery refuses only the
 * start of an upload; an abandoned upload hands the partition back after
 * OTA_ABANDON_MS of silence, not before, and image traffic keeps it alive;
 * an image marked for install is installed after OTA_INSTALL_WAIT_MS even
 * with no reset from the phone. What it does not: the MCUmgr hooks that
 * call these, or MCUboot itself -- only the board can say that.
 */

#include "ota.h"
#include "sim.h"

#include <zephyr/kernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool verbose;

k_spinlock_key_t k_spin_lock(struct k_spinlock *l)
{
	(void)l;
	return 0;
}

void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key)
{
	(void)l;
	(void)key;
}

void zstub_log(const char *fmt, ...)
{
	va_list ap;

	if (!verbose) {
		return;
	}
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

#define MIN_MS	(60LL * 1000)

/* Back to no upload, whatever the last test left: tests start uploads up
 * to OTA_ABANDON_MS past their own clock, so jump well beyond that.
 */
static void settle(int64_t *t)
{
	*t += 2 * OTA_ABANDON_MS + MIN_MS;
	(void)ota_decide(*t, false);
	CHECK(!ota_active(), "settled to idle");
}

static void test_idle(void)
{
	sim_section("idle: nothing to do, mid-upload chunks refused");

	CHECK(!ota_active(), "starts idle");
	CHECK(ota_decide(0, false) == OTA_NONE, "nothing to decide");
	CHECK(ota_decide(0, true) == OTA_NONE,
	      "a trailer with no upload this boot is MCUboot's business");
	CHECK(ota_on_chunk(4096, 0) == OTA_REJECT_RESTART,
	      "a chunk past 0 with no upload under way is refused");
	CHECK(!ota_active(), "and does not start one");

	ota_on_activity(10);
	CHECK(!ota_active(), "image list traffic does not start one either");
}

static void test_battery(void)
{
	int64_t t = 1000;

	sim_section("battery: refuses the start, never the middle");

	ota_set_battery_mv(OTA_MIN_BATTERY_MV - 1);
	CHECK(ota_on_chunk(0, t) == OTA_REJECT_BATTERY, "low: start refused");
	CHECK(!ota_active(), "refused start leaves it idle");

	ota_set_battery_mv(0);
	CHECK(ota_on_chunk(0, t) == OTA_ACCEPT, "unknown battery: allowed");

	ota_set_battery_mv(OTA_MIN_BATTERY_MV - 200);
	CHECK(ota_on_chunk(4096, t + 100) == OTA_ACCEPT,
	      "battery sagging mid-upload does not strand it halfway");

	ota_set_battery_mv(OTA_MIN_BATTERY_MV);
	CHECK(ota_on_chunk(0, t + 200) == OTA_ACCEPT,
	      "exactly the minimum: allowed");
	settle(&t);
}

static void test_abandon(void)
{
	int64_t t = 10LL * OTA_ABANDON_MS;

	sim_section("abandoned upload: partition handed back, upload restarts");

	ota_set_battery_mv(3900);
	CHECK(ota_on_chunk(0, t) == OTA_ACCEPT, "start");
	CHECK(ota_active(), "active");
	CHECK(ota_on_chunk(4096, t + 500) == OTA_ACCEPT, "next chunk");
	t += 500;

	CHECK(ota_decide(t + OTA_ABANDON_MS - 1, false) == OTA_NONE,
	      "not before OTA_ABANDON_MS of silence");

	/* Image traffic short of a chunk -- a list, a status -- keeps it. */
	ota_on_activity(t + OTA_ABANDON_MS - 1000);
	CHECK(ota_decide(t + OTA_ABANDON_MS, false) == OTA_NONE,
	      "activity restarts the clock");
	t += OTA_ABANDON_MS - 1000;

	CHECK(ota_decide(t + OTA_ABANDON_MS, false) == OTA_RESUME_HISTORY,
	      "handed back after OTA_ABANDON_MS");
	CHECK(!ota_active(), "idle again");
	CHECK(ota_decide(t + 2 * OTA_ABANDON_MS, false) == OTA_NONE,
	      "and only once");

	CHECK(ota_on_chunk(8192, t + OTA_ABANDON_MS + 10) == OTA_REJECT_RESTART,
	      "the phone resuming mid-image is refused: the log owns it now");
	CHECK(ota_on_chunk(0, t + OTA_ABANDON_MS + 20) == OTA_ACCEPT,
	      "starting again from 0 is fine");
	settle(&t);
}

static void test_install(void)
{
	int64_t t = 30LL * OTA_ABANDON_MS;

	sim_section("finished upload: installed, with or without a reset");

	CHECK(ota_on_chunk(0, t) == OTA_ACCEPT, "start");
	ota_on_uploaded(t + 30000);
	t += 30000;
	CHECK(ota_active(), "still active once uploaded");

	CHECK(ota_decide(t + OTA_INSTALL_WAIT_MS - 1, true) == OTA_NONE,
	      "marked: the phone gets OTA_INSTALL_WAIT_MS to reset it");
	CHECK(ota_decide(t + OTA_INSTALL_WAIT_MS, true) == OTA_REBOOT,
	      "then the ring reboots to install it");
	CHECK(ota_decide(t + OTA_INSTALL_WAIT_MS, false) == OTA_NONE,
	      "unmarked: no reboot, the wait is the abandon one");
	CHECK(ota_decide(t + OTA_ABANDON_MS, false) == OTA_RESUME_HISTORY,
	      "uploaded but never marked: handed back after OTA_ABANDON_MS");
	CHECK(!ota_active(), "idle");
	settle(&t);
}

static void test_erase(void)
{
	int64_t t = 50LL * OTA_ABANDON_MS;

	sim_section("an erase command claims slot 1 like an upload start");

	ota_on_erase(t);
	CHECK(ota_active(), "active after an erase");
	CHECK(ota_on_chunk(4096, t + 10) == OTA_ACCEPT,
	      "chunks after an erase are not refused");
	CHECK(ota_decide(t + 10 + OTA_ABANDON_MS, false) == OTA_RESUME_HISTORY,
	      "an erase with no upload after it is abandoned the same way");
	settle(&t);
}

int main(int argc, char **argv)
{
	verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

	test_idle();
	test_battery();
	test_abandon();
	test_install();
	test_erase();

	return sim_report("ota");
}
