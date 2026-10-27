/*
 * Feed a recorded accelerometer trace through a steps.c, for
 * tools/steps_eval.py to score against a counted walk.
 *
 *   steps_replay <file>      lines of "<t_ms> <mg>"; others skipped
 *
 * Prints the final count, "TOTAL <steps>". Built by steps_eval.py against
 * the current src/steps.c and against the frozen test copy, so the two can
 * be compared on the same recording.
 */

#include "steps.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	char line[96];
	FILE *f;

	if (argc < 2 || !(f = fopen(argv[1], "r"))) {
		fprintf(stderr, "usage: steps_replay <file>\n");
		return 2;
	}

	steps_init();
	while (fgets(line, sizeof(line), f)) {
		char *end;
		long long t = strtoll(line, &end, 10);

		if (end == line) {
			continue;
		}

		char *end2;
		long mg = strtol(end, &end2, 10);

		if (end2 == end) {
			continue;
		}
		steps_update((int32_t)mg, (int64_t)t);
	}
	fclose(f);
	printf("TOTAL %u\n", steps_count());
	return 0;
}
