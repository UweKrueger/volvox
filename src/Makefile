LLVMVERSION = -12
CXX = clang++$(LLVMVERSION)
LLVMCONFIG = llvm-config$(LLVMVERSION)
CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags)
LDFLAGS := $(shell $(LLVMCONFIG) --ldflags --libs all)

OPT = -O3
PROG = ../volvox

OBJS := main.o

SRCS := $(OBJS:%.o=%.cc)

all: $(PROG)

$(PROG): $(OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(OPT) $+

.PHONY: clean
clean:
	rm -r $(OBJS)
