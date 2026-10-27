#include "hr.h"

#include <zephyr/sys/util.h>

#include <string.h>

/*
 * Integer only, like the rest of this firmware: the FPU is not enabled, and
 * everything here fits comfortably in 32/64-bit fixed point.
 *
 * Measured on ring 2 against a WHOOP, 2026-10-07/08 (tools/hr_eval.py):
 * the detector this replaces averaged ~50 bpm of error with no reading
 * within 5 bpm, because single-sample glitches (~10-20 % of samples, on a
 * 70 ms rhythm) crossed its threshold and drift set its threshold. Its
 * successor below despikes first, estimates the rate from a spectrum rather
 * than from threshold crossings, and says nothing when the spectrum is not
 * clear.
 */

/*
 * Finger detection, measured on this board (green LED1, PA 0x80, ADC range
 * 16 uA, TINT 117.3 us):
 *
 *   nothing on the sensor : DC ~2860,  AC ~118
 *   finger held on        : DC ~53000, AC ~1500
 *
 * A factor of nineteen with no overlap, so DC alone is a clean gate.
 * 15000 sits five times above the empty reading and a third of the loaded
 * one. Doubling the LED current (opcode 0x10) doubles both, and the empty
 * reading is still well under the gate.
 */
#define FINGER_DC_MIN	15000

/* Physiological limits on a beat-to-beat interval: 30-220 BPM. */
#define BPM_MIN		30
#define BPM_MAX		220

/*
 * An interval feeds HRV only within this much of the period the spectral
 * rate implies. 20 % is the conventional artifact-rejection bound in the HRV
 * literature (Malik's rule and its descendants); ordinary beat-to-beat
 * variation sits far inside it -- an RMSSD of 40 ms on 1000 ms intervals is
 * a 4 % swing.
 */
#define HRV_SPREAD_MAX_PCT	20

/*
 * Confidence gates on the time-averaged spectrum. Overridable so
 * tools/hr_eval.py can sweep them; the defaults were chosen on 2026-10-15
 * against two measures at once:
 *
 *   - real recordings (9 recordings, ~13 min against a WHOOP)
 *   - pulse-free noise (test_hr, and a 20-minute probe), where any reading
 *     is a false one
 *
 *   v0.6.2: PMR 4, no averaging    noise false 50 %, 7.2 bpm, 60 % in 5
 *   v0.7.0: PMR 7, HR_SPEC_AVG 3   noise false  5 %, 5.6 bpm, 66 % in 5
 *
 * The cost is coverage on the real recordings, 68 % -> 50 % of seconds,
 * much of it seconds that were wrong anyway. Averaging 1/16 tracked a rate
 * step too slowly for test_hr; no averaging could not get noise below
 * 11 % without halving coverage.
 *
 *   HR_PMR_MIN   the chosen peak's power against the median bin. A pulse
 *                is a peak; drift and noise are a slope or a floor.
 *   HR_UNIQ_PCT  the strongest peak more than 0.15 Hz away may be at most
 *                this share of the chosen one. Two rival rhythms means
 *                neither is trustworthy.
 */
#ifndef HR_PMR_MIN
#define HR_PMR_MIN	7
#endif
#ifndef HR_UNIQ_PCT
#define HR_UNIQ_PCT	75
#endif

/*
 * Spectral averaging: each new spectrum moves the average 1/2^HR_SPEC_AVG of
 * the way. 0 is no averaging. Overridable for tools/hr_eval.py.
 */
#ifndef HR_SPEC_AVG
#define HR_SPEC_AVG	3
#endif

/* Of the last HR_RECENT estimates, this many must exist and agree to within
 * HR_AGREE_X10 of their median before a rate is reported.
 */
#define HR_MIN_AGREE	3
#define HR_AGREE_X10	60

/* 0.15 Hz in bins: closer than this counts as the same peak. */
#define HR_SAME_PEAK	6

/* Decimation: 100 sps in, 25 sps to the spectrum. */
#define HR_DECIM	4
#define HR_SPECTRUM_FS	25

/*
 * Goertzel coefficients, 2 cos(2 pi f / 25) in Q14, for f = 0.70 + 0.025 k
 * Hz, k = 0..104. Generated, not hand-typed: see tools/hr_eval.py.
 */
static const int16_t goertzel_coef[HR_BINS] = {
	32262, 32226, 32188, 32148, 32108, 32066, 32023, 31979, 31933, 31886,
	31838, 31789, 31739, 31687, 31634, 31579, 31524, 31467, 31409, 31350,
	31289, 31227, 31164, 31100, 31035, 30968, 30900, 30831, 30760, 30689,
	30616, 30542, 30467, 30391, 30313, 30234, 30154, 30073, 29991, 29907,
	29822, 29736, 29649, 29561, 29472, 29381, 29289, 29197, 29102, 29007,
	28911, 28813, 28715, 28615, 28514, 28412, 28309, 28205, 28099, 27993,
	27885, 27777, 27667, 27556, 27444, 27331, 27217, 27102, 26986, 26868,
	26750, 26630, 26510, 26388, 26266, 26142, 26017, 25892, 25765, 25637,
	25509, 25379, 25248, 25116, 24984, 24850, 24715, 24580, 24443, 24305,
	24167, 24027, 23887, 23745, 23603, 23460, 23316, 23170, 23024, 22877,
	22730, 22581, 22431, 22281, 22129,
};

/* First half of a 250-point Hann window in Q15; it is symmetric. */
static const int16_t hann_half[HR_WIN / 2] = {
	0, 5, 21, 47, 83, 130, 187, 255, 333, 421,
	519, 627, 745, 874, 1012, 1160, 1317, 1484, 1661, 1847,
	2042, 2247, 2460, 2683, 2914, 3153, 3401, 3658, 3922, 4194,
	4474, 4762, 5057, 5359, 5669, 5985, 6308, 6637, 6972, 7314,
	7661, 8013, 8371, 8735, 9103, 9475, 9852, 10233, 10618, 11007,
	11399, 11795, 12193, 12594, 12997, 13403, 13810, 14219, 14630, 15041,
	15454, 15867, 16280, 16694, 17107, 17519, 17931, 18343, 18752, 19161,
	19567, 19972, 20374, 20773, 21170, 21564, 21955, 22342, 22725, 23104,
	23479, 23849, 24215, 24575, 24931, 25281, 25625, 25963, 26296, 26622,
	26941, 27254, 27560, 27858, 28150, 28434, 28710, 28978, 29239, 29491,
	29735, 29970, 30197, 30414, 30623, 30823, 31014, 31195, 31367, 31530,
	31683, 31826, 31959, 32082, 32195, 32299, 32392, 32474, 32547, 32609,
	32661, 32703, 32734, 32755, 32766,
};

BUILD_ASSERT(ARRAY_SIZE(goertzel_coef) == HR_BINS, "one coefficient a bin");

/*
 * Scratch for the spectrum. Static rather than on the stack: 2.7 kB would
 * be most of the main thread's. This makes hr_update() non-reentrant, which
 * it always was in practice -- there is one PPG and one caller.
 */
static int32_t spec_x[HR_WIN];
static int64_t spec_p[HR_BINS];
static int64_t spec_sorted[HR_BINS];

void hr_init(struct hr *hr, uint32_t sample_rate_hz)
{
	memset(hr, 0, sizeof(*hr));
	hr->sample_rate_hz = sample_rate_hz;
}

int32_t hr_amplitude(const struct hr *hr)
{
	return hr->amplitude;
}

uint16_t hr_last_ibi_ms(const struct hr *hr)
{
	uint32_t per_s_q8 = hr->sample_rate_hz * 256U;

	if (hr->sample_rate_hz == 0 || hr->last_ibi_q8 == 0) {
		return 0;
	}

	/*
	 * Each end of the interval is placed between samples by
	 * peak_offset_q8(), so this is not limited to the 10 ms sample
	 * spacing at 100 sps. Rounded to the nearest millisecond.
	 */
	return (uint16_t)((hr->last_ibi_q8 * 1000U + per_s_q8 / 2U) /
			  per_s_q8);
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

bool hr_primed(const struct hr *hr)
{
	return hr->primed;
}

/*
 * Median of five. The glitches this exists for are single samples, so a
 * five-wide median removes them outright, where any linear filter only
 * smears them -- the old 4 Hz low-pass left a quarter of each spike, which
 * was still enough to cross a beat threshold.
 */
static int32_t median5(const int32_t *in)
{
	int32_t v[5];

	memcpy(v, in, sizeof(v));
	for (int i = 1; i < 5; i++) {
		int32_t key = v[i];
		int j = i - 1;

		while (j >= 0 && v[j] > key) {
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = key;
	}

	return v[2];
}

/* Median of the spectrum's bins: the noise floor a pulse must stand above. */
static int64_t spectrum_median(void)
{
	memcpy(spec_sorted, spec_p, sizeof(spec_sorted));
	for (int i = 1; i < HR_BINS; i++) {
		int64_t key = spec_sorted[i];
		int j = i - 1;

		while (j >= 0 && spec_sorted[j] > key) {
			spec_sorted[j + 1] = spec_sorted[j];
			j--;
		}
		spec_sorted[j + 1] = key;
	}

	return spec_sorted[HR_BINS / 2];
}

/*
 * One rate estimate from the last 10 s, in bpm x10, or 0 when the spectrum
 * does not show one clear rhythm.
 */
static uint16_t spectrum_estimate(struct hr *hr)
{
	int64_t sum = 0;
	int32_t maxabs = 0;
	int shift = 0;

	/* Oldest first: win_i is the next write, so the oldest sample. */
	for (int n = 0; n < HR_WIN; n++) {
		spec_x[n] = hr->win[(hr->win_i + n) % HR_WIN];
		sum += spec_x[n];
	}

	int32_t mean = (int32_t)(sum / HR_WIN);

	for (int n = 0; n < HR_WIN; n++) {
		spec_x[n] -= mean;
		maxabs = MAX(maxabs, (spec_x[n] < 0) ? -spec_x[n] : spec_x[n]);
	}

	/*
	 * Block scaling: bring the window under 2^17 so the Goertzel state
	 * (at most ~N/2 times the input) and its squares stay well inside
	 * int64 whatever the LED current and averaging are.
	 */
	while ((maxabs >> shift) > (1 << 17)) {
		shift++;
	}

	for (int n = 0; n < HR_WIN; n++) {
		int32_t w = (n < HR_WIN / 2) ? hann_half[n]
					     : hann_half[HR_WIN - 1 - n];

		spec_x[n] = (int32_t)(((int64_t)(spec_x[n] >> shift) * w) >> 15);
	}

	/* |X(f)|^2 at each bin, by Goertzel: the DFT at one frequency. */
	for (int k = 0; k < HR_BINS; k++) {
		const int64_t c = goertzel_coef[k];
		int64_t s1 = 0;
		int64_t s2 = 0;

		for (int n = 0; n < HR_WIN; n++) {
			int64_t s = spec_x[n] + ((c * s1) >> 14) - s2;

			s2 = s1;
			s1 = s;
		}

		int64_t p = s1 * s1 + s2 * s2 - (((c * s1) >> 14) * s2);

		spec_p[k] = (p > 0) ? p : 0;
	}

	/*
	 * Average the spectrum over time. A heartbeat sits at the same
	 * frequency second after second and reinforces itself; a noise peak
	 * lands somewhere new each time and averages away. Without this, a
	 * finger with no detectable pulse read as a heart rate half the time
	 * (test_hr), because the largest of ~17 independent noise bins clears
	 * 4x their median almost always, and consecutive windows share 9 s of
	 * data, so noise even "agreed with itself".
	 *
	 * Each spectrum is normalised to its own median first: block scaling
	 * gives every window a different scale, and only shapes average.
	 */
	{
		int64_t m = spectrum_median();

		if (m <= 0) {
			m = 1;
		}
		for (int k = 0; k < HR_BINS; k++) {
			int64_t rel = (spec_p[k] * 1024) / m;

			hr->spec_avg[k] = hr->spec_avg_primed
					  ? hr->spec_avg[k] +
					    ((rel - hr->spec_avg[k]) >> HR_SPEC_AVG)
					  : rel;
			spec_p[k] = hr->spec_avg[k];
		}
		hr->spec_avg_primed = true;
	}

	/*
	 * The strongest local peak, credited with half its second
	 * harmonic's power. A pulse has a harmonic and drift does not, and
	 * the credit stops a strong harmonic winning on its own -- which is
	 * the double-rate error -- because it can earn no credit of its own
	 * from a 4x bin that does not exist.
	 */
	int best = -1;
	int64_t best_score = 0;

	for (int k = 1; k < HR_BINS - 1; k++) {
		if (spec_p[k] < spec_p[k - 1] || spec_p[k] <= spec_p[k + 1]) {
			continue;
		}

		int k2 = 28 + 2 * k;	/* the bin at twice this frequency */
		int64_t score = spec_p[k] +
				((k2 < HR_BINS) ? spec_p[k2] / 2 : 0);

		if (best < 0 || score > best_score) {
			best = k;
			best_score = score;
		}
	}

	if (best < 0) {
		return 0;
	}

	int64_t floor_p = spectrum_median();

	if (floor_p <= 0) {
		floor_p = 1;
	}
	if (spec_p[best] < floor_p * HR_PMR_MIN) {
		return 0;
	}

	/*
	 * A rival rhythm anywhere else means neither is trustworthy -- but a
	 * pulse's own second harmonic is not a rival, and neither is the
	 * fundamental when the harmonic won. Without excluding them, a pulse
	 * with a strong dicrotic wave (harmonic power over 75 % of the
	 * fundamental's) was rejected outright: test_hr found it reading
	 * nothing at all.
	 */
	int harm = 28 + 2 * best;		/* bin at twice this frequency */
	int sub = ((best - 28) >= 0 && ((best - 28) % 2) == 0)
		  ? (best - 28) / 2 : -100;	/* bin at half of it */

	for (int k = 1; k < HR_BINS - 1; k++) {
		bool peak = spec_p[k] >= spec_p[k - 1] &&
			    spec_p[k] > spec_p[k + 1];
		int d = (k > best) ? (k - best) : (best - k);
		int dh = (k > harm) ? (k - harm) : (harm - k);
		int ds = (k > sub) ? (k - sub) : (sub - k);

		if (peak && d > HR_SAME_PEAK && dh > HR_SAME_PEAK &&
		    ds > HR_SAME_PEAK &&
		    spec_p[k] * 100 > spec_p[best] * HR_UNIQ_PCT) {
			return 0;
		}
	}

	/* Parabolic interpolation between bins: 1.5 bpm a bin is coarse. */
	int64_t pm = spec_p[best - 1];
	int64_t p0 = spec_p[best];
	int64_t pp = spec_p[best + 1];

	while (p0 > ((int64_t)1 << 40)) {
		pm >>= 1;
		p0 >>= 1;
		pp >>= 1;
	}

	int64_t den = pm - 2 * p0 + pp;	/* negative at a maximum */
	int32_t off = 0;

	if (den < 0) {
		off = (int32_t)((15 * (pm - pp)) / (2 * den));
		off = CLAMP(off, -8, 8);
	}

	return (uint16_t)(420 + 15 * best + off);
}

/* The reported rate: median of recent estimates, if enough of them agree. */
static uint16_t combine_estimates(const struct hr *hr)
{
	uint16_t v[HR_RECENT];
	int n = 0;

	for (int i = 0; i < HR_RECENT; i++) {
		if (hr->recent[i]) {
			v[n++] = hr->recent[i];
		}
	}
	if (n < HR_MIN_AGREE) {
		return 0;
	}

	for (int i = 1; i < n; i++) {
		uint16_t key = v[i];
		int j = i - 1;

		while (j >= 0 && v[j] > key) {
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = key;
	}

	uint16_t med = v[n / 2];
	int agree = 0;

	for (int i = 0; i < n; i++) {
		int d = (int)v[i] - (int)med;

		if (d >= -HR_AGREE_X10 && d <= HR_AGREE_X10) {
			agree++;
		}
	}

	return (agree >= HR_MIN_AGREE) ? med : 0;
}

/*
 * Where between samples a peak really was.
 *
 * The beat detector finds a peak as the sample y0 that beats both its
 * neighbours, so a beat's time is only known to the nearest sample: 10 ms
 * at 100 sps. Successive intervals then wander by a sample even when the
 * heart does not, which put a floor of ~8 ms under RMSSD -- larger than a
 * stressed or unwell wearer's whole real HRV.
 *
 * So the peak is placed between samples: the vertex of the least-squares
 * parabola through the five samples ym2..yp2 centred on it,
 *
 *     d = -7 * (-2 ym2 - ym1 + yp1 + 2 yp2)
 *         / (10 * (2 ym2 - ym1 - 2 y0 - yp1 + 2 yp2))
 *
 * samples from y0. Five points rather than the three that just fit a
 * parabola exactly: sensor noise moves a three-point vertex a lot, and on
 * test_hrv's synthetic hearts with noise at 1 % of the pulse swing the
 * five-point fit read 4.1 ms of a true 3.6 where three points read 6.0
 * (whole samples: 9.8). The cost is that the beat is reported one sample
 * after its peak, when yp2 exists.
 *
 * Returned in 1/256 sample, clamped to +-1/2. A fit that is not concave
 * -- noise around a shallow peak -- returns 0, the whole-sample answer.
 */
static int16_t peak_offset_q8(int32_t ym2, int32_t ym1, int32_t y0,
			      int32_t yp1, int32_t yp2)
{
	int64_t num = -7 * (-2 * (int64_t)ym2 - ym1 + yp1 + 2 * (int64_t)yp2);
	int64_t den = 10 * (2 * (int64_t)ym2 - ym1 - 2 * (int64_t)y0 - yp1 +
			    2 * (int64_t)yp2);
	int64_t off;

	if (den >= 0) {
		return 0;
	}

	off = num * 256 / den;

	return (int16_t)CLAMP(off, -128, 128);
}

/* hr->pend: a peak found on the last sample, awaiting its fraction. */
#define PEND_NONE		0
#define PEND_TIME_ONLY		1	/* first beat, or one outside 30-220 bpm */
#define PEND_INTERVAL		2	/* an interval to report */

/* Nothing on the sensor: forget the rate, keep the filters running. */
static void forget_rate(struct hr *hr)
{
	hr->win_n = 0;
	hr->since_spectrum = 0;
	hr->spec_avg_primed = false;
	memset(hr->recent, 0, sizeof(hr->recent));
	hr->bpm_x10 = 0;
	hr->last_beat_tick = 0;
	hr->prev_ibi_trusted = false;
	hr->peak_level = 0;
	hr->pend = PEND_NONE;
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
		for (int i = 0; i < 5; i++) {
			hr->med[i] = x;
		}
		hr->baseline = x;
		hr->b_hp1 = x << 4;
		hr->s_hp1 = (x * HR_DECIM) << 2;
		hr->primed = true;
		if (bpm_x10) {
			*bpm_x10 = 0;
		}
		return false;
	}

	hr->med[hr->med_i] = x;
	hr->med_i = (hr->med_i + 1) % 5;

	int32_t xm = median5(hr->med);

	hr->baseline += (xm - hr->baseline) >> 6;

	/*
	 * Beat path, 100 sps, Q4. Two one-pole high-passes at ~0.5 Hz take
	 * out the drift that dominated the raw signal (84 % of its power
	 * below 0.6 Hz in the 2026-10-07 recording), and a ~4 Hz low-pass
	 * leaves the pulse shape.
	 */
	int32_t xs = xm << 4;

	hr->b_hp1 += (xs - hr->b_hp1) >> 5;
	int32_t h1 = xs - hr->b_hp1;

	hr->b_hp2 += (h1 - hr->b_hp2) >> 5;
	int32_t h2 = h1 - hr->b_hp2;

	hr->b_lp += (h2 - hr->b_lp) >> 2;
	int32_t y = hr->b_lp;

	/*
	 * Last sample's peak, now that the sample two after it exists: place
	 * it between samples (peak_offset_q8()) and report the beat.
	 */
	if (hr->pend != PEND_NONE) {
		int16_t frac = peak_offset_q8(hr->y4, hr->y3, hr->y2, hr->y1, y);

		if (hr->pend == PEND_INTERVAL) {
			hr->last_ibi = hr->pend_since;
			hr->last_ibi_q8 = (uint32_t)(
				(int32_t)(hr->pend_since * 256U) + frac -
				hr->last_beat_frac_q8);
			hr->last_ibi_trusted = hr->pend_trusted;
			hr->last_ibi_successive = hr->pend_successive;
			beat = true;
		}
		hr->last_beat_frac_q8 = frac;
		hr->pend = PEND_NONE;
	}

	hr->pp_max = MAX(hr->pp_max, y);
	hr->pp_min = MIN(hr->pp_min, y);
	if ((hr->ticks % hr->sample_rate_hz) == 0) {
		int32_t pp = (hr->pp_max - hr->pp_min) >> 4;

		hr->amplitude += (pp - hr->amplitude) >> 1;
		hr->pp_max = y;
		hr->pp_min = y;
		/* Let one tall artifact fade instead of blinding the detector. */
		hr->peak_level -= hr->peak_level >> 4;
	}

	/* Rate path: decimate to 25 sps, band-pass, keep the last 10 s. */
	hr->dec_acc += xm;
	if (++hr->dec_n == HR_DECIM) {
		int32_t v = hr->dec_acc << 2;

		hr->dec_acc = 0;
		hr->dec_n = 0;

		hr->s_hp1 += (v - hr->s_hp1) >> 3;
		int32_t g1 = v - hr->s_hp1;

		hr->s_hp2 += (g1 - hr->s_hp2) >> 3;

		/*
		 * Not differenced first. Tried 2026-10-12 to flatten the drift
		 * slope under the pulse: it lifts the harmonic and the noise
		 * floor instead, and the detector locked onto double the rate
		 * (+84 bpm mean across the recordings).
		 */
		hr->win[hr->win_i] = g1 - hr->s_hp2;
		hr->win_i = (hr->win_i + 1) % HR_WIN;
		if (hr->win_n < HR_WIN) {
			hr->win_n++;
		}

		if (hr->win_n == HR_WIN &&
		    ++hr->since_spectrum >= HR_SPECTRUM_FS) {
			hr->since_spectrum = 0;
			hr->recent[hr->recent_i] = spectrum_estimate(hr);
			hr->recent_i = (hr->recent_i + 1) % HR_RECENT;
			hr->bpm_x10 = combine_estimates(hr);
		}
	}

	/*
	 * No finger, no pulse. Checked after the filters so they keep
	 * tracking and a finger arriving does not start from stale state.
	 */
	if (hr->baseline < FINGER_DC_MIN) {
		forget_rate(hr);
		hr->y4 = hr->y3;
		hr->y3 = hr->y2;
		hr->y2 = hr->y1;
		hr->y1 = y;
		if (bpm_x10) {
			*bpm_x10 = 0;
		}
		return false;
	}

	/*
	 * Beats: a local maximum of the band-passed signal, at least half as
	 * tall as recent beats, outside a refractory window of 60 % of the
	 * period the spectral rate implies -- which is what keeps the
	 * dicrotic notch from counting as a beat. With no rate yet, only the
	 * 220 bpm limit applies, and nothing is trusted for HRV.
	 */
	if (hr->y1 > hr->y2 && hr->y1 >= y && hr->y1 > 0 &&
	    hr->y1 >= hr->peak_level / 2) {
		uint32_t period = hr->bpm_x10
				  ? (600U * hr->sample_rate_hz) / hr->bpm_x10 : 0;
		uint32_t refractory = period
				      ? (period * 6U) / 10U
				      : (60U * hr->sample_rate_hz) / BPM_MAX;
		uint32_t peak_tick = hr->ticks - 1;
		uint32_t since = peak_tick - hr->last_beat_tick;

		if (hr->last_beat_tick == 0 || since >= refractory) {
			hr->peak_level += (hr->y1 - hr->peak_level) >> 3;

			if (hr->last_beat_tick != 0) {
				uint32_t bpm = (60U * hr->sample_rate_hz) / since;

				if (bpm >= BPM_MIN && bpm <= BPM_MAX) {
					uint32_t d = (since > period)
						     ? since - period
						     : period - since;
					bool trusted = period != 0 &&
						       d * 100U <=
						       period * HRV_SPREAD_MAX_PCT;

					hr->beats++;
					/* Reported on the next sample. */
					hr->pend = PEND_INTERVAL;
					hr->pend_since = since;
					hr->pend_trusted = trusted;
					hr->pend_successive =
						trusted && hr->prev_ibi_trusted;
					hr->prev_ibi_trusted = trusted;
				} else {
					/* A missed run of beats: start over. */
					hr->prev_ibi_trusted = false;
				}
			}
			hr->last_beat_tick = peak_tick;
			/* Its fraction is still wanted, as the next one's start. */
			if (hr->pend == PEND_NONE) {
				hr->pend = PEND_TIME_ONLY;
			}
		}
	}

	hr->y4 = hr->y3;
	hr->y3 = hr->y2;
	hr->y2 = hr->y1;
	hr->y1 = y;

	if (bpm_x10) {
		*bpm_x10 = hr->bpm_x10;
	}

	return beat;
}
