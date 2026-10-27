/*
 * Heart rate from a single green PPG channel.
 *
 * Feeds on raw 19-bit MAXM86161 samples at 100 sps and reports beats per
 * minute, plus beat-to-beat intervals for HRV.
 *
 * Two paths share one despiked input:
 *
 *  - RATE. Decimated to 25 sps, band-passed, and once a second a 10 s
 *    spectrum over 42-198 bpm picks the heart rate: strongest peak, credited
 *    with its second harmonic, gated on how clearly it stands out, then the
 *    median of the last five estimates. A rhythm has to be in the signal for
 *    10 s before anything is reported, and "no reading" is the answer
 *    whenever it is not clear -- never a guess.
 *
 *  - BEATS. Band-passed at 100 sps and peak-picked, with the refractory
 *    window set from the spectral rate. Intervals feed HRV only when they
 *    agree with that rate, so beat timing never decides the heart rate
 *    itself.
 *
 * Replaced a threshold-crossing detector in v0.6.2 that, measured against a
 * WHOOP, read half or double the true rate: single-sample glitches crossed
 * its threshold, and drift inflated the threshold until real beats did not.
 * See tools/hr_eval.py for the recordings and the scores.
 */

#ifndef HR_H_
#define HR_H_

#include <stdbool.h>
#include <stdint.h>

/* 10 s at 25 sps. */
#define HR_WIN		250
/* 0.70-3.30 Hz in 0.025 Hz steps: 42-198 bpm, 1.5 bpm a bin. */
#define HR_BINS		105
/* Spectral estimates the reported rate is the median of. */
#define HR_RECENT	5

struct hr {
	/* configuration */
	uint32_t sample_rate_hz;

	bool primed;
	uint32_t ticks;

	/* Despiking: the last five raw samples, median taken. */
	int32_t med[5];
	uint8_t med_i;

	/* DC level of the despiked signal, for finger detection and status. */
	int32_t baseline;

	/* Beat path, 100 sps, Q4: two high-pass stages then a low-pass. */
	int32_t b_hp1;
	int32_t b_hp2;
	int32_t b_lp;
	int32_t y1, y2;			/* the last two outputs, for peak picking */
	int32_t y3, y4;			/* and the two before, for its fraction */
	int32_t pp_min, pp_max;		/* this second's swing */
	int32_t amplitude;		/* EMA of that swing, raw counts */
	int32_t peak_level;		/* EMA of accepted beat heights */

	/* Rate path, 25 sps. */
	int32_t dec_acc;
	uint8_t dec_n;
	int32_t s_hp1;
	int32_t s_hp2;
	int32_t win[HR_WIN];		/* ring of band-passed samples */
	uint16_t win_i;
	uint16_t win_n;
	uint8_t since_spectrum;		/* decimated samples since the last */

	/* Time-averaged spectrum, each bin relative to its window's median. */
	int64_t spec_avg[HR_BINS];
	bool spec_avg_primed;

	uint16_t recent[HR_RECENT];	/* bpm x10, 0 = no confident estimate */
	uint8_t recent_i;
	uint16_t bpm_x10;		/* what hr_update() reports */

	/* beat timing, in samples */
	uint32_t last_beat_tick;
	/* where between samples that beat's peak was, 1/256 sample, +-128 */
	int16_t last_beat_frac_q8;

	/* A peak found one sample ago; see PEND_* in hr.c. */
	uint8_t pend;
	uint32_t pend_since;
	bool pend_trusted;
	bool pend_successive;

	/*
	 * The interval accepted on the most recent beat, for HRV. Only
	 * meaningful on the sample where hr_update() returned true; both
	 * flags are cleared on every other sample.
	 */
	uint32_t last_ibi;		/* samples */
	uint32_t last_ibi_q8;		/* the same, to 1/256 sample */
	bool last_ibi_trusted;
	bool last_ibi_successive;
	bool prev_ibi_trusted;		/* internal, drives last_ibi_successive */

	uint32_t beats;
};

void hr_init(struct hr *hr, uint32_t sample_rate_hz);

/*
 * Feed one raw sample. Returns true when a beat was detected -- one sample
 * after its peak, which is when its position between samples is known. *bpm_x10 is updated with the current estimate in tenths of a BPM
 * (0 when there is no confident estimate).
 *
 * Built for 100 sps. Once a second this runs the spectrum: ~26,000
 * multiply-adds, a few milliseconds on the nRF52833.
 */
bool hr_update(struct hr *hr, uint32_t raw, uint16_t *bpm_x10);

/*
 * The inter-beat interval accepted on the most recent beat, in
 * milliseconds. Only valid on the sample where hr_update() returned true;
 * 0 otherwise. Feeds RMSSD -- see hrv.h.
 */
uint16_t hr_last_ibi_ms(const struct hr *hr);

/*
 * True when that interval is within 20 % of the period the spectral rate
 * implies -- clean enough for HRV, where the error enters squared. Nothing
 * is trusted while there is no confident rate.
 */
bool hr_last_ibi_trusted(const struct hr *hr);

/*
 * True when that interval AND the one before it were both trusted and
 * genuinely adjacent -- which is what makes their difference a successive
 * difference. A peak that was seen and then rejected breaks the run.
 */
bool hr_last_ibi_successive(const struct hr *hr);

/* Current AC amplitude, peak to peak, raw counts. */
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
