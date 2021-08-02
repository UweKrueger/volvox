LLVMVERSION = -12
CXX = clang++$(LLVMVERSION)
LLVMCONFIG = llvm-config$(LLVMVERSION)
OPT := -O0
CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) $(OPTF)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all)
PRJH = ../include/volvox.hh
PCH = $(PRJH:%.hh=%.hh.gch)

PROG = ../volvox

OBJS := main.o lexer.o parser.o

SRCS := $(OBJS:%.o=%.cc)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) -include-pch $(PCH) $<

all: $(PROG)

$(PCH): $(PRJH) ../include/VolvoxJIT.h
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
