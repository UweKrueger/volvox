#include <stdio.h>
#include <inttypes.h>
#include "types.h"
#include "str.h"
#if defined (_MSC_VER)
#include <windows.h>
#include <io.h>
#include <malloc.h>
#define nullptr ((void*)0)
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
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

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
static void prtstring(char** s, unsigned* cap, unsigned* pos, const char* str) {
	int space = *cap - *pos;
	for (int n = 0;;) {
		int m = snprintf(*s + *pos, space, "%s", str + n);
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

static void sprt(char** s, unsigned* cap, unsigned* pos, const char* pre, const VOLVOX_RtType* ft, ... /* val, int w, int p, unsigned flags */);

static void print_array(char** s, unsigned* cap, unsigned* pos, const VOLVOX_RtType*elem_type, const char* elem_ptr,
                        uint64_t dims[], uint64_t subsz[], int order, int indent, int w, int p, unsigned flags)
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
	pre1[idx1++] = ',';
	if (order > 1) {
		pre1[idx1++] = '\n';
		memset(pre1 + idx1, ' ', indent);
		idx1 += indent;
	}
	pre1[idx1] = '\0';
	int suborder = order - 1;
	if (!suborder) {
		// we want the field to be in an eye-pleasing pitch by default
		if (!w)
			w = ARRAY_DEFAULT_FIELD_WIDTH;
		if (!p)
			p = ARRAY_DEFAULT_PRECISION;
	}
	uint64_t offset = 0;
	for (int i = 0; i < dims[0]; i++) {
		const char* pre = i ? pre1 : pre0;
		if (suborder) {
			prtstring(s, cap, pos, pre);
			print_array(s, cap, pos, elem_type, elem_ptr + offset, &dims[1], &subsz[1], suborder, indent, w, p, flags);
		} else {
			if (elem_type->ID == VOLVOX_FloatTyID) {
				sprt(s, cap, pos, pre, elem_type, (double)*((float*)elem_ptr + i), w, p, flags, nullptr, nullptr);
			} else if (elem_type->ID == VOLVOX_IntegerTyID && subsz[1] <= 4) {
				unsigned elem;
				memcpy(&elem, (char*)elem_ptr + offset, subsz[1]);
				if (subsz[1] < 4 && (elem_type->type_attr & A_signed)) {
					// sign expand integer using logic left and arithmetic right shifts
					unsigned shift = 8 * (4 - subsz[1]);
					elem = (unsigned)((int)(elem << shift) >> shift);
				}
				sprt(s, cap, pos, pre, elem_type, elem, w, p, flags, nullptr, nullptr);
			} else if (elem_type->ID == VOLVOX_IntegerTyID) {
				sprt(s, cap, pos, pre, elem_type, *(uint64_t*)(elem_ptr + offset), w, p, flags, nullptr, nullptr);
			} else if (elem_type->ID == VOLVOX_DoubleTyID) {
				sprt(s, cap, pos, pre, elem_type, *(double*)(elem_ptr + offset), w, p, flags, nullptr, nullptr);
			} else {
				prtstring(s, cap, pos, "<unsupported type>");
			}
		}
		offset += subsz[1];
	}
	prtstring(s, cap, pos, " ]");
}

static void vsprt(char** s, unsigned* cap, unsigned* pos, const char* pre, const VOLVOX_RtType* ft, va_list ap) {
	if (!*cap) {
		*cap = 128;
		*s = (char*)realloc(*s, *cap);
	}
	if (pre)
		prtstring(s, cap, pos, pre);
	int space = *cap - *pos;
	unsigned long long rt_len;
	while (ft) {
		switch (ft->ID) {
		case VOLVOX_BFloatTyID:
		case VOLVOX_FloatTyID:
		case VOLVOX_DoubleTyID: {
			double val = va_arg(ap, double);
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			const char* fmt = getFmtFlt(flags);
			if (p <= 0) p = (ft->ID == VOLVOX_DoubleTyID) ? F64_DEFAULT_PRECISION : F32_DEFAULT_PRECISION;
			int expected_nchar = Max(abs(w)+1, p+7+1);
			while (space < expected_nchar) {
				*cap += expected_nchar + (*cap >> 1);
				*s = (char*)realloc(*s, *cap);
				space = *cap - *pos;
			}
			*pos += sprintf(*s + *pos, fmt, w, p, val);
			space = *cap - *pos;
			if (space < 1)
				abort(); // error in calculation 
					            }
			break;
		case VOLVOX_IntegerTyID: {
			if (ft->SubclassData <= 32) {
				int val = va_arg(ap, int);
				int w = va_arg(ap, int);
				int p = va_arg(ap, int);
				unsigned flags = va_arg(ap, unsigned);
				if (!(ft->type_attr & A_signed))
					flags |= FMT_UNSIGNED;
				if (ft->SubclassData < 32)
					if (ft->SubclassData == 1) {
						// bool
						prtstring(s, cap, pos, val & 1 ? "true" : "false");
						break;
					}
				// extend upper bits according to signedness
				if (ft->type_attr & A_signed)
					val = (int)((unsigned)val << (32 - ft->SubclassData)) >> (32 - ft->SubclassData);
				else
					val = (int)(((unsigned)val << (32 - ft->SubclassData)) >> (32 - ft->SubclassData));
				const char* fmt = getFmtInt(flags);
				int expected_nchar = Max(abs(w)+1, 21+1);
				while (space < expected_nchar) {
					*cap += expected_nchar + (*cap >> 1);
					*s = (char*)realloc(*s, *cap);
					space = *cap - *pos;
				}
				*pos += sprintf(*s + *pos, fmt, w, val);
				space = *cap - *pos;
				if (space < 1)
					abort(); // error in calculation 
			} else {
				long long int val = va_arg(ap, long long int);
				int w = va_arg(ap, int);
				int p = va_arg(ap, int);
				unsigned flags = va_arg(ap, unsigned);
				const char* fmt = getFmtLong(flags);
				int expected_nchar = Max(abs(w)+1, 11+1);
				while (space < expected_nchar) {
					*cap += expected_nchar + (*cap >> 1);
					*s = (char*)realloc(*s, *cap);
					space = *cap - *pos;
				}
				*pos += sprintf(*s + *pos, fmt, w, val);
				space = *cap - *pos;
				if (space < 1)
					abort(); // error in calculation
			}
		}
			break;
		case VOLVOX_ArrayTyID: {
			// rt_len = ft->num_fields;
			unsigned order = ft->SubclassData;
			uint64_t* dims = (uint64_t*)alloca(order * sizeof(uint64_t));
			uint64_t* subsz = (uint64_t*)alloca((order + 1) * sizeof(uint64_t));
			for (unsigned n = 0; n < order; n++)
				dims[n] = va_arg(ap, long long);
			subsz[order] = ft->elem_type->type_size;
			for (int n = order - 1; n >= 0; n--)
				subsz[n] = dims[n] * subsz[n + 1];
			char* elem_ptr = va_arg(ap, char*);
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			long long elem_size = ft->elem_type->type_size;
			if (subsz[0]) {
				print_array(s, cap, pos, ft->elem_type, elem_ptr, dims, subsz, order, 0, w, p, flags);
			} else {
				for (unsigned n = 0; n < order; n++)
					prtstring(s, cap, pos, "[");
				for (unsigned n = 0; n < order; n++)
					prtstring(s, cap, pos, "]");
			}
			space = *cap - *pos;
		}
			break;
		case VOLVOX_PointerTyID: {
			char* str = va_arg(ap, char*);
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			prtstring(s, cap, pos, str);
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
			space = *cap - *pos;
			if (space < 1)
				abort(); // error in calculation 
					            }
			break;
		default:
			fprintf(stderr, "TypeID: %u\n", ft->ID);
			abort();
		}
		const char* post = va_arg(ap, char*);
		if (post) {
			prtstring(s, cap, pos, post);
			space = *cap - *pos;
		}
		ft = va_arg(ap, const VOLVOX_RtType*);
	}
}
		
static void sprt(char** s, unsigned* cap, unsigned* pos, const char* pre, const VOLVOX_RtType* ft, ... /* val, int w, int p, unsigned flags */) {
	va_list ap;
	va_start(ap, ft);
	vsprt(s, cap, pos, pre, ft, ap);
	va_end(ap);
}

static char* str(const VOLVOX_RtType* ft, ...) {
	va_list ap;
	char* s = NULL;
	unsigned cap = 0;
	unsigned pos = 0;
	va_start(ap, ft);
	sprt(&s, &cap, &pos, nullptr, ft, ap);
	va_end(ap);
	return s;
}

static bool vfprint(int fd, bool newline, const char* pre, const VOLVOX_RtType* ft, va_list ap) {
	char* s = NULL;
	unsigned cap = 0;
	unsigned pos = 0;
	vsprt(&s, &cap, &pos, pre, ft, ap);
	unsigned bytes_to_write = pos;
	if (newline) {
		s[pos] = '\n';
		bytes_to_write++;
	}
	int n = write(fd, s, bytes_to_write);
	free(s);
	return n == bytes_to_write;
}

#if defined (_MSC_VER)
// Volvox uses the Itanium mangling scheme for global symbols
// which is also used by C++ compilers on most Unix-like systems.
// MSVC++ used it's own mangling scheme - however, if we use this
// compiler we can provide mangled functions as "C" functions
#define fprint _ZN6volvox6fprintEiPKcPKNS_6RtTypeEz
#define fprintln _ZN6volvox8fprintlnEiPKcPKNS_6RtTypeEz
#define print _ZN6volvox5printEPKcPKNS_6RtTypeEz
#define println _ZN6volvox7printlnEPKcPKNS_6RtTypeEz
#else
#undef VOLVOX_RtType
#define VOLVOX_RtType RtType
namespace volvox {
#endif

	_DECL bool fprint(int fd, const char* pre, const VOLVOX_RtType* ft, ... /* val, int w, int p, unsigned flags, ..., char* post */) {
		va_list ap;
		va_start(ap, ft);
		bool has_succeeded = vfprint(fd, false, pre, ft, ap);
		va_end(ap);
		return has_succeeded;
	}

	_DECL bool fprintln(int fd, const char* pre, const VOLVOX_RtType* ft, ... /* val, int w, int p, unsigned flags, ..., char* post */) {
		va_list ap;
		va_start(ap, ft);
		bool has_succeeded = vfprint(fd, true, pre, ft, ap);
		va_end(ap);
		return has_succeeded;
	}

	_DECL bool print(const char* pre, const VOLVOX_RtType* ft, ... /* val, int w, int p, unsigned flags, ..., char* post */) {
		va_list ap;
		va_start(ap, ft);
		bool has_succeeded = vfprint(1, false, pre, ft, ap);
		va_end(ap);
		return has_succeeded;
	}

	_DECL bool println(const char* pre, const VOLVOX_RtType* ft, ... /* val, int w, int p, unsigned flags, ..., char* post */) {
		va_list ap;
		va_start(ap, ft);
		bool has_succeeded = vfprint(1, true, pre, ft, ap);
		va_end(ap);
		return has_succeeded;
	}
#if !defined (_MSC_VER)
}
#endif

_CDECL bool enableColorANSI(int fd) {
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

_CDECL void showtestres(int fd, int width, const char* testcase, bool result) {
	if (width < 6)
		width = 6;
	bool have_color = enableColorANSI(fd);

	char* buf = (char*)alloca(width + (have_color ? 11 : 2));
	snprintf(buf, width-4, "%*s", -width+5, testcase);
	strcpy(buf+width-5, have_color ? (result ? " \033[32mPASS\033[0m\n" : " \033[31mFAIL\033[0m\n") : (result ? " PASS\n" : " FAIL\n"));
	write(fd, buf, width + (have_color ? 10 : 1));
}

// find out the terminal size #rows are stored in the lower 16 bits
// and #columns in the upper. In case of failure errno is set and -1 is returned
_CDECL int getTermSize(int fd)
{
#if defined (_MSC_VER)
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

#if defined (_MSC_VER)
static bool Glob_impl(char* buf, int s_len, int cur_index, const char* argv, char*** rets, size_t* n_rets, size_t* max_rets) {
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
	return found;
}
#endif

_CDECL void volvox_free_glob(volvox_glob_t* rets) {
#if defined (_MSC_VER)
	if (rets->dirs) {
		for (size_t i=0; i < rets->size; i++)
			free(rets->dirs[i]);
		free(rets->dirs);
	}
#else
	glob_t glob = {
		.gl_pathc = rets->size,
		.gl_pathv = rets->dirs,
		.gl_offs = 0,
	};
	globfree(&glob);
#endif
	rets->dirs = nullptr;
	rets->size = 0;
}

_CDECL volvox_glob_t volvox_glob(const char* pattern) {
	volvox_glob_t rets = {
		.size = 0,
		.dirs = nullptr,
	};
#if defined (_MSC_VER)
	char buf[MAX_PATH] = "";
	size_t max_rets = 0;
	if (Glob_impl(buf, 0, 0, pattern, &rets.dirs, &rets.size, &max_rets))
		return rets;
	volvox_free_glob(&rets);
#else
	glob_t glob_rets = {
		.gl_pathc = 0,
		.gl_pathv = nullptr,
		.gl_offs = 0,
	};
	int res = glob(pattern, GLOB_MARK, NULL, &glob_rets);
	if (!res) {
		rets.size = glob_rets.gl_pathc;
		rets.dirs = glob_rets.gl_pathv;
	}
	// not clear if the glob_t struct must be freed in case of error... :-(
#endif
	return rets;
}

#ifdef _WIN32
// dest must be 32767 bytes in size - maximum length of command line on Windows
static bool getCmdLine(char* dest, const char* cmd, char* const argv[]) {
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

_CDECL bool volvox_spawn(int* pid, int* child_stdin, int* child_stdout,
                         int* child_stderr, char* const argv[]) {
#ifdef _WIN32
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
   // this is not correct - TODO: map/merge Windows errors / POSIX errows
   errno = GetLastError();
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
#endif
   return false;
}

// Wait for a process to finish
// Return exit code of process
// return -1 and sets errno on failure
_CDECL int volvox_wait(int pid) {
#ifdef _WIN32
	HANDLE p_handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
	if (!p_handle)
		goto error;
	unsigned res = WaitForSingleObject(p_handle, INFINITE);
	if (res)
		goto error;
	DWORD ecode;
	bool res2 = GetExitCodeProcess(p_handle, &ecode);
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
_CDECL int volvox_try_wait(int pid) {
#ifdef _WIN32
	HANDLE p_handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
	if (!p_handle)
		goto error;
	DWORD ecode;
	bool res2 = GetExitCodeProcess(p_handle, &ecode);
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
