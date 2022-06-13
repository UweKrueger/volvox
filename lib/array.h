#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined (_WIN32)
#define _DECL __declspec(dllexport)
#else
#define _DECL
#endif

typedef size_t usize;

class ArrayNode {
	void* data;
	size_t size;
	_DECL ArrayNode(usize nelem, usize obj_size);
	_DECL ~ArrayNode();
	_DECL void pushback(char* obj, usize obj_size);
};

extern "C" {
	_DECL void __signal_out_of_range();
}
