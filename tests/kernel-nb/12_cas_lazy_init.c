// SPDX-License-Identifier: GPL-2.0
// 12_cas_lazy_init.c — Lazy singleton initialization (double-checked with CAS)
//
// Non-blocking pattern: LAZY / ONCE INITIALIZATION via CAS.
//
// A global pointer g_singleton starts NULL.  The first caller to need the
// object allocates it; the CAS ensures that exactly one allocation "wins"
// and becomes the singleton.  Losers free their allocation and use the winner.
//
// Fast path (lock-free):
//   p = smp_load_acquire(&g_singleton)
//   if (p != NULL)  →  return p   (no allocation needed)
//
// Slow path (CAS install):
//   candidate = kmalloc(...)
//   init candidate
//   prev = cmpxchg(&g_singleton, NULL, candidate)
//   if (prev != NULL)  →  kfree(candidate); return prev
//   else               →  return candidate   (we won the race)
//
// This is structurally identical to Java's lock-free singleton and to
// folly::Singleton / folly::DelayedInit.
//
// Key APIs (kernel-memory-ordering-apis.md §2.2, §2.4):
//   smp_load_acquire(&p)       — acquire load for fast path check
//   cmpxchg(&p, NULL, new)     — CAS to install (full barrier)
//   smp_store_release(&p, new) — could also be used after a successful CAS
//
// Ordering edges of interest for pointer analysis:
//   slow path: kmalloc() → candidate    HeapObjVar
//              cmpxchg(&g_singleton, NULL, candidate) → g_singleton →{heap}
//   fast path: smp_load_acquire(&g_singleton) → p  p →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-12: lazy singleton via CAS (double-checked lock-free init)");

struct singleton {
	int   magic;
	char  name[16];
};

static struct singleton *g_singleton;

/* Returns the singleton, initialising it on first call. */
static struct singleton *get_singleton(void)
{
	struct singleton *p, *candidate, *prev;

	/* Fast path: already initialised? */
	p = smp_load_acquire(&g_singleton);          /* p →{heap|NULL} */
	if (p)
		return p;                             /* p →{heap} */

	/* Slow path: race to install. */
	candidate = kmalloc(sizeof(*candidate), GFP_KERNEL); /* HeapObjVar */
	if (!candidate)
		return NULL;

	candidate->magic = 0xcafe;
	__builtin_memcpy(candidate->name, "singleton\0\0\0\0\0\0\0", 16);

	/*
	 * CAS: atomically set g_singleton = candidate only if still NULL.
	 * If another CPU won the race, prev != NULL and we free our allocation.
	 */
	prev = cmpxchg(&g_singleton, (struct singleton *)NULL, candidate);
	if (prev) {
		kfree(candidate);   /* lost the race; use prev */
		return prev;        /* prev →{heap} */
	}
	/* We won: g_singleton →{heap} (candidate) */
	return candidate;
}

static int __init lazy_init(void)
{
	struct singleton *s = get_singleton();   /* s →{heap} */

	if (!s)
		return -ENOMEM;
	pr_info("lazy: magic=0x%x name=%s\n", s->magic, s->name);
	return 0;
}

static void __exit lazy_exit(void)
{
	struct singleton *s = smp_load_acquire(&g_singleton); /* s →{heap|NULL} */

	if (s) {
		kfree(s);
	}
}

module_init(lazy_init);
module_exit(lazy_exit);
