SHELL := /bin/bash
ifeq ($(shell test -x /usr/bin/clang++-13 && echo -n true),true)
    LLVMSUFFIX := -13
else
ifeq ($(shell test -x /usr/bin/clang++-12 && echo -n true),true)
    LLVMSUFFIX := -12
else
ifeq ($(shell test -x /usr/bin/clang++ && echo -n true),true)
    LLVMSUFFIX :=
endif
endif
endif
CXX := clang++$(LLVMSUFFIX)
LLVMCONFIG := llvm-config$(LLVMSUFFIX)
LLVMVERSION := $(shell $(CXX) -v |& head -n 1 | cut -d. -f1 | rev | cut -d' ' -f1 | rev)
OPT := -O0 -g
CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) $(OPTF)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all)
PRJH = ../include/volvox.hh
PCH = $(PRJH:%.hh=%.hh.gch)

PROG = ../volvox

OBJS := main.o lexer.o parser.o token.o

SRCS := $(OBJS:%.o=%.cc)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) -include-pch $(PCH) $<

all: $(PROG)

$(PCH): $(PRJH) ../include/volvox.hh
	$(CXX) $(CXXFLAGS) $(OPT) $<

$(PROG): $(OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(OPT) ../lib/map.o $+

.PHONY: clean
clean:
	rm -f $(OBJS) $(PCH)

.PHONY: depend
depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) > .depend

.depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) > .depend

-include .depend
