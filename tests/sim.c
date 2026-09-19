#include "sim.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int sim_failures;
int sim_checks;

void sim_wearer_init(struct sim_wearer *w, double cadence_spm, double swing_mg,
		     int noise_mg)
{
	w->cadence_spm = cadence_spm;
	w->swing_mg = swing_mg;
	w->noise_mg = noise_mg;
	w->phase = 0;
	w->seed = 0x5eed1234u;
}

static int sim_noise(struct sim_wearer *w)
{
	if (w->noise_mg <= 0) {
		return 0;
	}

	w->seed = w->seed * 1103515245u + 12345u;

	return (int)((w->seed >> 16) % (2u * (unsigned)w->noise_mg + 1u)) -
	       w->noise_mg;
}

int32_t sim_sample(struct sim_wearer *w, int64_t dt_ms)
{
	if (w->cadence_spm <= 0) {
		/* Still: gravity plus sensor noise. */
		return 1000 + sim_noise(w);
	}

	w->phase += (w->cadence_spm / 60.0) * ((double)dt_ms / 1000.0) *
		    2.0 * M_PI;

	double s = 0.7 * sin(w->phase) + 0.3 * sin(2.0 * w->phase + 0.6);

	return 1000 + (int32_t)(w->swing_mg * s) + sim_noise(w);
}

void sim_check(bool ok, const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	sim_checks++;

	if (ok) {
		return;
	}

	sim_failures++;

	(void)file;
	printf("  FAIL (line %d): ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

void sim_section(const char *name)
{
	printf("\n-- %s\n", name);
}

int sim_report(const char *suite)
{
	printf("\n%s: %d checks, %d failures\n", suite, sim_checks,
	       sim_failures);

	return sim_failures ? 1 : 0;
}
