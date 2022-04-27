#include <unistd.h>
#include <stdlib.h>

#define RESP "Got: "
#define RLEN (sizeof(RESP) - 1)
#define BSIZE (1 << 15)

int main(int argc, char* argv[]) {
	int n;
	char buf[BSIZE] = RESP;
	do {
		int i;
		for (i = RLEN; i < BSIZE; ) {
			n = read(0, buf + i, 1);
			if (buf[i++] == '\n')
				break;
		}
		int r = write(1, buf, i);
		if (r != i)
			exit(1);
	} while (n);
}
