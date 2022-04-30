#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "../lib/str.h"

#define RESP "Received: "
#define RLEN (sizeof(RESP) - 1)
#define BSIZE (1 << 15)
#define MSG(s) s, sizeof(s) - 1

int main(int argc, char* argv) {
	char buf[BSIZE] = RESP;
	const char* msgs[3] = { "Hello there\n", "What's up?\n", "Bye!\n" };
	int c_in, c_out;
	char* const args[] = {
#ifdef _WIN32
		"echoer",
#else
		"./echoer",
#endif
		"1st argument",
		"2nd-arg",
		NULL };
	write(2, MSG("starting other process\n"));
	if (!volvox_spawn(NULL, &c_in, &c_out, NULL, args)) {
		write(2, MSG("Could not spawn process\n"));
		exit(1);
	}
	write(2, MSG("process spawned\n"));
	int n;
	for (int k = 0; k < 3; k++) {
		write(2, MSG("Parent: Try to send msg\n"));
		n = write(c_in, msgs[k], strlen(msgs[k]));
		if (!n) {
			write(2, MSG("Could not send message\n"));
			exit(1);
		}
		else
			write(2, MSG("Message sent\n"));
		int i;
		for (i = RLEN; i < BSIZE; ) {
			n = read(c_out, buf + i, 1);
			if (buf[i++] == '\n')
				break;
		}
		int r = write(1, buf, i);
		if (r != i)
			exit(1);
	}
}
