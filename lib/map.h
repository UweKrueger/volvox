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
			unsigned int len;
		};
		char* c_str;
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

extern MapNode* map_string_new_map();
extern MapNode* map_num_new_map();

// val_str_len: length including 0 when string values
extern void map_string_insert(MapNode** root_ptr, char* key, MapValue value, bool value_is_string);
extern void map_string_dump(MapNode* root);
extern bool map_string_delete(MapNode** root_ptr, char* key);
extern void map_destroy(MapNode* root);
extern int map_check_avl_get_depth(MapNode* node);
extern MapNode* map_min(MapNode* node);
extern MapNode* map_max(MapNode* node);
extern MapNode* map_iter_up(MapNode* elem);
extern MapNode* map_iter_down(MapNode* elem);
extern MapValue* map_string_get(MapNode* root, char* key);
