# do 'make distclean' when changing optimization options or compiler version

OPT = -O2

SHELL := bash
RM = rm -f
LINKEXTRAFLAGS := -ledit -lpthread -rdynamic
TARGETDIR := ../
PREFIX := /usr
ifeq ($(shell test -x /ucrt64/bin/llvm-config && echo -n true),true)
	PREFIX := /ucrt64
	SUFFIX :=
	NIXTYPE := mingw64
	LINKEXTRAFLAGS := -ledit -lpthread
else
ifeq ($(shell test -x /usr/local/bin/llvm-config16 && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX := 16
else
ifeq ($(shell test -x /usr/local/bin/llvm-config-devel && echo -n true),true)
	PREFIX := /usr/local
	SUFFIX := -devel
else
ifeq ($(shell test -x /usr/bin/llvm-config-15 && echo -n true),true)
	SUFFIX := -15
else
ifeq ($(shell test -x /usr/bin/llvm-config-14 && echo -n true),true)
	SUFFIX := -14
else
ifeq ($(shell test -x /usr/bin/llvm-config-13 && echo -n true),true)
	SUFFIX := -13
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
ifeq ($(shell test -x /usr/bin/llvm-config && echo -n true),true)
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
else
ifeq ($(shell uname),OpenBSD)
	CPPEXTRAFLAGS := -Wno-reorder-init-list -DMONOCHROME_PROMPT
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
	CPPEXTRAFLAGS := -stdlib++-isystem /system/develop/tools/lib/gcc/x86_64-unknown-haiku/11.2.0/include/c++ -stdlib++-isystem /system/develop/tools/lib/gcc/x86_64-unknown-haiku/11.2.0/include/c++/x86_64-unknown-haiku -I/system/develop/headers/editline -fPIC
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
	LINKEXTRAFLAGS := -leditline -lpthread
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

# use new pass manager for LLVM-14, 15 and 16
ifeq ($(LLVMVERSION),12)
	CPPEXTRAFLAGS := $(CPPEXTRAFLAGS) -DLEGACY_PASS_MANAGER
endif
ifeq ($(LLVMVERSION),13)
	CPPEXTRAFLAGS := $(CPPEXTRAFLAGS) -DLEGACY_PASS_MANAGER
endif

CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) -std=c++20 -DLINKER=\"$(LINKER)\" -frtti $(CPPEXTRAFLAGS)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all-targets orcjit mcjit)
PRJH = ../include/volvox-$(LLVMVERSION).hh

PCH = $(PRJH:%.hh=%.hh.gch)

ifeq ($(CC),gcc)
	CXX := g++
	INCPCH := -include $(PRJH)
else
	CXX := clang++$(SUFFIX)
	INCPCH := -include-pch $(PCH)
endif

PROG = $(TARGETDIR)volvox

CCOBJS :=  main.o codegen.o parser.o lexer.o token.o functions.o arrays.o types.o mangler.o environment.o
OBJS := $(CCOBJS) $(EXTRAOBJS) $(EDITLINEOBJS)

LIBOBJS := map.o str.o array.o
LIBCOBJS := str_c.o

LIBVOLVOXVERS := 0
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
RPATH := -Wl,-rpath,$(shell pwd)/../clib
endif
LIBVOLVOX := ../clib/$(LIBVOLVOXSONAME)
LIBVOLVOXLINK := ../clib/$(LIBVOLVOXBASE)

SRCS := $(CCOBJS:%.o=%.cc)

EXTRASRC := $(EXTRAOBJS:%.o=../clib/%.c)
EDITLINESRC := $(EDITLINEOBJS:%.o=../wineditline/%.c)

LIBSRC := $(LIBOBJS:%.o=../clib/%.cc)
LIBCSRC := $(LIBCOBJS:%.o=../clib/%.c)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) $(INCPCH) $(EXTRAINC) $<

%.o: ../clib/%.cc
	$(CXX) -c -std=c++20 -fno-rtti -fno-exceptions -DLLVM_VERSION_MAJOR=$(LLVMVERSION) $(CPPEXTRAFLAGS) -fPIC $(OPT) $<

%.o: ../clib/%.c
	$(CC) -c $(CPPEXTRAFLAGS) -DLLVM_VERSION_MAJOR=$(LLVMVERSION) -fPIC $(OPT) $<

ifeq ($(NIXTYPE),mingw64)
%.o: ../wineditline/%.c
	$(CC) -c $(EXTRAINC) $(OPT) $<
endif

all: $(PROG)

$(PCH): $(PRJH) ../include/VolvoxJIT-$(LLVMVERSION).h ../include/volvox.hh
ifeq ($(CC),gcc)
	$(CXX) -o $@ -x c++-header $(CXXFLAGS) $(OPT) $<
else
	$(CXX) $(CXXFLAGS) $(OPT) $<
endif

$(LIBVOLVOX): $(LIBOBJS) $(LIBCOBJS)
	$(CC) -shared -fPIC $(LIBVOLVOXSONAMESPEC) -o $(LIBVOLVOX) $+
	ln -sf $(LIBVOLVOXSONAME) $(LIBVOLVOXLINK)

$(PROG): $(OBJS) $(LIBVOLVOX)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS) $(OPT) -L../clib -lvolvox $(RPATH) $(LINKEXTRAFLAGS)

.PHONY: clean
clean:
	rm -f $(OBJS) $(LIBOBJS)

.PHONY: distclean
distclean:
	rm -f $(PROG) $(PROG).exe $(OBJS) $(LIBOBJS)  $(LIBCOBJS) $(LIBVOLVOX) $(LIBVOLVOXBASE) *~ ../include/*~ ../include/*.gch .depend

.depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) $(EXTRAINC) > .depend
ifneq ($(EXTRASRC),)
	$(CC) -MM $(OPT) $(CPPEXTRAFLAGS) $(EXTRASRC) >> .depend
endif
ifneq ($(EDITLINESRC),)
	$(CC) -MM $(OPT) $(EXTRAINC) $(EDITLINESRC) >> .depend
endif
	$(CC) -MM $(LIBCSRC) $(CPPEXTRAFLAGS) $(OPT) $(EXTRASRC) >> .depend
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(LIBSRC) >> .depend

-include .depend
