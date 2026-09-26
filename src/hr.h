/*
 * Minimal heart-rate estimator for a single PPG channel.
 *
 * Feeds on raw 19-bit MAXM86161 samples at a fixed rate and reports beats
 * per minute. Deliberately simple: a DC tracker to remove the (large)
 * static reflection, a light low-pass, and adaptive-threshold peak
 * detection with a refractory period.
 */

#ifndef HR_H_
#define HR_H_

#include <stdbool.h>
#include <stdint.h>

struct hr {
	/* configuration */
	uint32_t sample_rate_hz;

	/* DC tracker and filtered AC signal */
	int32_t baseline;
	int32_t smooth;
	int32_t amplitude;	/* EMA of recent peak-to-peak */
	int32_t last_min;
	int32_t last_max;
	bool primed;
	bool above;

	/* beat timing, in samples */
	uint32_t ticks;
	uint32_t last_beat_tick;

	/* inter-beat intervals, in samples */
	uint32_t ibi[8];
	uint8_t ibi_count;
	uint8_t ibi_next;

	/*
	 * The interval accepted on the most recent beat, for HRV. Only
	 * meaningful on the sample where hr_update() returned true; both
	 * flags are cleared on every other sample.
	 */
	uint32_t last_ibi;		/* samples */
	bool last_ibi_trusted;
	bool last_ibi_successive;
	bool prev_ibi_trusted;		/* internal, drives last_ibi_successive */

	uint32_t beats;
};

void hr_init(struct hr *hr, uint32_t sample_rate_hz);

/*
 * Feed one raw sample. Returns true when a beat was detected on this
 * sample. *bpm_x10 is updated with the current estimate in tenths of a BPM
 * (0 when there is not enough data yet).
 */
bool hr_update(struct hr *hr, uint32_t raw, uint16_t *bpm_x10);

/*
 * The inter-beat interval accepted on the most recent beat, in
 * milliseconds. Only valid on the sample where hr_update() returned true;
 * 0 otherwise. Feeds RMSSD -- see hrv.h.
 */
uint16_t hr_last_ibi_ms(const struct hr *hr);

/*
 * True when that interval passed both gates the beat detector applies:
 * the 30-220 bpm plausibility range, and agreement with the median of the
 * recent intervals. An interval that fails either is real enough to keep
 * a heart rate honest but not clean enough for HRV, where the error would
 * enter squared.
 */
bool hr_last_ibi_trusted(const struct hr *hr);

/*
 * True when that interval AND the one before it were both trusted and
 * genuinely adjacent -- which is what makes their difference a successive
 * difference. A beat that was detected and then rejected still moves the
 * beat clock, so the interval following it is measured from a suspect
 * beat and is adjacent to nothing.
 */
bool hr_last_ibi_successive(const struct hr *hr);

/* Current AC amplitude. */
int32_t hr_amplitude(const struct hr *hr);

/* True when the DC level says something is on the sensor. */
bool hr_finger_present(const struct hr *hr);

/*
 * True once at least one sample has been fed since hr_init().
 *
 * hr_finger_present() answers false both for "the sensor is empty" and for
 * "nothing has been measured yet", and those are not the same claim. A
 * window that starts the part successfully but reads an empty FIFO for its
 * whole length -- which does not trip the FIFO error path, because an empty
 * read is not a failed one -- would otherwise look exactly like a ring on a
 * table. main.c separates the two before recording a sleep wear verdict.
 */
bool hr_primed(const struct hr *hr);

/* Tracked DC level. Finger-on should raise this a lot; the threshold has
 * to come from measuring this board, not from a guess.
 */
int32_t hr_baseline(const struct hr *hr);

#endif /* HR_H_ */
