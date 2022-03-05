#include <stdio.h>
#include <inttypes.h>
#include "types.h"
#include "str.h"

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

void vtostr(char** s, unsigned* cap, unsigned* pos, va_list ap) {
	char fmt[16] = {};
	while (FullType* ft = va_arg(ap, FullType*)) {
		int w = va_arg(ap, int);
		int p = va_arg(ap, int);
		unsigned flags = va_arg(ap, unsigned);
		int space = *cap - *pos;
		switch (ft->type->getTypeID()) {
		case llvm::Type::BFloatTyID:
		case llvm::Type::FloatTyID:
		case llvm::Type::DoubleTyID: {
			double val = va_arg(ap, double);
			const char* fmt = getFmtFlt(flags);
			if (p <= 0) p = 16; 
			int expected_nchar = max(abs(w)+1, p+7+1);
			while (space < expected_nchar) {
				*cap += *cap ? (expected_nchar + (*cap >> 1)) : 128;
				*s = (char*)realloc(*s, *cap);
				space = *cap - *pos;
			}
			*pos += sprintf(*s, fmt, val);
			if (*pos + 1 > *cap)
				abort();
		}
			break;
		case llvm::Type::IntegerTyID: {
			if (ft->type->getIntegerBitWidth() <= 32) {
				int val = va_arg(ap, int);
				const char* fmt = getFmtInt(flags);
				int expected_nchar = max(abs(w)+1, 11+1);
				while (space < expected_nchar) {
					*cap += *cap ? (expected_nchar + (*cap >> 1)) : 128;
					*s = (char*)realloc(*s, *cap);
					space = *cap - *pos;
				}
				*pos += sprintf(*s, fmt, val);
				if (*pos + 1 > *cap)
					abort();
			} else {
				abort();
			}
		}
			break;
		default:
			abort();
		}
	}
}
		
void tostr(char** s, unsigned* cap, unsigned* pos, ... /* FullType* ft, int w, int p, unsigned flags, val */) {
	va_list ap;
	va_start(ap, pos);
	vtostr(s, cap, pos, ap);
	va_end(ap);
}
