#include "array.h"

extern "C" void* calloc(usize nelem, usize obj_size);
extern "C" void* realloc(void* ptr, usize new_size);
extern "C" void free(void* ptr);
extern "C" void* memcpy(void* dest, void* src, usize sz);

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
