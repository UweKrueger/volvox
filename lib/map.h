#pragma once

#include <stdint.h>
#include <stdbool.h>

#if defined (_MSC_VER)
#define EXTERN __declspec(dllexport)
#define EXTERNVAR extern  __declspec(dllexport)
#ifdef __cplusplus
extern "C"
{
#endif
#else
#define EXTERN extern
#define EXTERNVAR extern
#endif

typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;
typedef int32_t i32;
typedef float f32;
typedef double f64;

typedef union MapValue {
	union {
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

EXTERN MapNode* map_string_new_map();
EXTERN MapNode* map_num_new_map();

// value_size: size of generic value (including 0 for strings)
// return address of inserted node with lowest bit set if existing node has been replaced

EXTERN MapNode* map_string_insert(MapNode** root_ptr, const char* key, MapValue value, int value_size, bool allow_replace);
EXTERN MapNode* map_string_tag_insert(MapNode** root_ptr, const char* key, unsigned tag, MapValue value, int value_size, bool allow_replace);
EXTERN MapNode* map_u64_insert(MapNode** root_ptr, u64 key, MapValue value, int value_size, bool allow_replace);
EXTERN MapNode* map_i64_insert(MapNode** root_ptr, i64 key, MapValue value, int value_size, bool allow_replace);
EXTERN MapNode* map_u32_insert(MapNode** root_ptr, u32 key, MapValue value, int value_size, bool allow_replace);
EXTERN MapNode* map_i32_insert(MapNode** root_ptr, i32 key, MapValue value, int value_size, bool allow_replace);
EXTERN MapNode* map_f32_insert(MapNode** root_ptr, f32 key, MapValue value, int value_size, bool allow_replace);
EXTERN MapNode* map_f64_insert(MapNode** root_ptr, f64 key, MapValue value, int value_size, bool allow_replace);

EXTERN bool map_string_delete(MapNode** root_ptr, const char* key);
EXTERN bool map_u64_delete(MapNode** root_ptr, u64 key);
EXTERN bool map_i64_delete(MapNode** root_ptr, i64 key);
EXTERN bool map_u32_delete(MapNode** root_ptr, u32 key);
EXTERN bool map_i32_delete(MapNode** root_ptr, i32 key);
EXTERN bool map_f32_delete(MapNode** root_ptr, f32 key);
EXTERN bool map_f64_delete(MapNode** root_ptr, f64 key);

EXTERN void map_destroy(MapNode* root);

EXTERN void map_dump(MapNode* root, map_node_printer* prt);
EXTERN int map_check_avl_get_depth(MapNode* node);
EXTERN MapNode* map_min(MapNode* node);
EXTERN MapNode* map_max(MapNode* node);
EXTERN MapNode* map_iter_up(MapNode* elem);
EXTERN MapNode* map_iter_down(MapNode* elem);
EXTERN MapValue* map_string_get(MapNode* root, const char* key);

EXTERN void map_prt_str_int(int bf, MapKey* key, MapValue* value);
EXTERN void map_prt_str_str(int bf, MapKey* key, MapValue* value);
EXTERN void map_prt_str_tag(int bf, MapKey* key, MapValue* value);


#if defined (_MSC_VER)
#ifdef __cplusplus
}
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
	typedef struct global_var_shadow {
		struct global_var_shadow* next;
		void* adr;
		size_t size;
		char data[8]; // dynamically extended
	} global_var_shadow;

	EXTERNVAR global_var_shadow* global_list;
	EXTERNVAR global_var_shadow** global_list_end;
#ifdef __cplusplus
}
#endif
