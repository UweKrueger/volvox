#include <stdio.h>

extern double qwert_z(int, double);

_Bool use_capi() {
	double r = qwert_z(7, 12.25);
	printf("expected: %.2f, got: %.2f -> %s\n", 47.25, r,
	       r == 47.25 ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m");
	return r == 47.25;
}
