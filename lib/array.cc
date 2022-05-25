#include "array.h"

extern "C" void* calloc(size_t nelem, size_t obj_size);
extern "C" void* realloc(void* ptr, size_t new_size);
extern "C" void free(void* ptr);
extern "C" void* memcpy(void* dest, void* src, size_t sz);

_DECL ArrayNode::ArrayNode(size_t nelem, size_t obj_size) :
	data(calloc(nelem, obj_size)), size(nelem) {}

_DECL ArrayNode::~ArrayNode() {
	free(this->data);
}

_DECL void ArrayNode::pushback(char* obj, size_t obj_size) {
	size_t dest = size++ * obj_size;
	data = realloc(data, dest + obj_size);
	memcpy((char*)data + dest, obj, obj_size);
}
