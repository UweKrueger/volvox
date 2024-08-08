/* Copyright (c) 2009, 2010, 2011, 2013  Chris Faylor

  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS
  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
  PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* This version has been modified by Uwe Krüger (2023)
   to have no dependency on Cygwin/MSYS */

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <libgen.h>
#include <stdbool.h>
#include <windows.h>
#include <winternl.h>
#include <imagehlp.h>
#define PSAPI_VERSION 1
#include <psapi.h>

struct option longopts[] =
{
  {"help", no_argument, NULL, 'h'},
  {"verbose", no_argument, NULL, 'v'},
  {"version", no_argument, NULL, 'V'},
  {"data-relocs", no_argument, NULL, 'd'},
  {"function-relocs", no_argument, NULL, 'r'},
  {"unused", no_argument, NULL, 'u'},
  {0, no_argument, NULL, 0}
};
const char *opts = "dhruvV";

static int process_file (const char *);

static int
error (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  fprintf (stderr, "ldd: ");
  vfprintf (stderr, fmt, ap);
  fprintf (stderr, "\nTry `ldd --help' for more information.\n");
  exit (1);
}

static void __attribute__ ((__noreturn__))
usage ()
{
  printf ("Usage: %s [OPTION]... FILE...\n\
\n\
Print shared library dependencies\n\
\n\
  -h, --help              print this help and exit\n\
  -V, --version           print version information and exit\n\
  -r, --function-relocs   process data and function relocations\n\
                          (currently unimplemented)\n\
  -u, --unused            print unused direct dependencies\n\
                          (currently unimplemented)\n\
  -v, --verbose           print all information\n\
                          (currently unimplemented)\n",
	   "ldd");
  exit (0);
}

static void
print_version ()
{
  printf ("ldd (mingw) %d.%d.%d\n"
	  "Print shared library dependencies\n"
	  "Copyright (C) 2009 - %s Chris Faylor\n"
	  "This is free software; see the source for copying conditions.  There is NO\n"
	  "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n",
	  __MINGW64_VERSION_MAJOR,
	  __MINGW64_VERSION_MINOR,
	  __MINGW64_VERSION_BUGFIX,
	  strrchr (__DATE__, ' ') + 1);
}

#define print_errno_error_and_return(__fn) \
  do {\
    fprintf (stderr, "ldd: %s: %s\n", (__fn), strerror (errno));\
    return 1;\
  } while (0)

#define set_errno_and_return(x) \
  do {\
    return (x);\
  } while (0)


static HANDLE hProcess;

static struct filelist
{
  struct filelist *next;
  char *name;
} *head = NULL;

static bool
saw_file (char *name)
{
  struct filelist *p;

  for (p = head; p; p = p->next)
    if (strcasecmp (name, p->name) == 0)
      return true;

  p = (struct filelist *) malloc(sizeof (struct filelist));
  p->next = head;
  p->name = strdup (name);
  head = p;
  return false;
}

static char *
get_module_filename (HANDLE hp, HMODULE hm)
{
  size_t len;
  char *buf = NULL;
  DWORD res;
  for (len = 1024; (res = GetModuleFileNameExA (hp, hm, (buf = (char *) realloc (buf, len * sizeof (char))), len)) == len; len += 1024)
    continue;
  if (!res)
    {
      free (buf);
      buf = NULL;
    }
  return buf;
}

static char *
load_dll (const char *fn)
{
  char *buf = get_module_filename (GetCurrentProcess (), NULL);
  if (!buf)
    {
      printf ("ldd: GetModuleFileName returned an error %u\n",
	      (unsigned int) GetLastError ());
      exit (1);		/* FIXME */
    }

  char *newbuf = (char *) malloc ((sizeof ("\"\" -- ") + strlen (buf) + strlen (fn)) * sizeof (char));
  newbuf[0] = '"';
  strcpy (newbuf + 1, buf);
  char *p = strstr (newbuf, "\\ldd");
  if (!p)
    {
      printf ("ldd: can't parse my own filename \"%ls\"\n", buf);
      exit (1);
    }
  p[3] = 'h';
  strcat (newbuf, "\" -- ");
  strcat (newbuf, fn);
  free (buf);
  return newbuf;
}

static int
start_process (const char *fn, bool *isdll)
{
  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi;
  si.cb = sizeof (si);
  char *cmd;
  /* OCaml natdynlink plugins (.cmxs) cannot be handled by ldd because they
     can only be loaded by flexdll_dlopen() */
  if (strlen (fn) < 4 || (strcasecmp (strchr (fn, '\0') - 4, ".dll") != 0
       && strcasecmp (strchr (fn, '\0') - 4, ".oct") != 0
       && strcasecmp (strchr (fn, '\0') - 3, ".so") != 0))
    {
      cmd = strdup (fn);
      *isdll = false;
    }
  else
    {
      cmd = load_dll (fn);
      *isdll = true;
    }
  if (CreateProcessA (NULL, cmd, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi))
    {
      free (cmd);
      hProcess = pi.hProcess;
      DebugSetProcessKillOnExit (true);
      return 0;
    }

  free (cmd);
  set_errno_and_return (1);
}

struct dlls
  {
    LPVOID lpBaseOfDll;
    HANDLE hFile;
    struct dlls *next;
  };

#define SLOP strlen (" (?)")
char *
tocyg (char *win_fn)
{
	return strdup(win_fn);
  /* ssize_t cwlen = cygwin_conv_path (CCP_WIN_W_TO_POSIX, win_fn, NULL, 0); */
  /* char *fn; */
  /* if (cwlen <= 0) */
  /*   { */
  /*     int len = wcstombs (NULL, win_fn, 0) + 1; */
  /*     if ((fn = (char *) malloc (len))) */
  /* 	wcstombs (fn, win_fn, len); */
  /*   } */
  /* else */
  /*   { */
  /*     char *fn_cyg = (char *) malloc (cwlen + SLOP + 1); */
  /*     if (cygwin_conv_path (CCP_WIN_W_TO_POSIX, win_fn, fn_cyg, cwlen) == 0) */
  /* 	fn = fn_cyg; */
  /*     else */
  /* 	{ */
  /* 	  free (fn_cyg); */
  /* 	  int len = wcstombs (NULL, win_fn, 0); */
  /* 	  fn = (char *) malloc (len + SLOP + 1); */
  /* 	  wcstombs (fn, win_fn, len + SLOP + 1); */
  /* 	} */
  /*   } */
  /* return fn; */
}

static int
print_dlls (struct dlls *dll, const char *dllfn, const char *process_fn)
{
  head = NULL;			/* FIXME: memory leak */
  while ((dll = dll->next))
    {
	  bool saw = false;
      char *fn;
      char *fullpath = get_module_filename (hProcess, (HMODULE) dll->lpBaseOfDll);
      if (!fullpath)
	{
	  // if no path found yet, try getting it from an open handle to the DLL
	  char dllname[PATH_MAX];
	  if (GetFinalPathNameByHandleA (dll->hFile, dllname, PATH_MAX, 0))
	    {
	      fn = tocyg (dllname);
	      saw = saw_file (basename (fn));
	    }
	  else
	    fn = strdup ("???");
	}
      else if (dllfn && strcmp (fullpath, dllfn) == 0)
	{
	  free (fullpath);
	  continue;
	}
      else
	{
	  fn = tocyg (fullpath);
	  saw = saw_file (basename (fn));
	  free (fullpath);
	}
      if (!saw)
    {
      char* beautified_path; // strip "\\?\" from "\\?\C:\"
      if (fn[0] == '\\' && fn[1] == '\\' && fn[2] == '?' && fn[3] == '\\')
	    beautified_path = fn + 4;
      else
	    beautified_path = fn;
      printf ("\t%s => %s (%p)\n", basename (fn), beautified_path, dll->lpBaseOfDll);
    }
      free (fn);
    }
  if (process_fn)
    return process_file (process_fn);
  return 0;
}

static int
report (const char *in_fn, bool multiple)
{
  if (multiple)
    printf ("%s:\n", in_fn);
  char *fn = _fullpath (NULL, in_fn, 0);
  if (!fn)
    print_errno_error_and_return (in_fn);

  bool isdll;

  if (start_process (fn, &isdll))
    print_errno_error_and_return (in_fn);

  DEBUG_EVENT ev;

  struct dlls dll_list = {};
  struct dlls *dll_last = &dll_list;
  const char *process_fn = NULL;

  int res = 0;

  while (1)
    {
      bool exitnow = false;
      DWORD cont = DBG_CONTINUE;
      if (!WaitForDebugEvent (&ev, INFINITE))
	break;
      switch (ev.dwDebugEventCode)
	{
	case CREATE_PROCESS_DEBUG_EVENT:
	  if (!isdll)
	    {
	      PIMAGE_DOS_HEADER dos_header = (PIMAGE_DOS_HEADER) alloca (4096);
	      PIMAGE_NT_HEADERS nt_header;
	      PVOID entry_point;
	      static const unsigned char int3 = 0xcc;
	      SIZE_T bytes;

	      if (!ReadProcessMemory (hProcess,
				      ev.u.CreateProcessInfo.lpBaseOfImage,
				      dos_header, 4096, &bytes))
		print_errno_error_and_return (in_fn);

	      nt_header = (PIMAGE_NT_HEADERS) ((PBYTE) (dos_header)
					     + dos_header->e_lfanew);
	      entry_point = (PVOID)
		  ((char*) ev.u.CreateProcessInfo.lpBaseOfImage
		   + nt_header->OptionalHeader.AddressOfEntryPoint);

	      if (!WriteProcessMemory (hProcess, entry_point, &int3, 1, &bytes))
		print_errno_error_and_return (in_fn);
	    }
	  break;
	case LOAD_DLL_DEBUG_EVENT:
	  dll_last->next = (struct dlls *) malloc (sizeof (struct dlls));
	  dll_last->next->lpBaseOfDll = ev.u.LoadDll.lpBaseOfDll;
	  dll_last->next->hFile = ev.u.LoadDll.hFile;
	  dll_last->next->next = NULL;
	  dll_last = dll_last->next;
	  break;
	case EXCEPTION_DEBUG_EVENT:
	  switch (ev.u.Exception.ExceptionRecord.ExceptionCode)
	    {
	    case STATUS_ENTRYPOINT_NOT_FOUND:
	      /* A STATUS_ENTRYPOINT_NOT_FOUND might be encountered right after
		 loading all DLLs.  We have to handle it here, otherwise ldd
		 runs into an endless loop. */
	      goto print_and_exit;
	    case STATUS_DLL_NOT_FOUND:
	      process_fn = fn;
	      break;
	    case STATUS_BREAKPOINT:
	      if (!isdll)
		TerminateProcess (hProcess, 0);
	      break;
	    }
	  if (ev.u.Exception.ExceptionRecord.ExceptionFlags &
	      EXCEPTION_NONCONTINUABLE) {
	    res = 1;
	    goto print_and_exit;
	  }
	  break;
	case EXIT_PROCESS_DEBUG_EVENT:
	  if (ev.u.ExitProcess.dwExitCode != 0)
	    process_fn = fn;
print_and_exit:
	  print_dlls (&dll_list, isdll ? fn : NULL, process_fn);
	  exitnow = true;
	  break;
	default:
	  break;
	}
      if (!ContinueDebugEvent (ev.dwProcessId, ev.dwThreadId, cont))
	{
	  print_errno_error_and_return (in_fn);
	}
      if (exitnow)
	break;
    }

  return res;
}

int
main (int argc, char **argv)
{
  int optch;

  /* Use locale from environment.  If not set or set to "C", use UTF-8. */
  setlocale (LC_CTYPE, "");
  if (!strcmp (setlocale (LC_CTYPE, NULL), "C"))
    setlocale (LC_CTYPE, "en_US.UTF-8");
  while ((optch = getopt_long (argc, argv, opts, longopts, NULL)) != -1)
    switch (optch)
      {
      case 'd':
      case 'r':
      case 'u':
	error ("option not implemented `-%c'", optch);
	exit (1);
      case 'h':
	usage ();
      case 'V':
	print_version ();
	return 0;
      default:
	fprintf (stderr, "Try `%s --help' for more information.\n",
		 "ldd");
	return 1;
      }
  argv += optind;
  if (!*argv)
    error ("missing file arguments");

  int ret = 0;
  bool multiple = !!argv[1];
  char *fn;
  while ((fn = *argv++))
    if (report (fn, multiple))
      ret = 1;
  exit (ret);
}

static bool printing = false;


/* dump of import directory
   section begins at pointer 'section base'
   section RVA is 'section_rva'
   import directory begins at pointer 'imp' */
static int
dump_import_directory (const void *const section_base,
		       const DWORD section_rva,
		       const IMAGE_IMPORT_DESCRIPTOR *imp)
{
  /* get memory address given the RVA */
  #define adr(rva) ((const void*) ((char*) section_base+((DWORD) (rva))-section_rva))

  /* continue until address inaccessible or there's no DLL name */
  for (; !IsBadReadPtr (imp, sizeof (*imp)) && imp->Name; imp++)
    {
      char full_path[PATH_MAX];
      char *dummy;
      char *fn = (char *) adr (imp->Name);

      if (saw_file (fn))
	continue;

      char *print_fn;
      if (!SearchPathA (NULL, fn, NULL, PATH_MAX, full_path, &dummy))
	{
	  print_fn = strdup ("not found");
	  printing = true;
	}
      else if (!printing)
	continue;
      else
	{
	  print_fn = tocyg (full_path);
	  strcat (print_fn, " (?)");
	}

      printf ("\t%s => %s\n", (char *) fn, print_fn);
      free (print_fn);
    }
  #undef adr

  return 0;
}

/* load a file in RAM (memory-mapped)
   return pointer to loaded file
   0 if no success  */
static void *
map_file (const char *filename)
{
  HANDLE hFile, hMapping;
  void *basepointer;
  if ((hFile = CreateFileA (filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0)) == INVALID_HANDLE_VALUE)
    {
      fprintf (stderr, "couldn't open %ls\n", filename);
      return 0;
    }
  if (!(hMapping = CreateFileMapping (hFile, 0, PAGE_READONLY | SEC_COMMIT, 0, 0, 0)))
    {
      fprintf (stderr, "CreateFileMapping failed with windows error %u\n",
	       (unsigned int) GetLastError ());
      CloseHandle (hFile);
      return 0;
    }
  if (!(basepointer = MapViewOfFile (hMapping, FILE_MAP_READ, 0, 0, 0)))
    {
      fprintf (stderr, "MapViewOfFile failed with windows error %u\n",
	       (unsigned int) GetLastError ());
      CloseHandle (hMapping);
      CloseHandle (hFile);
      return 0;
    }

  CloseHandle (hMapping);
  CloseHandle (hFile);

  return basepointer;
}


/* this will return a pointer immediatly behind the DOS-header
   0 if error */
static void *
skip_dos_stub (const IMAGE_DOS_HEADER *dos_ptr)
{
  /* look there's enough space for a DOS-header */
  if (IsBadReadPtr (dos_ptr, sizeof (*dos_ptr)))
      {
	fprintf (stderr, "not enough space for DOS-header\n");
	return 0;
      }

   /* validate MZ */
   if (dos_ptr->e_magic != IMAGE_DOS_SIGNATURE)
      {
	fprintf (stderr, "not a DOS-stub\n");
	return 0;
      }

  /* ok, then, go get it */
  return (char*) dos_ptr + dos_ptr->e_lfanew;
}


/* find the directory's section index given the RVA
   Returns -1 if impossible */
static int
get_directory_index (const unsigned dir_rva,
		     const unsigned dir_length,
		     const int number_of_sections,
		     const IMAGE_SECTION_HEADER *sections)
{
  int sect;
  for (sect = 0; sect < number_of_sections; sect++)
  {
    /* compare directory RVA to section RVA */
    if (sections[sect].VirtualAddress <= dir_rva
       && dir_rva < sections[sect].VirtualAddress+sections[sect].SizeOfRawData)
      return sect;
  }

  return -1;
}

/* dump imports of a single file
   Returns 0 if successful, !=0 else */
static int
process_file (const char *filename)
{
  void *basepointer;    /* Points to loaded PE file
			 * This is memory mapped stuff
			 */
  int number_of_sections;
  DWORD import_rva;           /* RVA of import directory */
  DWORD import_length;        /* length of import directory */
  int import_index;           /* index of section with import directory */

  /* ensure byte-alignment for struct tag_header */
  #include <pshpack1.h>

  const struct tag_header
    {
      DWORD signature;
      IMAGE_FILE_HEADER file_head;
      IMAGE_OPTIONAL_HEADER opt_head;
      IMAGE_SECTION_HEADER section_header[1];  /* an array of unknown length */
    } *header;

  /* revert to regular alignment */
  #include <poppack.h>

  printing = false;

  /* first, load file */
  basepointer = map_file (filename);
  if (!basepointer)
      {
	puts ("cannot load file");
	return 1;
      }

  /* get header pointer; validate a little bit */
  header = (struct tag_header *) skip_dos_stub ((IMAGE_DOS_HEADER *) basepointer);
  if (!header)
      {
	puts ("cannot skip DOS stub");
	UnmapViewOfFile (basepointer);
	return 2;
      }

  /* look there's enough space for PE headers */
  if (IsBadReadPtr (header, sizeof (*header)))
      {
	puts ("not enough space for PE headers");
	UnmapViewOfFile (basepointer);
	return 3;
      }

  /* validate PE signature */
  if (header->signature != IMAGE_NT_SIGNATURE)
      {
	puts ("not a PE file");
	UnmapViewOfFile (basepointer);
	return 4;
      }

  /* get number of sections */
  number_of_sections = header->file_head.NumberOfSections;

  /* check there are sections... */
  if (number_of_sections < 1)
      {
	UnmapViewOfFile (basepointer);
	return 5;
      }

  /* validate there's enough space for section headers */
  if (IsBadReadPtr (header->section_header, number_of_sections*sizeof (IMAGE_SECTION_HEADER)))
      {
	puts ("not enough space for section headers");
	UnmapViewOfFile (basepointer);
	return 6;
      }

  /* get RVA and length of import directory */
  import_rva = header->opt_head.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  import_length = header->opt_head.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;

  /* check there's stuff to care about */
  if (!import_rva || !import_length)
      {
	UnmapViewOfFile (basepointer);
	return 0;       /* success! */
    }

  /* get import directory pointer */
  import_index = get_directory_index (import_rva,import_length,number_of_sections,header->section_header);

  /* check directory was found */
  if (import_index < 0)
      {
	puts ("couldn't find import directory in sections");
	UnmapViewOfFile (basepointer);
	return 7;
      }

  /* The pointer to the start of the import directory's section */
  const void *section_address = (char*) basepointer + header->section_header[import_index].PointerToRawData;
  if (dump_import_directory (section_address,
			   header->section_header[import_index].VirtualAddress,
				    /* the last parameter is the pointer to the import directory:
				       section address + (import RVA - section RVA)
				       The difference is the offset of the import directory in the section */
			   (const IMAGE_IMPORT_DESCRIPTOR *) ((char *) section_address+import_rva-header->section_header[import_index].VirtualAddress)))
    {
      UnmapViewOfFile (basepointer);
      return 8;
    }

  UnmapViewOfFile (basepointer);
  return 0;
}
