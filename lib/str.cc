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
#include <termios.h>
#include <alloca.h>
#endif
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

static void sprt(char** s, unsigned* cap, unsigned* pos, const char* pre, const VOLVOX_RtType* ft, ... /* int w, int p, unsigned flags, val */);

static void vsprt(char** s, unsigned* cap, unsigned* pos, const char* pre, const VOLVOX_RtType* ft, va_list ap) {
	if (!*cap) {
		*cap = 128;
		*s = (char*)realloc(*s, *cap);
	}
	if (pre)
		prtstring(s, cap, pos, pre);
	int space = *cap - *pos;
	while (ft) {
		int w = va_arg(ap, int);
		int p = va_arg(ap, int);
		unsigned flags = va_arg(ap, unsigned);
		switch (ft->ID) {
		case VOLVOX_BFloatTyID:
		case VOLVOX_FloatTyID:
			if (p <= 0) p = F32_DEFAULT_PRECISION;
		case VOLVOX_DoubleTyID: {
			double val = va_arg(ap, double);
			const char* fmt = getFmtFlt(flags);
			if (p <= 0) p = F64_DEFAULT_PRECISION;
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
			if (!(ft->type_attr & A_signed))
				flags |= FMT_UNSIGNED;
			if (ft->SubclassData <= 32) {
				int val = va_arg(ap, int);
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
			char* elem_ptr = va_arg(ap, char*);
			int elem_size = ft->elem_type->type_size;
			if (ft->num_fields) {
				for (uint64_t i = 0; i < ft->num_fields; i++) {
					if (ft->elem_type->ID == VOLVOX_FloatTyID) {
						sprt(s, cap, pos, i ? ", " : "[ ", ft->elem_type, w, p, flags, (double)*((float*)elem_ptr + i), nullptr, nullptr);
					} else if (ft->elem_type->ID == VOLVOX_IntegerTyID && elem_size <= 4) {
						unsigned elem = 0;
						memcpy(&elem, (char*)elem_ptr + i * elem_size, elem_size);
						if (elem_size < 4 && (ft->elem_type->type_attr & A_signed)) {
							// sign expand integer using logic left and arithmetic right shifts
							unsigned shift = 8 * (4 - elem_size);
							elem = (unsigned)((int)(elem << shift) >> shift);
						}
						sprt(s, cap, pos, i ? ", " : "[ ", ft->elem_type, w, p, flags, elem, nullptr, nullptr);
					} else if (ft->elem_type->ID == VOLVOX_IntegerTyID) {
						sprt(s, cap, pos, i ? ", " : "[ ", ft->elem_type, w, p, flags, *((uint64_t*)elem_ptr + i), nullptr, nullptr);
					} else if (ft->elem_type->ID == VOLVOX_DoubleTyID) {
						sprt(s, cap, pos, i ? ", " : "[ ", ft->elem_type, w, p, flags, *((double*)elem_ptr + i), nullptr, nullptr);
					} else {
						prtstring(s, cap, pos, "<unsupported type>");
					}
				}
				prtstring(s, cap, pos, " ]");
			} else {
				prtstring(s, cap, pos, "[]");
			}
			space = *cap - *pos;
		}
			break;
		case VOLVOX_PointerTyID: {
			char* str = va_arg(ap, char*);
			prtstring(s, cap, pos, str);
		}
			break;
		case VOLVOX_FunctionTyID: {
			char* fn = va_arg(ap, char*);
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
		
static void sprt(char** s, unsigned* cap, unsigned* pos, const char* pre, const VOLVOX_RtType* ft, ... /* int w, int p, unsigned flags, val */) {
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
#define _DECL __declspec(dllexport)
#define _CDECL __declspec(dllexport)
// Volvox uses the Itanium mangling scheme for global symbols
// which is also used by C++ compilers on most Unix-like systems.
// MSVC++ used it's own mangling scheme - however, if we use this
// compiler we can provide mangled functions as "C" functions
#define fprint _ZN6volvox6fprintEiPKcPKNS_6RtTypeEz
#define fprintln _ZN6volvox8fprintlnEiPKcPKNS_6RtTypeEz
#define print _ZN6volvox5printEPKcPKNS_6RtTypeEz
#define println _ZN6volvox7printlnEPKcPKNS_6RtTypeEz
#else
#define _DECL
#define _CDECL extern "C"
#undef VOLVOX_RtType
#define VOLVOX_RtType RtType
namespace volvox {
#endif

	_DECL bool fprint(int fd, const char* pre, const VOLVOX_RtType* ft, ... /* int w, int p, unsigned flags, val, char* post */) {
		va_list ap;
		va_start(ap, ft);
		bool has_succeeded = vfprint(fd, false, pre, ft, ap);
		va_end(ap);
		return has_succeeded;
	}

	_DECL bool fprintln(int fd, const char* pre, const VOLVOX_RtType* ft, ... /* int w, int p, unsigned flags, val, char* post */) {
		va_list ap;
		va_start(ap, ft);
		bool has_succeeded = vfprint(fd, true, pre, ft, ap);
		va_end(ap);
		return has_succeeded;
	}

	_DECL bool print(const char* pre, const VOLVOX_RtType* ft, ... /* int w, int p, unsigned flags, val, char* post */) {
		va_list ap;
		va_start(ap, ft);
		bool has_succeeded = vfprint(1, false, pre, ft, ap);
		va_end(ap);
		return has_succeeded;
	}

	_DECL bool println(const char* pre, const VOLVOX_RtType* ft, ... /* int w, int p, unsigned flags, val, char* post */) {
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
bool wGlob(char* buf, int s_len, int cur_index, char* argv, char*** rets, int* n_rets, int* max_rets) {
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
				if (wGlob(buf, s_len + 1 + dirname_len, new_index, argv, rets, n_rets, max_rets))
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
