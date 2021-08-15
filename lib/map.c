#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

typedef union MapValue {
	unsigned long long int u64;
	long long int i64;
	unsigned int u32;
	int i32;
	float f32;
	double f64;
	struct {
		unsigned int offset;
		unsigned int len;
	} string;
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

/* balace factor of nodes require only 2 bits and parent pointer is
 * aligned to 8 bytes so these two can share a 64 bit value */

#define PARENT(node) ((MapNode*)((uintptr_t)node->parent & ~0x03))
#define SET_PARENT(node, p) node->parent = (MapNode*)((uintptr_t)p | node->u_bf)

MapNode* map_string_new_map() { return NULL; }
MapNode* map_num_new_map() { return NULL; }

MapNode* map_string_new_node(char* key, MapValue value, bool value_is_string) {
	size_t keylen = strlen(key);
	size_t nodesz = keylen<8 ? sizeof(MapNode) : sizeof(MapNode)+keylen-7;
	MapNode* node = malloc(nodesz);
	strcpy(&node->key.string[0], key);
	node->value = value;
	node->parent = node->leftChild = node->rightChild = NULL;
	return node;
}

static MapNode* rotLeft(MapNode* a, MapNode* b) {
	/*      a                   b
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
	/*        a              b
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
	/*      a                   c
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
	/*          a                   c
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

static NodePosition map_string_find(MapNode** parent_ptr, char* key) {
	NodePosition pos;
	MapNode* curr;
	MapNode* parent = *parent_ptr ? (*parent_ptr)->parent : NULL;
	for(;;) {
		curr = *parent_ptr;
		if (!curr) break;
		char* k = key;
		for (char* c = curr->key.string; ; c++, k++) {
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

void map_string_insert(MapNode** root_ptr, char* key, MapValue value, bool value_is_string) {
	MapNode* node = map_string_new_node(key, value, value_is_string);
	NodePosition insert_pos = map_string_find(root_ptr, key);
	if(insert_pos.is_parent) {
		map_insert_priv(root_ptr, node, (MapNode*)((uintptr_t)insert_pos.node & ~0x01), insert_pos.parent_ptr);
	} else {
		// replace current element with new
		node->parent = insert_pos.node->parent;
		node->leftChild = insert_pos.node->leftChild;
		node->rightChild = insert_pos.node->rightChild;
		free(insert_pos.node);
	}
	return;
}

void map_string_dump_priv(MapNode* curr, char* indent, bool is_right) {
	if (!curr) return;
	int cur_len = strlen(indent);
	if (curr->leftChild) {
		if (is_right) {
			strncat(indent, "┃", 255-cur_len);
		} else {
			strncat(indent, " ", 255-cur_len);
		}
		map_string_dump_priv(curr->leftChild, indent, false);
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
	fputs(" \"", stdout);
	fputs(curr->key.string, stdout);
	printf("\": %d %d\n", curr->bf, curr->value.i32);
	if (curr->rightChild) {
		if (is_right) {
			strncat(indent, " ", 255-cur_len);
		} else {
			strncat(indent, "┃", 255-cur_len);
		}
		map_string_dump_priv(curr->rightChild, indent, true);
	}
}

void map_string_dump(MapNode* root) {
	char buf[256] = "";
	map_string_dump_priv(root, buf, false);
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

bool map_string_delete(MapNode** root_ptr, char* key) {
	MapNode* curr = *root_ptr;
	for(;;) {
		if (!curr) return false;
		char* k = key;
		for (char* c = curr->key.string; ; c++, k++) {
			if (!*c) {
				if(!*k) {
					goto found_node;
				} else {
					curr = curr->rightChild;
					break;
				}
			}
			if (*k < *c) {
				curr = curr->leftChild;
				break;
			} else if (*k > *c) {
				curr = curr->rightChild;
				break;
			}
		}
	}
found_node:
	return map_delete_priv(root_ptr, curr);
}

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

int map_check_avl(MapNode* node) {
	if (!node)
		return 0;
	int left = map_check_avl(node->leftChild);
	if (left) {
		if (PARENT(node->leftChild) != node) {
			char* parkey = PARENT(node->leftChild) ? (char*)PARENT(node->leftChild)->key.string : "<none>";
			fprintf(stderr, "wrong parent of \"%s\"; expected \"%s\", got \"%s\"\n", (char*)node->leftChild->key.string, (char*)node->key.string, parkey);
		}
		if (strcmp(node->leftChild->key.string, node->key.string) >= 0) {
			fprintf(stderr, "wrong order: \"%s\" \"%s\"\n", (char*)node->leftChild->key.string, (char*)node->key.string);
		}

	}
	int right = map_check_avl(node->rightChild);
	if (right) {
		if (PARENT(node->rightChild) != node) {
			char* parkey = PARENT(node->rightChild) ? (char*)PARENT(node->rightChild)->key.string : "<none>";
			fprintf(stderr, "wrong parent of \"%s\"; expected \"%s\", got \"%s\"\n", (char*)node->rightChild->key.string, (char*)node->key.string, parkey);
		}
		if (strcmp(node->key.string, node->rightChild->key.string) >= 0) {
			fprintf(stderr, "wrong order: \"%s\" \"%s\"\n", (char*)node->key.string, (char*)node->rightChild->key.string);
		}
	}
	int diff = right - left;
	if (node->bf != diff) {
		fprintf(stderr, "wrong balance factor for \"%s\"; expected %d, got %d\n", (char*)node->key.string, diff, node->bf);
	}
	if (diff < -1 || diff > 1) {
		fprintf(stderr, "wrong balance factor for \"%s\" - %d out of range\n", (char*)node->key.string, diff);
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

enum Token {
	tok_eof,
	// commands,
	tok_fn,
	tok_extern,
	// primary,
	tok_identifier,
	tok_number,
	// control,
	tok_if,
	tok_then,
	tok_else,
	tok_for,
	tok_in,
	// operators,
	tok_binary,
	tok_unary,
	// var definition,
	tok_var,
	// built-in types,
	tok_u8,
	tok_u16,
	tok_u32,
	tok_u64,
	tok_i8,
	tok_i16,
	tok_i32,
	tok_i64,
	tok_bool,
	tok_uint,
	tok_int,
	tok_usize,
	tok_ssize,
	tok_voidptr,
	tok_string,
	tok_self,
	// braces,
	tok_lparen,
	tok_rparen,
	tok_lbrack,
	tok_rbrack,
	tok_lbrace,
	tok_rbrace,
	tok_colon,
	tok_semicolon,
	tok_comma,
	tok_dot,
	tok_space,
	tok_newline,
};

MapValue* map_string_get(MapNode* root, char* key) {
	NodePosition pos =  map_string_find(&root, key);
	return pos.is_parent ? NULL : &pos.node->value; 
}

int main(int argc, char* argv[]) {
	struct timespec real_timespec;
	clock_gettime(CLOCK_REALTIME, &real_timespec);
	long long real_time = (unsigned)real_timespec.tv_nsec + 1000000000u * (unsigned)real_timespec.tv_sec;
	srand((unsigned)real_time);
	MapNode* map;
	for (int i=0; i>=0; i--) {
		map = map_string_new_map();
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "eof", (MapValue){.i32 = (int)tok_eof}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "fn", (MapValue){.i32 = (int)tok_fn}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "extern", (MapValue){.i32 = (int)tok_extern}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "identifier", (MapValue){.i32 = (int)tok_identifier}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "number", (MapValue){.i32 = (int)tok_number}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "if", (MapValue){.i32 = (int)tok_if}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "then", (MapValue){.i32 = (int)tok_then}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "else", (MapValue){.i32 = (int)tok_else}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "for", (MapValue){.i32 = (int)tok_for}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "in", (MapValue){.i32 = (int)tok_in}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "binary", (MapValue){.i32 = (int)tok_binary}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "unary", (MapValue){.i32 = (int)tok_unary}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "var", (MapValue){.i32 = (int)tok_var}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u8", (MapValue){.i32 = (int)tok_u8}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u16", (MapValue){.i32 = (int)tok_u16}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u32", (MapValue){.i32 = (int)tok_u32}, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u64", (MapValue){.i32 = (int)tok_u64}, false);
		map_string_insert(&map, "i8", (MapValue){.i32 = (int)tok_i8}, false);
		map_string_insert(&map, "i16", (MapValue){.i32 = (int)tok_i16}, false);
		map_string_insert(&map, "i32", (MapValue){.i32 = (int)tok_i32}, false);
		map_string_insert(&map, "i64", (MapValue){.i32 = (int)tok_i64}, false);
		map_string_insert(&map, "bool", (MapValue){.i32 = (int)tok_bool}, false);
		map_string_insert(&map, "int", (MapValue){.i32 = (int)tok_int}, false);
		map_string_insert(&map, "uint", (MapValue){.i32 = (int)tok_uint}, false);
		map_string_insert(&map, "usize", (MapValue){.i32 = (int)tok_usize}, false);
		map_string_insert(&map, "ssize", (MapValue){.i32 = (int)tok_ssize}, false);
		map_string_insert(&map, "voidptr", (MapValue){.i32 = (int)tok_voidptr}, false);
		map_string_insert(&map, "string", (MapValue){.i32 = (int)tok_string}, false);
		map_string_insert(&map, "self", (MapValue){.i32 = (int)tok_self}, false);
		map_string_insert(&map, "(", (MapValue){.i32 = (int)tok_lparen}, false);
		map_string_insert(&map, ")", (MapValue){.i32 = (int)tok_rparen}, false);
		map_string_insert(&map, "[", (MapValue){.i32 = (int)tok_lbrack}, false);
		map_string_insert(&map, "]", (MapValue){.i32 = (int)tok_rbrack}, false);
		map_string_insert(&map, "{", (MapValue){.i32 = (int)tok_lbrace}, false);
		map_string_insert(&map, "}", (MapValue){.i32 = (int)tok_rbrace}, false);
		map_string_insert(&map, ":", (MapValue){.i32 = (int)tok_colon}, false);
		map_string_insert(&map, ";", (MapValue){.i32 = (int)tok_semicolon}, false);
		map_string_insert(&map, ",", (MapValue){.i32 = (int)tok_comma}, false);
		map_string_insert(&map, ".", (MapValue){.i32 = (int)tok_dot}, false);
		map_string_insert(&map, " ", (MapValue){.i32 = (int)tok_space}, false);
		map_string_insert(&map, "\n", (MapValue){.i32 = (int)tok_newline}, false);
		if (i) map_destroy(map);
	}
	char* v[41] = {
		"eof", "fn", "extern", "identifier", "number", "if", "then", "else", "for", "in",
		"binary", "unary", "var", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "bool",
		"int", "uint", "usize", "ssize", "voidptr", "string", "self", "(", ")", "[", "]", "{",
		"}", ":", ";", ",", ".", " ", "\n"
	};
	long long int sum = 0;
	for (int i = 0; i < 10000000; ++i) {
		for (int j=0; j<41; j++) {
			MapValue* p = map_string_get(map, v[j]);
			if (p)
				sum += p->i32;
		}
	}
	fprintf(stderr, "sum: %lld\n", sum);
	if (argc == 2 && argv[1][0] == 'x')
		exit(0);
	int height = map_check_avl(map);
	printf("Height: %d\n", height);
	map_string_dump(map);
	height = map_check_avl(map);
	printf("Height: %d\n", height);
	char* key;
	if (argc > 1) {
		key = argv[1];
	} else {
		int index = (int)(((long long)rand() * 41) / ((long long)RAND_MAX + 1));
		key = v[index];
	}
	MapValue* delta = map_string_get(map, key);
	char* res = sum == 820 - delta->i32 ? "good" : "bad";
	fprintf(stderr, "delete \"%s\"\n", key);
	if (map_string_delete(&map, key)) {
		printf("deleted \"%s\"\n", key);
	}
	MapValue* k = map_string_get(map, key);
	if (k) {
		fprintf(stderr, "found %d\n", k->i32);
	} else {
		fprintf(stderr, "not found\n");
	}
	map_string_dump(map);
	for (MapNode* elem = map_max(map); elem; elem = map_iter_down(elem)) {
		printf("\"%s\"\n", (char*)elem->key.string);
	}
	do {
		int index = (int)(((long long)rand() * 41) / ((long long)RAND_MAX + 1));
		key = v[index];
		MapValue* delta_ptr = map_string_get(map, key);
		if (delta_ptr) {
			if (map_string_delete(&map, key)) {
				printf("deleted \"%s\"\n", key);
				map_string_dump(map);
				height = map_check_avl(map);
				printf("Height: %d\n", height);
			} else {
				fprintf(stderr, "big problem\n");
				exit(1);
			}
		}
	} while(map);
}
