# do 'make distclean' when changing optimization options or compiler version

OPT = -O3

SHELL := bash
RM = rm -f
LINKEXTRAFLAGS := -ledit -lpthread -rdynamic
TARGETDIR := ../
ifeq ($(shell test -x /ucrt64/bin/llvm-config && echo -n true),true)
    SUFFIX :=
	NIXTYPE := mingw64
else
ifeq ($(shell test -x /usr/local/bin/llvm-config14 && echo -n true),true)
    SUFFIX := 14
else
ifeq ($(shell test -x /usr/bin/llvm-config-14 && echo -n true),true)
    SUFFIX := -14
else
ifeq ($(shell test -x /usr/local/bin/llvm-config-devel && echo -n true),true)
    SUFFIX := -devel
else
ifeq ($(shell test -x /usr/bin/llvm-config && echo -n true),true)
    SUFFIX :=
else
ifeq ($(shell test -x /usr/bin/llvm-config-13 && echo -n true),true)
    SUFFIX := -13
else
ifeq ($(shell test -x /usr/bin/llvm-config-12 && echo -n true),true)
    SUFFIX := -12
endif
endif
endif
endif
endif
endif
	NIXTYPE := posix
endif

LLVMCONFIG := llvm-config$(SUFFIX)
LLVMVERSION := $(shell llvm-config$(SUFFIX) --version | cut -d. -f1)

ifeq ($(shell uname),FreeBSD)
	CPPEXTRAFLAGS :=  -I/usr/local/include -Wno-reorder-init-list
	EXTRAOBJS := emutls.o
	EDITLINEOBJS := # we use system provided BSD editline
else
ifeq ($(NIXTYPE),mingw64)
	CPPEXTRAFLAGS :=
	EXTRAOBJS := emutls.o
	EDITLINEOBJS := editline.o history.o fn_complete.o # our patched colored-promt wineditline
else
	CPPEXTRAFLAGS :=
	EXTRAOBJS :=
	EDITLINEOBJS :=
endif
endif

CC = clang$(SUFFIX)
LINKER := $(shell which $(CC))

ifneq ($(LLVMVERSION),14)
ifneq ($(LLVMVERSION),15)
ifneq ($(LLVMVERSION),16)
	CPPEXTRAFLAGS := $(CPPEXTRAFLAGS) -DLEGACY_PASS_MANAGER
endif
endif
endif

CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) -DLINKER=\"$(LINKER)\" -frtti $(CPPEXTRAFLAGS)
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

CCOBJS :=  main.o lexer.o parser.o token.o codegen.o types.o mangler.o
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
RPATH := -Wl,-rpath,$(shell pwd)/../lib
endif
LIBVOLVOX := ../lib/$(LIBVOLVOXSONAME)
LIBVOLVOXLINK := ../lib/$(LIBVOLVOXBASE)

SRCS := $(CCOBJS:%.o=%.cc)

EXTRASRC := $(EXTRAOBJS:%.o=../lib/%.c)
EDITLINESRC := $(EDITLINEOBJS:%.o=../wineditline/%.c)

LIBSRC := $(LIBOBJS:%.o=../lib/%.cc)
LIBCSRC := $(LIBCOBJS:%.o=../lib/%.c)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) $(INCPCH) $(EXTRAINC) $<

%.o: ../lib/%.cc
	$(CXX) -c $(CXXFLAGS) -DLLVM_VERSION_MAJOR=$(LLVMVERSION) -fPIC $(OPT) $<

%.o: ../lib/%.c
	$(CC) -c -DLLVM_VERSION_MAJOR=$(LLVMVERSION) -fPIC $(OPT) $<

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
	$(CXX) -o $@ $(OBJS) $(LDFLAGS) $(OPT) -L../lib -lvolvox $(RPATH) $(LINKEXTRAFLAGS)

.PHONY: clean
clean:
	rm -f $(OBJS) $(LIBOBJS)

.PHONY: distclean
distclean:
	rm -f $(PROG) $(PROG).exe $(OBJS) $(LIBOBJS)  $(LIBCOBJS) $(LIBVOLVOX) $(LIBVOLVOXBASE) *~ ../include/*~ ../include/*.gch .depend

.depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) $(EXTRAINC) > .depend
ifneq ($(EXTRASRC),)
	$(CC) -MM $(OPT) $(EXTRASRC) >> .depend
endif
ifneq ($(EDITLINESRC),)
	$(CC) -MM $(OPT) $(EXTRAINC) $(EDITLINESRC) >> .depend
endif
	$(CC) -MM $(LIBCSRC) $(OPT) $(EXTRASRC) >> .depend
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(LIBSRC) >> .depend

-include .depend
