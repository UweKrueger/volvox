#pragma once

#include <stdint.h>

typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef int32_t i32;
typedef float f32;
typedef double f64;

typedef union MapValue {
	unsigned long long int u64;
	long long int i64;
	unsigned int u32;
	int i32;
	float f32;
	double f64;
	union {
		struct {
			unsigned int offset;
			unsigned int size;
		};
		void* src_ptr; // to pass generic value to `insert()`
	};
} MapValue;

typedef union MapKey {
	unsigned long long int u64;
	long long int i64;
	unsigned int u32;
	int i32;
	float f32;
	double f64;
	char string[8]; // will expand dynamically
} MapKey;

typedef struct MapNode {
	union {
		struct MapNode* parent;
		int bf : 2;
		unsigned u_bf : 2;
	};
	struct MapNode* leftChild;
	struct MapNode* rightChild;
	MapValue value;
	MapKey key;
} MapNode;

typedef struct NodePosition {
	union {
		MapNode* node;
		bool is_parent : 1; // for insert
	};
	MapNode** parent_ptr;
} NodePosition;

typedef void (map_node_printer)(int bf, MapKey* key, MapValue* value); 

extern MapNode* map_string_new_map();
extern MapNode* map_num_new_map();

// value_size: size of generic value (including 0 for strings)
extern void map_string_insert(MapNode** root_ptr, char* key, MapValue value, int value_size);
extern void map_u64_insert(MapNode** root_ptr, u64 key, MapValue value, int value_size);
extern void map_i64_insert(MapNode** root_ptr, i64 key, MapValue value, int value_size);
extern void map_u32_insert(MapNode** root_ptr, u32 key, MapValue value, int value_size);
extern void map_i32_insert(MapNode** root_ptr, i32 key, MapValue value, int value_size);
extern void map_f32_insert(MapNode** root_ptr, f32 key, MapValue value, int value_size);
extern void map_f64_insert(MapNode** root_ptr, f64 key, MapValue value, int value_size);

extern _Bool map_string_delete(MapNode** root_ptr, char* key);
extern _Bool map_u64_delete(MapNode** root_ptr, u64 key);
extern _Bool map_i64_delete(MapNode** root_ptr, i64 key);
extern _Bool map_u32_delete(MapNode** root_ptr, u32 key);
extern _Bool map_i32_delete(MapNode** root_ptr, i32 key);
extern _Bool map_f32_delete(MapNode** root_ptr, f32 key);
extern _Bool map_f64_delete(MapNode** root_ptr, f64 key);

extern void map_destroy(MapNode* root);

extern void map_dump(MapNode* root, map_node_printer* prt);
extern int map_check_avl_get_depth(MapNode* node);
extern MapNode* map_min(MapNode* node);
extern MapNode* map_max(MapNode* node);
extern MapNode* map_iter_up(MapNode* elem);
extern MapNode* map_iter_down(MapNode* elem);
extern MapValue* map_string_get(MapNode* root, char* key);

extern void map_prt_str_int(int bf, MapKey* key, MapValue* value);
extern void map_prt_str_str(int bf, MapKey* key, MapValue* value);
