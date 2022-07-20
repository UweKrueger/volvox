#pragma once

/* This is another stub file to allow compiling part of libvolvox.dll
 * with "clang++ -target x86_64-pc-windows-gnu" (see comment in winstub.j) */

typedef __INT64_TYPE__ int64_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __INT16_TYPE__ int16_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __INT8_TYPE__ int8_t;
typedef __UINT8_TYPE__ uint8_t;
#ifdef _WIN64
typedef __UINT64_TYPE__ size_t;
typedef __UINT64_TYPE__ uintptr_t;
typedef __INT64_TYPE__ intptr_t;
#else
typedef __UINT32_TYPE__ size_t;
typedef __UINT32_TYPE__ uintptr_t;
typedef __INT32_TYPE__ intptr_t;
#endif
