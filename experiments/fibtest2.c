#include <stdio.h>
#include <stdlib.h>

double fib(double x) {
	return (x < 3) ? 1 : fib(x-1) + fib(x-2);
}

extern double fibi(double x) {
	double a=1;
	double b=1;
	for (double i=3; i<=x; i++) {
		double c = a+b;
		a=b;
		b=c;
	}
	return b;
}

int main(int argc, char* argv[]) {
	double n = atof(argv[1]);
	double f1 = fib(n);
	double f2 = fibi(n);
	printf("%g %g\n", f1, f2);
}
