/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025, 2026
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include <inttypes.h>
#include <stdio.h>
#if !defined(_MSC_VER)
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <minwindef.h>
#include <minwinbase.h>
#include <wtypes.h>
#include <apisetcconv.h>
#include <wincon.h>
#else
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <glob.h>
#if defined(__linux__)
#include <alloca.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#endif
#include <stdarg.h>
#include "types.h"
#include "str.h"

namespace volvox {

#if defined (_WIN32)
	extern "C" bool Glob_impl(char* buf, int s_len, int cur_index, const char* argv, char*** rets, size_t* n_rets, size_t* max_rets);
#endif

	_DECL void free_glob(volvox_glob_t* rets) {
#if defined (_WIN32)
		if (rets->dirs) {
			for (size_t i=0; i < rets->size; i++)
				free(rets->dirs[i]);
			free(rets->dirs);
		}
#else
		glob_t glob = {
			.gl_pathc = rets->size,
			.gl_pathv = rets->dirs,
		};
		globfree(&glob);
#endif
		rets->dirs = nullptr;
		rets->size = 0;
	}

	_DECL volvox_glob_t glob(const char* pattern) {
		volvox_glob_t rets = {
			.size = 0,
			.dirs = nullptr,
		};
#if defined (_WIN32)
		char buf[MAX_PATH] = "";
		size_t max_rets = 0;
		if (Glob_impl(buf, 0, 0, pattern, &rets.dirs, &rets.size, &max_rets))
			return rets;
		free_glob(&rets);
#else
		glob_t glob_rets = {
			.gl_pathc = 0,
			.gl_pathv = nullptr,
		};
		int res = ::glob(pattern, GLOB_MARK, NULL, &glob_rets);
		if (!res) {
			rets.size = glob_rets.gl_pathc;
			rets.dirs = glob_rets.gl_pathv;
		}
		// not clear if the glob_t struct must be freed in case of error... :-(
#endif
		return rets;
	}

	_DECL volvox_glob_t glob(const char* bases, const char* patterntail) {
		volvox_glob_t rets = {
			.size = 0,
			.dirs = nullptr,
		};
		size_t patterntaillen = strlen(patterntail);
		const char* baseptr = bases;
#if defined (_WIN32)
		size_t max_rets = 0;
#else
		glob_t glob_rets = {
			.gl_pathc = 0,
			.gl_pathv = nullptr,
		};
		int globflags = GLOB_MARK;
#endif
		while (*baseptr) {
			unsigned n = 0;
			while (*(baseptr+n) && *(baseptr+n) != PATHLISTSEP)
				n++;
			if (!n)
				continue;
			size_t patternlen = patterntaillen + 1 + n + 1;
			char* pattern = (char*)malloc(patternlen); // TODO use alloca() when switched back to MSVC
			memcpy(pattern, baseptr, n);
			baseptr += n;
			if (*baseptr)
				baseptr++; // eat ':'/';' but keep '\0'
			if (pattern[n-1] != '/' && pattern[n-1] != '\\')
				pattern[n++] = PATHDIRSEP;
			strncpy(pattern+n, patterntail, patternlen-n);
#if defined (_WIN32)
			char buf[MAX_PATH] = "";
			auto res = Glob_impl(buf, 0, 0, pattern, &rets.dirs, &rets.size, &max_rets);
			free(pattern);
#else
			int res = ::glob(pattern, globflags, NULL, &glob_rets);
			free(pattern);
			if (res && res != GLOB_NOMATCH) {
				// not clear if the glob_t struct must be freed in case of error... :-(
				// TODO: set errno to represent glob error
				return rets;
			}
			globflags |= GLOB_APPEND;
#endif
		}
#ifndef _WIN32
		rets.size = glob_rets.gl_pathc;
		rets.dirs = glob_rets.gl_pathv;
#endif
		return rets;
	}

#ifdef _WIN32
// dest must be 32767 bytes in size - maximum length of command line on Windows
	extern "C" bool getCmdLine(char* dest, const char* cmd, char* const argv[]);
	extern "C" bool volvox_spawn_c(int* pid, int* child_stdin, int* child_stdout,
	                               int* child_stderr, char* const argv[]);
#endif

	_DECL bool spawn(int* pid, int* child_stdin, int* child_stdout,
	                 int* child_stderr, char* const argv[]) {
#ifdef _WIN32
		return volvox_spawn_c(pid, child_stdin, child_stdout, child_stderr, argv);
#else
		int inpipefd[2];
		if (child_stdin)
			if(pipe(inpipefd))
				return false;
		int outpipefd[2];
		if (child_stdout)
			if(pipe(outpipefd))
				return false;
		int errpipefd[2];
		if (child_stderr)
			if(pipe(errpipefd))
				return false;
		pid_t childpid = fork();
		if (childpid) { // parent process
			if (pid)
				*pid = childpid;
			if (child_stdin) {
				*child_stdin = inpipefd[1];
				close(inpipefd[0]);
			}
			if (child_stdout) {
				*child_stdout = outpipefd[0];
				close(outpipefd[1]);
			}
			if (child_stderr) {
				*child_stderr = errpipefd[0];
				close(errpipefd[1]);
			}
			return true;
		} else { // child process
			if (child_stdin) {
				dup2(inpipefd[0], 0);
				close(inpipefd[1]);
			}
			if (child_stdout) {
				dup2(outpipefd[1], 1);
				close(outpipefd[0]);
			}
			if (child_stderr) {
				dup2(errpipefd[1], 2);
				close(errpipefd[0]);
			}
			if (execvp(argv[0], argv)) {
				fprintf(stderr, "Error calling '%s': %s\n", argv[0], strerror(errno));
				exit(1);
			}
		}
		return false;
#endif
	}

// Wait for a process to finish
// Return exit code of process
// return -1 and sets errno on failure
	_DECL int wait(int pid) {
#ifdef _WIN32
		HANDLE p_handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
		unsigned res = 0;
		BOOL res2 = 0;
		DWORD ecode = 0;
		if (!p_handle)
			goto error;
		res = WaitForSingleObject(p_handle, INFINITE);
		if (res)
			goto error;
		res2 = GetExitCodeProcess(p_handle, &ecode);
		if (!res2)
			goto error;
		return (int)ecode;
	error:
		errno = GetLastError();
		return -1;
#else
		int status;
		int res = waitpid(pid, &status, 0);
		if (res <= 0 || !WIFEXITED(status))
			return -1;
		return WEXITSTATUS(status);
#endif
	}

// Checks if a process has finished
// return STILL_ACTIVE (0x103, 259) if the process is still running
// Return exit code of process
// return -1 and sets errno on failure
	_DECL int try_wait(int pid) {
#ifdef _WIN32
		bool res2;
		HANDLE p_handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
		if (!p_handle)
			goto error;
		DWORD ecode;
		res2 = GetExitCodeProcess(p_handle, &ecode);
		if (!res2)
			goto error;
		return (int)ecode;
	error:
		errno = GetLastError();
		return -1;
#else
		int status;
		int res = waitpid(pid, &status, WNOHANG);
		if (!res)
			return STILL_ACTIVE;
		if (res < 0 || !WIFEXITED(status))
			return -1;
		return WEXITSTATUS(status);
#endif
	}
}

#ifdef _MSC_VER

/* In Volvox we always use standard "Itanium" mangling, however MSVC++ uses a different mangling scheme.
   For this reason we provide C-Style function wrappers with our mangling scheme */

_CDECL void _ZN6volvox9free_globEP13volvox_glob_t(volvox_glob_t* rets) {
	volvox::free_glob(rets);
}

_CDECL volvox_glob_t _ZN6volvox4globEPKc(const char* pattern) {
	return volvox::glob(pattern);
}

_CDECL volvox_glob_t _ZN6volvox4globEPKcS1_(const char* bases, const char* patterntail) {
	return volvox::glob(bases, patterntail);
}

_CDECL bool _ZN6volvox5spawnEPiS0_S0_S0_PKPc(int* pid, int* child_stdin, int* child_stdout,
                                             int* child_stderr, char* const argv[]) {
	return volvox::spawn(pid, child_stdin, child_stdout, child_stderr, argv);
}

_CDECL int _ZN6volvox4waitEi(int pid) {
	return volvox::wait(pid);
}

#endif
