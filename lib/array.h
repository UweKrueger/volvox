#pragma once
#include <stddef.h>

#if defined (_WIN32)
#define _DECL __declspec(dllexport)
#else
#define _DECL
#endif

class ArrayNode {
	void* data;
	size_t size;
	_DECL ArrayNode(size_t nelem, size_t obj_size);
	_DECL ~ArrayNode();
	_DECL void pushback(char* obj, size_t obj_size);
};
