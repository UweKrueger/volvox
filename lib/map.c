#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "map.h"

/* balace factor of nodes require only 2 bits and parent pointer is
 * aligned to 8 bytes so these two can share a 64 bit value */

#define PARENT(node) ((MapNode*)((uintptr_t)node->parent & ~0x03))
#define SET_PARENT(node, p) node->parent = (MapNode*)((uintptr_t)p | node->u_bf)

MapNode* map_string_new_map() { return NULL; }
MapNode* map_num_new_map() { return NULL; }

// value_size: length including 0 when string values
static MapNode* map_string_new_node(const char* key, MapValue value, unsigned int value_size) {
	unsigned keylen = (unsigned)strlen(key) + 1;
	if (value_size >= 8) {
		keylen = ((keylen + 7) >> 3) << 3;
	} else if (value_size >= 4) {
		keylen = ((keylen + 3) >> 2) << 2;
	} else if (value_size >= 2) {
		keylen = ((keylen + 1) >> 1) << 1;
	}
	size_t nodesz = keylen + value_size <= 8 ? sizeof(MapNode) : sizeof(MapNode) + keylen - 8 + value_size;
	MapNode* node = malloc(nodesz);
	strcpy(&node->key.string[0], key);
	if (value_size) {
		char* val_ptr = &node->key.string[0] + keylen;
		memcpy(val_ptr, value.src_ptr, value_size);
		node->value.offset = val_ptr - (char*)&node->value;
		node->value.size = value_size;
	} else {
		node->value = value;
	}
	node->parent = node->leftChild = node->rightChild = NULL;
	return node;
}

#define DEFINE_MAP_NEW_NODE_FOR(typ) static MapNode* map_ ## typ ## _new_node(typ key, MapValue value, unsigned int value_size) { \
	size_t nodesz = sizeof(MapNode) + value_size; \
	MapNode* node = malloc(nodesz); \
	node->key.typ = key; \
	if (value_size) { \
		memcpy((char*)node + sizeof(MapNode), value.src_ptr, value_size); \
		node->value.offset = sizeof(MapKey) + sizeof(MapValue); \
		node->value.size = value_size; \
	} else { \
		node->value = value; \
	} \
	node->parent = node->leftChild = node->rightChild = NULL; \
	return node; \
}

DEFINE_MAP_NEW_NODE_FOR(u64)
DEFINE_MAP_NEW_NODE_FOR(i64)
DEFINE_MAP_NEW_NODE_FOR(u32)
DEFINE_MAP_NEW_NODE_FOR(i32)
DEFINE_MAP_NEW_NODE_FOR(f32)
DEFINE_MAP_NEW_NODE_FOR(f64)

static MapNode* rotLeft(MapNode* a, MapNode* b) {
	/*
	 *      a                   b
	 *     / \                 / \
	 *    X   b      =>       a   Y
	 *       / \             / \
	 *      c   Y           X   c
	 */	  
	MapNode* c = b->leftChild;
	a->rightChild = c;
	if (c)
		SET_PARENT(c, a);
	b->leftChild = a;
	SET_PARENT(a, b);
	if (b->bf == 0) {
		a->bf = 1;
		b->bf = -1;
	} else {
		a->bf = 0;
		b->bf = 0;
	}
	return b;
}

static MapNode* rotRight(MapNode* a, MapNode* b) {
	/*
	 *        a              b
	 *       / \            / \
	 *      b   Y    =>    X   a
	 *     / \                / \
	 *    X   c              c   Y
	 */	  
	MapNode* c = b->rightChild;
	a->leftChild = c;
	if (c)
		SET_PARENT(c, a);
	b->rightChild = a;
	SET_PARENT(a, b);
	if (b->bf == 0) {
		a->bf = -1;
		b->bf = 1;
	} else {
		a->bf = 0;
		b->bf = 0;
	}
	return b;
}

static MapNode* rotRightLeft(MapNode* a, MapNode* b) {
	/*
	 *      a                   c
	 *     / \                 / \
	 *    X   b               /   \
	 *       / \     =>      /     \
	 *      c   Y           a       b
	 *     / \             / \     / \
	 *    d   e           X   d   e   Y
	 */	  
	MapNode* c = b->leftChild;
	MapNode* d = c->leftChild;
	MapNode* e = c->rightChild;
	c->leftChild = a;
	SET_PARENT(a, c);
	a->rightChild = d;
	if (d)
		SET_PARENT(d, a);
	c->rightChild = b;
	SET_PARENT(b, c);
	b->leftChild = e;
	if (e)
		SET_PARENT(e, b);
	if (c->bf == 0) {
		a->bf = 0;
		b->bf = 0;
	} else {
		if (c->bf < 0) {
			a->bf = 0;
			b->bf = 1;
		} else {
			a->bf = -1;
			b->bf = 0;
		}
	}
	c->bf = 0;
	return c;
}

static MapNode* rotLeftRight(MapNode* a, MapNode* b) {
	/*
	 *          a                   c
	 *         / \                 / \
	 *        b   Y               /   \
	 *       / \         =>      /     \
	 *      Y   c               b       a
	 *         / \             / \     / \
	 *        e   d           X   e   d   Y
	 */	  
	MapNode* c = b->rightChild;
	MapNode* d = c->rightChild;
	MapNode* e = c->leftChild;
	c->rightChild = a;
	SET_PARENT(a, c);
	a->leftChild = d;
	if (d)
		SET_PARENT(d, a);
	c->leftChild = b;
	SET_PARENT(b, c);
	b->rightChild = e;
	if (e)
		SET_PARENT(e, b);
	if (c->bf == 0) {
		a->bf = 0;
		b->bf = 0;
	} else {
		if (c->bf > 0) {
			a->bf = 0;
			b->bf = -1;
		} else {
			a->bf = 1;
			b->bf = 0;
		}
	}
	c->bf = 0;
	return c;
}

static void map_insert_priv(MapNode** root_ptr, MapNode* node, MapNode* parent, MapNode** parent_ptr) {
	// insert node
	SET_PARENT(node, parent);
	*parent_ptr = node;
	MapNode* parent_save;
	MapNode* n0;
	// loop from leaf to root and adjust balance factors
	for (MapNode* a = parent; a; a = PARENT(node)) {
		if (node == a->rightChild) {
			if (a->bf <= 0) {
				if (a->bf < 0) {
					a->bf = 0;
					break;
				}
				a->bf = 1;
				node = a;
				continue;
			} else {
				// a->bf would become 2 -> rebalance
				parent_save = PARENT(a);
				if (node->bf < 0)
					n0 = rotRightLeft(a, node);
				else
					n0 = rotLeft(a, node);
			}
		} else {
			if (a->bf >= 0) {
				if (a->bf > 0) {
					a->bf = 0;
					break;
				}
				a->bf = -1;
				node = a;
				continue;
			} else {
				//a->bf would become -2 -> rebalance
				parent_save = PARENT(a);
				if (node->bf > 0)
					n0 = rotLeftRight(a, node);
				else
					n0 = rotRight(a, node);
			}
		}
		SET_PARENT(n0, parent_save);
		if (parent_save) {
			if (a == parent_save->leftChild)
				parent_save->leftChild = n0;
			else
				parent_save->rightChild = n0;
		} else {
			*root_ptr = n0;
		}
		break;
	}
}

static NodePosition map_string_find(MapNode** parent_ptr, const char* key) {
	NodePosition pos;
	MapNode* curr;
	MapNode* parent = *parent_ptr ? (*parent_ptr)->parent : NULL;
	for(;;) {
		curr = *parent_ptr;
		if (!curr) break;
		const char* k = key;
		for (const char* c = curr->key.string; ; c++, k++) {
			if (!*c) {
				if(!*k) {
					goto pos_found;
				} else {
					parent_ptr = &curr->rightChild;
					break;
				}
			}
			if (*k < *c) {
				parent_ptr = &curr->leftChild;
				break;
			} else if (*k > *c) {
				parent_ptr = &curr->rightChild;
				break;
			}
		}
		parent = curr;
	}
pos_found:
	pos = (NodePosition){ .node = curr ? curr : parent, .parent_ptr = parent_ptr };
	if (!curr)
		pos.is_parent = true;
	return pos;
}

#define DEFINE_MAP_FIND_FOR(typ) static NodePosition map_ ## typ ## _find(MapNode** parent_ptr, typ key) { \
	NodePosition pos; \
	MapNode* curr; \
	MapNode* parent = *parent_ptr ? (*parent_ptr)->parent : NULL; \
	for(;;) { \
		curr = *parent_ptr; \
		if (!curr) break; \
		if (key == curr->value.typ) { \
				goto pos_found; \
		} else if (key > curr->value.typ) { \
			parent_ptr = &curr->rightChild; \
		} else { \
			parent_ptr = &curr->leftChild; \
		} \
		parent = curr; \
	} \
pos_found: \
	pos = (NodePosition){ .node = curr ? curr : parent, .parent_ptr = parent_ptr }; \
	if (!curr) \
		pos.is_parent = true; \
	return pos; \
}	

DEFINE_MAP_FIND_FOR(u64)
DEFINE_MAP_FIND_FOR(i64)
DEFINE_MAP_FIND_FOR(u32)
DEFINE_MAP_FIND_FOR(i32)
DEFINE_MAP_FIND_FOR(f32)
DEFINE_MAP_FIND_FOR(f64)

bool map_string_insert(MapNode** root_ptr, const char* key, MapValue value, int value_size) {
	MapNode* node = map_string_new_node(key, value, value_size);
	NodePosition insert_pos = map_string_find(root_ptr, key);
	if(insert_pos.is_parent) {
		map_insert_priv(root_ptr, node, (MapNode*)((uintptr_t)insert_pos.node & ~0x01), insert_pos.parent_ptr);
		return true;
	} else {
		// replace current element with new
		node->parent = insert_pos.node->parent;
		node->leftChild = insert_pos.node->leftChild;
		node->rightChild = insert_pos.node->rightChild;
		free(insert_pos.node);
		return false;
	}
}

#define DEFINE_MAP_INSERT_FOR(typ) bool map_ ## typ ## _insert(MapNode** root_ptr, typ key, MapValue value, int value_size) { \
	MapNode* node = map_ ## typ ## _new_node(key, value, value_size); \
	NodePosition insert_pos = map_ ## typ ## _find(root_ptr, key); \
	if(insert_pos.is_parent) { \
		map_insert_priv(root_ptr, node, (MapNode*)((uintptr_t)insert_pos.node & ~0x01), insert_pos.parent_ptr); \
		return true; \
	} else { \
		node->parent = insert_pos.node->parent; \
		node->leftChild = insert_pos.node->leftChild; \
		node->rightChild = insert_pos.node->rightChild; \
		free(insert_pos.node); \
		return false; \
	} \
}

DEFINE_MAP_INSERT_FOR(u64)
DEFINE_MAP_INSERT_FOR(i64)
DEFINE_MAP_INSERT_FOR(u32)
DEFINE_MAP_INSERT_FOR(i32)
DEFINE_MAP_INSERT_FOR(f32)
DEFINE_MAP_INSERT_FOR(f64)

void map_dump_priv(MapNode* curr, char* indent, bool is_right, map_node_printer* prt) {
	if (!curr) return;
	int cur_len = strlen(indent);
	if (curr->leftChild) {
		if (is_right) {
			strncat(indent, "┃", 255-cur_len);
		} else {
			strncat(indent, " ", 255-cur_len);
		}
		map_dump_priv(curr->leftChild, indent, false, prt);
	}
	indent[cur_len] = '\0';
	fputs(indent, stdout);
	if (is_right) {
		fputs("┗", stdout);
	} else {
		fputs("┏", stdout);
	}
	if (curr->leftChild) {
		if (curr->rightChild) {
			fputs("╋", stdout);
		} else {
			fputs("┻", stdout);
		}
	} else {
		if (curr->rightChild) {
			fputs("┳", stdout);
		} else {
			fputs("━", stdout);
		}
	}
	prt(curr->bf, &curr->key, &curr->value);
	if (curr->rightChild) {
		if (is_right) {
			strncat(indent, " ", 255-cur_len);
		} else {
			strncat(indent, "┃", 255-cur_len);
		}
		map_dump_priv(curr->rightChild, indent, true, prt);
	}
}

void map_dump(MapNode* root, map_node_printer* prt) {
	char buf[256] = "";
	map_dump_priv(root, buf, false, prt);
}

void map_prt_str_int(int bf, MapKey* key, MapValue* value) {
	fputs(" \"", stdout);
	fputs(key->string, stdout);
	printf("\": %d %d\n", bf, value->i32);
}

void map_prt_str_str(int bf, MapKey* key, MapValue* value) {
	fputs(" \"", stdout);
	fputs(key->string, stdout);
	printf("\": %d \"%s\"\n", bf, (const char*)value + value->offset);
}

static bool map_delete_priv(MapNode** root_ptr, MapNode* curr) {
	MapNode* new_mom;
	MapNode* parent = PARENT(curr);
	MapNode* n0 = curr;
	MapNode* a = parent;
	bool twoleaves = false;
	bool is_right = false;
	bool is_left = false;
	if (curr->leftChild) {
		if (curr->rightChild) {
			twoleaves = true;
		} else {
			new_mom = curr->leftChild;
		}
	} else {
		if (curr->rightChild) {
			new_mom = curr->rightChild;
		} else {
			new_mom = NULL;
		}
	}
	MapNode** parent_child_ptr;
	if (!parent) {
		parent_child_ptr = root_ptr;
	} else {
		if (curr == parent->rightChild) {
			parent_child_ptr = &parent->rightChild;
			if (!twoleaves)
				is_right = true;
		} else {
			parent_child_ptr = &parent->leftChild;
			if (!twoleaves)
				is_left = true;
		}
	}
	if (twoleaves) {
		if (curr->bf <= 0) {
			new_mom = curr->leftChild;
			// find most right leaf of left side
			while (new_mom->rightChild)
				new_mom = new_mom->rightChild;
			new_mom->rightChild = curr->rightChild;
			SET_PARENT(new_mom->rightChild, new_mom);
			if (PARENT(new_mom) != curr) {
				new_mom->bf = curr->bf;
				n0 = PARENT(new_mom)->rightChild = new_mom->leftChild;
				if (n0) {
					a = PARENT(new_mom);
					SET_PARENT(n0, a);
				} else {
					is_right = true;
					a = PARENT(new_mom);
				}
				new_mom->leftChild = curr->leftChild;
				SET_PARENT(new_mom->leftChild, new_mom);
			} else {
				SET_PARENT(new_mom, parent);
				new_mom->bf = curr->bf;
				is_left = true;
				a = new_mom;
			    n0 = new_mom->leftChild;
			}
		} else {
			new_mom = curr->rightChild;
			// find most left leaf of right side
			while (new_mom->leftChild)
				new_mom = new_mom->leftChild;
			new_mom->leftChild = curr->leftChild;
			SET_PARENT(new_mom->leftChild, new_mom);
			if (PARENT(new_mom) != curr) {
				new_mom->bf = curr->bf;
				n0 = PARENT(new_mom)->leftChild = new_mom->rightChild;
				if (n0) {
					a = PARENT(new_mom);
					SET_PARENT(n0, a);
				} else {
					is_left = true;
					a = PARENT(new_mom);
				}
				new_mom->rightChild = curr->rightChild;
				SET_PARENT(new_mom->rightChild, new_mom);
			} else {
				SET_PARENT(new_mom, parent);
				new_mom->bf = curr->bf;
				is_right = true;
				a = new_mom;
			    n0 = new_mom->rightChild;
			}
		}
	}
	*parent_child_ptr = new_mom;
	if (new_mom)
		SET_PARENT(new_mom, parent);
	MapNode* parent_save = NULL;
	MapNode* b = NULL;
	int bf;
	// loop from action point (leaf) to root and adjust balance factors
	for ( ; a; a = parent_save) {
		parent_save = PARENT(a);
		if (is_left || (!is_right && n0 == a->leftChild)) {
			is_left = false;
			if (a->bf <= 0) {
				if (a->bf < 0) {
					a->bf = 0;
					n0 = a;
					continue;
				}
				a->bf = 1;
				break;
			} else {
				// a->bf would become 2 -> rebalance
				b = a->rightChild;
				bf = b->bf;
				if (bf < 0)
					n0 = rotRightLeft(a, b);
				else
					n0 = rotLeft(a, b);
			}
		} else {
			is_right = false;
			if (a->bf >= 0) {
				if (a->bf > 0) {
					a->bf = 0;
					n0 = a;
					continue;
				}
				a->bf = -1;
				break;
			} else {
				// a->bf would become -2 -> rebalance
				b = a->leftChild;
				bf = b->bf;
				if (bf > 0)
					n0 = rotLeftRight(a, b);
                else
					n0 = rotRight(a, b);
			}
		}
		SET_PARENT(n0, parent_save);
		if (parent_save) {
			if (a == parent_save->leftChild)
				parent_save->leftChild = n0;
			else
				parent_save->rightChild = n0;
		} else {
			*root_ptr = n0;
		}
		if (!bf)
			break;
 	}
	free(curr);
	return true;
}

bool map_string_delete(MapNode** root_ptr, const char* key) {
	NodePosition pos = map_string_find(root_ptr, key);
	return pos.is_parent ? false : map_delete_priv(root_ptr, pos.node);
}

#define DEFINE_MAP_DELETE_FOR(typ) bool map_ ## typ ## _delete(MapNode** root_ptr, typ key) { \
	NodePosition pos = map_ ## typ ## _find(root_ptr, key); \
	return pos.is_parent ? false : map_delete_priv(root_ptr, pos.node); \
}

DEFINE_MAP_DELETE_FOR(u64)
DEFINE_MAP_DELETE_FOR(i64)
DEFINE_MAP_DELETE_FOR(u32)
DEFINE_MAP_DELETE_FOR(i32)
DEFINE_MAP_DELETE_FOR(f32)
DEFINE_MAP_DELETE_FOR(f64)

static void map_destroy_priv(MapNode* node) {
	MapNode* leftChild = node->leftChild;
	if (leftChild)
		map_destroy_priv(leftChild);
	MapNode* rightChild = node->rightChild;
	if (rightChild)
		map_destroy_priv(rightChild);
	free(node);
}

void map_destroy(MapNode* root) {
	if (root)
		map_destroy_priv(root);
}

int map_check_avl_get_depth(MapNode* node) {
	if (!node)
		return 0;
	int left = map_check_avl_get_depth(node->leftChild);
	if (left) {
		if (PARENT(node->leftChild) != node) {
			const char* parkey = PARENT(node->leftChild) ? (const char*)PARENT(node->leftChild)->key.string : "<none>";
			fprintf(stderr, "wrong parent of \"%s\"; expected \"%s\", got \"%s\"\n", (const char*)node->leftChild->key.string, (const char*)node->key.string, parkey);
		}
		if (strcmp(node->leftChild->key.string, node->key.string) >= 0) {
			fprintf(stderr, "wrong order: \"%s\" \"%s\"\n", (const char*)node->leftChild->key.string, (const char*)node->key.string);
		}

	}
	int right = map_check_avl_get_depth(node->rightChild);
	if (right) {
		if (PARENT(node->rightChild) != node) {
			const char* parkey = PARENT(node->rightChild) ? (const char*)PARENT(node->rightChild)->key.string : "<none>";
			fprintf(stderr, "wrong parent of \"%s\"; expected \"%s\", got \"%s\"\n", (const char*)node->rightChild->key.string, (const char*)node->key.string, parkey);
		}
		if (strcmp(node->key.string, node->rightChild->key.string) >= 0) {
			fprintf(stderr, "wrong order: \"%s\" \"%s\"\n", (const char*)node->key.string, (const char*)node->rightChild->key.string);
		}
	}
	int diff = right - left;
	if (node->bf != diff) {
		fprintf(stderr, "wrong balance factor for \"%s\"; expected %d, got %d\n", (const char*)node->key.string, diff, node->bf);
	}
	if (diff < -1 || diff > 1) {
		fprintf(stderr, "wrong balance factor for \"%s\" - %d out of range\n", (const char*)node->key.string, diff);
	}
	return ((right > left) ? right : left) + 1;
}

MapNode* map_min(MapNode* node) {
	if (!node) {
		return node;
	}
	while (node->leftChild)
		node = node->leftChild;
	return node;
}

MapNode* map_max(MapNode* node) {
	if (!node) {
		return node;
	}
	while (node->rightChild)
		node = node->rightChild;
	return node;
}

MapNode* map_iter_up(MapNode* elem) {
	if (elem->rightChild) {
		elem = elem->rightChild;
		while (elem->leftChild)
			elem = elem->leftChild;
	} else {
		MapNode* old_elem;
		do {
			old_elem = elem;
			elem = PARENT(elem);
		} while (elem && old_elem == elem->rightChild);
	}
	return elem;
}

MapNode* map_iter_down(MapNode* elem) {
	if (elem->leftChild) {
		elem = elem->leftChild;
		while (elem->rightChild)
			elem = elem->rightChild;
	} else {
		MapNode* old_elem;
		do {
			old_elem = elem;
			elem = PARENT(elem);
		} while (elem && old_elem == elem->leftChild);
	}
	return elem;
}

MapValue* map_string_get(MapNode* root, const char* key) {
	NodePosition pos =  map_string_find(&root, key);
	return pos.is_parent ? NULL : &pos.node->value; 
}
