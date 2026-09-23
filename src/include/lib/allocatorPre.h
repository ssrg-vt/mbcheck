#pragma once

#include "llvm/IR/Module.h"

// Rewrites calls to kernel heap allocators (kmalloc, vmalloc, devm_kmalloc,
// etc.) into equivalent malloc(size) calls so that SVF's extapi recognises
// them as heap allocations and creates proper HeapObjVar nodes.
//
void normaliseKernelAllocators(llvm::Module &M);
