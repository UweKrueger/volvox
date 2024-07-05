#include <stdio.h>

extern int capi_cdefs_init(int argc, char* argv[]);
extern double qwert_z(int, double);

int main(int argc, char* argv[]) {
	int volvox_init_res = capi_cdefs_init(argc, argv);
	if (volvox_init_res)
		return volvox_init_res;
	double r = qwert_z(7, 12.25);
	printf("expected: %.2f, got: %.2f -> %s\n", 47.25, r,
	       r == 47.25 ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m");
	return r == 47.25;
}
