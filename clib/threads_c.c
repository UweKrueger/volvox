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
#endif
#include <stdbool.h>

ssize_t __create_thread(void* f, void* arg, bool detached) {
#ifdef _WIN32
	HANDLE t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)f, arg, 0, NULL);
	if (!t)
		return -1;
	if (detached) {
		BOOL res = CloseHandle(t);
		if (!res)
			return -1;
		else
			return 0;
	}
	return (ssize_t)t;
#else
	pthread_t t;
	pthread_attr_t attr;
	if (pthread_attr_init(&attr))
		return -1;
	pthread_attr_setdetachstate(
		&attr,
		detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
	int res = pthread_create(&t, &attr, f, arg);
	pthread_attr_destroy(&attr);
	if (res)
		return -1;
	return (ssize_t)t;
#endif
}


// return <0 for failure or 32 bit return value
ssize_t __join_thread(ssize_t t) {
#ifdef _WIN32
	DWORD res = WaitForSingleObject((HANDLE)t, INFINITE);
	if (res == WAIT_FAILED)
		return -1;
	DWORD code;
	if (GetExitCodeThread((HANDLE)t, &code))
		return (ssize_t)(size_t)code;
	return -1;
#else
	void* val;
	int res = pthread_join((pthread_t)t, &val);
	if (res)
		return -res;
	return (ssize_t)((uintptr_t)val & ~1U);
#endif
}

	
		
