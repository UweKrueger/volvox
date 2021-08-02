LLVMVERSION = -12
CXX = clang++$(LLVMVERSION)
LLVMCONFIG = llvm-config$(LLVMVERSION)
OPT := -O0
CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags) $(OPTF)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all)
PRJH := ../include/volvox.hh
PCH := $(PRJH:%.hh=%.hh.gch)

PROG = ../volvox

OBJS := main.o

SRCS := $(OBJS:%.o=%.cc)

%.o: %.cc $(PCH)
	$(CXX) -c $(CXXFLAGS) $(OPT) -include-pch $(PCH) $<

all: $(PROG)

$(PCH): $(PRJH)
	$(CXX) $(CXXFLAGS) $(OPT) $<

$(PROG): $(OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(OPT) $+

.PHONY: clean
clean:
	rm -f $(OBJS) $(PCH)
