#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "map.h"

#define MAP_INSERT_STRING(map, k, s, t) map_string_tag_insert(&map, k, t, (MapValue){.src_ptr = s}, sizeof(s)+1, true)

int main(int argc, char* argv[]) {
	struct timespec real_timespec;
	clock_gettime(CLOCK_REALTIME, &real_timespec);
	long long real_time = (unsigned)real_timespec.tv_nsec + 1000000000u * (unsigned)real_timespec.tv_sec;
	srand((unsigned)real_time);
	MapNode* map = map_string_new_map();
	unsigned t = 0;
	for (int i=0; i>=0; i--) {
		MAP_INSERT_STRING(map, "eof", "uint", ++t);
		MAP_INSERT_STRING(map, "fn", "self", ++t);
		MAP_INSERT_STRING(map, "extern", "identifier", ++t);
		MAP_INSERT_STRING(map, "number", "if", ++t);
		MAP_INSERT_STRING(map, "then", "else", ++t);
		MAP_INSERT_STRING(map, "for", "in", ++t);
		MAP_INSERT_STRING(map, ":", "llo", ++t);
		MAP_INSERT_STRING(map, ";", "asd", ++t);
		MAP_INSERT_STRING(map, "undo", "x", ++t);
		MAP_INSERT_STRING(map, "u8", "eof", ++t);
		MAP_INSERT_STRING(map, "uint", "i64", ++t);
		MAP_INSERT_STRING(map, "}", "identifier", ++t);
		map_dump(map, map_prt_str_tag);
	}
	char* v[12] = {
		"eof", "fn", "extern", "number", "then", "undo", "for", ":", ";", "u8", "uint", "}"
	};
	long long int sum = 0;
	for (int i = 0; i < 1; ++i) {
		for (int j=0; j<12; j++) {
			MapValue* p = map_string_get(map, v[j]);
			if (p)
				printf("%s -> %u, %s\n", v[j], *(unsigned*)((char*)p + p->offset), (char*)p + p->offset + 4);
		}
	}
	int height = map_check_avl_get_depth(map);
	printf("Height: %d\n", height);
	map_dump(map, map_prt_str_tag);
	height = map_check_avl_get_depth(map);
	printf("Height: %d\n", height);
	char* key;
	if (argc > 1) {
		key = argv[1];
	} else {
		int index = (int)(((long long)rand() * 11) / ((long long)RAND_MAX + 1));
		key = v[index];
	}
	fprintf(stderr, "delete \"%s\"\n", key);
	if (map_string_delete(&map, key)) {
		printf("deleted \"%s\"\n", key);
	}
	map_dump(map, map_prt_str_tag);
	for (MapNode* elem = map_max(map); elem; elem = map_iter_down(elem)) {
		printf("\"%s\"\n", (char*)elem->key.string);
	}
	do {
		int index = (int)(((long long)rand() * 12) / ((long long)RAND_MAX + 1));
		key = v[index];
		MapValue* delta_ptr = map_string_get(map, key);
		if (delta_ptr) {
			if (map_string_delete(&map, key)) {
				printf("deleted \"%s\"\n", key);
				map_dump(map, map_prt_str_tag);
				height = map_check_avl_get_depth(map);
				printf("Height: %d\n", height);
			} else {
				fprintf(stderr, "big problem\n");
				exit(1);
			}
		}
	} while(map);
}
