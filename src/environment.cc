#include "global.h"

#if defined(_WIN32)
#include <windows.h>
#define BUF_IS_TOO_SMALL ERROR_INSUFFICIENT_BUFFER
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#define THISEXELINK "/proc/self/exe"
#elif defined(__OpenBSD__)
// needs probably fiddling with argv[0]/KERN_PROC_ARGS, PATH, realpath(), ...
#else
// what else - MacOS?
#endif

/* get the full path of the current executable
 * unfortunately there is no standard way to do this in a portable
 * way so we have to do some OS specific trickery... */
const char* getThisExePath() {
#if defined(_WIN32)
	return _pgmptr;
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
	size_t bufsize = 0;
	char* buf = nullptr;
	int cmd[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
	int res = sysctl(cmd, 4, buf, &bufsize, nullptr, 0);
	buf = (char*)malloc(bufsize);
	res = sysctl(cmd, 4, buf, &bufsize, nullptr, 0);
	return buf;
#elif defined(__linux__)
	// we cannot get the necessary buffer size in advance so
	// a loop is required to gradually increase the buffer
	uint32_t bufsize = 64;
	char* buf = nullptr; // to make realloc() behave like malloc()
	ssize_t res;
	do {
		bufsize = bufsize + (bufsize >> 1);
		buf = (char*)realloc(buf, bufsize);
		res = readlink(THISEXELINK, buf, bufsize);
		if (res < 0) {
			errs() << llvm::format("cannot read '%s': %s\n", strerror(errno));
			abort();
		}
	} while (res >= bufsize);
	buf[res] = '\0';
	return buf;
#else
	#error "this operating system is no supported (yet)"
#endif
}
