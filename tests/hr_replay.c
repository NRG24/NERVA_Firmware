/*
 * Feed a recorded raw PPG file through the real src/hr.c, for
 * tools/hr_eval.py to score against a reference monitor.
 *
 *   hr_replay <file>      one raw sample per line; non-numeric lines skipped
 *
 * Prints one line a second, "S <second> <bpm_x10>", and one per beat,
 * "B <sample> <ibi_ms> <trusted> <successive>". Built by hr_eval.py with
 * whatever -DHR_* gate values it is sweeping.
 */

#include "hr.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	static struct hr h;
	char line[64];
	unsigned long n = 0;
	uint16_t bpm = 0;
	FILE *f;

	if (argc < 2 || !(f = fopen(argv[1], "r"))) {
		fprintf(stderr, "usage: hr_replay <file>\n");
		return 2;
	}

	hr_init(&h, 100);
	while (fgets(line, sizeof(line), f)) {
		char *end;
		unsigned long v = strtoul(line, &end, 10);

		if (end == line) {
			continue;
		}
		n++;
		if (hr_update(&h, (uint32_t)v, &bpm)) {
			printf("B %lu %u %d %d\n", n, hr_last_ibi_ms(&h),
			       hr_last_ibi_trusted(&h), hr_last_ibi_successive(&h));
		}
		if (n % 100 == 0) {
			printf("S %lu %u\n", n / 100, bpm);
		}
	}
	fclose(f);
	return 0;
}
