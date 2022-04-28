#define EXPORTING_DLL
#include "str.h"
#include <windows.h>
#include <io.h>
#include <malloc.h>
#include <fcntl.h>
#define nullptr ((void*)0)

/* This file contains routine that do not work in a DLL on Windows
   so this file has to be linked statically. The functions are still
   defined as __declspec(dllexport) to be available in JIT */

_CDECL bool volvox_spawn(int* pid, int* child_stdin, int* child_stdout,
                         int* child_stderr, char* const argv[]) {
	char cmd_path[MAX_PATH];
	char cmd_line[32768];
	if (!argv) {
		errno = EINVAL;
		return false;
	}
	unsigned pathlen = SearchPath(NULL, argv[0], ".exe", MAX_PATH, cmd_path, NULL);
	if (!pathlen)
		goto error;
	if (!volvox_getCmdLine(cmd_line, cmd_path, argv))
		return false;
	SECURITY_ATTRIBUTES saAttr = {
		.nLength = sizeof(SECURITY_ATTRIBUTES), 
		.lpSecurityDescriptor = NULL,
		.bInheritHandle = true // for pipe handles to be inherited
	};
	// Create communication pipes and make sure the child's end is inherited
	HANDLE h_child_stdin_r = NULL;
	HANDLE h_child_stdin_w = NULL;
	if (child_stdin) {
		if (!CreatePipe(&h_child_stdin_r, &h_child_stdin_w, &saAttr, 4096))
			goto error; 
		if (!SetHandleInformation(h_child_stdin_w, HANDLE_FLAG_INHERIT, 0))
			goto error;
	}
	HANDLE h_child_stdout_r = NULL;
	HANDLE h_child_stdout_w = NULL;
	if (child_stdout) {
		if (!CreatePipe(&h_child_stdout_r, &h_child_stdout_w, &saAttr, 4096)) 
			goto error;
		if (!SetHandleInformation(h_child_stdout_r, HANDLE_FLAG_INHERIT, 0))
			goto error;
	}
	HANDLE h_child_stderr_r = NULL;
	HANDLE h_child_stderr_w = NULL;
	if (child_stderr) {
		if (!CreatePipe(&h_child_stderr_r, &h_child_stderr_w, &saAttr, 4096)) 
			goto error;
		if (!SetHandleInformation(h_child_stderr_r, HANDLE_FLAG_INHERIT, 0))
			goto error;
	}
	STARTUPINFO StartInfo = {
		.cb = sizeof(STARTUPINFO),
		.hStdInput = h_child_stdin_r,
		.hStdOutput = h_child_stdout_w,
		.hStdError = h_child_stderr_w,
		.dwFlags = STARTF_USESTDHANDLES
	};
	PROCESS_INFORMATION ProcInfo = {0};
	if (CreateProcess(
		    cmd_path,   // ApplicationName
		    cmd_line,   // CommandLine
		    NULL,       // ProcessAttributes
		    NULL,       // ThreadAttributes
		    true,       // InheritHandles
		    0,          // CreationFlags
		    NULL,       // Environment
		    NULL,       // CurrentDirectory
		    &StartInfo, // StartupInfo
		    &ProcInfo)  // ProcessInformation
		) {
		// get pid if desired, otherwise close the process handle to detach the process
		if (pid)
			*pid = ProcInfo.dwProcessId;
		else
			CloseHandle(ProcInfo.hProcess);
		CloseHandle(ProcInfo.hThread);
		// close here in the parent those pipe ends that are used in the child
		if (h_child_stdin_r)
			CloseHandle(h_child_stdin_r);
		if (h_child_stdout_w)
			CloseHandle(h_child_stdout_w);
		if (h_child_stderr_w)
			CloseHandle(h_child_stderr_w);
		if (child_stdin)
			*child_stdin = _open_osfhandle((uintptr_t)h_child_stdin_w, 0);
		if (child_stdout)
			*child_stdout = _open_osfhandle((uintptr_t)h_child_stdout_r, _O_RDONLY);
		if (child_stderr)
			*child_stderr = _open_osfhandle((uintptr_t)h_child_stderr_r, _O_RDONLY);
		return true;
	}
error:
   // this is not correct - TODO: map/merge Windows errors / POSIX arrows
   errno = GetLastError();
   return false;
}
