# Makefile – compile all litmus tests to human-readable LLVM IR (.ll)
#
# Usage:
#   make          # compile everything into ll/
#   make clean    # remove ll/

CC       := clang-18
CXX      := clang++-18
OPT      := opt-18
CFLAGS   := -O0 -g0 -Xclang -disable-O0-optnone -S -emit-llvm
CXXFLAGS := -O0 -g0 -std=c++17 -Xclang -disable-O0-optnone -S -emit-llvm

LL_DIR   := ll

C_SRCS   := $(wildcard *.c)
CXX_SRCS := $(wildcard *.cpp)

C_TARGETS   := $(patsubst %.c,   $(LL_DIR)/%.ll, $(C_SRCS))
CXX_TARGETS := $(patsubst %.cpp, $(LL_DIR)/%.ll, $(CXX_SRCS))

ALL_TARGETS := $(C_TARGETS) $(CXX_TARGETS)

.PHONY: all clean

all: $(LL_DIR) $(ALL_TARGETS)

$(LL_DIR):
	mkdir -p $@

$(LL_DIR)/%.ll: %.c | $(LL_DIR)
	$(CC) $(CFLAGS) -o - $< | $(OPT) -passes=mem2reg -S -o $@

$(LL_DIR)/%.ll: %.cpp | $(LL_DIR)
	$(CXX) $(CXXFLAGS) -o - $< | $(OPT) -passes=mem2reg -S -o $@

clean:
	rm -rf $(LL_DIR)
