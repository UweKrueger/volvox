#include <stdio.h>
#include <stdlib.h>

extern double fib(double x);
extern double fibi(double x);

int main(int argc, char* argv[]) {
	double n = atof(argv[1]);
	double f1 = fib(n);
	double f2 = fibi(n);
	printf("%g %g\n", f1, f2);
}
