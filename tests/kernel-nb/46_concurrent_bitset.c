// SPDX-License-Identifier: GPL-2.0
// 46_concurrent_bitset.c — Concurrent bitset (atomic bit operations)
//
// Non-blocking pattern: CONCURRENT BITSET (folly::ConcurrentBitSet).
//
// A bitset whose individual bits can be set, cleared, and tested atomically.
// Each word is an unsigned long; the kernel provides atomic bit operations
// that use LL/SC (AArch64) or LOCK CMPXCHG (x86) under the hood.
//
// Non-blocking: all operations on a single bit are lock-free (single atomic
// RMW).  Multi-bit operations (bulk set via atomic_or) are also lock-free.
//
// Used in:
//   - folly::ConcurrentBitSet
//   - Linux kernel bitmap (bits/linux/bitmap.h) — but the kernel variant
//     uses non-atomic ops; this litmus uses atomic bit ops explicitly.
//   - eBPF bloom filter maps (atomic bit manipulations)
//   - Network driver RX/TX ring descriptor bitmaps
//
// Key APIs (kernel-memory-ordering-apis.md §1.4):
//   test_and_set_bit(nr, addr)    — atomic TAS, returns old bit value
//   test_and_clear_bit(nr, addr)  — atomic TAC, returns old bit value
//   test_and_change_bit(nr, addr) — atomic toggle, returns old bit value
//   set_bit(nr, addr)             — atomic set (no return)
//   clear_bit(nr, addr)           — atomic clear (no return)
//   test_bit(nr, addr)            — non-atomic test (for read-only)
//
// Pointer-analysis angle: g_bitset points to a heap-allocated array of
// unsigned longs, making every bit operation a potential alias through g_bitset.
//
// Ordering edges of interest:
//   alloc:  kmalloc() → g_bitset →{heap}; g_bitset loaded before each op
//   TAS:    test_and_set_bit(nr, g_bitset) → g_bitset →{heap} modified
//   TAC:    test_and_clear_bit(nr, g_bitset) → g_bitset →{heap} modified
//   toggle: test_and_change_bit(nr, g_bitset) → g_bitset →{heap} modified
//   bulk:   atomic_or on individual words of g_bitset

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/bitmap.h>
#include <linux/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-46: concurrent bitset (atomic bit operations)");

#define BITSET_NBITS   64
#define BITSET_NWORDS  BITS_TO_LONGS(BITSET_NBITS)

/* Heap-allocated bitset (array of unsigned long). */
static unsigned long *g_bitset;

/* Atomic set a bit. */
static void cbs_set(int nr)
{
	set_bit(nr, g_bitset);                     /* g_bitset →{heap} */
}

/* Atomic clear a bit. */
static void cbs_clear(int nr)
{
	clear_bit(nr, g_bitset);                   /* g_bitset →{heap} */
}

/* Atomic test-and-set: returns old value. */
static int cbs_test_and_set(int nr)
{
	return test_and_set_bit(nr, g_bitset);     /* g_bitset →{heap} */
}

/* Atomic test-and-clear: returns old value. */
static int cbs_test_and_clear(int nr)
{
	return test_and_clear_bit(nr, g_bitset);   /* g_bitset →{heap} */
}

/* Atomic toggle: returns old value. */
static int cbs_toggle(int nr)
{
	return test_and_change_bit(nr, g_bitset);  /* g_bitset →{heap} */
}

/* Bulk atomic OR: set a mask of bits in a single word (word index widx). */
static void cbs_bulk_set(int widx, unsigned long mask)
{
	/* atomic_long_or is lock-free (LL/SC or LOCK OR). */
	atomic_long_or((long)mask,
		       (atomic_long_t *)&g_bitset[widx]); /* g_bitset →{heap} */
}

/* Non-atomic read (safe only under quiescence or for advisory checks). */
static bool cbs_read(int nr)
{
	return test_bit(nr, g_bitset);             /* g_bitset →{heap} */
}

static int __init cbs_init(void)
{
	int old;

	g_bitset = kcalloc(BITSET_NWORDS, sizeof(unsigned long),
			   GFP_KERNEL);                    /* HeapObjVar */
	if (!g_bitset)
		return -ENOMEM;

	/* Set bits 0, 5, 63. */
	cbs_set(0);
	cbs_set(5);
	cbs_set(63);
	pr_info("cbs: after set: bit0=%d bit5=%d bit63=%d\n",
		(int)cbs_read(0), (int)cbs_read(5), (int)cbs_read(63));

	/* Test-and-set bit 5 (already set → returns 1). */
	old = cbs_test_and_set(5);
	pr_info("cbs: TAS bit5 old=%d\n", old);

	/* Test-and-clear bit 63. */
	old = cbs_test_and_clear(63);
	pr_info("cbs: TAC bit63 old=%d now=%d\n", old, (int)cbs_read(63));

	/* Toggle bit 10. */
	old = cbs_toggle(10);
	pr_info("cbs: toggle bit10 old=%d now=%d\n", old, (int)cbs_read(10));

	/* Bulk set bits 32-39 (mask = 0xFF in word 0 at offset 32). */
	cbs_bulk_set(0, 0xFFUL << 32);
	pr_info("cbs: bulk_set word0 → word0=0x%lx\n", g_bitset[0]);

	return 0;
}

static void __exit cbs_exit(void)
{
	unsigned long *p = g_bitset;   /* p →{heap} */

	g_bitset = NULL;
	kfree(p);
}

module_init(cbs_init);
module_exit(cbs_exit);
