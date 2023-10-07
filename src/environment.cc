/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include "global.h"

#define VOLVOX_ROOT "VOLVOX"
#define VOLVOX_LIB "VOLVOX_LIB"
#define VOLVOX_PROJECT "VOLVOX_PROJECT"

#if defined(_WIN32)
#include <windows.h>
#define BUF_IS_TOO_SMALL ERROR_INSUFFICIENT_BUFFER
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#define THISEXELINK "/proc/self/exe"
#elif defined(__OpenBSD__) || defined(__HAIKU__)
// needs probably fiddling with argv[0]/KERN_PROC_ARGS, PATH, realpath(), ...
#else
// what else - MacOS?
#endif

/* get the full path of the current executable
 * unfortunately there is no standard way to do this in a portable
 * way so we have to do some OS specific trickery... */
const char* getThisExePath() {
	static char* volvox_exe_path = nullptr;
	if (volvox_exe_path)
		return volvox_exe_path;
#if defined(_WIN32)
	errno_t err = _get_pgmptr(&volvox_exe_path);
	if (volvox_exe_path) {
		return volvox_exe_path;
	} else {
		errno = err;
		goto generror;
	}
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
	// all BSDs (except OpenBSD) have a KERN_PROC_PATHNAME sysctl
	size_t bufsize = 0;
#if defined(__NetBSD__)
	char* oldbuf;
	int cmd[4] = { CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME };
#else
	int cmd[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
#endif
	int res = sysctl(cmd, 4, volvox_exe_path, &bufsize, nullptr, 0);
	if (res == -1)
		goto generror;
	volvox_exe_path = (char*)malloc(bufsize);
	if (!volvox_exe_path)
		goto generror;
	res = sysctl(cmd, 4, volvox_exe_path, &bufsize, nullptr, 0);
	if (res == -1)
		goto generror;
#if defined(__NetBSD__)
	oldbuf = volvox_exe_path;
	volvox_exe_path = realpath(oldbuf, nullptr);
	if (!volvox_exe_path)
		goto generror;
	free(oldbuf);
#endif
	return volvox_exe_path;
#elif defined(__linux__)
	// we cannot get the necessary buffer size in advance so
	// a loop is required to gradually increase the buffer
	uint32_t bufsize = 64;
	ssize_t res;
	do {
		bufsize = bufsize + (bufsize >> 1);
		volvox_exe_path = (char*)realloc(volvox_exe_path, bufsize);
		if (!volvox_exe_path)
			goto generror;
		res = readlink(THISEXELINK, volvox_exe_path, bufsize);
		if (res < 0)
			goto generror;
	} while (res >= bufsize);
	volvox_exe_path[res] = '\0';
	return volvox_exe_path;
#elif defined(__OpenBSD__) || defined(__HAIKU__)
	// we rely on environment variables and hard coded paths for now
	return nullptr;
#else
#error "this operating system is no supported (yet)"
#endif
generror:
	errs() << llvm::format("cannot get 'volvox' full pathname: %s\n", strerror(errno));
	abort();
}

const char* volvox_root() {
	static char* root = nullptr; // static variable to cache result
	if (root)
		return root;
	root = getenv(VOLVOX_ROOT);
	if (root)
		return root;
#if defined(__OpenBSD__)
	// getThisExePath() does not work - return supposed installation directory
	return "/usr/local";
#else
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
#endif
}

const char* volvox_lib() {
	static char* lib = nullptr; // cache result
	if (lib)
		return lib;
	lib = getenv(VOLVOX_LIB);
	if (lib)
		return lib;
	static const char* project = getenv(VOLVOX_PROJECT);
	if (!project)
		project = ".";
	// get root and append 'lib'
	const char* root = volvox_root();
	size_t l = strlen(root);
	size_t pl = strlen(project);
	//                                  x   /  lib  :   xx   0
	char* lib_from_root = (char*)malloc(l + 1 + 3 + 1 + pl + 1);
	memcpy(lib_from_root, root, l);
	lib_from_root[l++] = PATHDIRSEP;
	lib_from_root[l++] = 'l';
	lib_from_root[l++] = 'i';
	lib_from_root[l++] = 'b';
	lib_from_root[l++] = PATHLISTSEP;
	memcpy(lib_from_root + l, project, pl + 1);
	lib = lib_from_root;
	return lib;
}
