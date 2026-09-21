#include "spo2.h"

#include <zephyr/sys/util.h>

#include <string.h>

/*
 * DC tracker, per channel. Same shift as hr.c's baseline: ~0.25 Hz corner
 * at 100 sps, slow enough to leave the pulse alone and fast enough to
 * follow a finger settling onto the sensor.
 */
#define DC_SHIFT		6

/*
 * How long a window the peak-to-peak is measured over. One second is a
 * whole cardiac cycle at any plausible rate, so the swing it sees is the
 * pulse amplitude rather than part of one.
 */
#define AC_WINDOW_SAMPLES	100

/* Peak-to-peak estimates are averaged over this many windows. */
#define AC_AVERAGE_SHIFT	2

/*
 * A finger has to be on the sensor for any of this to mean anything. The
 * green channel's threshold was measured at 15,000 (hr.c), and the IR
 * channel returns more light than green for the same perfusion, so this is
 * not a tight bound -- it only rejects an empty sensor.
 */
#define DC_MIN			15000

/*
 * Minimum pulsatile swing, in counts, before R is worth computing. Below
 * this the ratio is dominated by noise in the denominator and swings
 * wildly between samples.
 */
#define AC_MIN			100

/*
 * R-to-SpO2 curve: SpO2 = CURVE_A - CURVE_B * R.
 *
 * THESE TWO NUMBERS ARE NOT CALIBRATED FOR THIS HARDWARE. They are the
 * widely published starting point (A=110, B=25) that appears in Maxim's
 * own application notes and most reference implementations, and they
 * assume an optical geometry, LED wavelengths and photodiode response that
 * nobody has checked against this board.
 *
 * Calibrating them means desaturating volunteers against a reference
 * oximeter and fitting the result -- which is a clinical study, not a
 * firmware change. Until that happens every percentage this produces
 * carries SPO2_FLAG_UNCALIBRATED, and the ratio is published alongside so
 * the eventual fit has something to fit against.
 */
#define CURVE_A_X10		1100
#define CURVE_B_X10		250

/* Physiologically reportable range. Outside it, report nothing. */
#define SPO2_MIN_PERCENT	70
#define SPO2_MAX_PERCENT	100

struct channel {
	int32_t dc;
	int32_t win_max;
	int32_t win_min;
	int32_t ac;		/* averaged peak-to-peak */
	bool primed;
};

static struct {
	struct channel red;
	struct channel ir;
	uint16_t window_count;
} sp;

void spo2_init(void)
{
	memset(&sp, 0, sizeof(sp));
}

void spo2_reset(void)
{
	memset(&sp, 0, sizeof(sp));
}

static void channel_feed(struct channel *c, uint32_t raw)
{
	int32_t x = (int32_t)raw;

	if (!c->primed) {
		c->dc = x;
		c->win_max = x;
		c->win_min = x;
		c->primed = true;
		return;
	}

	c->dc += (x - c->dc) >> DC_SHIFT;
	c->win_max = MAX(c->win_max, x);
	c->win_min = MIN(c->win_min, x);
}

/* Close the peak-to-peak window on both channels at once. */
static void channel_close_window(struct channel *c)
{
	int32_t pp = c->win_max - c->win_min;

	if (c->ac == 0) {
		c->ac = pp;
	} else {
		c->ac += (pp - c->ac) >> AC_AVERAGE_SHIFT;
	}

	c->win_max = c->dc;
	c->win_min = c->dc;
}

void spo2_feed(uint32_t red, uint32_t ir)
{
	channel_feed(&sp.red, red);
	channel_feed(&sp.ir, ir);

	if (++sp.window_count < AC_WINDOW_SAMPLES) {
		return;
	}

	sp.window_count = 0;
	channel_close_window(&sp.red);
	channel_close_window(&sp.ir);
}

bool spo2_finger_present(void)
{
	return sp.red.primed && sp.ir.primed &&
	       sp.red.dc >= DC_MIN && sp.ir.dc >= DC_MIN;
}

void spo2_result(struct spo2_result *out)
{
	/*
	 * The flag is set on the way out no matter what happens below,
	 * including on the paths that report nothing. An app that reads
	 * flags before checking the values must never see a cleared
	 * uncalibrated bit and conclude the number is trustworthy.
	 */
	out->ratio_x1000 = 0;
	out->percent = 0;
	out->flags = SPO2_FLAG_UNCALIBRATED;

	if (!sp.red.primed || !sp.ir.primed) {
		return;
	}

	if (sp.red.dc < DC_MIN || sp.ir.dc < DC_MIN) {
		return;
	}

	if (sp.red.ac < AC_MIN || sp.ir.ac < AC_MIN) {
		return;
	}

	/*
	 * R = (AC_red / DC_red) / (AC_ir / DC_ir)
	 *   = (AC_red * DC_ir) / (DC_red * AC_ir)
	 *
	 * 64-bit for the numerator: AC can reach a few thousand counts and
	 * DC the full 19-bit 524,287, so AC*DC alone approaches 2^31 before
	 * the x1000 is applied.
	 */
	uint64_t num = (uint64_t)sp.red.ac * (uint64_t)sp.ir.dc * 1000U;
	uint64_t den = (uint64_t)sp.red.dc * (uint64_t)sp.ir.ac;

	if (den == 0) {
		return;
	}

	uint64_t r_x1000 = num / den;

	if (r_x1000 == 0 || r_x1000 > UINT16_MAX) {
		return;
	}

	out->ratio_x1000 = (uint16_t)r_x1000;
	out->flags |= SPO2_FLAG_VALID;

	/*
	 * SpO2 = A - B*R, with A and B in tenths. Signed, because an R above
	 * A/B produces a negative saturation -- which is not a reading, it
	 * is evidence the signal or the curve is wrong, and it must not wrap
	 * into a plausible-looking percentage.
	 */
	int32_t pct_x10 = CURVE_A_X10 -
			  (int32_t)((CURVE_B_X10 * r_x1000) / 1000U);
	int32_t pct = (pct_x10 + 5) / 10;

	if (pct < SPO2_MIN_PERCENT || pct > SPO2_MAX_PERCENT) {
		return;
	}

	out->percent = (uint8_t)pct;
}
