// SPDX-License-Identifier: GPL-2.0
// 19_double_checked.c — Double-checked locking (lock-free fast path)
//
// Non-blocking pattern: DOUBLE-CHECKED LOCKING with lock-free read path.
//
// The classic double-checked locking pattern has a lock-free fast path and a
// locked slow path for initialization.  To be correct under the C11/C++11
// memory model (and the Linux kernel's equivalent):
//   1. The fast path read must be an acquire load.
//   2. The slow path write must be a release store.
//   3. A full barrier (smp_mb) must separate the lock acquisition from
//      the second check, OR the spinlock acquire itself provides the barrier.
//
// The kernel's spinlock acquire provides an implicit acquire barrier, so:
//   Fast path: p = smp_load_acquire(&g_ptr); if (p) return p;
//   Slow path: spin_lock(&g_lock);
//              p = READ_ONCE(&g_ptr);     // locked re-check
//              if (!p) { p = alloc; smp_store_release(&g_ptr, p); }
//              spin_unlock(&g_lock);
//              return p;
//
// Note: this uses a spinlock for the slow path (so the slow path IS blocking),
// but the fast path is lock-free.  This is the pattern in the kernel's
// get_unused_fd_flags(), crypto subsystem, etc.
//
// Key APIs (kernel-memory-ordering-apis.md §2.2, §3.1):
//   smp_load_acquire(&p)          — acquire load (fast path)
//   smp_store_release(&p, v)      — release store (slow path write)
//   spin_lock / spin_unlock       — implicit acquire/release
//
// Ordering edges of interest for pointer analysis:
//   fast path: smp_load_acquire(&g_ptr) → p     p →{heap} or NULL
//   slow path: kmalloc() → obj              HeapObjVar
//              smp_store_release(&g_ptr, obj)   g_ptr →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-19: double-checked locking (lock-free fast path, locked slow path)");

struct dcl_obj {
	int id;
	int value;
};

static DEFINE_SPINLOCK(g_lock);
static struct dcl_obj *g_ptr;

/* Returns the object, initialising it at most once. */
static struct dcl_obj *dcl_get(void)
{
	struct dcl_obj *p;

	/* Fast path: lock-free acquire load. */
	p = smp_load_acquire(&g_ptr);             /* p →{heap|NULL} */
	if (p)
		return p;                         /* p →{heap} */

	/* Slow path: lock and re-check. */
	spin_lock(&g_lock);                       /* implicit acquire */

	p = READ_ONCE(g_ptr);                     /* p →{heap|NULL} */
	if (!p) {
		/* We are the initialiser. */
		p = kmalloc(sizeof(*p), GFP_KERNEL);  /* HeapObjVar */
		if (p) {
			p->id    = 99;
			p->value = 0x1234;
			/*
			 * Release store: all writes to *p above are ordered
			 * before g_ptr becomes visible to fast-path readers.
			 */
			smp_store_release(&g_ptr, p); /* g_ptr →{heap} */
		}
	}

	spin_unlock(&g_lock);                     /* implicit release */
	return p;                                 /* p →{heap} or NULL */
}

static int __init dcl_init(void)
{
	struct dcl_obj *obj = dcl_get();          /* obj →{heap} */

	if (!obj)
		return -ENOMEM;
	pr_info("dcl: id=%d value=0x%x\n", obj->id, obj->value);
	return 0;
}

static void __exit dcl_exit(void)
{
	struct dcl_obj *p;

	spin_lock(&g_lock);
	p = g_ptr;
	g_ptr = NULL;
	spin_unlock(&g_lock);

	kfree(p);                                 /* p →{heap|NULL} */
}

module_init(dcl_init);
module_exit(dcl_exit);
