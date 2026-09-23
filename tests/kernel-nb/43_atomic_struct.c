// SPDX-License-Identifier: GPL-2.0
// 43_atomic_struct.c — AtomicStruct: pack a small struct into atomic64_t
//
// Non-blocking pattern: ATOMIC STRUCT (folly::AtomicStruct).
//
// Small structs (≤ 8 bytes on 64-bit) can be atomically swapped by packing
// them into an integer and using atomic64_cmpxchg / atomic64_xchg.  This
// achieves true CAS on a struct without a mutex.
//
// Protocol:
//   Load:  atomic64_read → u64 → memcpy to struct
//   Store: memcpy struct → u64 → atomic64_xchg
//   CAS:   memcpy old/new → u64; atomic64_cmpxchg; compare result
//
// Used in:
//   - folly::AtomicStruct<T> (requires sizeof(T) ≤ 8)
//   - Kernel's atomic64_t with type-punned accesses (e.g., storing two u32s)
//
// The pointer-analysis angle: the "struct" stored here contains a pointer
// field, so each atomic64_cmpxchg implicitly updates a pointer.
//
// Key APIs: atomic64_read, atomic64_cmpxchg, atomic64_xchg (§1.3)
//
// Ordering edges of interest:
//   store: atomic64_xchg(&g_atom, pack(my_ptr, gen)) → g_atom contains ptr
//   load:  atomic64_read(&g_atom) → unpack → ptr →{heap}
//   cas:   atomic64_cmpxchg(&g_atom, old_u64, new_u64) → ptr updated

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-43: AtomicStruct — pack small struct into atomic64_t");

/* The struct to be stored atomically: a pointer + generation counter. */
struct atom_val {
	u32 ptr_lo;   /* lower 32 bits of pointer (or full ptr on ILP32) */
	u32 gen;      /* generation counter */
};

/* On LP64 systems we store a condensed form: low 32 bits of ptr + gen.
 * For full 64-bit pointer, we'd need a 128-bit CAS (CMPXCHG16B / LL-SC pair).
 * This litmus uses a pre-allocated pool index (u32) instead of a raw pointer
 * to keep everything within 8 bytes. */

struct pool_obj {
	int id;
	int data;
};

#define POOL_SIZE 4
static struct pool_obj g_pool[POOL_SIZE];

static atomic64_t g_atom = ATOMIC64_INIT(0);

/* Pack (pool_index, generation) into a u64. */
static u64 as_pack(u32 idx, u32 gen)
{
	return ((u64)gen << 32) | (u64)idx;
}

static u32 as_idx(u64 v) { return (u32)(v & 0xFFFFFFFF); }
static u32 as_gen(u64 v) { return (u32)(v >> 32); }

/* Atomically read the current (pool_index, generation). */
static void as_load(u32 *idx_out, u32 *gen_out)
{
	u64 raw = (u64)atomic64_read(&g_atom);     /* raw encodes ptr+gen */
	*idx_out = as_idx(raw);
	*gen_out = as_gen(raw);
}

/* Atomically store a new (pool_index, generation). */
static void as_store(u32 idx, u32 gen)
{
	atomic64_xchg(&g_atom, (s64)as_pack(idx, gen));   /* g_atom ← new */
}

/* CAS on the packed struct.  Returns true if succeeded. */
static bool as_cas(u32 old_idx, u32 old_gen, u32 new_idx, u32 new_gen)
{
	s64 old_v = (s64)as_pack(old_idx, old_gen);
	s64 new_v = (s64)as_pack(new_idx, new_gen);

	return atomic64_cmpxchg(&g_atom, old_v, new_v) == old_v;
}

static int __init as_init(void)
{
	u32 idx, gen;
	struct pool_obj *p;
	int i;

	/* Initialise pool objects. */
	for (i = 0; i < POOL_SIZE; i++) {
		g_pool[i].id   = i;
		g_pool[i].data = i * 100;
	}

	/* Store pool index 0, generation 0. */
	as_store(0, 0);

	/* Read back. */
	as_load(&idx, &gen);
	p = &g_pool[idx % POOL_SIZE];           /* p →{stack (g_pool)} */
	pr_info("as: load idx=%u gen=%u → id=%d data=%d\n",
		idx, gen, p->id, p->data);

	/* CAS: advance to index 1, generation 1. */
	if (as_cas(idx, gen, 1, 1))
		pr_info("as: CAS succeeded → idx=1 gen=1\n");
	else
		pr_info("as: CAS failed\n");

	return 0;
}

static void __exit as_exit(void)
{
	u32 idx, gen;

	as_load(&idx, &gen);
	pr_info("as: final idx=%u gen=%u\n", idx, gen);
}

module_init(as_init);
module_exit(as_exit);
