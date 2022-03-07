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

namespace volvox {

	extern bool is_compiler;

	class i1 {
	public:
		bool v;
		i1() : v(false) {}
		i1(bool v) : v(v) {}
		operator bool() const { return v; }

		static const char* fmt;
		static const char* fmt_w;
		static const char* fmt_wp;

		const char* str();
		void str(char** s, unsigned cap, unsigned pos);
	};

}
