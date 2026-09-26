#include "hrv.h"

#include <string.h>

/*
 * Successive differences retained. At roughly one beat a second this is a
 * little over a minute of beats when the PPG runs continuously, and
 * several minutes of wall time at the default 25 % duty cycle.
 */
#define HRV_WINDOW		64

/*
 * Below this many differences, report nothing rather than a number built
 * from a handful of beats.
 *
 * Deliberately lower than the 30-60 s the literature wants: at a 15 s
 * measurement window the detector produces maybe ten usable intervals per
 * window, so a floor of 30 would mean reporting nothing at all until
 * three or four windows had passed. Ten is enough to be indicative, and
 * hrv_diffs() is published alongside so an app that wants the stricter bar
 * can apply it. It is not enough to be clinical, and nothing here is.
 */
#define HRV_MIN_DIFFS		10

struct hrv_state {
	uint32_t sq[HRV_WINDOW];	/* squared successive differences, ms^2 */
	uint32_t sum;			/* running sum of sq[] */
	uint8_t count;
	uint8_t next;

	uint16_t prev_ibi_ms;
	bool prev_valid;
};

static struct hrv_state hv;

void hrv_init(void)
{
	memset(&hv, 0, sizeof(hv));
}

void hrv_reset(void)
{
	memset(&hv, 0, sizeof(hv));
}

void hrv_add_interval(uint16_t ibi_ms, bool successive)
{
	if (ibi_ms == 0) {
		/* No interval is no evidence; start a new run. */
		hv.prev_valid = false;
		return;
	}

	if (successive && hv.prev_valid) {
		int32_t diff = (int32_t)ibi_ms - (int32_t)hv.prev_ibi_ms;
		/*
		 * hr.c rejects anything outside 30-220 bpm, so an interval is
		 * 273-2000 ms and a difference cannot exceed ~1727 ms. Squared
		 * that is under 3.0e6, and HRV_WINDOW of them under 2.0e8 --
		 * both comfortably inside uint32.
		 */
		uint32_t sq = (uint32_t)(diff * diff);

		if (hv.count == HRV_WINDOW) {
			hv.sum -= hv.sq[hv.next];
		} else {
			hv.count++;
		}

		hv.sq[hv.next] = sq;
		hv.sum += sq;
		hv.next = (uint8_t)((hv.next + 1) % HRV_WINDOW);
	}

	hv.prev_ibi_ms = ibi_ms;
	hv.prev_valid = true;
}

/* Integer square root, so this file stays free of floating point like the
 * rest of the signal chain. Same bit-by-bit method as imu.c.
 */
static uint32_t isqrt64(uint64_t n)
{
	uint64_t rem = n;
	uint64_t root = 0;
	uint64_t bit = (uint64_t)1 << 62;

	while (bit > rem) {
		bit >>= 2;
	}

	while (bit) {
		if (rem >= root + bit) {
			rem -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}

	return (uint32_t)root;
}

uint16_t hrv_rmssd_x10(void)
{
	if (hv.count < HRV_MIN_DIFFS) {
		return 0;
	}

	/*
	 * rmssd_x10 = sqrt(sum/count) * 10 = sqrt(sum * 100 / count).
	 *
	 * The multiply is done before the divide so the x10 precision is not
	 * lost to integer truncation, which needs 64 bits: sum reaches 2.0e8
	 * and sum*100 reaches 2.0e10, well past uint32.
	 */
	uint64_t mean_sq_x100 = ((uint64_t)hv.sum * 100U) / hv.count;

	return (uint16_t)isqrt64(mean_sq_x100);
}

uint8_t hrv_diffs(void)
{
	return hv.count;
}
