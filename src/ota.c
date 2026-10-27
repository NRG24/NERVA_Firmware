#include "ota.h"

#include "history.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

enum ota_phase {
	PHASE_IDLE,		/* slot 1 is the history log */
	PHASE_UPLOADING,	/* an upload owns slot 1 */
	PHASE_UPLOADED,		/* ...and finished writing it */
};

/*
 * Written from the SMP work queue (the hooks below) and read from the
 * main loop (ota_service()), so every access is under the lock.
 */
static struct k_spinlock lock;
static enum ota_phase phase;
static int64_t last_ms;		/* last image-management activity */
static uint16_t battery_mv;

void ota_set_battery_mv(uint16_t mv)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	battery_mv = mv;
	k_spin_unlock(&lock, key);
}

bool ota_active(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool active = phase != PHASE_IDLE;

	k_spin_unlock(&lock, key);
	return active;
}

enum ota_verdict ota_on_chunk(size_t off, int64_t now_ms)
{
	enum ota_verdict v = OTA_ACCEPT;
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (off == 0U) {
		if (battery_mv != 0U && battery_mv < OTA_MIN_BATTERY_MV) {
			v = OTA_REJECT_BATTERY;
		} else {
			phase = PHASE_UPLOADING;
			last_ms = now_ms;
		}
	} else if (phase == PHASE_IDLE) {
		/*
		 * img_mgmt still remembers this upload, but the history has
		 * had the partition back since: the bytes before `off` are
		 * gone. Writing on would put image data in the log.
		 */
		v = OTA_REJECT_RESTART;
	} else {
		last_ms = now_ms;
	}

	k_spin_unlock(&lock, key);
	return v;
}

void ota_on_erase(int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	phase = PHASE_UPLOADING;
	last_ms = now_ms;
	k_spin_unlock(&lock, key);
}

void ota_on_uploaded(int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	phase = PHASE_UPLOADED;
	last_ms = now_ms;
	k_spin_unlock(&lock, key);
}

void ota_on_activity(int64_t now_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (phase != PHASE_IDLE) {
		last_ms = now_ms;
	}
	k_spin_unlock(&lock, key);
}

enum ota_action ota_decide(int64_t now_ms, bool upgrade_requested)
{
	enum ota_action a = OTA_NONE;
	k_spinlock_key_t key = k_spin_lock(&lock);
	int64_t idle = now_ms - last_ms;

	if (phase == PHASE_IDLE) {
		/* nothing to do */
	} else if (upgrade_requested) {
		/*
		 * The phone marked the image and should reset the ring next.
		 * If it disconnected first, install it anyway rather than
		 * leave the ring running with no history.
		 */
		if (idle >= OTA_INSTALL_WAIT_MS) {
			a = OTA_REBOOT;
		}
	} else if (idle >= OTA_ABANDON_MS) {
		/* Never marked, or never finished: the log gets it back. */
		phase = PHASE_IDLE;
		a = OTA_RESUME_HISTORY;
	}

	k_spin_unlock(&lock, key);
	return a;
}

/* --- MCUmgr glue --------------------------------------------------------- */

#if defined(CONFIG_MCUMGR)

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/sys/reboot.h>

/* How often ota_service() reads slot 1's trailer while an upload is live. */
#define CHECK_PERIOD_MS		1000

/* Runs on the SMP work queue, before img_mgmt writes the chunk. */
static enum mgmt_cb_return img_hook(uint32_t event,
				    enum mgmt_cb_return prev_status,
				    int32_t *rc, uint16_t *group,
				    bool *abort_more, void *data,
				    size_t data_size)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data_size);

	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK: {
		const struct img_mgmt_upload_check *chk = data;
		size_t off = chk->req->off;

		switch (ota_on_chunk(off, now)) {
		case OTA_ACCEPT:
			if (off == 0U) {
				LOG_INF("ota: upload started, %u bytes",
					(unsigned int)chk->req->size);
				history_suspend();
			}
			return MGMT_CB_OK;
		case OTA_REJECT_BATTERY:
			LOG_WRN("ota: upload refused, battery below %u mV",
				OTA_MIN_BATTERY_MV);
			*rc = MGMT_ERR_EBUSY;
			return MGMT_CB_ERROR_RC;
		case OTA_REJECT_RESTART:
		default:
			LOG_WRN("ota: chunk at %u refused, upload must restart",
				(unsigned int)off);
			*rc = MGMT_ERR_EBADSTATE;
			return MGMT_CB_ERROR_RC;
		}
	}
	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		LOG_INF("ota: upload complete");
		ota_on_uploaded(now);
		break;
	default:
		ota_on_activity(now);
		break;
	}

	return MGMT_CB_OK;
}

/* Every SMP command, before its handler runs. Only image erase matters. */
static enum mgmt_cb_return cmd_hook(uint32_t event,
				    enum mgmt_cb_return prev_status,
				    int32_t *rc, uint16_t *group,
				    bool *abort_more, void *data,
				    size_t data_size)
{
	const struct mgmt_evt_op_cmd_arg *arg = data;

	ARG_UNUSED(event);
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data_size);

	if (arg->group == MGMT_GROUP_ID_IMAGE && arg->id == IMG_MGMT_ID_ERASE &&
	    arg->op == MGMT_OP_WRITE) {
		LOG_INF("ota: slot 1 erase requested");
		history_suspend();
		ota_on_erase(k_uptime_get());
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback img_cb = {
	.callback = img_hook,
	.event_id = MGMT_EVT_OP_IMG_MGMT_ALL,
};

static struct mgmt_callback cmd_cb = {
	.callback = cmd_hook,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};

void ota_init(void)
{
	mgmt_callback_register(&img_cb);
	mgmt_callback_register(&cmd_cb);

	if (!boot_is_img_confirmed()) {
		int err = boot_write_img_confirmed();

		if (err) {
			LOG_WRN("ota: could not confirm the running image (%d)",
				err);
		} else {
			LOG_INF("ota: running image confirmed");
		}
	}
}

void ota_service(int64_t now_ms)
{
	static int64_t next_check_ms;

	if (!ota_active() || now_ms < next_check_ms) {
		return;
	}
	next_check_ms = now_ms + CHECK_PERIOD_MS;

	int swap = mcuboot_swap_type();
	bool requested = swap == BOOT_SWAP_TYPE_TEST ||
			 swap == BOOT_SWAP_TYPE_PERM;

	switch (ota_decide(now_ms, requested)) {
	case OTA_RESUME_HISTORY:
		LOG_WRN("ota: upload abandoned, history takes slot 1 back");
		(void)history_resume();
		break;
	case OTA_REBOOT:
		LOG_WRN("ota: update waiting and no reset came, rebooting to "
			"install it");
		/* Let the log line out over RTT first. */
		k_msleep(100);
		sys_reboot(SYS_REBOOT_WARM);
		break;
	case OTA_NONE:
	default:
		break;
	}
}

#else /* !CONFIG_MCUMGR */

void ota_init(void)
{
}

void ota_service(int64_t now_ms)
{
	(void)now_ms;
}

#endif /* CONFIG_MCUMGR */
