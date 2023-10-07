/*
 * Copyright © Uwe Krüger 2021, 2022, 2023
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "str.h"

#if defined (_WIN32)
#define _DECL __declspec(dllexport)
#define _CDECL extern "C" __declspec(dllexport)
#else
#define _DECL
#define _CDECL extern "C"
#endif

typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef int32_t i32;
typedef float f32;
typedef double f64;

namespace volvox {

	namespace map {
		
		union Value {
			unsigned long long int u64;
			long long int i64;
			unsigned int u32;
			int i32;
			float f32;
			double f64;
			struct {
				unsigned int offset;
				unsigned int size;
			};
			void* src_ptr; // to pass generic value to `insert()`
		};

		union Key {
			unsigned long long int u64;
			long long int i64;
			unsigned int u32;
			int i32;
			float f32;
			double f64;
			char string[8]; // will expand dynamically
		};

		struct Node {
			union {
				struct Node* parent;
				int bf : 2;
				unsigned u_bf : 2;
			};
			struct Node* leftChild;
			struct Node* rightChild;
			Value value;
			Key key;
		};

		struct NodePosition {
			union {
				Node* node;
				bool is_parent : 1; // for insert
			};
			Node** parent_ptr;
		};

		typedef void (node_printer)(int bf, Key* key, Value* value); 

		_DECL Node* string_new_map();
		_DECL Node* num_new_map();

// value_size: size of generic value (including 0 for strings)
// return address of inserted node with lowest bit set if existing node has been replaced

		_DECL Node* string_insert(Node** root_ptr, const char* key, Value value, int value_size, Node** target);
		_DECL Node* string_tag_insert(Node** root_ptr, const char* key, unsigned tag, Value value, int value_size, Node** target);
		_DECL Node* u64_insert(Node** root_ptr, u64 key, Value value, int value_size, Node** target);
		_DECL Node* i64_insert(Node** root_ptr, i64 key, Value value, int value_size, Node** target);
		_DECL Node* u32_insert(Node** root_ptr, u32 key, Value value, int value_size, Node** target);
		_DECL Node* i32_insert(Node** root_ptr, i32 key, Value value, int value_size, Node** target);
		_DECL Node* f32_insert(Node** root_ptr, f32 key, Value value, int value_size, Node** target);
		_DECL Node* f64_insert(Node** root_ptr, f64 key, Value value, int value_size, Node** target);

		_DECL bool string_delete(Node** root_ptr, const char* key, void (*destruct)(Value* ptr));
		_DECL bool u64_delete(Node** root_ptr, u64 key, void (*destruct)(Value* ptr));
		_DECL bool i64_delete(Node** root_ptr, i64 key, void (*destruct)(Value* ptr));
		_DECL bool u32_delete(Node** root_ptr, u32 key, void (*destruct)(Value* ptr));
		_DECL bool i32_delete(Node** root_ptr, i32 key, void (*destruct)(Value* ptr));
		_DECL bool f32_delete(Node** root_ptr, f32 key, void (*destruct)(Value* ptr));
		_DECL bool f64_delete(Node** root_ptr, f64 key, void (*destruct)(Value* ptr));

		_DECL void destroy(Node* root, void (*destruct)(Value* ptr));

		_DECL void dump(Node* root, node_printer* prt);
		_DECL int check_avl_get_depth(Node* node);
		_DECL Node* Min(Node* node);
		_DECL Node* Max(Node* node);
		_DECL Node* iter_up(Node* elem);
		_DECL Node* iter_down(Node* elem);
		_DECL Value* string_get(Node* root, const char* key);

		_DECL void prt_str_int(int bf, Key* key, Value* value);
		_DECL void prt_str_str(int bf, Key* key, Value* value);
		_DECL void prt_str_tag(int bf, Key* key, Value* value);

	}

}
