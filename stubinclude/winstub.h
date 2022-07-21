#pragma once

/* On Windows most of libvolvox.dll is compiled with "clang++ --target x86_64-pc-windows-gnu"
 * to get Itanium/GNU mangling for function symbols. Unfortunately Clang cannot
 * read MS Windows-SDK/Visual Studio header files with this setting.
 * This stub file contains some of the most needed definitions to work around
 * this limitation */

#include <stdint.h>

extern "C" void* calloc(size_t nelem, size_t obj_size);
extern "C" void* realloc(void* ptr, size_t new_size);
extern "C" void free(void* ptr);
extern "C" void* malloc(size_t sz);
extern "C" void* memcpy(void* dest, const void* src, size_t sz);
extern "C" void* memset(void* dest, int val, size_t sz);
extern "C" int abs(int x);
extern "C" int raise(int sig);
extern "C" int _write(int fd, const void* buf, unsigned int count); // on Linux it's size_t!
#define write(fd, buf, count) _write(fd, buf, count)
extern "C" int _read(int fd, void* buf, unsigned int count);
#define read(fd, buf, count) _read(fd, buf, count)

typedef struct _iobuf { void* _Placeholder; } FILE;
extern "C" int fprintf(FILE* f , const char* fmt, ...);
extern "C" int fputs(const char* buf, FILE* f);
#define printf(...) fprintf(stdout, __VA_ARGS__)

extern "C" char* strcpy(char* dest, const char* src);
extern "C" char* strncpy(char* dest, const char* src, size_t count);
extern "C" int strcmp(const char* s1, const char* s2);
extern "C" size_t strlen(const char* s);
extern "C" char* strncat(char *dest, const char* src, size_t count);
extern "C" void abort(void);

#ifndef NULL
#define NULL 0ULL
#endif

typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef uint32_t DWORD;
typedef void* LPVOID;
typedef int BOOL;
typedef short SHORT;
typedef uint16_t WORD;
typedef WORD* LPWORD;
typedef DWORD* LPDWORD;
typedef wchar_t WCHAR;
typedef char CHAR;
typedef unsigned char BYTE;
typedef BYTE* LPBYTE;
typedef BYTE* PBYTE;
typedef const CHAR* LPCSTR;
typedef const CHAR* LPSTR;

#define INVALID_HANDLE_VALUE ((HANDLE)(int64_t)-1)

extern "C" intptr_t _get_osfhandle(int fd);
extern "C" int _open_osfhandle(intptr_t h, int flags);
extern "C" int _isatty(int fd);
#define isatty(fd)  _isatty(fd)

extern "C" FILE* __acrt_iob_func(unsigned i);
#define stdin (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
extern "C" int* _errno(void);
#define errno (*_errno())

extern "C" int _dup(int fd);
extern "C" int _dup2(int srcfd, int destfd);

#define ENOTTY 25
#define E2BIG 7
#define EINVAL 22
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#define MAX_PATH 260
#define HANDLE_FLAG_INHERIT 0x00000001

typedef struct _SECURITY_ATTRIBUTES {
	DWORD nLength;
	LPVOID lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _COORD {
	SHORT X;
	SHORT Y;
} COORD;

typedef struct _SMALL_RECT {
	SHORT Left;
	SHORT Top;
	SHORT Right;
	SHORT Bottom;
} SMALL_RECT;

typedef struct _CONSOLE_SCREEN_BUFFER_INFO {
	COORD dwSize;
	COORD dwCursorPosition;
	WORD  wAttributes;
	SMALL_RECT srWindow;
	COORD dwMaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO, *PCONSOLE_SCREEN_BUFFER_INFO;

extern "C" BOOL __stdcall GetConsoleMode(HANDLE h, LPDWORD mode);

extern "C" BOOL __stdcall SetConsoleMode(HANDLE h, DWORD mode);

extern "C" BOOL __stdcall GetConsoleScreenBufferInfo(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info);

typedef struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME;

typedef struct _WIN32_FIND_DATAA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	CHAR cFileName[MAX_PATH];
	CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef WIN32_FIND_DATAA WIN32_FIND_DATA;

#define FILE_ATTRIBUTE_DIRECTORY 0x00000010

extern "C" HANDLE __stdcall FindFirstFileA(
	LPCSTR lpFileName,
	LPWIN32_FIND_DATAA lpFindFileData
	);

#define FindFirstFile(a, b) FindFirstFileA(a, b)

extern "C" BOOL __stdcall FindNextFileA(
	HANDLE hFindFile,
	LPWIN32_FIND_DATAA lpFindFileData
	);

#define FindNextFile(h, d) FindNextFileA(h, d)

extern "C" BOOL __stdcall FindClose(
	HANDLE hFindFile
	);

extern "C" BOOL __stdcall CreatePipe(
	PHANDLE hReadPipe,
	PHANDLE hWritePipe,
	LPSECURITY_ATTRIBUTES lpPipeAttributes,
	DWORD nSize
	);

extern "C" BOOL __stdcall SetHandleInformation(
	HANDLE hObject,
	DWORD dwMask,
	DWORD dwFlags
	);

typedef struct _STARTUPINFOA {
	DWORD   cb;
	LPSTR   lpReserved;
	LPSTR   lpDesktop;
	LPSTR   lpTitle;
	DWORD   dwX;
	DWORD   dwY;
	DWORD   dwXSize;
	DWORD   dwYSize;
	DWORD   dwXCountChars;
	DWORD   dwYCountChars;
	DWORD   dwFillAttribute;
	DWORD   dwFlags;
	WORD    wShowWindow;
	WORD    cbReserved2;
	LPBYTE  lpReserved2;
	HANDLE  hStdInput;
	HANDLE  hStdOutput;
	HANDLE  hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

#define STARTUPINFO STARTUPINFOA

extern "C" BOOL __stdcall CloseHandle(HANDLE h);

extern "C" DWORD __stdcall SearchPathA(
	LPCSTR lpPath,
	LPCSTR lpFileName,
	LPCSTR lpExtension,
	DWORD nBufferLength,
	LPSTR lpBuffer,
	LPSTR* lpFilePart
	);

#define SearchPath SearchPathA

#define STARTF_USESTDHANDLES 0x00000100
#define _O_RDONLY 0x0000
#define _O_WRONLY 0x0001

typedef struct _PROCESS_INFORMATION {
	HANDLE hProcess;
	HANDLE hThread;
	DWORD dwProcessId;
	DWORD dwThreadId;
} PROCESS_INFORMATION, *PPROCESS_INFORMATION, *LPPROCESS_INFORMATION;

extern "C" BOOL __stdcall CreateProcessA(
	LPCSTR lpApplicationName,
	LPSTR lpCommandLine,
	LPSECURITY_ATTRIBUTES lpProcessAttributes,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	BOOL bInheritHandles,
	DWORD dwCreationFlags,
	LPVOID lpEnvironment,
	LPCSTR lpCurrentDirectory,
	LPSTARTUPINFOA lpStartupInfo,
	LPPROCESS_INFORMATION lpProcessInformation
	);

#define CreateProcess CreateProcessA

extern "C" DWORD __stdcall GetLastError(void);

extern "C" BOOL __stdcall GetExitCodeProcess(
	HANDLE hProcess,
	LPDWORD lpExitCode
	);

#define SYNCHRONIZE 0x00100000L
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define INFINITE 0xFFFFFFFF

extern "C" HANDLE __stdcall OpenProcess(
	DWORD dwDesiredAccess,
	BOOL bInheritHandle,
	DWORD dwProcessId
	);

extern "C" DWORD __stdcall WaitForSingleObject(
	HANDLE hHandle,
	DWORD dwMilliseconds
	);
