# do 'make distclean' when changing optimization options or compiler version
# OPT := -O3
OPT := -O0 -ggdb

SHELL := bash
RM = rm -f
LINKEXTRAFLAGS := -ledit -lpthread -rdynamic
TARGETDIR := ../
ifeq ($(shell test -x /usr/bin/llvm-config-14 && echo -n true),true)
    SUFFIX := -14
else
ifeq ($(shell test -x /usr/bin/llvm-config && echo -n true),true)
    SUFFIX :=
else
ifeq ($(shell test -x /usr/local/bin/llvm-config12 && echo -n true),true)
    SUFFIX := 12
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

CC = clang$(SUFFIX)
LINKER := $(shell which $(CC))

LLVMCONFIG := llvm-config$(SUFFIX)
LLVMVERSION := $(shell llvm-config$(SUFFIX) --version | cut -d. -f1)

CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) -DLINKER=\"$(LINKER)\" -frtti
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

OBJS := main.o lexer.o parser.o token.o codegen.o types.o mangler.o

LIB := map.o str.o

LIBLIB := libvolvox.so.0

SRCS := $(OBJS:%.o=%.cc)

LIBSRC := $(LIB:%.o=../lib/%.cc)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) $(INCPCH) $<

%.o: ../lib/%.cc
	$(CXX) -c $(CXXFLAGS) -DLLVM_VERSION_MAJOR=$(LLVMVERSION) -fPIC $(OPT) $<

all: $(PROG)

$(PCH): $(PRJH) ../include/VolvoxJIT-$(LLVMVERSION).h ../include/volvox.hh
ifeq ($(CC),gcc)
	$(CXX) -o $@ -x c++-header $(CXXFLAGS) $(OPT) $<
else
	$(CXX) $(CXXFLAGS) $(OPT) $<
endif

$(LIBLIB): $(LIB)
	$(CC) -shared -fPIC -Wl,-soname,$(LIBLIB) -o $(LIBLIB) $(LIB)
	ln -sf $(LIBLIB) libvolvox.so
	mv $(LIBLIB) libvolvox.so ../lib

$(PROG): $(OBJS) $(LIBLIB)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS) $(OPT) -L../lib -lvolvox -Wl,-rpath,$(shell pwd)/../lib $(LINKEXTRAFLAGS)

.PHONY: clean
clean:
	rm -f $(OBJS)

.PHONY: distclean
distclean:
	rm -f $(PROG) $(PROG).exe $(OBJS) $(LIB) $(LIBLIB) *~ ../include/*~ ../include/*.gch .depend

.depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) > .depend
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(LIBSRC) >> .depend

-include .depend
