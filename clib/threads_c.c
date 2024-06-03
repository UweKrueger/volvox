/*
 * Copyright © Uwe Krüger 2021, 2022, 2023, 2024
 * Licensed under the Apache License, Version 2.0
 * see file LICENSE or https://www.apache.org/licenses/LICENSE-2.0.txt
 */
#ifdef _WIN32
#include <processthreadsapi.h>
#define _DECL __declspec(dllexport)
#define _CDECL extern "C" __declspec(dllexport)
#ifdef _WIN64
typedef long long int ssize_t;
#else
typedef int ssize_t;
#endif
#else
#include <pthread.h>
#define _DECL
#define _CDECL extern "C"
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdbool.h>

// new thread - return handle, (-1) if detached or 0 on error
void* __create_thread(void* f, void* arg, bool detached) {
#ifdef _WIN32
	HANDLE t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)f, arg, 0, NULL);
	if (!t)
		return NULL;
	if (detached) {
		BOOL res = CloseHandle(t);
		if (!res)
			return NULL;
		else
			return (void*)(intptr_t)(-1);;
	}
	return (void*)t;
#else
	pthread_t t;
	pthread_attr_t attr;
	if (pthread_attr_init(&attr))
		return NULL;
	pthread_attr_setdetachstate(
		&attr,
		detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
	int res = pthread_create(&t, &attr, f, arg);
	pthread_attr_destroy(&attr);
	if (res)
		return NULL;
	return (void*)t;
#endif
}

#ifdef _WIN32
#define MAX_NUM_THREADS 1000
static void* __thread_return_map[MAX_NUM_THREADS] = {0};

void* __get_thread_return_adr(DWORD idx) {
	if (idx >= MAX_NUM_THREADS)
		abort();
	void* adr = __thread_return_map[idx];
	__thread_return_map[idx] = NULL;
	return adr;
}

DWORD __get_thread_return_idx(void* adr) {
	for (unsigned i=0; i<MAX_NUM_THREADS; i++) {
		if (!__thread_return_map[i]) {
			__thread_return_map[i] = adr;
			return i;
		}
	}
	abort();
}
#endif

// return 0 for failure or 32 bit return value
void* __join_thread(void* t) {
#ifdef _WIN32
	DWORD res = WaitForSingleObject((HANDLE)t, INFINITE);
	if (res == WAIT_FAILED)
		return -1;
	DWORD code;
	if (GetExitCodeThread((HANDLE)t, &code))
		return __get_thread_return_adr(code);
	return NULL;
#else
	void* val;
	int res = pthread_join((pthread_t)t, &val);
	if (res)
		return NULL;
	return val;
#endif
}

	
		
