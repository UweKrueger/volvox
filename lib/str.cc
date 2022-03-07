#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include "types.h"
#include "str.h"

extern "C" {
double qwertz(double x) {
	double y = x*x;
	printf("Result of mult: >%g<\n", y);
	return y;
}
}

namespace volvox {

	bool is_compiler = false;

	const char* i1::str() { return v ? "true" : "false"; }

	const char* i1::fmt = nullptr;
	const char* i1::fmt_w = nullptr;
	const char* i1::fmt_wp = nullptr;


/* create the printf-format string to print given Type */

	const char* getFmtInt(unsigned fmt_flags) {
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

	const char* getFmtLong(unsigned fmt_flags) {
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

	const char* getFmtFlt(unsigned fmt_flags) {
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
	static inline int max(int a, int b) {
		return a > b ? a : b;
	}

	void vsprt(char** s, unsigned* cap, unsigned* pos, const char* pre, FullType* ft, va_list ap) {
		if (!*cap) {
			*cap = 128;
			*s = (char*)realloc(*s, *cap);
		}
		int space = *cap - *pos;
		if (pre)
			for (int n = 0;;) {
				int m = snprintf(*s, space, "%s", pre + n);
				if (m >= space) {
					n += space - 1;
					*cap += (*cap >> 1) + (m - space) + 1;
					*s = (char*)realloc(*s, *cap);
					*pos += space - 1;
					space = *cap - *pos;
				} else {
					*pos += m;
					space = *cap - *pos;
					break;
				}
			}
		do {
			int w = va_arg(ap, int);
			int p = va_arg(ap, int);
			unsigned flags = va_arg(ap, unsigned);
			switch (is_compiler ? ft->type->getTypeID() : ft->rt_type.ID) {
			case llvm::Type::BFloatTyID:
			case llvm::Type::FloatTyID:
				if (p <= 0) p = F32_DEFAULT_PRECISION;
			case llvm::Type::DoubleTyID: {
				double val = va_arg(ap, double);
				const char* fmt = getFmtFlt(flags);
				if (p <= 0) p = F64_DEFAULT_PRECISION;
				int expected_nchar = max(abs(w)+1, p+7+1);
				while (space < expected_nchar) {
					*cap += expected_nchar + (*cap >> 1);
					*s = (char*)realloc(*s, *cap);
					space = *cap - *pos;
				}
				*pos += sprintf(*s, fmt, val);
				space = *cap - *pos;
				if (space < 1)
					abort(); // error in calculation 
			}
				break;
			case llvm::Type::IntegerTyID: {
				if ((is_compiler ? ft->type->getIntegerBitWidth() : ft->rt_type.SubclassData) <= 32) {
					int val = va_arg(ap, int);
					const char* fmt = getFmtInt(flags);
					int expected_nchar = max(abs(w)+1, 21+1);
					while (space < expected_nchar) {
						*cap += expected_nchar + (*cap >> 1);
						*s = (char*)realloc(*s, *cap);
						space = *cap - *pos;
					}
					*pos += sprintf(*s, fmt, val);
					space = *cap - *pos;
					if (space < 1)
						abort(); // error in calculation 
				} else {
					long long int val = va_arg(ap, long long int);
					const char* fmt = getFmtLong(flags);
					int expected_nchar = max(abs(w)+1, 11+1);
					while (space < expected_nchar) {
						*cap += expected_nchar + (*cap >> 1);
						*s = (char*)realloc(*s, *cap);
						space = *cap - *pos;
					}
					*pos += sprintf(*s, fmt, val);
					space = *cap - *pos;
					if (space < 1)
						abort(); // error in calculation
				}
			}
				break;
			default:
				abort();
			}
			const char* post = va_arg(ap, char*);
			if (post)
				for (int n = 0;;) {
					int m = snprintf(*s, space, "%s", post + n);
					if (m >= space) {
						n += space - 1;
						*cap += (*cap >> 1) + (m - space) + 1;
						*s = (char*)realloc(*s, *cap);
						*pos += space - 1;
						space = *cap - *pos;
					} else {
						*pos += m;
						space = *cap - *pos;
						break;
					}
				}
		} while ((ft = va_arg(ap, FullType*)));
	}
		
	void sprt(char** s, unsigned* cap, unsigned* pos, const char* pre, FullType* ft, ... /* int w, int p, unsigned flags, val */) {
		va_list ap;
		va_start(ap, ft);
		vsprt(s, cap, pos, pre, ft, ap);
		va_end(ap);
	}

	char* str(FullType* ft, ...) {
		va_list ap;
		char* s = NULL;
		unsigned cap = 0;
		unsigned pos = 0;
		va_start(ap, ft);
		sprt(&s, &cap, &pos, nullptr, ft, ap);
		va_end(ap);
		return s;
	}

	bool print(int fd, const char* pre, FullType* ft, ... /* int w, int p, unsigned flags, val, char* post */) {
		va_list ap;
		char* s = NULL;
		unsigned cap = 0;
		unsigned pos = 0;
		va_start(ap, ft);
		sprt(&s, &cap, &pos, nullptr, ft, ap);
		va_end(ap);
		int n = write(fd, s, pos);
		free(s);
		return n != pos;
	}

	bool println(int fd, const char* pre, FullType* ft, ... /* int w, int p, unsigned flags, val, char* post */) {
		va_list ap;
		char* s = NULL;
		unsigned cap = 0;
		unsigned pos = 0;
		va_start(ap, ft);
		sprt(&s, &cap, &pos, nullptr, ft, ap);
		va_end(ap);
		s[pos] = '\n';
		int n = write(fd, s, pos + 1);
		free(s);
		return n != pos + 1;
	}

}
