#include "global.h"

#define VOLVOX_ROOT "VOLVOX"
#define VOLVOX_LIB "VOLVOX_LIB"

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
	// all BSDs (except OpenBSD) have a KERN_PROC_PATHNAME sysctl
	size_t bufsize = 0;
	char* buf = nullptr;
#if defined(__NetBSD__)
	char* oldbuf;
	int cmd[4] = { CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME };
#else
	int cmd[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
#endif
	int res = sysctl(cmd, 4, buf, &bufsize, nullptr, 0);
	if (res == -1)
		goto generror;
	buf = (char*)malloc(bufsize);
	if (!buf)
		goto generror;
	res = sysctl(cmd, 4, buf, &bufsize, nullptr, 0);
	if (res == -1)
		goto generror;
#if defined(__NetBSD__)
	oldbuf = buf;
	buf = realpath(oldbuf, nullptr);
	if (!buf)
		goto generror;
	free(oldbuf);
#endif
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
		if (!buf)
			goto generror;
		res = readlink(THISEXELINK, buf, bufsize);
		if (res < 0)
			goto generror;
	} while (res >= bufsize);
	buf[res] = '\0';
	return buf;
#else
#error "this operating system is no supported (yet)"
#endif
generror:
	errs() << llvm::format("cannot get 'volvox' full pathname: %s\n", strerror(errno));
	abort();
}

const char* volvox_root() {
	static char* root = nullptr; // cache result
	if (root)
		return root;
	root = getenv(VOLVOX_ROOT);
	if (root)
		return root;
	const char* exe_path = getThisExePath();
	size_t l = strlen(exe_path);
	// cut off last element
	for (l -= 1; l && exe_path[l] != '/' && exe_path[l] != '\\'; l--);
	// if the last part is '/bin' strip that, too
	if (l >= 4 &&
	    (exe_path[l-4] == '/' || exe_path[l-4] == '\\') &&
	    (exe_path[l-3] == 'b' || exe_path[l-3] == 'B') &&
	    (exe_path[l-2] == 'i' || exe_path[l-2] == 'I') &&
	    (exe_path[l-1] == 'n' || exe_path[l-1] == 'N'))
		l -= 4;
	// we must not modify exe_path - so make a copy
	char* root_from_exe = (char*)malloc(l+1);
	memcpy(root_from_exe, exe_path, l);
	root_from_exe[l] = '\0';
	root = root_from_exe;
	return root;
}

const char* volvox_lib() {
	static char* lib = nullptr; // cache result
	if (lib)
		return lib;
	lib = getenv(VOLVOX_LIB);
	if (lib)
		return lib;
	// get root and append 'lib'
	const char* root = volvox_root();
	size_t l = strlen(root);
	char* lib_from_root = (char*)malloc(l + 4 + 1);
	memcpy(lib_from_root, root, l);
#if defined (_MSC_VER)
	lib_from_root[l++] = '\\';
#else
	lib_from_root[l++] = '/';
#endif
	lib_from_root[l++] = 'l';
	lib_from_root[l++] = 'i';
	lib_from_root[l++] = 'b';
	lib_from_root[l] = '\0';
	lib = lib_from_root;
	return lib;
}
