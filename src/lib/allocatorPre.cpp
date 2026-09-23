#include "lib/allocatorPre.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

namespace
{

struct KernelAllocInfo
{
    llvm::StringRef name;
    unsigned        sizeArgIdx; // which argument carries the byte-size
};

static constexpr KernelAllocInfo kKernelAllocs[] = {
    // kmalloc inline forms (kernel 6.x with __noprof suffix)
    {"__kmalloc_cache_noprof",               2}, // (cache*, flags, size)
    {"__kmalloc_noprof",                     0}, // (size, flags)
    {"__kmalloc_node_noprof",                0}, // (size, flags, node)
    {"__kmalloc_node_track_caller_noprof",   0},
    {"__kmalloc_track_caller_noprof",        0},
    // kzalloc / kvmalloc inline forms
    {"__kvmalloc_node_noprof",               0}, // (size, flags, node)
    {"kvmalloc_node_noprof",                 0},
    // vmalloc family
    {"vmalloc",                              0}, // (size)
    {"vzalloc",                              0},
    {"vmalloc_user",                         0},
    {"vmalloc_node",                         0}, // (size, node)
    {"vmalloc_32",                           0},
    {"vmalloc_32_user",                      0},
    // device-managed
    {"devm_kmalloc",                         1}, // (dev*, size, flags)
    {"devm_kzalloc",                         1},
    {"devm_kvmalloc",                        1},
    // slab cache (size comes from the cache descriptor, not an argument)
    {"kmem_cache_alloc",                     1}, // (cache*, flags) — size unknown; use 1
    {"kmem_cache_alloc_noprof",              1},
    {"kmem_cache_alloc_node_noprof",         1},
    // page allocator (size = PAGE_SIZE << order, not directly available)
    {"__get_free_pages",                     0},
    {"get_zeroed_page",                      0},
    {"alloc_pages_exact",                    0}, // (size, flags)
};

} // anonymous namespace

void normaliseKernelAllocators(llvm::Module &M)
{
    llvm::LLVMContext &Ctx    = M.getContext();
    llvm::Type        *i64    = llvm::Type::getInt64Ty(Ctx);
    llvm::Type        *voidPtr = llvm::PointerType::getUnqual(Ctx);

    // Declare malloc if not already present.
    llvm::FunctionCallee mallocFn = M.getOrInsertFunction(
        "malloc",
        llvm::FunctionType::get(voidPtr, {i64}, /*isVarArg=*/false));

    llvm::SmallVector<llvm::CallInst *, 32> toReplace;

    for (auto &F : M)
        for (auto &BB : F)
            for (auto &I : BB)
            {
                auto *CI = llvm::dyn_cast<llvm::CallInst>(&I);
                if (!CI)
                    continue;
                llvm::Function *callee = CI->getCalledFunction();
                if (!callee)
                    continue;
                for (const auto &info : kKernelAllocs)
                    if (callee->getName() == info.name)
                    {
                        toReplace.push_back(CI);
                        break;
                    }
            }

    for (llvm::CallInst *CI : toReplace)
    {
        llvm::Function  *callee = CI->getCalledFunction();
        llvm::StringRef  name   = callee->getName();

        const KernelAllocInfo *info = nullptr;
        for (const auto &e : kKernelAllocs)
            if (e.name == name) { info = &e; break; }

        llvm::IRBuilder<> B(CI);
        llvm::Value *sizeArg;
        if (info->sizeArgIdx < CI->arg_size())
        {
            llvm::Value *raw = CI->getArgOperand(info->sizeArgIdx);
            sizeArg = (raw->getType() == i64)
                        ? raw
                        : B.CreateZExtOrTrunc(raw, i64, "alloc.size");
        }
        else
        {
            // Size not available (e.g. kmem_cache_alloc); use a dummy 1.
            sizeArg = llvm::ConstantInt::get(i64, 1);
        }

        // Create malloc(size) in place of the original call.
        llvm::CallInst *mallocCI = B.CreateCall(mallocFn, {sizeArg}, CI->getName());
        mallocCI->setDebugLoc(CI->getDebugLoc());

        // Replace all uses of the old call result with the new one.
        CI->replaceAllUsesWith(mallocCI);
        CI->eraseFromParent();
    }
}
