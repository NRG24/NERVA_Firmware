/*
 * A simulated wearer, and a tiny check framework.
 *
 * The point of this file is that the accelerometer signal is generated
 * from a clock rather than from a fixed-rate recording: sim_sample() asks
 * "what does the magnitude read at time t", so a test can sample it at
 * whatever interval the firmware would have chosen. That is what makes it
 * possible to test main.c's poll-rate policy rather than just the filters.
 *
 * Everything is deterministic -- a hand-rolled LCG, no rand() -- so a
 * failure reproduces exactly on someone else's machine.
 */

#ifndef SIM_H_
#define SIM_H_

#include <stdbool.h>
#include <stdint.h>

/* --- the wearer -------------------------------------------------------- */

struct sim_wearer {
	double cadence_spm;	/* 0 = not walking */
	double swing_mg;	/* peak magnitude excursion of a footfall */
	int noise_mg;		/* +/- sensor noise */

	/* internal */
	double phase;
	uint32_t seed;
};

void sim_wearer_init(struct sim_wearer *w, double cadence_spm, double swing_mg,
		     int noise_mg);

/*
 * Magnitude in milli-g at the current instant, advancing the wearer's gait
 * phase by dt_ms. Models a footfall as a fundamental plus a second
 * harmonic, which is closer to a real impact than a pure sine and is a
 * fairer test of a peak detector.
 */
int32_t sim_sample(struct sim_wearer *w, int64_t dt_ms);

/* --- check framework --------------------------------------------------- */

extern int sim_failures;
extern int sim_checks;

void sim_check(bool ok, const char *file, int line, const char *fmt, ...);
void sim_section(const char *name);
int sim_report(const char *suite);

#define CHECK(cond, ...) \
	sim_check((cond), __FILE__, __LINE__, __VA_ARGS__)

/* Within tol_pct of want. */
#define CHECK_NEAR(got, want, tol_pct, label)				\
	do {								\
		double _g = (double)(got), _w = (double)(want);		\
		double _err = (_w == 0) ? (_g == 0 ? 0 : 100)		\
					: (_g - _w) * 100.0 / _w;	\
		sim_check(_err <= (tol_pct) && _err >= -(tol_pct),	\
			  __FILE__, __LINE__,				\
			  "%s: got %.0f, want %.0f (%+.1f%%, tol %.1f%%)", \
			  (label), _g, _w, _err, (double)(tol_pct));	\
	} while (0)

#endif /* SIM_H_ */
