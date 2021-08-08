SHELL := /bin/bash
ifeq ($(shell test -x /usr/bin/clang++ && echo -n true),true)
    LLVMSUFFIX :=
else
ifeq ($(shell test -x /usr/bin/clang++-12 && echo -n true),true)
    LLVMSUFFIX := -12
else
ifeq ($(shell test -x /usr/bin/clang++-13 && echo -n true),true)
    LLVMSUFFIX := -13
endif
endif
endif
CXX := clang++$(LLVMSUFFIX)
LLVMCONFIG := llvm-config$(LLVMSUFFIX)
LLVMVERSION := $(shell $(CXX) -v |& head -n 1 | cut -d. -f1 | rev | cut -d' ' -f1 | rev)
OPT := -O0
CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) $(OPTF)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all)
PRJH = ../include/volvox-$(LLVMVERSION).hh
PCH = $(PRJH:%.hh=%.hh.gch)

PROG = ../volvox

OBJS := main.o lexer.o parser.o

SRCS := $(OBJS:%.o=%.cc)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) -include-pch $(PCH) $<

all: $(PROG)

$(PCH): $(PRJH) ../include/VolvoxJIT-$(LLVMVERSION).h
	$(CXX) $(CXXFLAGS) $(OPT) $<

$(PROG): $(OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(OPT) $+

.PHONY: clean
clean:
	rm -f $(OBJS) $(PCH)

.PHONY: depend
depend:
	$(CXX) -MM $(CXXFLAGS) $(OPT) $(SRCS) > .depend

.depend: depend

-include .depend
