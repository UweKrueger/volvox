#include "array.h"

extern "C" void* calloc(usize nelem, usize obj_size);
extern "C" void* realloc(void* ptr, usize new_size);
extern "C" void free(void* ptr);
extern "C" void* memcpy(void* dest, void* src, usize sz);
extern "C" int raise(int sig);
// "long" is 64 bit on Linux and 32 bit on Windows - so this matches both systems
extern "C" long write(int fd, const void* buf, unsigned long count);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif
#define WR_STRING(a) a, ARRAY_SIZE(a) - 1
#ifndef SIGABRT
#define SIGABRT 6
#endif

_DECL ArrayNode::ArrayNode(usize nelem, usize obj_size) :
	data(calloc(nelem, obj_size)), size(nelem) {}

_DECL ArrayNode::~ArrayNode() {
	free(this->data);
}

_DECL void ArrayNode::pushback(char* obj, usize obj_size) {
	size_t dest = size++ * obj_size;
	data = realloc(data, dest + obj_size);
	memcpy((char*)data + dest, obj, obj_size);
}

extern "C" {
	_DECL void __signal_out_of_range() {
		write(2, WR_STRING("Error: array index out of range\n"));
		raise(SIGABRT);
	}
}
