#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "map.h"

#define MAP_INSERT_STRING(map, k, s) map_string_insert(&map, k, (MapValue){.src_ptr = s}, sizeof(s))

int main(int argc, char* argv[]) {
	struct timespec real_timespec;
	clock_gettime(CLOCK_REALTIME, &real_timespec);
	long long real_time = (unsigned)real_timespec.tv_nsec + 1000000000u * (unsigned)real_timespec.tv_sec;
	srand((unsigned)real_time);
	MapNode* map = map_string_new_map();
	for (int i=0; i>=0; i--) {
		MAP_INSERT_STRING(map, "eof", "uint");
		MAP_INSERT_STRING(map, "fn", "self");
		MAP_INSERT_STRING(map, "extern", "identifier");
		MAP_INSERT_STRING(map, "number", "if");
		MAP_INSERT_STRING(map, "then", "else");
		MAP_INSERT_STRING(map, "for", "in");
		MAP_INSERT_STRING(map, ":", "llo");
		MAP_INSERT_STRING(map, ";", "asd");
		MAP_INSERT_STRING(map, "u8", "eof");
		MAP_INSERT_STRING(map, "uint", "i64");
		MAP_INSERT_STRING(map, "}", "identifier");
		map_dump(map, map_prt_str_str);
	}
	char* v[11] = {
		"eof", "fn", "extern", "number", "then", "for", ":", ";", "u8", "uint", "}"
	};
	long long int sum = 0;
	for (int i = 0; i < 1; ++i) {
		for (int j=0; j<11; j++) {
			MapValue* p = map_string_get(map, v[j]);
			if (p)
				printf("%s -> %s\n", v[j], (char*)p + p->offset);
		}
	}
	int height = map_check_avl_get_depth(map);
	printf("Height: %d\n", height);
	map_dump(map, map_prt_str_str);
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
	map_dump(map, map_prt_str_str);
	for (MapNode* elem = map_max(map); elem; elem = map_iter_down(elem)) {
		printf("\"%s\"\n", (char*)elem->key.string);
	}
	do {
		int index = (int)(((long long)rand() * 11) / ((long long)RAND_MAX + 1));
		key = v[index];
		MapValue* delta_ptr = map_string_get(map, key);
		if (delta_ptr) {
			if (map_string_delete(&map, key)) {
				printf("deleted \"%s\"\n", key);
				map_dump(map, map_prt_str_str);
				height = map_check_avl_get_depth(map);
				printf("Height: %d\n", height);
			} else {
				fprintf(stderr, "big problem\n");
				exit(1);
			}
		}
	} while(map);
}
