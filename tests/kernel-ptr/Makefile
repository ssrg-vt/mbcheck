# ── Module list ────────────────────────────────────────────────────────────
# shared_global is the original hand-written example; 01-25 are the kernel
# adaptations of the pointer-analysis litmus tests from tmp/tests/.
MODULES := \
	shared_global
# 	01_simple_addr \
# 	02_two_ptrs_same_obj \
# 	03_copy_propagation \
# 	04_flow_overwrite \
# 	05_conditional_branch \
# 	06_heap_malloc \
# 	07_two_heaps \
# 	08_pointer_to_pointer \
# 	09_array_decay \
# 	10_struct_field_ptr \
# 	11_ptr_to_struct \
# 	12_return_ptr_static \
# 	13_function_arg_write \
# 	14_global_multi_fn \
# 	15_function_ptr \
# 	16_static_local \
# 	17_array_of_ptrs \
# 	18_struct_of_ptrs \
# 	19_linked_list \
# 	20_void_ptr_cast \
# 	21_loop_addr \
# 	22_multi_level_heap \
# 	23_swap_ptrs \
# 	24_chain_deref \
# 	25_union_ptr

obj-m := $(patsubst %,%.o,$(MODULES))

KDIR          ?= $(error set KDIR to a built Linux kernel source tree)
CLANG_VERSION := 18
ARCH          := arm64

.PHONY: all module ir asm clean

all: module ir asm

# ── Build all .ko files with the kernel's own Makefile (LLVM toolchain) ────
# V=1 gives verbose output; the log is parsed by the ir/asm targets to
# extract the exact compiler flags the kernel used for each source file.
module:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=-$(CLANG_VERSION) ARCH=$(ARCH) V=1 \
	    modules 2>&1 | tee .build.log

# ── Emit mem2reg-normalised LLVM IR for every module ───────────────────────
# For each module: grep its compile command from .build.log, strip the
# "-c -o <mod>.o <mod>.c" suffix to isolate the preprocessor/arch flags,
# then replay with -S -emit-llvm and pipe through mem2reg.
ir: module
	@for mod in $(MODULES); do \
	    grep "clang-$(CLANG_VERSION).*$${mod}\.c" .build.log | head -1 | \
	        sed "s|.*clang-$(CLANG_VERSION)||" | \
	        sed "s|-c -o $${mod}\.o $${mod}\.c.*||" | \
	        tr -s ' ' > .cflags_$${mod}; \
	    eval clang-$(CLANG_VERSION) $$(cat .cflags_$${mod}) \
			-fno-inline -fno-inline-functions -fno-optimize-sibling-calls \
	        -Xclang -disable-O0-optnone -g0 -S -emit-llvm -o - $${mod}.c \
	        | opt-$(CLANG_VERSION) -passes=mem2reg -S -o $${mod}.ll; \
	    echo "  IR  $${mod}.ll"; \
	done

# ── Emit assembly for every module (optional, useful for cross-checking) ───
# asm: module
# 	@for mod in $(MODULES); do \
# 	    grep "clang-$(CLANG_VERSION).*$${mod}\.c" .build.log | head -1 | \
# 	        sed "s|.*clang-$(CLANG_VERSION)||" | \
# 	        sed "s|-c -o $${mod}\.o $${mod}\.c.*||" | \
# 	        tr -s ' ' > .cflags_$${mod}; \
# 	    eval clang-$(CLANG_VERSION) $$(cat .cflags_$${mod}) \
# 	        -S -o $${mod}.s $${mod}.c; \
# 	    echo "  ASM $${mod}.s"; \
# 	done

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=-$(CLANG_VERSION) ARCH=$(ARCH) clean
	rm -f $(patsubst %,%.ll,$(MODULES)) \
	      $(patsubst %,%.s,$(MODULES)) \
	      $(patsubst %,.cflags_%,$(MODULES)) \
	      .build.log

