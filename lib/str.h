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

typedef struct volvox_glob_t {
	size_t size;
	char** dirs;
} volvox_glob_t;

#if defined (_MSC_VER)
#define _DECL __declspec(dllexport)
#define _CDECL __declspec(dllexport)
#else
#define _DECL
#define _CDECL extern "C"
#endif


#ifdef __cplusplus
extern "C"
{
#endif

	_DECL void volvox_free_glob(volvox_glob_t* rets);

	_DECL volvox_glob_t volvox_glob(const char* pattern);

	_DECL bool volvox_spawn(int* pid, int* child_stdin, int* child_stdout,
	                        int* child_stderr, char* const argv[]);

#ifdef __cplusplus
}
#endif
