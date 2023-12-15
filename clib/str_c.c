/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include <stdio.h>
#include <limits.h>
#include <inttypes.h>
#include "types.h"
#include "str.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <malloc.h>
#include <mbstring.h>
#if defined(_MSC_VER)
#include <BaseTsd.h>
#define complex_float _Fcomplex
#define complex_double _Dcomplex
#else
#define complex_float complex float
#define complex_double complex double 
#endif
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <glob.h>
#if defined(__linux__)
#include <alloca.h>
#endif
#endif
#include <locale.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <complex.h>

#define nullptr ((void*)0)

/* create the printf-format string to print given Type */

static const char* getFmtInt(unsigned fmt_flags) {
	if ((fmt_flags & FMT_PREFIX_MASK) == FMT_PREFIX_NONE)
		if ((fmt_flags & FMT_UPPER) == 0U)
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%*d";
					else
						return "%*u";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#*x";
					else
						return "%*x";
			else // FMT_ZEROPAD
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%0*d";
					else
						return "%0*u";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#0*x";
					else
						return "%0*x";
		else // FMT_UPPER
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%*D";
					else
						return "%*U";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#*X";
					else
						return "%*X";
			else // FMT_ZEROPAD
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%0*D";
					else
						return "%0*U";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#0*X";
					else
						return "%0*X";
	else // FMT_PREFIX...
		if (fmt_flags & (FMT_DISPLAY_HEX | FMT_UNSIGNED))
			return NULL;
		else
			if ((fmt_flags & FMT_PREFIX_MASK) == FMT_PREFIX_PLUS)
				if ((fmt_flags & FMT_UPPER) == 0U)
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "%+*d";
					else // FMT_ZEROPAD
						return "%0+*d";
				else // FMT_UPPER
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "%+*D";
					else // FMT_ZEROPAD
						return "%0+*D";
			else // FMT_PREFIX_SPACE
				if ((fmt_flags & FMT_UPPER) == 0U)
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "% *d";
					else // FMT_ZEROPAD
						return "%0 *d";
				else // FMT_UPPER
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "% *D";
					else // FMT_ZEROPAD
						return "%0 *D";
}

static const char* getFmtLong(unsigned fmt_flags) {
	if ((fmt_flags & FMT_PREFIX_MASK) == FMT_PREFIX_NONE)
		if ((fmt_flags & FMT_UPPER) == 0U)
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%*lld";
					else
						return "%*llu";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#*llx";
					else
						return "%*llx";
			else // FMT_ZEROPAD
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%0*lld";
					else
						return "%0*llu";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#0*llx";
					else
						return "%0*llx";
		else // FMT_UPPER
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%*llD";
					else
						return "%*llU";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#*llX";
					else
						return "%*llX";
			else // FMT_ZEROPAD
				if ((fmt_flags & FMT_DISPLAY_HEX) == 0U)
					if ((fmt_flags & FMT_UNSIGNED) == 0U)
						return "%0*llD";
					else
						return "%0*llU";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%#0*llX";
					else
						return "%0*llX";
	else // FMT_PREFIX...
		if (fmt_flags & (FMT_DISPLAY_HEX | FMT_UNSIGNED))
			return NULL;
		else
			if ((fmt_flags & FMT_PREFIX_MASK) == FMT_PREFIX_PLUS)
				if ((fmt_flags & FMT_UPPER) == 0U)
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "%+*lld";
					else // FMT_ZEROPAD
						return "%0+*lld";
				else // FMT_UPPER
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "%+*llD";
					else // FMT_ZEROPAD
						return "%0+*llD";
			else // FMT_PREFIX_SPACE
				if ((fmt_flags & FMT_UPPER) == 0U)
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "% *lld";
					else // FMT_ZEROPAD
						return "%0 *lld";
				else // FMT_UPPER
					if ((fmt_flags & FMT_ZEROPAD) == 0U)
						return "% *llD";
					else // FMT_ZEROPAD
						return "%0 *llD";
}

static const char* getFmtFlt(unsigned fmt_flags) {
	if ((fmt_flags & FMT_PREFIX_MASK) == FMT_PREFIX_NONE)
		if ((fmt_flags & FMT_UPPER) == 0U)
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%*.*g";
					else
						return "%#*.*g";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%*.*f";
					else
						return "%#*.*f";
				else // FMT_DISPLAY_HEX
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%*.*a";
					else
						return "%#*.*a";
			else // FMT_ZEROPAD
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0*.*g";
					else
						return "%#0*.*g";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0*.*f";
					else
						return "%#0*.*f";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0*.*a";
					else
						return "%#0*.*a";
		else // FMT_UPPER
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%*.*G";
					else
						return "%#*.*G";
				else if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%*.*F";
					else
						return "%#*.*F";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%*.*A";
					else
						return "%#*.*A";
			else
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0*.*G";
					else
						return "%#0*.*G";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0*.*F";
					else
						return "%#0*.*F";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0*.*A";
					else
						return "%#0*.*A";
	else if ((fmt_flags & FMT_PREFIX_MASK) == FMT_PREFIX_PLUS)
		if ((fmt_flags & FMT_UPPER) == 0U)
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%+*.*g";
					else
						return "%#+*.*g";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%+*.*f";
					else
						return "%#+*.*f";
				else // FMT_DISPLAY_HEX
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%+*.*a";
					else
						return "%#+*.*a";
			else // FMT_ZEROPAD
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0+*.*g";
					else
						return "%#0+*.*g";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0+*.*f";
					else
						return "%#0+*.*f";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0+*.*a";
					else
						return "%#0+*.*a";
		else // FMT_UPPER
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%+*.*G";
					else
						return "%#+*.*G";
				else if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%+*.*F";
					else
						return "%#+*.*F";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%+*.*A";
					else
						return "%#+*.*A";
			else
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0+*.*G";
					else
						return "%#0+*.*G";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0+*.*F";
					else
						return "%#0+*.*F";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0+*.*A";
					else
						return "%#0+*.*A";
	else // FMT_PREFIX_SPACE
		if ((fmt_flags & FMT_UPPER) == 0U)
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "% *.*g";
					else
						return "%# *.*g";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "% *.*f";
					else
						return "%# *.*f";
				else // FMT_DISPLAY_HEX
					if ((fmt_flags & FMT_ALT) == 0U)
						return "% *.*a";
					else
						return "%# *.*a";
			else // FMT_ZEROPAD
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0 *.*g";
					else
						return "%#0 *.*g";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0 *.*f";
					else
						return "%#0 *.*f";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0 *.*a";
					else
						return "%#0 *.*a";
		else // FMT_UPPER
			if ((fmt_flags & FMT_ZEROPAD) == 0U)
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "% *.*G";
					else
						return "%# *.*G";
				else if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "% *.*F";
					else
						return "%# *.*F";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "% *.*A";
					else
						return "%# *.*A";
			else
				if ((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_STD)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0 *.*G";
					else
						return "%#0 *.*G";
				else if((fmt_flags & FMT_DISPLAY_MASK) == FMT_DISPLAY_FIXED)
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0 *.*F";
					else
						return "%#0 *.*F";
				else
					if ((fmt_flags & FMT_ALT) == 0U)
						return "%0 *.*A";
					else
						return "%#0 *.*A";
}

// find maximum of 2 numbers
static inline int Max(int a, int b) {
	return a > b ? a : b;
}

// similar to strcat but assures enough space
static void prtstring(char** s, unsigned* cap, unsigned* pos, const char* str, int w) {
	int space = *cap - *pos;
	for (int n = 0;;) {
		int m;
		if (w == INT_MIN)
			m = snprintf(*s + *pos, space, "\"%s\"", str + n);
		else
			m = snprintf(*s + *pos, space, "%*s", w, str + n);
		if (m >= space) {
			n += space - 1;
			*cap += (*cap >> 1) + (m - space) + 1;
			*s = (char*)realloc(*s, *cap);
			*pos += space - 1;
			space = *cap - *pos;
		} else {
			*pos += m;
			break;
		}
	}
}

static void sprt(char** s, unsigned* cap, unsigned* pos, const char* pre, ... /* ft, val, int w, int p, unsigned flags */);

static const char* ptr_align(const char* ptr, size_t bytes) {
	if (bytes) {
		unsigned align = sizeof(size_t);
		while (bytes < align)
			align = align >> 1;
		align -= 1;
		uintptr_t mask = ~(uintptr_t)(align);
		ptr = (const char*)(((uintptr_t)ptr + align) & mask);
	}
	return ptr;
}

static void print_struct(char** s, unsigned* cap, unsigned* pos, const VOLVOX_RtType* struct_type,
                         const char* elem_ptr, unsigned num_fields, int indent, int w, int p, unsigned flags);

static void print_array(char** s, unsigned* cap, unsigned* pos, const VOLVOX_RtType* elem_type,
                        const char* elem_ptr, size_t dims[], size_t subsz[], int order, int indent,
                        int w, int p, unsigned flags);

static void prt_float(char** s, unsigned* cap, unsigned* pos, int space, double val, int w, int p,
                      unsigned flags);

static void prt_int(char** s, unsigned* cap, unsigned* pos, int space, unsigned long long vall,
                    unsigned bits, int w, int p, unsigned flags);

static void prt_pointer(char** s, unsigned* cap, unsigned* pos, const char* ptr, unsigned w, unsigned attr) {
	if (!ptr)
		prtstring(s, cap, pos, "nil", w == INT_MIN ? 0 : w);
	else if (attr & A_cstring)
		prtstring(s, cap, pos, ptr, w);
	else if (attr & A_string)
		prtstring(s, cap, pos, volvox2cstr(ptr), w);
	else
		prt_int(s, cap, pos, *cap - *pos, (size_t)ptr, 8*sizeof(size_t),
		        w == INT_MIN ? 0 : w, 10, FMT_ZEROPAD | FMT_DISPLAY_HEX | FMT_UNSIGNED);
}

static const char* prt_aggregate_elem(char** s, unsigned* cap, unsigned* pos, const char* FieldName,
                                      const VOLVOX_RtType* elem_type, const char* elem_ptr, int indent,
                                      int w, int p, unsigned flags)
{
	if (indent >= 0) {
		char* indentbuf = (char*)alloca(indent+2);
		indentbuf[0] = '\n';
		memset(indentbuf + 1, ' ', indent);
		indentbuf[indent + 1] = '\0';
		prtstring(s, cap, pos, indentbuf, w);
	}
	if (FieldName) {
		prtstring(s, cap, pos, FieldName, w);
		prtstring(s, cap, pos, ": ", w);
	}
	const char* pre = nullptr;
	if (elem_type->ID == VOLVOX_FloatTyID) {
		if (!(flags & A_packed))
			elem_ptr = ptr_align(elem_ptr, sizeof(float));
		if (p <= 0)
			p = F32_DEFAULT_PRECISION;
		prt_float(s, cap, pos, *cap - *pos, (double)*((float*)elem_ptr), w, p, flags);
		if (elem_type->type_attr & A_signed)
			prtstring(s, cap, pos, "i", w);
		elem_ptr = (const char*)((float*)elem_ptr + 1); // TODO: packed/unpacked?
	} else if (elem_type->ID == VOLVOX_IntegerTyID && elem_type->SubclassData <= 4*8) {
		unsigned elem = 0;
		if (!(flags & A_packed))
			elem_ptr = ptr_align(elem_ptr, elem_type->SubclassData / 8);
		memcpy(&elem, (char*)elem_ptr, elem_type->SubclassData / 8);
		if (elem_type->SubclassData < 4*8 && (elem_type->type_attr & A_signed)) {
			// sign expand integer using logic left and arithmetic right shifts
			unsigned shift = 4*8 - elem_type->SubclassData;
			elem = (unsigned)((int)(elem << shift) >> shift);
		}
		prt_int(s, cap, pos, *cap - *pos, (unsigned long long)elem, elem_type->SubclassData, w, p, flags);
		elem_ptr += elem_type->SubclassData / 8;
	} else if (elem_type->ID == VOLVOX_IntegerTyID) {
		if (!(flags & A_packed))
			elem_ptr = ptr_align(elem_ptr, sizeof(size_t));
		prt_int(s, cap, pos, *cap - *pos, *(size_t*)elem_ptr, elem_type->SubclassData, w, p, flags);
		elem_ptr += elem_type->SubclassData / 8;
	} else if (elem_type->ID == VOLVOX_DoubleTyID) {
		if (!(flags & A_packed))
			elem_ptr = ptr_align(elem_ptr, sizeof(size_t));
		if (p <= 0)
			p = F64_DEFAULT_PRECISION;
		prt_float(s, cap, pos, *cap - *pos, *(double*)elem_ptr, w, p, flags);
		if (elem_type->type_attr & A_signed)
			prtstring(s, cap, pos, "i", w);
		elem_ptr = (const char*)((double*)elem_ptr + 1);
	} else if (elem_type->ID == VOLVOX_StructTyID) {
		if (!(flags & A_packed))
			elem_ptr = ptr_align(elem_ptr, elem_type->type_size);
		print_struct(s, cap, pos, elem_type, elem_ptr, elem_type->SubclassData, indent, w, p, flags);
		elem_ptr += elem_type->type_size;
	} else if (elem_type->ID == VOLVOX_ArrayTyID) {
		if (!(flags & A_packed))
			elem_ptr = ptr_align(elem_ptr, elem_type->elem_type->type_size);
		unsigned order = elem_type->SubclassData;
		size_t* subsz = (size_t*)alloca((order + 1) * sizeof(size_t));
		subsz[order] = elem_type->elem_type->type_size;
		for (int n = order - 1; n >= 0; n--)
			subsz[n] = elem_type->dims[n] * subsz[n + 1];
		if (subsz[0]) {
			if (FieldName)
				indent += strlen(FieldName) + 2;
			print_array(s, cap, pos, elem_type->elem_type, elem_ptr,
			            elem_type->dims, subsz, order, indent, w, p, flags);
			elem_ptr += subsz[0];
		} else {
			for (unsigned n = 0; n < order; n++)
				prtstring(s, cap, pos, "[", 0);
			for (unsigned n = 0; n < order; n++)
				prtstring(s, cap, pos, "]", 0);
		}
	} else if (elem_type->ID == VOLVOX_FixedVectorTyID) {
		if (true /* A_complex */) {
			if (!(flags & A_packed))
				elem_ptr = ptr_align(elem_ptr, sizeof(size_t));
			if (p <= 0)
				p = F64_DEFAULT_PRECISION;
			prt_float(s, cap, pos, *cap - *pos, crealf(*(complex_float*)elem_ptr), w, p, flags);
			const char* sp = cimagf(*(complex_float*)elem_ptr) < 0 ? " - " : " + ";
			float im = cimagf(*(complex_float*)elem_ptr) < 0 ? -cimagf(*(complex_float*)elem_ptr) : cimagf(*(complex_float*)elem_ptr);
			prtstring(s, cap, pos, sp, w);
			prt_float(s, cap, pos, 0, im, w, p, flags);
			prtstring(s, cap, pos, "i", w);
		}
	} else if (elem_type->ID == VOLVOX_PointerTyID) {
		if (!(flags & A_packed))
			elem_ptr = ptr_align(elem_ptr, sizeof(size_t));
		prt_pointer(s, cap, pos, *(char**)elem_ptr, INT_MIN, elem_type->type_attr);
		elem_ptr += sizeof(size_t);
	} else {
		prtstring(s, cap, pos, "<unsupported type>", w);
	}
	return elem_ptr;
}

static void print_struct(char** s, unsigned* cap, unsigned* pos, const VOLVOX_RtType* struct_type, const char* elem_ptr,
                         unsigned num_fields, int indent, int w, int p, unsigned flags)
{
	if (indent < 0)
		prtstring(s, cap, pos, " ", 0);
	if (struct_type->name)
		prtstring(s, cap, pos, struct_type->name, w);
	if (num_fields) { // should empty structs be allowed? not sure...
		prtstring(s, cap, pos, "{", 0);
		if (struct_type->type_attr & A_packed)
			flags |= A_packed;
		for (unsigned n=0; n<num_fields; ++n) {
			elem_ptr = prt_aggregate_elem(s, cap, pos,
			                              ((VOLVOX_RtType*)((char*)struct_type + n * sizeof(VOLVOX_RtStructField)))->fields.FieldName,
			                              ((VOLVOX_RtType*)((char*)struct_type + n * sizeof(VOLVOX_RtStructField)))->fields.rttype,
			                              elem_ptr, indent + 4, w, p, flags);
		}
		if (indent >= 0) {
			char* indentbuf = (char*)alloca(indent+3);
			indentbuf[0] = '\n';
			memset(indentbuf + 1, ' ', indent);
			indentbuf[indent + 1] = '}';
			indentbuf[indent + 2] = '\0';
			prtstring(s, cap, pos, indentbuf, 0);
		} else {
			prtstring(s, cap, pos, "\n}", 0);
		}
	} else {
		prtstring(s, cap, pos, "{}", 0);
	}
}

static void print_array(char** s, unsigned* cap, unsigned* pos, const VOLVOX_RtType* elem_type, const char* elem_ptr,
                        size_t dims[], size_t subsz[], int order, int indent, int w, int p, unsigned flags)
{
	indent += 2;
	char* pre0 = (char*)alloca(3);
	char* pre1 = (char*)alloca(order > 1 ? indent + 5 : 3);
	unsigned idx0 = 0;
	pre0[idx0++] = '[';
	if (order > 1)
		pre0[idx0++] = ' ';
	pre0[idx0] = '\0';
	unsigned idx1 = 0;
	if (order > 1) {
		pre1[idx1++] = '\n';
		memset(pre1 + idx1, ' ', indent);
		idx1 += indent;
	} else {
		pre1[idx1++] = ',';
	}
	pre1[idx1] = '\0';
	int suborder = order - 1;
	if (!suborder && (elem_type->ID == VOLVOX_BFloatTyID || elem_type->ID == VOLVOX_FloatTyID
		    || elem_type->ID == VOLVOX_DoubleTyID || elem_type->ID == VOLVOX_IntegerTyID)) {
		// we want the field to be in an eye-pleasing pitch by default
		if (!w)
			w = ARRAY_DEFAULT_FIELD_WIDTH;
		if (!p)
			p = ARRAY_DEFAULT_PRECISION;
	}
	for (int i = 0; i < dims[0]; i++) {
		const char* pre = i ? pre1 : pre0;
		prtstring(s, cap, pos, pre, 0);
		if (suborder) {
			print_array(s, cap, pos, elem_type, elem_ptr, &dims[1], &subsz[1], suborder, indent, w, p, flags);
			elem_ptr += subsz[1];
		} else {
			elem_ptr = prt_aggregate_elem(s, cap, pos, nullptr, elem_type, elem_ptr, -1, w, p, flags);
		}
	}
	prtstring(s, cap, pos, " ]", 0);
}

static void prt_float(char** s, unsigned* cap, unsigned* pos, int space, double val, int w, int p, unsigned flags) {
	const char* fmt = getFmtFlt(flags);
	int expected_nchar = Max(abs(w)+1, p+7+1);
	while (space < expected_nchar) {
		*cap += expected_nchar + (*cap >> 1);
		*s = (char*)realloc(*s, *cap);
		space = *cap - *pos;
	}
	*pos += sprintf(*s + *pos, fmt, w, p, val);
}

static void prt_int(char** s, unsigned* cap, unsigned* pos, int space, unsigned long long vall, unsigned bits, int w, int p, unsigned flags) {
	int expected_nchar;
	unsigned val;
	const char* fmt;
	if (bits <= 32) {
		val = (unsigned)vall;
		if (bits < 32) {
			if (bits == 1) {
				// bool
				prtstring(s, cap, pos, val & 1 ? "true" : "false", w);
				return;
			}
			// extend upper bits according to signedness
			if (!(flags & FMT_UNSIGNED))
				val = (unsigned)((int)(val << (32 - bits)) >> (32 - bits));
		}
		fmt = getFmtInt(flags);
		expected_nchar = Max(abs(w)+1, 21+1);
	} else {
		fmt = getFmtLong(flags);
		expected_nchar = Max(abs(w)+1, 11+1);
	}
	while (space < expected_nchar) {
		*cap += expected_nchar + (*cap >> 1);
		*s = (char*)realloc(*s, *cap);
		space = *cap - *pos;
	}
	if (bits <= 32)
		*pos += sprintf(*s + *pos, fmt, w, val);
	else
		*pos += sprintf(*s + *pos, fmt, w, vall);
}

static void vsprt(char** s, unsigned* cap, unsigned* pos, const char* pre, va_list ap) {
	if (!*cap) {
		*cap = 128;
		*s = (char*)realloc(*s, *cap);
	}
	if (pre)
		prtstring(s, cap, pos, volvox2cstr(pre), 0);
	int space = *cap - *pos;
	const VOLVOX_RtType* ft = va_arg(ap, const VOLVOX_RtType*);
	while (ft) {
		switch (ft->ID) {
		case VOLVOX_BFloatTyID:
		case VOLVOX_FloatTyID:
		case VOLVOX_DoubleTyID: {
			double val;
			if (ft->ID != VOLVOX_DoubleTyID) {
				// C does not support variadic floats - so use some workaround
				unsigned u = va_arg(ap, unsigned);
				val = (double)*(float*)&u;
			} else {
				val = va_arg(ap, double);
			}
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			const char* fmt = getFmtFlt(flags);
			if (p <= 0) p = (ft->ID == VOLVOX_DoubleTyID) ? F64_DEFAULT_PRECISION : F32_DEFAULT_PRECISION;
			prt_float(s, cap, pos, space, val, w, p, flags);
			if (ft->type_attr & A_signed)
				prtstring(s, cap, pos, "i", w);
		}
			break;
		case VOLVOX_IntegerTyID: {
			unsigned long long vall;
			if (ft->SubclassData <= 32) {
				unsigned val = va_arg(ap, unsigned);
				vall = val;
			} else {
				vall = va_arg(ap, unsigned long long);
			}
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			if (!(ft->type_attr & A_signed))
				flags |= FMT_UNSIGNED;
			prt_int(s, cap, pos, space, vall, ft->SubclassData, w, p, flags);
		}
			break;
		case VOLVOX_ArrayTyID: {
			// rt_len = ft->num_fields;
			unsigned order = ft->SubclassData;
			size_t* dims = (size_t*)alloca(order * sizeof(size_t));
			size_t* subsz = (size_t*)alloca((order + 1) * sizeof(size_t));
			for (unsigned n = 0; n < order; n++)
				dims[n] = va_arg(ap, size_t);
			subsz[order] = ft->elem_type->type_size;
			for (int n = order - 1; n >= 0; n--)
				subsz[n] = dims[n] * subsz[n + 1];
			char* elem_ptr = va_arg(ap, char*);
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			size_t elem_size = ft->elem_type->type_size;
			if (subsz[0]) {
				print_array(s, cap, pos, ft->elem_type, elem_ptr, dims, subsz, order, 0, w, p, flags);
			} else {
				for (unsigned n = 0; n < order; n++)
					prtstring(s, cap, pos, "[", 0);
				for (unsigned n = 0; n < order; n++)
					prtstring(s, cap, pos, "]", 0);
			}
		}
			break;
		case VOLVOX_FixedVectorTyID: {
			if (true || (ft->type_attr & A_complex)) {
				complex_float c = va_arg(ap, complex_float);
				int w = va_arg(ap, int);
				int p = va_arg(ap, int);
				unsigned flags = va_arg(ap, unsigned);
				if (p <= 0) p = (ft->ID == VOLVOX_DoubleTyID) ? F64_DEFAULT_PRECISION : F32_DEFAULT_PRECISION;
				prt_float(s, cap, pos, space, crealf(c), w, p, flags);
				const char* sp = cimagf(c) < 0 ? " - " : " + ";
				float im = cimagf(c) < 0 ? -cimagf(c) : cimagf(c);
				prtstring(s, cap, pos, sp, w);
				prt_float(s, cap, pos, 0, im, w, p, flags);
				prtstring(s, cap, pos, "i", w);
			}
		}
			break;
		case VOLVOX_PointerTyID: {
			char* str = va_arg(ap, char*);
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			prt_pointer(s, cap, pos, str, w, ft->type_attr);
		}
			break;
		case VOLVOX_FunctionTyID: {
			char* fn = va_arg(ap, char*);
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			int expected_nchar = Max(abs(w)+1, 30+1);
			while (space < expected_nchar) {
				*cap += expected_nchar + (*cap >> 1);
				*s = (char*)realloc(*s, *cap);
				space = *cap - *pos;
			}
			*pos += sprintf(*s + *pos, "function: <%p>", fn);
		}
			break;
		case VOLVOX_StructTyID: {
			unsigned num_fields = ft->SubclassData;
			char* elem_ptr = va_arg(ap, char*);
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			print_struct(s, cap, pos, ft, elem_ptr, num_fields, 0, w, p, flags);
		}
			break;
		default:
			fprintf(stderr, "TypeID: %u\n", ft->ID);
			abort();
		}
		space = *cap - *pos;
		if (space < 1)
			// error in calculation
			abort();
		const char* post = va_arg(ap, char*);
		if (post) {
			prtstring(s, cap, pos, post, 0);
			space = *cap - *pos;
		}
		ft = va_arg(ap, const VOLVOX_RtType*);
	}
}
		
static void sprt(char** s, unsigned* cap, unsigned* pos, const char* pre, ... /* ft, val, int w, int p, unsigned flags */) {
	va_list ap;
	va_start(ap, pre);
	vsprt(s, cap, pos, pre, ap);
	va_end(ap);
}

_DECL bool vfprint(int fd, bool newline, const char* pre, va_list ap) {
	char* s = NULL;
	unsigned cap = 0;
	unsigned pos = 0;
	vsprt(&s, &cap, &pos, pre, ap);
	unsigned bytes_to_write = pos;
	if (newline) {
		s[pos] = '\n';
		bytes_to_write++;
	}
	int n = write(fd, s, bytes_to_write);
	free(s);
	return n == bytes_to_write;
}

_DECL bool enableColorANSI(int fd) {
#if defined (_MSC_VER)
	static bool is_set = false;
	if (!is_set) {
		HANDLE h = (HANDLE)_get_osfhandle(fd);
		if ((intptr_t)h == -1)
			return false;
		DWORD mode;
		if (!GetConsoleMode(h, &mode)) {
			errno = ENOTTY;
			return false;
		}
		mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		if (!SetConsoleMode(h, mode)) {
			errno = ENOTTY;
			return false;
		}
		is_set = true;
	}
	return true;
#else
	return isatty(fd);
#endif
}

_DECL void showtestres(int fd, int width, const char* testcase, bool result) {
	if (width < 20)
		width = 20;
	bool have_color = enableColorANSI(fd);

	char* buf = (char*)alloca(width + (have_color ? 11 : 2));
	snprintf(buf, width-4, "%*s", -width+5, volvox2cstr(testcase));
	strcpy(buf+width-5, have_color ? (result ? " \033[32mPASS\033[0m\n" : " \033[31mFAIL\033[0m\n") : (result ? " PASS\n" : " FAIL\n"));
	write(fd, buf, width + (have_color ? 10 : 1));
}

// find out the terminal size #rows are stored in the lower 16 bits
// and #columns in the upper. In case of failure errno is set and -1 is returned
_DECL int getTermSize(int fd)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if ((intptr_t)h == -1)
		return -1;
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (!GetConsoleScreenBufferInfo(h, &info)) {
		errno = ENOTTY;
		return -1;
	}
	else
		return (int)(((info.srWindow.Bottom - info.srWindow.Top + 1) << 16)
					 | (info.srWindow.Right - info.srWindow.Left + 1));
#else
	struct winsize ws;
	int res = ioctl(fd, TIOCGWINSZ, &ws);
	if (res < 0)
		return res;
	else
		return (int)((ws.ws_col << 16) | ws.ws_row);
#endif
}

#ifdef _WIN32
/* The Windows functions 'FindFirstFile()/FindNextFile()' return a sorted list of filenames.
   However, apparently they do not handle UTF-8 filenames correctly when sorting even if
   codepage and locale LC_ALL are set to UTF-8. We compensate this here with a tiny bubble sort
   using _mbscoll() which supports correct lexical sorting on UTF-8.
   Even though bubble sort is O(n^2) in worst case it will perform much better here (often O(n)) 
   as the list is already basically sorted.
*/
static void pathBubblesort(char** paths, unsigned n) {
	bool swapped;
	for(;;) {
		swapped = false;
		unsigned m = n - 1;
		for (unsigned i = 0; i < m; i++)
			if (_mbscoll((unsigned char*)paths[i], (unsigned char*)paths[i+1]) > 0) {
				char* tmp = paths[i];
				paths[i] = paths[i+1];
				paths[i+1] = tmp;
				swapped = true;
			}
		if (!swapped)
			break;
		n = m;
	}
}

_DECL bool Glob_impl(char* buf, int s_len, int cur_index, const char* argv, char*** rets, size_t* n_rets, size_t* max_rets) {
	bool found = false;
	int last_slash = -1;
	int new_index;
	bool have_wildcard = false;
	bool is_lastpart = false;
	char slash = (cur_index && argv[cur_index-1] == '\\') ? '\\' : '/';
	for (new_index = cur_index; argv[new_index]; new_index++) {
		switch (argv[new_index]) {
		case '*':
		case '?':
			have_wildcard = true;
			break;
		case '\\':
		case '/':
			slash = argv[new_index];
			if (have_wildcard)
				goto continue_search;
			else
				last_slash = new_index;
			break;
		default:
			;
		}
	}
	is_lastpart = true;
continue_search:
	; // C standard expects expression (not declaration) after label
	int plus_len = new_index - cur_index;
	if (s_len + plus_len + 2 > MAX_PATH) {
		return false;
	}
	strncpy(buf + s_len, argv + cur_index, plus_len);
	buf[s_len + plus_len] = '\0';
	s_len += last_slash - cur_index;
	WIN32_FIND_DATA wfd;
	HANDLE h = FindFirstFile(buf, &wfd);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	if (is_lastpart && !*rets) {
		*max_rets = 8;
		*n_rets = 0;
		*rets = (char**)malloc(*max_rets * sizeof(char*));
	}
	size_t old_n_rets = *n_rets;
	do {
		if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!strcmp(wfd.cFileName, ".") || !strcmp(wfd.cFileName, ".."))
				continue;
			if (!is_lastpart) {
				int dirname_len = strlen(wfd.cFileName);
				if (dirname_len + s_len + 5 > MAX_PATH) {
					return false;
				}
				strcpy(buf + s_len + 1, wfd.cFileName);
				if (Glob_impl(buf, s_len + 1 + dirname_len, new_index, argv, rets, n_rets, max_rets))
					found = true;
			}
		}
		if (is_lastpart) {
			plus_len = strlen(wfd.cFileName);
			if (*n_rets >= *max_rets) {
				*max_rets = *max_rets + (*max_rets >> 1);
				*rets = (char**)realloc(*rets, *max_rets * sizeof(char*));
			}
			int full_len = s_len + 1 + plus_len + 1;
			if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				full_len++;
			(*rets)[*n_rets] = malloc(full_len);
			strncpy((*rets)[*n_rets], buf, s_len + 1 );
			strcpy((*rets)[*n_rets] + s_len + 1, wfd.cFileName);
			if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				(*rets)[*n_rets][s_len + 1 + plus_len] = slash;
				(*rets)[*n_rets][s_len + 1 + plus_len + 1] = '\0';
			}
			(*n_rets)++;
			found = true;
		}
	} while (FindNextFile(h, &wfd));
	FindClose(h);
	if (found)
		pathBubblesort(*rets + old_n_rets, *n_rets - old_n_rets);
	return found;
}
#endif

#ifdef _WIN32
// dest must be 32767 bytes in size - maximum length of command line on Windows
_DECL bool getCmdLine(char* dest, const char* cmd, char* const argv[]) {
	int pos = 0;
	for (int argidx = 0; argv[argidx]; argidx++) {
		if (pos > 32760)
			goto error;
		if (argidx)
			dest[pos++] = ' ';
		dest[pos++] = '"';
		const char* arg = argidx ? argv[argidx] : cmd;
		for (int i=0; arg[i]; i++) {
			if (pos > 32760)
				goto error;
			char c = arg[i];
			if (arg[i] == '"')
				dest[pos++] = '\\';
			dest[pos++] = c;
		}
		dest[pos++] = '"';
	}
	dest[pos++] = '\0';
	return true;
error:
	errno = E2BIG;
	return false;
}
#endif

#ifdef _WIN32
_DECL bool volvox_spawn_c(int* pid, int* child_stdin, int* child_stdout,
                         int* child_stderr, char* const argv[]) {
	char cmd_path[MAX_PATH];
	char cmd_line[32768];
	if (!argv) {
		errno = EINVAL;
		return false;
	}
	unsigned pathlen = SearchPath(NULL, argv[0], ".exe", MAX_PATH, cmd_path, NULL);
	if (!pathlen)
		goto error;
	if (!getCmdLine(cmd_line, cmd_path, argv))
		return false;
	SECURITY_ATTRIBUTES saAttr = {
		.nLength = sizeof(SECURITY_ATTRIBUTES), 
		.lpSecurityDescriptor = NULL,
		.bInheritHandle = true // for pipe handles to be inherited
	};
	// Create communication pipes and make sure the child's end is inherited
	HANDLE h_child_stdin_r = NULL;
	HANDLE h_child_stdin_w = NULL;
	if (child_stdin) {
		if (!CreatePipe(&h_child_stdin_r, &h_child_stdin_w, &saAttr, 4096))
			goto error; 
		if (!SetHandleInformation(h_child_stdin_w, HANDLE_FLAG_INHERIT, 0))
			goto error;
	} else {
		h_child_stdin_r = (HANDLE)_get_osfhandle(_dup(0));
	}
	HANDLE h_child_stdout_r = NULL;
	HANDLE h_child_stdout_w = NULL;
	if (child_stdout) {
		if (!CreatePipe(&h_child_stdout_r, &h_child_stdout_w, &saAttr, 4096)) 
			goto error;
		if (!SetHandleInformation(h_child_stdout_r, HANDLE_FLAG_INHERIT, 0))
			goto error;
	} else {
		h_child_stdout_w = (HANDLE)_get_osfhandle(_dup(1));
	}
	HANDLE h_child_stderr_r = NULL;
	HANDLE h_child_stderr_w = NULL;
	if (child_stderr) {
		if (!CreatePipe(&h_child_stderr_r, &h_child_stderr_w, &saAttr, 4096)) 
			goto error;
		if (!SetHandleInformation(h_child_stderr_r, HANDLE_FLAG_INHERIT, 0))
			goto error;
	} else {
		h_child_stderr_w = (HANDLE)_get_osfhandle(_dup(2));
	}
	STARTUPINFO StartInfo = {
		.cb = sizeof(STARTUPINFO),
		.hStdInput = h_child_stdin_r,
		.hStdOutput = h_child_stdout_w,
		.hStdError = h_child_stderr_w,
		.dwFlags = STARTF_USESTDHANDLES
	};
	PROCESS_INFORMATION ProcInfo = {0};
	if (CreateProcess(
		    cmd_path,   // ApplicationName
		    cmd_line,   // CommandLine
		    NULL,       // ProcessAttributes
		    NULL,       // ThreadAttributes
		    true,       // InheritHandles
		    0,          // CreationFlags
		    NULL,       // Environment
		    NULL,       // CurrentDirectory
		    &StartInfo, // StartupInfo
		    &ProcInfo)  // ProcessInformation
		) {
		// get pid if desired, otherwise close the process handle to detach the process
		if (pid)
			*pid = ProcInfo.dwProcessId;
		else
			CloseHandle(ProcInfo.hProcess);
		CloseHandle(ProcInfo.hThread);
		// close here in the parent those pipe ends that are used in the child
		if (h_child_stdin_r)
			CloseHandle(h_child_stdin_r);
		if (h_child_stdout_w)
			CloseHandle(h_child_stdout_w);
		if (h_child_stderr_w)
			CloseHandle(h_child_stderr_w);
		if (child_stdin)
			*child_stdin = _open_osfhandle((uintptr_t)h_child_stdin_w, _O_WRONLY);
		if (child_stdout)
			*child_stdout = _open_osfhandle((uintptr_t)h_child_stdout_r, _O_RDONLY);
		if (child_stderr)
			*child_stderr = _open_osfhandle((uintptr_t)h_child_stderr_r, _O_RDONLY);
		return true;
	}
error:
   // this is not correct - TODO: map/merge Windows errors / POSIX errors
   errno = GetLastError();
   return false;
}
#endif

_DECL void printstr(int fd, char* s) {
	size_t l = volvox_string_len(s);
	char* sc = volvox2cstr(s);
	write(fd, sc, l);
	char n = '\n';
	write(fd, &n, 1);
}

_DECL void modstr(char* s, int idx, char c) {
	char* sc = volvox2cstr(s);
	sc[idx] = c;
}

// size including terminating '\0'
#define __string_raw_size(s) *(size_t*)(s)

// allocation size including metadata
#define __string_raw_cap(s) *((size_t*)(s) + 1)
#define __string_is_heap(s) (bool)__string_raw_cap(s)
#define __string_allocsize(len) ((len + 3*sizeof(size_t)) & ~(sizeof(size_t)-1))
#define __string_c_ptr(s, raw_sz) (char*)((uintptr_t)(s - raw_sz) & ~((sizeof(size_t)-1)))
#define __string_min_cap(raw_sz) ((raw_sz + 3*sizeof(size_t) - 1) & ~(sizeof(size_t)-1))
#define __raw_offset(alloc) (alloc - 2*sizeof(size_t))
#define __x_min(a, b) (((a) <= (b)) ? (a) : (b))

static char* __string_accumulate(size_t m, char* a[], bool is_add_assign) {
	size_t new_l = 0;
	for (size_t i = 0; i<m; i++)
		new_l += volvox_string_len(a[i]);
	size_t new_alloc = __string_allocsize(new_l);
	char* n = malloc(new_alloc);
	char* offset = n;
	for (size_t idx=0; idx < m; idx++) {
		char* cstr = volvox2cstr(a[idx]);
		size_t len = volvox_string_len(a[idx]);
		memcpy(offset, cstr, len);
		offset += len;
	}
	*offset = 0;
	char* res = n + __raw_offset(new_alloc);
	*(size_t*)res = (new_l + 1);
	*((size_t*)res + 1) = new_alloc;
	return res;
}

_DECL char* __string_add(char* a, char* b) {
	char* x[2] = { a, b };
	return __string_accumulate(2, x, false);
}

_DECL char* __string_dup(char* s) {
	size_t sz = __string_raw_size(s);
	char* s_c = __string_c_ptr(s, sz);
	size_t min_cap = __string_min_cap(sz);
	char* new_str = (char*)malloc(min_cap);
	memcpy(new_str, s_c, min_cap - sizeof(size_t));
	char* res = new_str + __raw_offset(min_cap);
	*((size_t*)res + 1) = min_cap;
	return res;
}

_DECL void __string_add_assign_gen(char** a, char* c_b, size_t sz_b) {
	size_t sz_a = __string_raw_size(*a);
	char* c_a = __string_c_ptr(*a, sz_a);
	size_t cap_a = __string_raw_cap(*a);
	size_t new_sz = sz_a + sz_b - 1; // terminating '\0' only once
	size_t min_cap = __string_min_cap(new_sz);
	char* new_a;
	if (cap_a < min_cap) {
		min_cap = min_cap + (min_cap >> 1) + 32; // exponential + linear growth
		if (cap_a)
			new_a = (char*)realloc(c_a, min_cap);
		else {
			new_a = (char*)malloc(min_cap);
			memcpy(new_a, c_a, sz_a - 1);
		}
		c_a = new_a;
		cap_a = min_cap;
	}
	memcpy(c_a + sz_a - 1, c_b, sz_b);
	char* res = (char*)((uintptr_t)(c_a + new_sz + sizeof(size_t) - 1) & ~(sizeof(size_t)-1));
	*(size_t*)res = new_sz;
	*((size_t*)res + 1) = cap_a;
	*a = res;
}

_DECL void __string_add_assign(char** a, char* b) {
	size_t sz_b = __string_raw_size(b);
	char* c_b = __string_c_ptr(b, sz_b);
	__string_add_assign_gen(a, c_b, sz_b);
}

_DECL void __string_add_c_assign(char** a, char* c_b) {
	size_t sz_b = strlen(c_b) + 1;
	__string_add_assign_gen(a, c_b, sz_b);
}

static char* __string_mult_general(size_t m, char* a, bool is_mult_assign) {
	char* cstr = volvox2cstr(a);
	size_t len = volvox_string_len(a);
	size_t new_l = m * len;
	size_t new_alloc = __string_allocsize(new_l);
	char* n;
	size_t offset;
	size_t idx;
	if (is_mult_assign && __string_is_heap(a)) {
		char* cstr0 = volvox2cstr(a);
		n = realloc(cstr0, new_alloc);
		idx = 1;
		offset = len;
	} else {
		n = malloc(new_alloc);
		idx = 0;
		offset = 0;
	}
	for ( ; idx < m; idx++) {
		memcpy(n + offset, cstr, len);
		offset += len;
	}
	n[new_l] = 0;
	char* volvox_str = n + __raw_offset(new_alloc);
	__string_raw_size(volvox_str) = (new_l + 1);
	__string_raw_cap(volvox_str) = new_alloc;
	return volvox_str;
}

_DECL char* __string_mult(size_t m, char* a) {
	return __string_mult_general(m, a, false);
}

_DECL void __string_mult_assign(char** a, size_t m) {
	*a = __string_mult_general(m, *a, true);
}

_DECL char* __string_make_writable(char** SizeRef) {
	size_t len = __string_raw_size(*SizeRef);
	size_t alloc_l = __string_allocsize(len - 1);
	char* cstr = malloc(alloc_l);
	size_t offset = __raw_offset(alloc_l);
	memcpy(cstr, *SizeRef - offset, len);
	char* volvox_str = cstr + offset;
	__string_raw_size(volvox_str) = len;
	__string_raw_cap(volvox_str) = alloc_l;
	return volvox_str;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
_DECL void printd(double X) {
	fprintf(stderr, "%g\n", X);
}

/// printu64 - printf that takes a u64 prints it as "%f\n", returning 0.
_DECL void printu64(uint64_t X) {
	fprintf(stderr, "%" PRIu64 "\n", X);
}

#define target_bytes sizeof(size_t)

_DECL char* __cstr2volvoxstr(const char* c_str, size_t len, bool mark_as_heap) {
	char* res;
	size_t l;
	char* targ;
	if (mark_as_heap) {
		cstr2volvoxstr_l(res, l, targ, c_str, malloc, len, l);
	} else {
		cstr2volvoxstr_l(res, l, targ, c_str, malloc, len, 0);
	}
	return res;
}

_DECL char* __cstr2volvox(const char* c_str) {
	size_t l = strlen(c_str);
	return __cstr2volvoxstr(c_str, l, true);
}

_DECL char* __volvox2cstr(const char* v) {
	size_t sz = __string_raw_size(v);
	const char* s_c = __string_c_ptr(v, sz);
	return strdup(s_c);
}

_DECL char* __transformcstr2volvox_l(char* c_str, ssize_t _l, size_t cap) {
	char* res;
	size_t min_cap;
	char* targ;
	min_cap = (_l+3*sizeof(size_t)) & ~(size_t)(sizeof(size_t)-1);
	if (min_cap > cap) {
		cap = min_cap;
		targ = (char*)realloc(c_str, cap);
	} else {
		targ = c_str;
	}
	res = targ + min_cap - 2*sizeof(size_t);
	*(size_t*)res = _l+1;
	*((size_t*)res + 1) = cap;
	return res;
}

_DECL char* __transformcstr2volvox(char* c_str, size_t cap) {
	ssize_t _l = strlen(c_str);
	return __transformcstr2volvox_l(c_str, _l, cap);
}

// remove trailing delimiter - or '\r\n' for DOS files
_DECL void __trim_cstring(char* s, ssize_t* l, char d) {
	if (*l>0) {
		size_t l_new = *l-1;
		if (s[l_new] == d) {
			if (d == '\n' && l_new > 0 && s[l_new-1] == '\r')
				l_new = l_new-1;
			s[l_new] = '\0';
			*l = l_new;
		}
	}
}

#undef target_bytes

_DECL void __printerr() {
	fprintf(stderr, "%s\n", strerror(errno));
}

_DECL void err_printf(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fflush(stdout);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

#ifndef _WIN32

_DECL unsigned GetLastError() {
	return errno;
}

_DECL void SetLastError(unsigned n) {
	errno = n;
}

#else

// These functions were originally GNU extensions but are now part of POSIX.
// BSD and Linux have them in libc - however Windows doesn't.
// So we provide provide Windows versions for them here:
//
_DECL ssize_t getdelim(char** buf, size_t* sz, int delim, FILE* f) {
	size_t n = 0;
	if (!*sz || !*buf) {
		*sz = 120;
		if (*buf)
			*buf = realloc(*buf, *sz);
		else
			*buf = malloc(*sz);
	}
	_lock_file(f);
	int c;
	do {
		if (*sz <= n+2) {
			*sz = *sz + (*sz >> 1) + 100;
			*buf = realloc(*buf, *sz);
		}
		c = _fgetc_nolock(f);
		if (c == EOF) {
			_unlock_file(f);
			(*buf)[n] = '\0';
			return -1;
		}
		(*buf)[n++] = (char)c;
	} while (c != delim);
	_unlock_file(f);
	(*buf)[n] = '\0';
	return n;
}

_DECL ssize_t getline(char** buf, size_t* sz, FILE* f) {
	return getdelim(buf, sz, '\n', f);
}

#endif

// read stream until EOL or EOF is read
// return number of non-end characters read - maybe 0 if EOF or EOL
// -1 means error
//

_DECL ssize_t __purgeline(FILE* f) {
	size_t nn = 0;
	const unsigned sz = 96;
	char buf[96];
	for(;;) {
		if (!fgets(buf, sz, f)) {
			if (feof(f))
				return 0;
			else
				return -1;
		}
		unsigned n;
		for (n=0; buf[n]; n++)
			if (buf[n] == '\n') { // EOL
				if (n>0 && buf[n-1] == '\r') {
					return nn + n - 1;
				} else
					return nn + n;
			}
		nn += n;
		if (n < sz-1)
			return nn; // EOF
	}
}

_DECL FILE* __get_stdin() {
	return stdin;
}

_DECL FILE* __get_stdout() {
	return stdout;
}

_DECL FILE* __get_stderr() {
	return stderr;
}


// read UTF-8 coded characters from string and return Unicode codepoint
// return -1 on error and set errno
//
_DECL uint32_t utf8_decode(const char** s) {
	const char* endp = nullptr;
	uint32_t cp = 0;
	int num_bytes = 0;
	uint8_t c = *(*s)++;
	if ((int8_t)c >= 0) {
		return c;
	}
	if ((c & 0b11100000) == 0b11000000) {
		cp = (c & 0b00011111);
		num_bytes = 2;
	} else if ((c & 0b11110000) == 0b11100000) {
		cp = (c & 0b00001111);
		num_bytes = 3;
	} else if ((c & 0b11111000) == 0b11110000) {
		cp = (c & 0b00000111);
		num_bytes = 4;
	} else {
		goto illegal_sequence;
	}
	endp = *s + num_bytes - 1;
	do {
		c = *(*s)++;
		if ((c & 0b11000000) != 0b10000000)
			goto illegal_sequence;
		cp = (cp << 6) | (c & 0b00111111);
	} while (*s < endp);
	return cp;
illegal_sequence:
	errno = EILSEQ;
	return (uint32_t)(-1);
}

union utf8_sequence {
	char byte[4];
	uint32_t err;
};

_DECL uint32_t utf8_encode(uint32_t codepoint) {
	union utf8_sequence c = {0};
	// optimize for ASCII - directly jump to end with only 1 ckeck
	if (codepoint & 0xffffff80) {
		if (codepoint >= 0x00110000) {
			// not valid unicode
			errno = EOVERFLOW;
			c.err = (uint32_t)(-1);
			return c.err;
		}
		signed char mask;
		signed char new_mask = (signed char)0b11000000;
		do {
			c.byte[0] = 0b10000000 | (codepoint & 0b00111111);
			c.err <<= 8;
			codepoint >>= 6;
			mask = new_mask;
			new_mask >>= 1; // mask is signed so a 1 is filled in from left
		} while (codepoint & new_mask);
		c.byte[0] = codepoint | mask;
	} else {
		c.byte[0] = codepoint;
	}
	return c.err;
}

// We have to switch to code page 65001 to enable UTF-8. This
// value here is used to restore the old state on exit
static unsigned old_cp = 0;
static unsigned old_input_cp = 0;

void __restore_console(void) {
#ifdef _WIN32
	if (old_cp)
		SetConsoleOutputCP(old_cp);
	if (old_cp)
		SetConsoleCP(old_input_cp);
#endif
}

_DECL void __setup_console() {
#ifdef _WIN32
	HANDLE con = GetStdHandle(STD_OUTPUT_HANDLE);
	if (con != INVALID_HANDLE_VALUE) {
		old_cp = GetConsoleOutputCP();
		old_input_cp = GetConsoleCP();
		SetConsoleCP(CP_UTF8);
		SetConsoleOutputCP(CP_UTF8);
		DWORD conmode;
		GetConsoleMode(con, &conmode);
		conmode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		SetConsoleMode(con, conmode);
	}
#endif
	setlocale(LC_CTYPE, "en_US.UTF-8");
	setlocale(LC_NUMERIC, "en_US.UTF-8");
	atexit(__restore_console);
}
