# do 'make clean' when changing optimization options or compiler version

OPT = -O2

SHELL := bash
RM = rm -f
LINKEXTRAFLAGS := -ledit -lpthread -rdynamic
TARGETDIR := ../
TARGET :=
PREFIX := /usr
ifeq ($(shell test -x C\:/Program\ Files/LLVM/bin/llvm-config.exe && echo -n true),true)
	PREFIX := C:/Program\ Files/LLVM
	SUFFIX := .exe
	NIXTYPE := mingw64
	TARGET := -target x86_64-pc-windows-gnu -Wno-gcc-compat
	# LINKEXTRAFLAGS := -lpsapi -lshell32 -lole32 -luuid -ladvapi32 -ldelayimp -lkernel32 -luser32 -lgdi32 -lwinspool -loleaut32 -lcomdlg32 -lWs2_32 -lucrt
	LINKEXTRAFLAGS := -lpthread
else
ifeq ($(shell test -x /usr/local/bin/llvm-config-devel && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX := -devel
else
ifeq ($(shell test -x /usr/local/bin/llvm-config18 && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX := 18
else
ifeq ($(shell test -x /usr/local/bin/llvm-config-19 && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX := -19
	LINKEXTRAFLAGS := -Wl,-rpath,/usr/local/llvm19/lib -lreadline -rdynamic
else
ifeq ($(shell test -x /usr/local/bin/llvm-config-18 && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX := -18
	LINKEXTRAFLAGS := -Wl,-rpath,/usr/local/llvm18/lib -lreadline -rdynamic
else
ifeq ($(shell test -x /usr/local/bin/llvm-config-17 && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX := -17
	LINKEXTRAFLAGS := -Wl,-rpath,/usr/local/llvm17/lib -lreadline -rdynamic
# readline on OpenBSD is GPLv2 - if you want a pure non-GPL build you can
# use (a very old) libedit by using the following line and adding
# "-DMONOCHROME_PROMPT=1" to CPPEXTRAFLAGS below 
#	LINKEXTRAFLAGS := -Wl,-rpath,/usr/local/llvm16/lib -ledit
else
ifeq ($(shell test -x /usr/bin/llvm-config-19 && echo -n true),true)
	SUFFIX := -19
else
ifeq ($(shell test -x /usr/bin/llvm-config-18 && echo -n true),true)
	SUFFIX := -18
else
ifeq ($(shell test -x /usr/bin/llvm-config-17 && echo -n true),true)
	SUFFIX := -17
else
ifeq ($(shell test -x /usr/bin/llvm-config-16 && echo -n true),true)
	SUFFIX := -16
else
ifeq ($(shell test -x /usr/bin/llvm-config && echo -n true),true)
	SUFFIX :=
else
ifeq ($(shell test -x /bin/llvm-config && echo -n true),true)
	PREFIX := /
	SUFFIX :=
	LINKEXTRAFLAGS := -ledit -lpthread
else
ifeq ($(shell test -x /usr/local/bin/llvm-config && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX :=
else
ifeq ($(shell test -x /usr/pkg/bin/llvm-config && echo -n true),true)
	PREFIX := /usr/pkg
	SUFFIX :=
endif
endif
endif
endif
endif
endif
endif
endif
endif
endif
endif
endif
endif
	NIXTYPE := posix
endif

LLVMCONFIG := $(PREFIX)/bin/llvm-config$(SUFFIX)

ifeq ($(shell uname),FreeBSD)
	CPPEXTRAFLAGS :=  -I/usr/include/edit/readline -I/usr/local/include -DEMUTLS_USE_POSIX_MEMALIGN
	EXTRAOBJS := emutls.o
	EDITLINEOBJS := # we use system provided BSD editline
else
ifeq ($(shell uname),NetBSD)
	CPPEXTRAFLAGS := -I/usr/include/readline
	EXTRAOBJS :=
	EDITLINEOBJS := # we use system provided BSD editline
	LINKEXTRAFLAGS := -latomic -rdynamic
else
ifeq ($(shell uname),OpenBSD)
	CPPEXTRAFLAGS := -Wno-reorder-init-list -I/usr/include/readline -DNEED_HISTORY_H=1
	EXTRAOBJS :=
	EDITLINEOBJS := # we use system provided BSD editline
else
ifeq ($(shell uname),DragonFly)
	CPPEXTRAFLAGS := -I/usr/local/include/editline -DEMUTLS_USE_POSIX_MEMALIGN
	EXTRAOBJS := emutls.o
	EDITLINEOBJS := # we use system provided BSD editline
	LINKEXTRAFLAGS := -L/usr/local/lib $(LINKEXTRAFLAGS)
else
ifeq ($(shell uname),Haiku)
	CPPEXTRAFLAGS := -stdlib++-isystem /system/develop/tools/lib/gcc/x86_64-unknown-haiku/13.2.0/include/c++ -stdlib++-isystem /system/develop/tools/lib/gcc/x86_64-unknown-haiku/13.2.0/include/c++/x86_64-unknown-haiku -I/system/develop/headers/editline -fPIC
	EXTRAOBJS :=
	EDITLINEOBJS := # we use system provided BSD editline
	LINKEXTRAFLAGS := -L/system/develop/lib $(LINKEXTRAFLAGS)
else
ifeq ($(NIXTYPE),mingw64) # only a reminder - none of the available 'mingw' versions works ;-(
	CPPEXTRAFLAGS :=
	EXTRAOBJS := emutls.o
	EDITLINEOBJS := editline.o history.o fn_complete.o # our patched colored-promt wineditline
else
ifneq ($(wildcard /usr/include/editline/.*),)
	CPPEXTRAFLAGS := -I/usr/include/editline
else
	CPPEXTRAFLAGS := -DUSE_EDITLINE
	LINKEXTRAFLAGS := -leditline -lpthread -rdynamic
endif
	EXTRAOBJS :=
	EDITLINEOBJS :=
endif
endif
endif
endif
endif
endif

LLVMVERSION := $(shell $(LLVMCONFIG) --version | cut -d. -f1)

CC = clang$(SUFFIX)
LINKER := $(shell which $(CC))

ifeq ($(NIXTYPE),mingw64)
	CXXFLAGS := -I"C:/Program Files/LLVM/include" -I../include/editline -D_FILE_OFFSET_BITS=64 -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -std=c++20 -DLINKER="\"C:/Program Files/LLVM/bin/clang.exe\"" -frtti $(CPPEXTRAFLAGS)
	LDFLAGS := -L"C:/Program Files/LLVM/lib" -lLLVM-18
	PIC :=
else
	CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) -std=c++20 -DLINKER=\"$(LINKER)\" -frtti $(CPPEXTRAFLAGS)
	LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all-targets orcjit mcjit)
	PIC := -fPIC
endif
PRJH := ../include/volvox.hh

ifeq ($(CC),gcc)
	CXX := g++
	LINK := g++
	INCPCH := -include $(PRJH)
	PCH := $(PRJH:%.hh=%.hh.gch)
	AR := ar
else
ifeq ($(CC),gcc-14)
	CXX := g++-14
	LINK := g++-14
	INCPCH := -include $(PRJH)
	PCH := $(PRJH:%.hh=%.hh.gch)
	AR := ar
else
	CXX := clang++$(SUFFIX)
	LINK := clang++$(SUFFIX)
ifeq ($(shell type -path llvm-ar$(SUFFIX)),)
ifeq ($(shell type -path llvm-ar),)
# Alpine Linux has a special form...
	AR := llvm18-ar
else
	AR := llvm-ar
endif
else
	AR := llvm-ar$(SUFFIX)
endif
ifeq ($(shell test $(LLVMVERSION) -ge 18; echo $$?),0)
	PCH := $(PRJH:%.hh=%.hh.pch)
else
	PCH := $(PRJH:%.hh=%.hh.gch)
endif
	INCPCH := -include-pch $(PCH)
endif
endif

PROG = $(TARGETDIR)volvox

CCOBJS := main.o codegen.o parser.o lexer.o token.o functions.o threads.o arrays.o types.o mangler.o environment.o
OBJS := $(CCOBJS) $(EXTRAOBJS) $(EDITLINEOBJS)

LIBOBJS := map.o str.o array.o
LIBCOBJS := str_c.o threads_c.o
LIBOBJSLTO := map.lto.o str.lto.o array.lto.o
LIBCOBJSLTO := str_c.lto.o threads_c.lto.o

LIBVOLVOXVERS := 0
LIBVOLVOXLTONAME := libvolvox.lto.a
ifeq ($(NIXTYPE),msvc)
LIBVOLVOXBASE := libvolvox.dll
LIBVOLVOXSONAME := $(LIBVOLVOXBASE)
LIBVOLVOXSONAMESPEC :=
EXTRAINC := -I../include
RPATH :=
LINK := llvm-link
else
ifeq ($(NIXTYPE),mingw64)
LIBVOLVOXBASE := libvolvox.dll
LIBVOLVOXSONAME := $(LIBVOLVOXBASE)
LIBVOLVOXSONAMESPEC :=
EXTRAINC := -I../include
RPATH := -Wl,--stack=10000000
else
LIBVOLVOXBASE := libvolvox.so
LIBVOLVOXSONAME := $(LIBVOLVOXBASE).$(LIBVOLVOXVERS)
LIBVOLVOXSONAMESPEC := -Wl,-soname,$(LIBVOLVOXSONAME)
EXTRAINC :=
RPATH := -Wl,-rpath,$(shell pwd)/../lib
endif
endif
LIBVOLVOX := ../lib/$(LIBVOLVOXSONAME)
LIBVOLVOXLINK := ../lib/$(LIBVOLVOXBASE)
LIBVOLVOXLTO := ../lib/$(LIBVOLVOXLTONAME)

SRCS := $(CCOBJS:%.o=%.cc)

EXTRASRC := $(EXTRAOBJS:%.o=../clib/%.c)
EDITLINESRC := $(EDITLINEOBJS:%.o=../wineditline/%.c)

LIBSRC := $(LIBOBJS:%.o=../clib/%.cc)
LIBCSRC := $(LIBCOBJS:%.o=../clib/%.c)

%.o: %.cc $(PCH)
	$(CXX) $(TARGET) -c $(CXXFLAGS) $(OPT) $(INCPCH) $(EXTRAINC) $<

%.o: ../clib/%.cc
	$(CXX) $(TARGET) -c -std=c++20 -fno-rtti -fno-exceptions -DLLVM_VERSION_MAJOR=$(LLVMVERSION) $(CPPEXTRAFLAGS) $(PIC) $(OPT) $<

%.lto.o: ../clib/%.cc
	$(CXX) $(TARGET) -c -o $@ -flto=thin -std=c++20 -fno-rtti -fno-exceptions -DLLVM_VERSION_MAJOR=$(LLVMVERSION) $(CPPEXTRAFLAGS) $(PIC) $(OPT) $<

%.o: ../clib/%.c
	$(CC) $(TARGET) -c $(CPPEXTRAFLAGS) -DLLVM_VERSION_MAJOR=$(LLVMVERSION) $(PIC) $(OPT) $<

%.lto.o: ../clib/%.c
	$(CC) $(TARGET) -c -o $@ -flto=thin $(CPPEXTRAFLAGS) -DLLVM_VERSION_MAJOR=$(LLVMVERSION) $(OPT) $<

ifeq ($(NIXTYPE),mingw64)
%.o: ../wineditline/%.c
	$(CC) $(TARGET) -c $(EXTRAINC) $(OPT) $<
endif

all: $(PROG) $(LIBVOLVOX) $(LIBVOLVOXLTO)

$(PCH): $(PRJH) ../include/VolvoxJIT.h ../clib/map.h ../clib/types.h ../clib/str.h AST.h global.h
ifeq ($(CC),gcc)
	$(CXX) $(TARGET) -o $@ -x c++-header $(CXXFLAGS) $(OPT) $<
else
	$(CXX) $(TARGET) $(CXXFLAGS) $(OPT) $<
endif

$(LIBVOLVOX): $(LIBOBJS) $(LIBCOBJS)
	$(CC) $(TARGET) -shared $(PIC) $(LIBVOLVOXSONAMESPEC) -o $@ $+
ifeq ($(NIXTYPE),mingw64)
	cp $(LIBVOLVOX) ..
else
	ln -sf $(LIBVOLVOXSONAME) $(LIBVOLVOXLINK)
endif

$(LIBVOLVOXLTO): $(LIBOBJSLTO) $(LIBCOBJSLTO)
	rm -f $@
	$(AR) -qc $@ $+

$(PROG): $(OBJS) $(LIBOBJS) $(LIBCOBJS)
	$(LINK) $(TARGET) -o $@ $(OBJS) $(LDFLAGS) $(LIBOBJS) $(LIBCOBJS) $(RPATH) $(LINKEXTRAFLAGS)

.PHONY: clean
clean:
	rm -f $(PROG) $(PROG).exe $(OBJS) $(LIBOBJS) $(LIBCOBJS) $(LIBOBJSLTO) $(LIBCOBJSLTO) $(LIBVOLVOX) $(LIBVOLVOXBASE) ../$(LIBVOLVOXBASE) $(LIBVOLVOXLINK) $(LIBVOLVOXLTO) *~ ../include/*~ ../include/*.pch ../include/*.gch .depend

.depend:
	$(CXX) $(TARGET) -MM $(CXXFLAGS) $(OPT) $(SRCS) $(EXTRAINC) > .depend
ifneq ($(EXTRASRC),)
	$(CC) $(TARGET) -MM $(OPT) $(CPPEXTRAFLAGS) $(EXTRASRC) >> .depend
endif
ifneq ($(EDITLINESRC),)
	$(CC) $(TARGET) -MM $(OPT) $(EXTRAINC) $(EDITLINESRC) >> .depend
endif
	$(CC) $(TARGET) -MM $(LIBCSRC) $(CPPEXTRAFLAGS) $(OPT) $(EXTRASRC) >> .depend
	$(CXX) $(TARGET) -MM $(CXXFLAGS) $(OPT) $(LIBSRC) >> .depend
	for qq in $(CCOBJS); do echo "$$qq: $(PCH)" >> .depend; done
-include .depend
