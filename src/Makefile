SHELL := /bin/bash
ifeq ($(shell test -x /usr/bin/clang++ && echo -n true),true)
    SUFFIX :=
else
ifeq ($(shell test -x /usr/bin/clang++-12 && echo -n true),true)
    SUFFIX := -12
else
ifeq ($(shell test -x /usr/bin/clang++-13 && echo -n true),true)
    SUFFIX := -13
endif
endif
endif
CC := clang$(SUFFIX)
CXX := clang++$(SUFFIX)
LLVMCONFIG := llvm-config$(SUFFIX)
LLVMVERSION := $(shell $(CXX) -v |& head -n 1 | cut -d. -f1 | rev | cut -d' ' -f1 | rev)
OPT := -O0 -ggdb
CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all)
PRJH = ../include/volvox-$(LLVMVERSION).hh
PCH = $(PRJH:%.hh=%.hh.gch)

PROG = ../volvox

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
	$(CXX) -o $@ $(LDFLAGS) $(OPT) $+

.PHONY: clean
clean:
	rm -f $(OBJS) $(PCH)

.PHONY: distclean
distclean:
	rm -f $(PROG) $(OBJS) $(LIB) *~ ../include/*~ ../include/*.gch .depend

.depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) > .depend
	$(CC) -MM $(OPT) $(LIBSRC) >> .depend

-include .depend
