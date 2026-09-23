// SPDX-License-Identifier: GPL-2.0
// 16_refcount_get_put.c — refcount_t get/put with last-reference callback
//
// Non-blocking pattern: SATURATING REFERENCE COUNT via refcount_t.
//
// refcount_t wraps atomic_t with overflow and underflow protection (the count
// saturates instead of wrapping, preventing use-after-free via misuse).
// The non-blocking reclamation pattern is:
//   init:   refcount_set(&obj->rc, 1)    — initial ref
//   get:    refcount_inc(&obj->rc)       — relaxed add (no ordering)
//   put:    refcount_dec_and_test(&obj->rc)  — full barrier; frees on last ref
//
// refcount_dec_and_test returns true exactly once (for the thread that drops
// the last reference), which is when it is safe to free the object.
//
// This differs from manual atomic_t refcounting (pattern 13) by providing
// KCSAN / KASAN / overflow hardening.
//
// Key APIs (kernel-memory-ordering-apis.md §1.5):
//   refcount_set(r, n)         — relaxed store
//   refcount_inc(r)            — relaxed increment
//   refcount_dec_and_test(r)   — full barrier, returns true on last drop
//   refcount_read(r)           — relaxed load
//
// Ordering edges of interest for pointer analysis:
//   make:  kmalloc() → obj          HeapObjVar
//          g_shared = obj            g_shared →{heap}
//   get:   g_shared → obj           obj →{heap} (borrowed ref)
//   put:   dec_and_test → kfree(obj)  obj freed when refcnt==0

#include <linux/init.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-16: refcount_t get/put with last-reference detection");

struct shared_obj {
	refcount_t rc;
	int        val;
};

static struct shared_obj *g_shared;

/* Bump refcount and return the object (or NULL if destroyed). */
static struct shared_obj *shared_get(void)
{
	struct shared_obj *obj = smp_load_acquire(&g_shared); /* obj →{heap|NULL} */

	if (!obj)
		return NULL;

	/*
	 * inc_not_zero: only increment if the count is > 0.
	 * Avoids resurrection of an object already at zero.
	 */
	if (!refcount_inc_not_zero(&obj->rc))
		return NULL;

	return obj;   /* obj →{heap} */
}

/* Drop a reference; free if last. */
static void shared_put(struct shared_obj *obj)
{
	/*
	 * Full barrier: all accesses to obj->val etc. above are ordered before
	 * the kfree below.  Returns true for the last dropper.
	 */
	if (refcount_dec_and_test(&obj->rc))
		kfree(obj);
}

static int __init refcnt_init(void)
{
	struct shared_obj *obj, *borrowed;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);  /* HeapObjVar */
	if (!obj)
		return -ENOMEM;

	refcount_set(&obj->rc, 1);   /* initial ref */
	obj->val = 77;
	smp_store_release(&g_shared, obj);        /* g_shared →{heap} */

	/* Simulate another user taking a reference. */
	borrowed = shared_get();                  /* borrowed →{heap} */
	if (borrowed)
		pr_info("refcnt: val=%d rc=%u\n",
			borrowed->val, refcount_read(&borrowed->rc));

	/* Drop the borrowed reference. */
	if (borrowed)
		shared_put(borrowed);

	return 0;
}

static void __exit refcnt_exit(void)
{
	struct shared_obj *obj = smp_load_acquire(&g_shared); /* obj →{heap|NULL} */

	/* Drop the initial reference from init; frees if no other holders. */
	if (obj)
		shared_put(obj);
}

module_init(refcnt_init);
module_exit(refcnt_exit);
