LLVMVERSION = -12
CXX = clang++$(LLVMVERSION)
LLVMCONFIG = llvm-config$(LLVMVERSION)
OPT := -O0
CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) $(OPTF)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all)
PRJH = ../include/volvox.hh
PCH = $(PRJH:%.hh=%.hh.gch)

PROG = ../volvox

OBJS := main.o

SRCS := $(OBJS:%.o=%.cc)

%.o: $(PCH) %.cc
	$(CXX) -c $(CXXFLAGS) $(OPT) -include-pch $+

all: $(PROG)

$(PCH): $(PRJH) ../include/VolvoxJIT.h
	$(CXX) $(CXXFLAGS) $(OPT) $<

$(PROG): $(OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(OPT) $+

.PHONY: clean
clean:
	rm -f $(OBJS) $(PCH)
