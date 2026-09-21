#include "hr.h"

#include <zephyr/sys/util.h>

#include <string.h>

/*
 * All filters are shift-based EMAs so this stays integer-only.
 *
 *   baseline: alpha 1/64  -> ~0.25 Hz corner at 100 sps, removes the DC
 *             reflection without eating the pulse
 *   smooth:   alpha 1/4   -> ~4 Hz corner, kills sample noise, keeps the
 *             pulse shape
 */
#define BASELINE_SHIFT	6
#define SMOOTH_SHIFT	2
#define AMP_SHIFT	5

/* A beat is only accepted if the signal swings this fraction of recent
 * amplitude above baseline, which keeps noise from triggering it.
 */
/*
 * Fraction of tracked amplitude the signal must rise above to count as an
 * upstroke. Measured at 20% this missed beats for seconds at a time: one
 * large swing inflates the amplitude EMA and the bar stays up. 10% tracks
 * real upstrokes without picking up the dicrotic notch, which the
 * refractory window handles anyway.
 */
#define THRESHOLD_NUM	2
#define THRESHOLD_DEN	10

/* Physiological limits: 30-220 BPM. */
#define BPM_MIN		30
#define BPM_MAX		220

/*
 * Finger detection, measured on this board (green LED1, PA 0x80, ADC range
 * 16 uA, TINT 117.3 us):
 *
 *   nothing on the sensor : DC ~2860,  AC ~118
 *   finger held on        : DC ~53000, AC ~1500
 *
 * A factor of nineteen with no overlap, so DC alone is a clean gate.
 * 15000 sits five times above the empty reading and a third of the loaded
 * one. Perfusion index is NOT usable here -- it measured 4.1% empty and
 * 3.0% with a finger, i.e. backwards.
 */
#define FINGER_DC_MIN	15000

/* Below this the signal is treated as no-finger and beats are ignored. */
#define AMPLITUDE_FLOOR	40

/*
 * Amplitude alone does not distinguish a pulse from noise -- with nothing
 * on the sensor this reported a confident 79 bpm off an AC amplitude of
 * 127. Real cardiac intervals are consistent beat to beat; noise is not.
 * Require several intervals that agree before reporting anything.
 */
#define MIN_INTERVALS	4
/* Relaxed now that FINGER_DC_MIN does the real gatekeeping: this only has
 * to reject wild outliers, not stand alone against noise. Real HRV plus
 * detector jitter needs the headroom.
 */
#define SPREAD_MAX_PCT	40

/*
 * The same question asked far more strictly, for HRV only.
 *
 * 40 % is the right bar for a heart rate, which takes a median and shrugs
 * off one odd interval. It is much too loose for RMSSD, where the error is
 * squared and lands in two successive differences. Measured: a metronome
 * pulse train with a motion artifact partway through each beat gets the
 * artifact accepted as a real beat, producing an alternating 600/400 ms
 * pattern that sails through a 40 % band and invents 200 ms of HRV out of
 * a rhythm that has none.
 *
 * 20 % is the conventional artifact-rejection bound in the HRV literature
 * (Malik's rule and its descendants), not a number invented here. Ordinary
 * beat-to-beat variation sits far inside it -- an RMSSD of 40 ms on
 * 1000 ms intervals is a 4 % swing -- so this rejects artifacts without
 * touching physiology.
 */
#define HRV_SPREAD_MAX_PCT	20

void hr_init(struct hr *hr, uint32_t sample_rate_hz)
{
	*hr = (struct hr){ .sample_rate_hz = sample_rate_hz };
}

int32_t hr_amplitude(const struct hr *hr)
{
	return hr->amplitude;
}

uint16_t hr_last_ibi_ms(const struct hr *hr)
{
	if (hr->sample_rate_hz == 0 || hr->last_ibi == 0) {
		return 0;
	}

	/*
	 * Intervals are counted in samples, so at 100 sps this is exact to
	 * 10 ms and no better. That quantisation is the dominant error in
	 * RMSSD -- see the note in hrv.h.
	 */
	return (uint16_t)((hr->last_ibi * 1000U) / hr->sample_rate_hz);
}

bool hr_last_ibi_trusted(const struct hr *hr)
{
	return hr->last_ibi_trusted;
}

bool hr_last_ibi_successive(const struct hr *hr)
{
	return hr->last_ibi_successive;
}

int32_t hr_baseline(const struct hr *hr)
{
	return hr->baseline;
}

bool hr_finger_present(const struct hr *hr)
{
	return hr->primed && hr->baseline >= FINGER_DC_MIN;
}

/* Median is far more robust than a mean here: one dicrotic-notch false
 * beat or one dropped beat skews an average badly, but barely moves a
 * median.
 */
static uint32_t median_ibi(const struct hr *hr)
{
	uint32_t v[ARRAY_SIZE(hr->ibi)];
	uint8_t n = hr->ibi_count;

	if (n == 0) {
		return 0;
	}

	memcpy(v, hr->ibi, n * sizeof(v[0]));

	for (uint8_t i = 1; i < n; i++) {
		uint32_t key = v[i];
		int8_t j = i - 1;

		while (j >= 0 && v[j] > key) {
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = key;
	}

	return v[n / 2];
}

static uint16_t bpm_from_ibis(const struct hr *hr)
{
	uint32_t med = median_ibi(hr);
	uint8_t agree = 0;

	if (hr->ibi_count < MIN_INTERVALS || med == 0) {
		return 0;
	}

	/*
	 * Count how many intervals sit close to the median rather than
	 * demanding the whole spread be tight. A single missed or extra beat
	 * then costs one sample instead of the entire estimate.
	 */
	for (uint8_t i = 0; i < hr->ibi_count; i++) {
		uint32_t d = (hr->ibi[i] > med) ? (hr->ibi[i] - med)
					        : (med - hr->ibi[i]);

		if ((d * 100U) <= (med * SPREAD_MAX_PCT)) {
			agree++;
		}
	}

	if (agree < MIN_INTERVALS) {
		return 0;
	}

	/* median interval in samples -> beats per minute, times ten */
	return (uint16_t)((600U * hr->sample_rate_hz) / med);
}

bool hr_update(struct hr *hr, uint32_t raw, uint16_t *bpm_x10)
{
	int32_t x = (int32_t)raw;
	bool beat = false;

	hr->ticks++;

	/* Only ever true on the sample that reports a beat. */
	hr->last_ibi_trusted = false;
	hr->last_ibi_successive = false;

	if (!hr->primed) {
		hr->baseline = x;
		hr->smooth = 0;
		hr->last_min = 0;
		hr->last_max = 0;
		hr->primed = true;
		if (bpm_x10) {
			*bpm_x10 = 0;
		}
		return false;
	}

	/* DC tracker, then the AC part we actually care about */
	hr->baseline += (x - hr->baseline) >> BASELINE_SHIFT;

	/*
	 * No finger, no pulse. Without this the detector reports a confident
	 * ~88 bpm from an empty sensor, because filtered noise crosses the
	 * threshold at roughly the low-pass corner frequency -- it ends up
	 * measuring the filter, not a heartbeat.
	 */
	if (hr->baseline < FINGER_DC_MIN) {
		hr->ibi_count = 0;
		hr->ibi_next = 0;
		hr->last_beat_tick = 0;
		hr->above = false;
		/* Whatever comes next starts a new run, not a difference. */
		hr->prev_ibi_trusted = false;
		if (bpm_x10) {
			*bpm_x10 = 0;
		}
		return false;
	}

	int32_t ac = x - hr->baseline;

	hr->smooth += (ac - hr->smooth) >> SMOOTH_SHIFT;

	/* Track swing so the threshold can follow perfusion changes. */
	hr->last_max = MAX(hr->last_max, hr->smooth);
	hr->last_min = MIN(hr->last_min, hr->smooth);

	if ((hr->ticks % hr->sample_rate_hz) == 0) {
		int32_t pp = hr->last_max - hr->last_min;

		hr->amplitude += (pp - hr->amplitude) >> 1;
		hr->last_max = hr->smooth;
		hr->last_min = hr->smooth;
	}

	int32_t threshold = (hr->amplitude * THRESHOLD_NUM) / (2 * THRESHOLD_DEN);

	/*
	 * Rising crossing of the threshold marks the systolic upstroke. The
	 * refractory window is set by BPM_MAX so a dicrotic notch cannot be
	 * counted as a second beat.
	 */
	/*
	 * Adaptive refractory. A fixed 0.27 s window (220 bpm) lets the
	 * dicrotic notch through as a phantom beat -- it arrives roughly
	 * 0.3-0.4 s after the systolic peak. Once a rate is established,
	 * blank for 60% of the median interval instead.
	 */
	uint32_t refractory = (hr->sample_rate_hz * 60U) / BPM_MAX;

	if (hr->ibi_count >= 2) {
		uint32_t adaptive = (median_ibi(hr) * 6U) / 10U;

		refractory = MAX(refractory, adaptive);
	}

	if (!hr->above && hr->smooth > threshold &&
	    hr->amplitude > AMPLITUDE_FLOOR) {
		hr->above = true;

		uint32_t since = hr->ticks - hr->last_beat_tick;

		/*
		 * Every path below advances last_beat_tick, so a crossing that
		 * does NOT yield a usable interval still moves the beat clock
		 * and contaminates the next one. Assume that until proven
		 * otherwise: `accepted` is what clears it.
		 */
		bool accepted = false;

		if (hr->last_beat_tick != 0 && since >= refractory) {
			uint32_t bpm = (60U * hr->sample_rate_hz) / since;

			if (bpm >= BPM_MIN && bpm <= BPM_MAX) {
				hr->ibi[hr->ibi_next] = since;
				hr->ibi_next = (hr->ibi_next + 1) %
					       ARRAY_SIZE(hr->ibi);
				if (hr->ibi_count < ARRAY_SIZE(hr->ibi)) {
					hr->ibi_count++;
				}
				hr->beats++;
				beat = true;

				/*
				 * Second gate, for HRV only: does this interval
				 * agree with the ones around it? The heart rate
				 * can absorb an odd interval because it takes a
				 * median; RMSSD cannot, because the error is
				 * squared and lands in two differences.
				 *
				 * The median is taken after inserting this
				 * interval, which lets a bad one pull its own
				 * bar -- with eight samples it moves the median
				 * by at most one position, so the effect is
				 * small, and taking it before would leave the
				 * first intervals of a window ungated entirely.
				 */
				uint32_t med = median_ibi(hr);
				bool trusted = false;

				if (hr->ibi_count >= MIN_INTERVALS && med != 0) {
					uint32_t d = (since > med) ? (since - med)
								  : (med - since);

					trusted = (d * 100U) <=
						  (med * HRV_SPREAD_MAX_PCT);
				}

				hr->last_ibi = since;
				hr->last_ibi_trusted = trusted;
				hr->last_ibi_successive = trusted &&
							  hr->prev_ibi_trusted;
				hr->prev_ibi_trusted = trusted;
				accepted = true;
			}
			/*
			 * An out-of-range interval means a missed or spurious
			 * beat, not that everything before it was wrong. Skip
			 * it and keep the history -- wiping it here meant the
			 * detector could never accumulate MIN_INTERVALS
			 * whenever a single beat was dropped.
			 */
		}

		if (!accepted) {
			/*
			 * This crossing produced no usable interval -- it was
			 * inside the refractory window, out of the plausible
			 * bpm range, or the very first one -- but it advances
			 * the beat clock all the same. Whatever interval comes
			 * next is measured from it, so it is adjacent to
			 * nothing and must not form a successive difference.
			 *
			 * Defence in depth rather than a fix for an observed
			 * failure: the tightened HRV_SPREAD_MAX_PCT band
			 * rejects the intervals this would catch before they
			 * reach RMSSD, and reverting this line alone does not
			 * make the test suite fail. It stays because the
			 * invariant is structural -- a crossing that moves the
			 * beat clock without producing an interval must break
			 * the run -- and relying on a threshold to cover a
			 * bookkeeping error is how the threshold ends up
			 * carrying weight nobody knows it carries.
			 */
			hr->prev_ibi_trusted = false;
		}

		hr->last_beat_tick = hr->ticks;
	} else if (hr->above && hr->smooth < (threshold / 2)) {
		hr->above = false;
	}

	if (hr->amplitude <= AMPLITUDE_FLOOR) {
		hr->ibi_count = 0;
		hr->ibi_next = 0;
		hr->prev_ibi_trusted = false;
	}

	if (bpm_x10) {
		*bpm_x10 = bpm_from_ibis(hr);
	}

	return beat;
}
