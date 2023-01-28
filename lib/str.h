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
#define SIZE_T_BITS ((sizeof(size_t) == 8) ? 64 : (sizeof(size_t) == 4) ? 32 : 16)
#define volvox_string_len(v) ((*(size_t*)v & ~((size_t)1 << (SIZE_T_BITS-1))) - 1)
// an LLVM implementation of this function is available as Volvox2CStr()
#define volvox2cstr(v) (v - ((*(size_t*)v + (sizeof(size_t)-1)) & ~(((size_t)1 << (SIZE_T_BITS-1)) | (sizeof(size_t)-1))))
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
_CDECL char* __cstr2volvoxstr(const char* c_str);

#endif

#ifndef cstr2volvoxstr
#define cstr2volvoxstr(result, lalloc, target, cstr, allocfn)	  \
	size_t _l = strlen(cstr); \
	lalloc = (_l+2*target_bytes) & ~(size_t)(target_bytes-1); /* add space for \0 and one aligend size_t */ \
	target = (char*)(((size_t)allocfn(lalloc + target_bytes - 1) + target_bytes - 1) & ~(size_t)(target_bytes - 1)); /* create target_bytes-byte aligned space */ \
	strcpy(target, cstr); \
	for (size_t n = _l; n < lalloc-target_bytes; n++) \
		target[n]=0; /* make sure padding is zerored */ \
	result = target + lalloc - target_bytes; \
	if (target_bytes == 8) \
		*(uint64_t*)result = _l + 1; /* store size including terminating 0 - make calculation of start easier */ \
	else if (target_bytes == 4) \
		*(uint32_t*)result = _l + 1; \
	else \
		*(uint16_t*)result = _l + 1
#endif
