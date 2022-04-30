#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>

#define RESP "Got: "
#define RLEN (sizeof(RESP) - 1)
#define BSIZE (1 << 15)
#define MSG(s) s, sizeof(s) - 1

int main(int argc, char* argv[]) {
	write(2, MSG("Child started\n"));
	for (int i=0; i<argc; i++) {
		write(2, MSG("Arg: >"));
		write(2, argv[i], strlen(argv[i]));
		write(2, MSG("<\n"));
	}
	int n;
	char buf[BSIZE] = RESP;
	do {
		int i;
		write(2, MSG("Child trying to read\n"));
		for (i = RLEN; i < BSIZE; ) {
			n = read(0, buf + i, 1);
			if (!n) {
				write(2, MSG("Child got EOF - exiting\n"));
				exit(0);
			}
			if (buf[i++] == '\n')
				break;
		}
		write(2, MSG("Child trying to write\n"));
		int r = write(1, buf, i);
		if (r != i)
			exit(1);
	} while (n);
}
