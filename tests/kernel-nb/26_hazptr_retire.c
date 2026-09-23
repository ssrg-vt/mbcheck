// SPDX-License-Identifier: GPL-2.0
// 26_hazptr_retire.c — Hazard pointer safe memory reclamation
//
// Non-blocking pattern: HAZARD POINTERS (Michael, 2004).
//
// A hazard pointer (HP) is a per-thread "I am using this pointer" declaration.
// Before a reader dereferences a pointer loaded from a shared location, it
// publishes the address into its hazard slot with a release store.  The
// allocator (writer / reclaimer) scans all hazard slots before freeing;
// if the pointer appears in any slot, reclamation is deferred.
//
// Three-step reader protocol:
//   1. p  = READ_ONCE(g_shared)                — load candidate
//   2. WRITE_ONCE(g_hp, p)                     — announce hazard (release)
//   3. smp_mb()                                — full barrier
//   4. q  = READ_ONCE(g_shared)                — reload; validate p == q
//   5. if (p != q) goto 1;  else use p safely
//
// Reclaimer protocol:
//   WRITE_ONCE(g_shared, NULL)                 — unlink
//   smp_mb()                                   — full barrier
//   if (g_hp == old_ptr) defer; else kfree(old_ptr)
//
// Linux kernel uses: list_lru, call_rcu (RCU is effectively 1-hazard-slot
// per-CPU EBR).  folly::HazptrDomain/HazptrHolder is the folly equivalent.
//
// Ordering edges of interest:
//   reader: READ_ONCE(g_shared) → p; WRITE_ONCE(g_hp, p); smp_mb(); re-read
//           p →{heap} after validation
//   writer: WRITE_ONCE(g_shared, NULL); smp_mb(); scan g_hp; kfree if safe

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-26: hazard pointer safe reclamation pattern");

struct hazptr_obj {
	int  id;
	long value;
};

/* The shared pointer being protected. */
static struct hazptr_obj *g_shared;

/*
 * The hazard slot: a reader writes its currently-observed pointer here
 * before dereferencing it.  In a real implementation this is per-CPU/thread.
 */
static struct hazptr_obj *g_hp;

/* Acquire the object via hazard pointer protocol. Returns NULL if gone. */
static struct hazptr_obj *hp_acquire(void)
{
	struct hazptr_obj *p, *q;

retry:
	p = READ_ONCE(g_shared);            /* p →{heap|NULL} */
	if (!p)
		return NULL;

	/*
	 * Announce: publish p as hazardous BEFORE the validation re-read.
	 * WRITE_ONCE prevents compiler reordering.
	 */
	WRITE_ONCE(g_hp, p);               /* hazard announced */

	/*
	 * Full barrier: the hazard store (above) must be globally visible
	 * before we re-read g_shared (below).  This prevents a TOCTOU where
	 * the reclaimer sees g_hp == NULL between our two reads.
	 */
	smp_mb();

	q = READ_ONCE(g_shared);           /* q →{heap|NULL} — re-validate */
	if (p != q)
		goto retry;                /* g_shared changed; announce again */

	return p;                          /* p →{heap} — safe to use */
}

/* Release our hazard slot. */
static void hp_release(void)
{
	WRITE_ONCE(g_hp, NULL);            /* clear hazard */
	smp_mb();                          /* order: clear before reclaimer scans */
}

/* Reclaim: unlink + scan hazard slots + conditionally free. */
static void hp_retire(struct hazptr_obj *obj)
{
	/*
	 * Full barrier: our unlink (WRITE_ONCE g_shared = NULL in caller)
	 * must be visible before we inspect g_hp.
	 */
	smp_mb();

	if (READ_ONCE(g_hp) == obj) {
		/* Still hazardous — defer (in a real implementation, add to
		 * a pending list and retry when g_hp changes). */
		pr_info("hazptr: %p is still hazardous — deferring\n", obj);
		kfree(obj);   /* simplified: just free for module exit */
	} else {
		kfree(obj);   /* safe: no reader has a hazard on this pointer */
	}
}

static int __init hp_init(void)
{
	struct hazptr_obj *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return -ENOMEM;
	obj->id    = 1;
	obj->value = 0xc0de;

	smp_store_release(&g_shared, obj);         /* g_shared →{heap} */
	pr_info("hazptr: published %p\n", obj);
	return 0;
}

static void __exit hp_exit(void)
{
	struct hazptr_obj *old, *p;

	/* Reader: safely acquire a reference via hazard pointer. */
	p = hp_acquire();                          /* p →{heap} */
	if (p) {
		pr_info("hazptr: read id=%d value=0x%lx\n", p->id, p->value);
		hp_release();
	}

	/* Writer: unlink and reclaim. */
	old = xchg(&g_shared, (struct hazptr_obj *)NULL); /* old →{heap} */
	if (old)
		hp_retire(old);
}

module_init(hp_init);
module_exit(hp_exit);
