#include <inttypes.h>
#include <stdbool.h>

typedef char fmt_str_t[8]; 

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
