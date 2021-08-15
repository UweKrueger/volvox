#pragma once

typedef unsigned long long int u64;
typedef long long int i64;
typedef unsigned int u32;
typedef int i32;
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
extern void map_dump(MapNode* root, map_node_printer* prt);
extern bool map_string_delete(MapNode** root_ptr, char* key);
extern void map_destroy(MapNode* root);
extern int map_check_avl_get_depth(MapNode* node);
extern MapNode* map_min(MapNode* node);
extern MapNode* map_max(MapNode* node);
extern MapNode* map_iter_up(MapNode* elem);
extern MapNode* map_iter_down(MapNode* elem);
extern MapValue* map_string_get(MapNode* root, char* key);

extern void map_prt_str_int(int bf, MapKey* key, MapValue* value);
extern void map_prt_str_str(int bf, MapKey* key, MapValue* value);
