#include <string.h>
#include <stdlib.h>
#include "array.h"

#if defined (_MSC_VER)
#define _DECL __declspec(dllexport)
#else
#define _DECL
#endif

_DECL void array_new_array(ArrayNode* a, size_t nelem, size_t obj_size) {
	a->data = calloc(nelem, obj_size);
	a->size = nelem;
}

_DECL void array_destroy(ArrayNode* a) {
	free(a->data);
}

_DECL void array_pushback(ArrayNode* a, char* obj, size_t obj_size) {
	a->data = realloc(a, a->size + 1);
	memcpy((char*)a->data + a->size * obj_size, obj, obj_size);
	a->size++;
}
