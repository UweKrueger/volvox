#pragma once

#include <stdint.h>
#include <stdbool.h>

#if defined (_MSC_VER)
#define EXTERN __declspec(dllexport)
#ifdef __cplusplus
extern "C"
{
#endif
#else
#define EXTERN extern
#endif

typedef struct ArrayNode {
	void* data;
	size_t size;
} ArrayNode;

EXTERN void array_new_array(ArrayNode* a, size_t nelem, size_t obj_size);

EXTERN void array_destroy(ArrayNode* a);

EXTERN void array_pushback(ArrayNode* a, char* obj, size_t obj_size);

#if defined (_MSC_VER)
#ifdef __cplusplus
}
#endif
#endif
