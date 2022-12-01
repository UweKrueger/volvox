/*
 * Copyright © Uwe Krüger 2021, 2022
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#pragma once

#include <inttypes.h>
#include <stdbool.h>

// format flags

#define FMT_PREFIX_MASK 3U
#define FMT_PREFIX_NONE 0U
#define FMT_PREFIX_PLUS 1U
#define FMT_PREFIX_SPACE 2U

#define FMT_UPPER 4U
#define FMT_ZEROPAD 8U

#define FMT_DISPLAY_MASK 48U
#define FMT_DISPLAY_STD 0U
#define FMT_DISPLAY_FIXED 16U
#define FMT_DISPLAY_EXP 48U
#define FMT_DISPLAY_HEX 32U

#define FMT_ALT 64U
#define FMT_UNSIGNED 128U

#define F32_DEFAULT_PRECISION 8
#define F64_DEFAULT_PRECISION 17
#define ARRAY_DEFAULT_FIELD_WIDTH 8
#define ARRAY_DEFAULT_PRECISION 5

typedef struct volvox_glob_t {
	size_t size;
	char** dirs;
} volvox_glob_t;

#if defined (_WIN32)
#define _DECL __declspec(dllexport)
#define _CDECL extern "C" __declspec(dllexport)
#define PATHLISTSEP ';'
#define PATHDIRSEP '\\'
#else
#define _DECL
#define _CDECL extern "C"
#ifndef STILL_ACTIVE
#define STILL_ACTIVE 0x103
#endif
#define PATHLISTSEP ':'
#define PATHDIRSEP '/'
#endif

#ifndef volvox2cstr
#define volvox2cstr(v) (v - ((*(unsigned*)v + 3) & ~((1U << 31) | 3U)))
#endif

#ifdef __cplusplus

namespace volvox {

	_DECL volvox_glob_t glob(const char* pattern);

	_DECL volvox_glob_t glob(const char* bases, const char* patterntail);

	_DECL void free_glob(volvox_glob_t* rets);

	_DECL bool spawn(int* pid, int* child_stdin, int* child_stdout,
	                        int* child_stderr, char* const argv[]);

	_DECL int wait(int pid);

	_DECL int try_wait(int pid);

};

_CDECL void showtestres(int fd, int width, const char* testcase, bool result);

#endif
