/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#if defined(WNATIVELIB)
#include <winstub.h>
#endif
#if !defined(_WIN32) || defined(__MINGW32__) && !defined(WNATIVELIB)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "map.h"

namespace volvox {

	namespace map {

/* balace factor of nodes require only 2 bits and parent pointer is
 * aligned to 8 bytes so these two can share a 64 bit value */

#define PARENT(node) ((Node*)((uintptr_t)node->parent & ~(uintptr_t)0x03))
#define SET_PARENT(node, p) node->parent = (Node*)((uintptr_t)p | (uintptr_t)node->u_bf)

		_DECL Node* string_new_map() { return NULL; }
		_DECL Node* num_new_map() { return NULL; }

// value_size: length including 0 when string values

		static Node* string_tag_new_node(const char* key, unsigned tag, Value value, unsigned int value_size, bool use_tag) {
			unsigned keylen = (unsigned)strlen(key) + 1;
			size_t nodesz;
			if (use_tag) {
				if (value_size >= 4) {
					keylen = ((keylen + 7) >> 3) << 3;
				}
				nodesz = keylen + value_size <= 4 ? sizeof(Node) : sizeof(Node) + keylen - 4 + value_size;
			} else {
				if (value_size >= 8) {
					keylen = ((keylen + 7) >> 3) << 3;
				} else if (value_size >= 4) {
					keylen = ((keylen + 3) >> 2) << 2;
				} else if (value_size >= 2) {
					keylen = ((keylen + 1) >> 1) << 1;
				}
				nodesz = keylen + value_size <= 8 ? sizeof(Node) : sizeof(Node) + keylen - 8 + value_size;
			}
			Node* node = (Node*)malloc(nodesz);
			memcpy(&node->key.string[0], key, keylen);
			char* val_ptr = &node->key.string[0] + keylen;
			if (use_tag) {
				memcpy(val_ptr, &tag, 4);
				if (value_size) {
					memcpy(val_ptr + 4, value.src_ptr, value_size);
				}
				node->value.offset = val_ptr - (char*)&node->value;
				node->value.size = value_size + 4;
			} else {
				if (value_size) {
					memcpy(val_ptr, value.src_ptr, value_size);
					node->value.offset = val_ptr - (char*)&node->value;
					node->value.size = value_size;
				} else {
					node->value = value;
				}
			}
			node->parent = node->leftChild = node->rightChild = NULL;
			return node;
		}

#define DEFINE_NEW_NODE_FOR(typ) static Node* typ ## _new_node(typ key, Value value, unsigned int value_size) { \
			size_t nodesz = sizeof(Node) + value_size; \
			Node* node = (Node*)malloc(nodesz); \
			node->key.typ = key; \
			if (value_size) { \
				memcpy((char*)node + sizeof(Node), value.src_ptr, value_size); \
				node->value.offset = sizeof(Key) + sizeof(Value); \
				node->value.size = value_size; \
			} else { \
				node->value = value; \
			} \
			node->parent = node->leftChild = node->rightChild = NULL; \
			return node; \
		}

		DEFINE_NEW_NODE_FOR(u64)
		DEFINE_NEW_NODE_FOR(i64)
		DEFINE_NEW_NODE_FOR(u32)
		DEFINE_NEW_NODE_FOR(i32)
		DEFINE_NEW_NODE_FOR(f32)
		DEFINE_NEW_NODE_FOR(f64)

		static Node* rotLeft(Node* a, Node* c) {
			/*
			 *      a                   c
			 *     / \                 / \
			 *    X   c      =>       a   Y
			 *       / \             / \
			 *      b   Y           X   b
			 */	  
			Node* b = c->leftChild;
			a->rightChild = b;
			if (b)
				SET_PARENT(b, a);
			c->leftChild = a;
			SET_PARENT(a, c);
			if (c->bf == 0) {
				a->bf = 1;
				c->bf = -1;
			} else {
				a->bf = 0;
				c->bf = 0;
			}
			return c;
		}

		static Node* rotRight(Node* c, Node* a) {
			/*
			 *        c              a
			 *       / \            / \
			 *      a   Y    =>    X   c
			 *     / \                / \
			 *    X   b              b   Y
			 */	  
			Node* b = a->rightChild;
			c->leftChild = b;
			if (b)
				SET_PARENT(b, c);
			a->rightChild = c;
			SET_PARENT(c, a);
			if (a->bf == 0) {
				c->bf = -1;
				a->bf = 1;
			} else {
				c->bf = 0;
				a->bf = 0;
			}
			return a;
		}

		static Node* rotRightLeft(Node* a, Node* e) {
			/*
			 *      a                   c
			 *     / \                 / \
			 *    X   e               /   \
			 *       / \     =>      /     \
			 *      c   Y           a       e
			 *     / \             / \     / \
			 *    b   d           X   b   d   Y
			 */	  
			Node* c = e->leftChild;
			Node* b = c->leftChild;
			Node* d = c->rightChild;
			c->leftChild = a;
			SET_PARENT(a, c);
			a->rightChild = b;
			if (b)
				SET_PARENT(b, a);
			c->rightChild = e;
			SET_PARENT(e, c);
			e->leftChild = d;
			if (d)
				SET_PARENT(d, e);
			if (c->bf == 0) {
				a->bf = 0;
				e->bf = 0;
			} else {
				if (c->bf < 0) {
					a->bf = 0;
					e->bf = 1;
				} else {
					a->bf = -1;
					e->bf = 0;
				}
			}
			c->bf = 0;
			return c;
		}

		static Node* rotLeftRight(Node* e, Node* a) {
			/*
			 *          e                   c
			 *         / \                 / \
			 *        a   Y               /   \
			 *       / \         =>      /     \
			 *      X   c               a       e
			 *         / \             / \     / \
			 *        b   d           X   b   d   Y
			 */	  
			Node* c = a->rightChild;
			Node* d = c->rightChild;
			Node* b = c->leftChild;
			c->rightChild = e;
			SET_PARENT(e, c);
			e->leftChild = d;
			if (d)
				SET_PARENT(d, e);
			c->leftChild = a;
			SET_PARENT(a, c);
			a->rightChild = b;
			if (b)
				SET_PARENT(b, a);
			if (c->bf == 0) {
				e->bf = 0;
				a->bf = 0;
			} else {
				if (c->bf > 0) {
					e->bf = 0;
					a->bf = -1;
				} else {
					e->bf = 1;
					a->bf = 0;
				}
			}
			c->bf = 0;
			return c;
		}

		static void insert_priv(Node** root_ptr, Node* node, Node* parent, Node** parent_ptr) {
			// insert node
			SET_PARENT(node, parent);
			*parent_ptr = node;
			Node* parent_save;
			Node* n0;
			// loop from leaf to root and adjust balance factors
			for (Node* a = parent; a; a = PARENT(node)) {
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

		static NodePosition string_find(Node** parent_ptr, const char* key) {
			NodePosition pos;
			Node* curr;
			Node* parent = *parent_ptr ? (*parent_ptr)->parent : NULL;
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

#define DEFINE_FIND_FOR(typ) static NodePosition typ ## _find(Node** parent_ptr, typ key) { \
			NodePosition pos; \
			Node* curr; \
			Node* parent = *parent_ptr ? (*parent_ptr)->parent : NULL; \
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

		DEFINE_FIND_FOR(u64)
		DEFINE_FIND_FOR(i64)
		DEFINE_FIND_FOR(u32)
		DEFINE_FIND_FOR(i32)
		DEFINE_FIND_FOR(f32)
		DEFINE_FIND_FOR(f64)

		/* inserting consists of two parts, each of which might be expensive:
		 * 1. find an insert position
		 * 2. insert new node and rebalance the tree if necessary
		 *
		 * There are two use cases: replacement of existing node is allowed or not - that's what 'replace' stands for.
		 * On return 'replace' indicates that the key had existed. The following table
		 * lists return values (explicit return and new value of 'replace') for all
		 * 4 conditions (passed value of 'replace' and if 'key' exists):
		 *
		 * ┏━━━━━━━━━━━━━━━━━┳━━━━━━━━━━━━━━━━━━━━┯━━━━━━━━━━━━━━━━━━┓
		 * ┃                 ┃ key does not exist │ key exists       ┃
		 * ┣━━━━━━━━━━━━━━━━━╋━━━━━━━━━━━━━━━━━━━━┿━━━━━━━━━━━━━━━━━━┫
		 * ┃ replace=nullptr ┃ newNode, nullptr   │ oldNode, (-1)    ┃
		 * ┠─────────────────╂────────────────────┼──────────────────┨
		 * ┃ replace=target  ┃ newNode, nullptr   │ newNode, oldNode ┃
		 * ┗━━━━━━━━━━━━━━━━━┻━━━━━━━━━━━━━━━━━━━━┷━━━━━━━━━━━━━━━━━━┛
		 */
		_DECL Node* string_tag_insert(Node** root_ptr, const char* key, unsigned tag, Value value, int value_size, Node** target) {
			bool use_tag = false;
			if ((uintptr_t)root_ptr & (uintptr_t)0x01)
				root_ptr = (Node**)((uintptr_t)root_ptr & ~(uintptr_t)0x01);
			else
				use_tag = true;
			NodePosition insert_pos = string_find(root_ptr, key);
			Node* insert_node = (Node*)((uintptr_t)insert_pos.node & ~(uintptr_t)0x01);
			if(insert_pos.is_parent || *target) {
				Node* node = string_tag_new_node(key, tag, value, value_size, use_tag);
				insert_priv(root_ptr, node, insert_node, insert_pos.parent_ptr);
				if (insert_pos.is_parent) {
					*target = nullptr;
					// fprintf(stderr, "### key '%s' is parent - node: %p\n", key, node);
				} else { // target was set
					// replace current element with new
					node->parent = insert_pos.node->parent;
					node->leftChild = insert_pos.node->leftChild;
					node->rightChild = insert_pos.node->rightChild;
					// return replaced element as new map
					insert_pos.node->parent = NULL;
					insert_pos.node->leftChild = NULL;
					insert_pos.node->rightChild = NULL;
					*target = insert_pos.node;
					// fprintf(stderr, "### key '%s' replaced - node: %p %p\n", key, node, *target);
				}
				return node;
			} else {
				*target = (Node*)(intptr_t)(-1); // indicate that key already exists
				return insert_node;
			}
		}

		_DECL Node* string_insert(Node** root_ptr, const char* key, Value value, int value_size, Node** target) {
			return string_tag_insert((Node**)((uintptr_t)root_ptr | (uintptr_t)0x01), key, 0, value, value_size, target);
		}

		_DECL Node* volvoxstring_insert(Node** root_ptr, const char* key, Value value, int value_size, Node** target) {
			return string_tag_insert((Node**)((uintptr_t)root_ptr | (uintptr_t)0x01), volvox2cstr(key), 0, value, value_size, target);
		}

#define DEFINE_INSERT_FOR(typ) _DECL Node* typ ## _insert(Node** root_ptr, typ key, Value value, int value_size, Node** target) { \
			NodePosition insert_pos = typ ## _find(root_ptr, key); \
			Node* insert_node = (Node*)((uintptr_t)insert_pos.node & ~(uintptr_t)0x01); \
			if(insert_pos.is_parent || target) { \
				Node* node = typ ## _new_node(key, value, value_size); \
				insert_priv(root_ptr, node, insert_node, insert_pos.parent_ptr); \
				if (insert_pos.is_parent) { \
					*target = nullptr; \
				} else { \
					node->parent = insert_pos.node->parent; \
					node->leftChild = insert_pos.node->leftChild; \
					node->rightChild = insert_pos.node->rightChild; \
					insert_pos.node->parent = NULL; \
					insert_pos.node->leftChild = NULL; \
					insert_pos.node->rightChild = NULL; \
					*target = insert_pos.node; \
				} \
				return node; \
			} else { \
				*target = (Node*)(intptr_t)(-1); \
				return insert_node; \
			} \
		}

		DEFINE_INSERT_FOR(u64)
		DEFINE_INSERT_FOR(i64)
		DEFINE_INSERT_FOR(u32)
		DEFINE_INSERT_FOR(i32)
		DEFINE_INSERT_FOR(f32)
		DEFINE_INSERT_FOR(f64)

		_DECL void dump_priv(Node* curr, char* indent, bool is_right, node_printer* prt) {
			if (!curr) return;
			int cur_len = strlen(indent);
			if (curr->leftChild) {
				if (is_right) {
					strncat(indent, "┃", 255-cur_len);
				} else {
					strncat(indent, " ", 255-cur_len);
				}
				dump_priv(curr->leftChild, indent, false, prt);
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
				dump_priv(curr->rightChild, indent, true, prt);
			}
		}

		_DECL void dump(Node* root, node_printer* prt) {
			char buf[256] = "";
			dump_priv(root, buf, false, prt);
		}

		_DECL void prt_str_int(int bf, Key* key, Value* value) {
			fputs(" \"", stdout);
			fputs(key->string, stdout);
			printf("\": %d %d\n", bf, value->i32);
		}

		_DECL void prt_str_str(int bf, Key* key, Value* value) {
			fputs(" \"", stdout);
			fputs(key->string, stdout);
			printf("\": %d \"%s\"\n", bf, (const char*)value + value->offset);
		}

		_DECL void prt_str_tag(int bf, Key* key, Value* value) {
			fputs(" \"", stdout);
			fputs(key->string, stdout);
			printf("\": %d %u \"%s\"\n", bf, *(unsigned*)((const char*)value + value->offset), (const char*)value + value->offset + 4);
		}

		static bool delete_priv(Node** root_ptr, Node* curr, void (*destruct)(Value* ptr)) {
			Node* new_mom;
			Node* parent = PARENT(curr);
			Node* n0 = curr;
			Node* a = parent;
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
			Node** parent_child_ptr;
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
			Node* parent_save = NULL;
			Node* b = NULL;
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
			if (destruct)
				destruct(&curr->value);
			free(curr);
			return true;
		}

		_DECL bool string_delete(Node** root_ptr, const char* key, void (*destruct)(Value* ptr)) {
			NodePosition pos = string_find(root_ptr, key);
			return pos.is_parent ? false : delete_priv(root_ptr, pos.node, destruct);
		}

		_DECL bool volvoxstring_delete(Node** root_ptr, const char* key, void (*destruct)(Value* ptr)) {
			NodePosition pos = string_find(root_ptr, volvox2cstr(key));
			return pos.is_parent ? false : delete_priv(root_ptr, pos.node, destruct);
		}

#define DEFINE_DELETE_FOR(typ) _DECL bool typ ## _delete(Node** root_ptr, typ key, void (*destruct)(Value* ptr)) { \
			NodePosition pos = typ ## _find(root_ptr, key); \
			return pos.is_parent ? false : delete_priv(root_ptr, pos.node, destruct); \
		}

		DEFINE_DELETE_FOR(u64)
		DEFINE_DELETE_FOR(i64)
		DEFINE_DELETE_FOR(u32)
		DEFINE_DELETE_FOR(i32)
		DEFINE_DELETE_FOR(f32)
		DEFINE_DELETE_FOR(f64)

		static void destroy_priv(Node* node, void (*destruct)(Value* ptr)) {
			Node* leftChild = node->leftChild;
			if (leftChild)
				destroy_priv(leftChild, destruct);
			Node* rightChild = node->rightChild;
			if (rightChild)
				destroy_priv(rightChild, destruct);
			if (destruct)
				destruct(&node->value);
			free(node);
		}

		_DECL void destroy(Node* root, void (*destruct)(Value* ptr)) {
			if (root)
				destroy_priv(root, destruct);
		}

		_DECL int check_avl_get_depth(Node* node) {
			if (!node)
				return 0;
			int left = check_avl_get_depth(node->leftChild);
			if (left) {
				if (PARENT(node->leftChild) != node) {
					const char* parkey = PARENT(node->leftChild) ? (const char*)PARENT(node->leftChild)->key.string : "<none>";
					fprintf(stderr, "wrong parent of \"%s\"; expected \"%s\", got \"%s\"\n", (const char*)node->leftChild->key.string, (const char*)node->key.string, parkey);
				}
				if (strcmp(node->leftChild->key.string, node->key.string) >= 0) {
					fprintf(stderr, "wrong order: \"%s\" \"%s\"\n", (const char*)node->leftChild->key.string, (const char*)node->key.string);
				}

			}
			int right = check_avl_get_depth(node->rightChild);
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

		_DECL Node* Min(Node* node) {
			if (!node) {
				return node;
			}
			while (node->leftChild)
				node = node->leftChild;
			return node;
		}

		_DECL Node* Max(Node* node) {
			if (!node) {
				return node;
			}
			while (node->rightChild)
				node = node->rightChild;
			return node;
		}

		_DECL Node* iter_up(Node* elem) {
			if (elem->rightChild) {
				elem = elem->rightChild;
				while (elem->leftChild)
					elem = elem->leftChild;
			} else {
				Node* old_elem;
				do {
					old_elem = elem;
					elem = PARENT(elem);
				} while (elem && old_elem == elem->rightChild);
			}
			return elem;
		}

		_DECL Node* iter_down(Node* elem) {
			if (elem->leftChild) {
				elem = elem->leftChild;
				while (elem->rightChild)
					elem = elem->rightChild;
			} else {
				Node* old_elem;
				do {
					old_elem = elem;
					elem = PARENT(elem);
				} while (elem && old_elem == elem->leftChild);
			}
			return elem;
		}

		_DECL Value* string_get(Node* root, const char* key) {
			NodePosition pos =  string_find(&root, key);
			return pos.is_parent ? NULL : &pos.node->value; 
		}

		_DECL Value* volvoxstring_get(Node* root, const char* key) {
			NodePosition pos =  string_find(&root, volvox2cstr(key));
			return pos.is_parent ? NULL : &pos.node->value; 
		}
	}

}
