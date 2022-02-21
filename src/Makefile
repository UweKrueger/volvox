# do 'make distclean' when changing optimization options or compiler version
OPT := -O3
# OPT := -O0 -ggdb

SHELL := bash
RM = rm -f
LINKEXTRAFLAGS :=
TARGETDIR := ../
ifeq ($(shell test -x /usr/local/bin/clang++12 && echo -n true),true)
    SUFFIX := 12
else
ifeq ($(shell test -x /usr/bin/clang++ && echo -n true),true)
    SUFFIX :=
else
ifeq ($(shell test -x /usr/bin/clang++-12 && echo -n true),true)
    SUFFIX := -12
endif
endif
endif
CC := clang$(SUFFIX)
CXX := clang++$(SUFFIX)
LLVMCONFIG := llvm-config$(SUFFIX)
LLVMVERSION := $(shell $(CXX) -v |& head -n 1 | cut -d. -f1 | rev | cut -d' ' -f1 | rev)

CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) -frtti
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all-targets orcjit mcjit)
PRJH = ../include/volvox-$(LLVMVERSION).hh

PCH = $(PRJH:%.hh=%.hh.gch)

PROG = $(TARGETDIR)volvox

OBJS := main.o lexer.o parser.o token.o codegen.o types.o

LIB := map.o

SRCS := $(OBJS:%.o=%.cc)

LIBSRC := $(LIB:%.o=../lib/%.c)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) -include-pch $(PCH) $<

%.o: ../lib/%.c
	$(CC) -c $(OPT) $<

all: $(PROG)

$(PCH): $(PRJH) ../include/VolvoxJIT-$(LLVMVERSION).h ../include/volvox.hh
	$(CXX) $(CXXFLAGS) $(OPT) $<

$(PROG): $(OBJS) $(LIB)
	$(CXX) -o $@ $+ $(LDFLAGS) $(OPT) $(LINKEXTRAFLAGS)

.PHONY: clean
clean:
	rm -f $(OBJS)

.PHONY: distclean
distclean:
	rm -f $(PROG) $(PROG).exe $(OBJS) $(LIB) *~ ../include/*~ ../include/*.gch .depend

.depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) > .depend
	$(CC) -MM $(OPT) $(LIBSRC) >> .depend

-include .depend
