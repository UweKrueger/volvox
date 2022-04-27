#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <stdlib.h>

#define RESP "Got: "
#define RLEN (sizeof(RESP) - 1)
#define BSIZE (1 << 15)
#define MSG(s) s, sizeof(s) - 1

int main(int argc, char* argv[]) {
	write(2, MSG("Child started\n"));
	int n;
	char buf[BSIZE] = RESP;
	do {
		int i;
		write(2, MSG("Child trying to read\n"));
		for (i = RLEN; i < BSIZE; ) {
			n = read(0, buf + i, 1);
			if (buf[i++] == '\n')
				break;
		}
		write(2, MSG("Child trying to write\n"));
		int r = write(1, buf, i);
		if (r != i)
			exit(1);
	} while (n);
}
