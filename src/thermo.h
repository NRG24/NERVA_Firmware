/*
 * Ring temperature, and hot-flash detection from it.
 *
 * A hot flash opens the blood vessels in the hands: finger skin warms by
 * one to a few degrees over a minute or two, stays up for a few minutes
 * and drifts back. That is a rise far faster than anything else a still,
 * worn ring sees -- room temperature, a blanket and falling asleep all
 * move it over tens of minutes -- and the shape this module looks for.
 *
 * SOURCE. The nRF die temperature, sampled by main.c every ~10 s. The die
 * sits inside the ring against the finger, so it follows skin temperature
 * with a lag of a minute or so and some attenuation, at 0.25 C resolution.
 * The PPG sensor face reads skin more directly but is only powered inside
 * measurement windows, so it goes into the history instead (skin_c100)
 * and this works on the one source that is always there.
 *
 * ALGORITHM. Two trackers on the samples:
 *
 *   temp      an exponential average, tau 60 s, to take the edge off the
 *             0.25 C steps
 *   baseline  where temp has been settled: rises with tau 40 min, falls
 *             with tau 5 min, so it hugs the lower edge of slow drift and
 *             a quick rise stands clear of it
 *
 * An onset needs both: temp at least rise_c10 above the baseline, and at
 * least three quarters of that gained in the last 10 minutes. The second
 * part is what rejects the slow warming of the hands at sleep onset. The
 * flash ends when temp is back within 0.3 C of the baseline, or after 30
 * minutes, whichever comes first; the baseline is frozen meanwhile, and a
 * 15-minute refractory period follows.
 *
 * GATES. Nothing is detected until the ring has been worn, off the charger
 * and out of a workout for 30 minutes without a break -- putting a ring on
 * warms it by ten degrees, and so does exercise -- and in the night-only
 * mode (the default) until the wearer has been asleep for 20 minutes.
 *
 * UNVALIDATED. The shape is from the literature on finger temperature in
 * hot flashes; the thresholds are a starting point, not a fit. A hand
 * wrapped round a hot mug, or pulled out from under a duvet and put back,
 * will look the same. The phone app is meant to let the wearer confirm or
 * dismiss each event, which is the data that would tune rise_c10.
 */

#ifndef THERMO_H_
#define THERMO_H_

#include <stdbool.h>
#include <stdint.h>

/* "No reading" for every temperature this module hands out. */
#define THERMO_NONE		INT16_MIN

/* Values for the detection mode. On the wire: opcode 0x13. */
#define THERMO_MODE_OFF		0
#define THERMO_MODE_NIGHT	1	/* only while asleep */
#define THERMO_MODE_ALWAYS	2

/* Range opcode 0x13 accepts for rise_c10. */
#define THERMO_RISE_MIN_C10	3
#define THERMO_RISE_MAX_C10	50

void thermo_init(void);

/* Mode and onset threshold. Out-of-range values are refused, -EINVAL. */
int thermo_set_config(uint8_t mode, uint8_t rise_c10);
void thermo_get_config(uint8_t *mode, uint8_t *rise_c10);

/*
 * One temperature sample, 0.01 C. `worn` is main.c's last wear verdict,
 * `blocked` is true on the charger or in a workout, `asleep` is sleep.c's
 * verdict. Any spacing works; main.c uses ~10 s.
 */
void thermo_feed(int16_t temp_c100, int64_t now_ms, bool worn, bool blocked,
		 bool asleep);

/* True once for each hot flash that starts. */
bool thermo_take_hot_flash_start(void);

bool thermo_in_hot_flash(void);

/* Settled long enough to judge, see GATES. */
bool thermo_ready(void);

/* Smoothed temperature; THERMO_NONE before the first sample. */
int16_t thermo_temp_c100(void);

/* Baseline; THERMO_NONE while not ready. */
int16_t thermo_baseline_c100(void);

/*
 * Highest temp-over-baseline seen while ready since the last call, in
 * 0.1 C, clamped to 0..255. For the history record; reading resets it.
 */
uint8_t thermo_take_peak_rise_c10(void);

/* Hot flashes since boot, wrapping at 256. */
uint8_t thermo_hot_flash_count(void);

#endif /* THERMO_H_ */
