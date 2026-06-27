/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024, 2025, 2026
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "map.h"

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
		map_string_insert(&map, "eof", (MapValue){.i32 = (int)tok_eof}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "fn", (MapValue){.i32 = (int)tok_fn}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "extern", (MapValue){.i32 = (int)tok_extern}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "identifier", (MapValue){.i32 = (int)tok_identifier}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "number", (MapValue){.i32 = (int)tok_number}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "if", (MapValue){.i32 = (int)tok_if}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "then", (MapValue){.i32 = (int)tok_then}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "else", (MapValue){.i32 = (int)tok_else}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "for", (MapValue){.i32 = (int)tok_for}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "in", (MapValue){.i32 = (int)tok_in}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "binary", (MapValue){.i32 = (int)tok_binary}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "unary", (MapValue){.i32 = (int)tok_unary}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "var", (MapValue){.i32 = (int)tok_var}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u8", (MapValue){.i32 = (int)tok_u8}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u16", (MapValue){.i32 = (int)tok_u16}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u32", (MapValue){.i32 = (int)tok_u32}, 0, false);
		// map_string_dump(map);
		// printf("----------------------------\n");
		map_string_insert(&map, "u64", (MapValue){.i32 = (int)tok_u64}, 0, false);
		map_string_insert(&map, "i8", (MapValue){.i32 = (int)tok_i8}, 0, false);
		map_string_insert(&map, "i16", (MapValue){.i32 = (int)tok_i16}, 0, false);
		map_string_insert(&map, "i32", (MapValue){.i32 = (int)tok_i32}, 0, false);
		map_string_insert(&map, "i64", (MapValue){.i32 = (int)tok_i64}, 0, false);
		map_string_insert(&map, "bool", (MapValue){.i32 = (int)tok_bool}, 0, false);
		map_string_insert(&map, "int", (MapValue){.i32 = (int)tok_int}, 0, false);
		map_string_insert(&map, "uint", (MapValue){.i32 = (int)tok_uint}, 0, false);
		map_string_insert(&map, "usize", (MapValue){.i32 = (int)tok_usize}, 0, false);
		map_string_insert(&map, "ssize", (MapValue){.i32 = (int)tok_ssize}, 0, false);
		map_string_insert(&map, "voidptr", (MapValue){.i32 = (int)tok_voidptr}, 0, false);
		map_string_insert(&map, "string", (MapValue){.i32 = (int)tok_string}, 0, false);
		map_string_insert(&map, "self", (MapValue){.i32 = (int)tok_self}, 0, false);
		map_string_insert(&map, "(", (MapValue){.i32 = (int)tok_lparen}, 0, false);
		map_string_insert(&map, ")", (MapValue){.i32 = (int)tok_rparen}, 0, false);
		map_string_insert(&map, "[", (MapValue){.i32 = (int)tok_lbrack}, 0, false);
		map_string_insert(&map, "]", (MapValue){.i32 = (int)tok_rbrack}, 0, false);
		map_string_insert(&map, "{", (MapValue){.i32 = (int)tok_lbrace}, 0, false);
		map_string_insert(&map, "}", (MapValue){.i32 = (int)tok_rbrace}, 0, false);
		map_string_insert(&map, ":", (MapValue){.i32 = (int)tok_colon}, 0, false);
		map_string_insert(&map, ";", (MapValue){.i32 = (int)tok_semicolon}, 0, false);
		map_string_insert(&map, ",", (MapValue){.i32 = (int)tok_comma}, 0, false);
		map_string_insert(&map, ".", (MapValue){.i32 = (int)tok_dot}, 0, false);
		map_string_insert(&map, " ", (MapValue){.i32 = (int)tok_space}, 0, false);
		map_string_insert(&map, "\n", (MapValue){.i32 = (int)tok_newline}, 0, false);
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
	int height = map_check_avl_get_depth(map);
	printf("Height: %d\n", height);
	map_dump(map, map_prt_str_int);
	height = map_check_avl_get_depth(map);
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
	map_dump(map, map_prt_str_int);
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
				map_dump(map, map_prt_str_int);
				height = map_check_avl_get_depth(map);
				printf("Height: %d\n", height);
			} else {
				fprintf(stderr, "big problem\n");
				exit(1);
			}
		}
	} while(map);
}
