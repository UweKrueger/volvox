/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#pragma once

#include <inttypes.h>
#include <stdbool.h>

// format flags

#define FMT_ALT            (1U << 0)
#define FMT_ZEROPAD        (1U << 1)
#define FMT_PREFIX_SPACE   (1U << 2)
#define FMT_PREFIX_PLUS    (1U << 3)
#define FMT_GROUPED        (1U << 4)

#define FMT_HAVE_WIDTH     (1U << 5)
#define FMT_HAVE_PRECISION (1U << 6)

#define FMT_DISPLAY_HEX    (1U << 7)
#define FMT_UPPER          (1U << 8)

#define FMT_FLOAT          (1U << 9)
#define FMT_DISPLAY_FIXED (1U << 10)
#define FMT_DISPLAY_EXP   (1U << 11)

#define FMT_LONG          (1U << 12)
#define FMT_DISPLAY_OCT   (1U << 13)
#define FMT_UNSIGNED      (1U << 14)
#define FMT_CHAR          (1U << 15)
#define FMT_STRING        (1U << 16)

#define F32_DEFAULT_PRECISION 7
#define F64_DEFAULT_PRECISION 16
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
#ifdef _WIN64
typedef long long int ssize_t;
#else
typedef int ssize_t;
#endif
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
#define SIZE_T_BITS ((sizeof(size_t) == 8) ? 64 : (sizeof(size_t) == 4) ? 32 : 16)
#define volvox_string_len(v) (*(size_t*)v - 1)
// an LLVM implementation of this function is available as Volvox2CStr()
#define volvox2cstr(v) (char*)((uintptr_t)(v - *(size_t*)v) & ~((sizeof(size_t)-1)))
#endif
#ifndef STR_WRITE
#define STR_WRITE(s) s, sizeof(s)-1
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
_CDECL char* __cstr2volvoxstr(const char* c_str, size_t len, bool mark_as_heap);
_CDECL uint32_t utf8_decode(const char** s);
_CDECL uint32_t utf8_encode(uint32_t codepoint);
_CDECL void __trim_cstring(char* s, ssize_t* l, char d);
_CDECL void __setup_console();

#ifdef _WIN32

_CDECL ssize_t getline(char** buf, size_t* sz, FILE* f);

#endif
#endif

#ifndef cstr2volvoxstr
#define cstr2volvoxstr_l(result, lalloc, target, cstr, allocfn, _l, cap)	  \
	lalloc = (_l+3*target_bytes) & ~(size_t)(target_bytes-1); /* add space for \0 and two aligend size_t */ \
	target = (char*)allocfn(lalloc); /* create target_bytes-byte aligned space */ \
	result = target + lalloc - 2*target_bytes; \
	*((size_t*)result - 1) = 0; \
	*(size_t*)result = _l + 1; /* store size including terminating 0 - make calculation of start easier */ \
	*((size_t*)result + 1) = cap; \
	memcpy(target, cstr, _l)

#define cstr2volvoxstr(result, lalloc, target, cstr, allocfn, cap)	  \
	size_t _l = strlen(cstr); \
	cstr2volvoxstr_l(result, lalloc, target, cstr, allocfn, _l, cap)
#endif
