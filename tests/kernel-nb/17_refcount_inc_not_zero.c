// SPDX-License-Identifier: GPL-2.0
// 17_refcount_inc_not_zero.c — Conditional reference increment (resurrection guard)
//
// Non-blocking pattern: CONDITIONAL REFERENCE BUMP (resurrection prevention).
//
// When an object can be concurrently destroyed while other threads are trying
// to acquire a reference to it (e.g. looked up from an RCU-protected hash
// table), refcount_inc_not_zero() provides an atomic "only bump if still alive"
// operation.  If it returns false the object is already dying and the caller
// must not use it.
//
// Typical usage (inside rcu_read_lock / rcu_read_unlock):
//   1. rcu_dereference(table[i]) → obj      — acquire load, obj may be dying
//   2. if (!refcount_inc_not_zero(&obj->rc)) continue  — guard
//   3. rcu_read_unlock()
//   4. use obj safely (we hold a ref)
//   5. refcount_dec_and_test(&obj->rc) → possible free
//
// This prevents the TOCTOU race between "is it alive?" and "grab a ref".
//
// Key APIs (kernel-memory-ordering-apis.md §1.5):
//   refcount_inc_not_zero(r)   — acquire on success; returns bool
//   refcount_dec_and_test(r)   — full barrier, frees on last ref
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → obj          HeapObjVar
//          rcu_assign_pointer(g_tbl, obj)  g_tbl →{heap}
//   lookup: rcu_dereference(g_tbl) → obj   obj →{heap}
//           refcount_inc_not_zero(&obj->rc) → bool  (conditional)
//           kfree(obj) only when refcnt→0

#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-17: resurrection guard via refcount_inc_not_zero in RCU context");

struct live_obj {
	refcount_t rc;
	int        id;
	int        data;
};

/* RCU-protected pointer used as a simple "table entry". */
static struct live_obj __rcu *g_tbl;

/* Look up the object and acquire a reference atomically.  May return NULL. */
static struct live_obj *tbl_get(void)
{
	struct live_obj *obj;

	rcu_read_lock();

	obj = rcu_dereference(g_tbl);   /* obj →{heap|NULL} — acquire load */
	if (obj && !refcount_inc_not_zero(&obj->rc))
		obj = NULL;             /* already at zero; don't use */

	rcu_read_unlock();
	return obj;                     /* obj →{heap} or NULL */
}

static void tbl_put(struct live_obj *obj)
{
	if (refcount_dec_and_test(&obj->rc))
		kfree(obj);
}

static int __init incnz_init(void)
{
	struct live_obj *obj, *got;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return -ENOMEM;

	refcount_set(&obj->rc, 1);
	obj->id   = 42;
	obj->data = 0xff;

	rcu_assign_pointer(g_tbl, obj);            /* g_tbl →{heap} */

	/* Simulate a lookup from another context. */
	got = tbl_get();                           /* got →{heap} */
	if (got) {
		pr_info("incnz: id=%d data=0x%x rc=%u\n",
			got->id, got->data, refcount_read(&got->rc));
		tbl_put(got);
	}
	return 0;
}

static void __exit incnz_exit(void)
{
	struct live_obj *obj;

	/* Remove from the table. */
	obj = rcu_dereference_protected(g_tbl, true);
	rcu_assign_pointer(g_tbl, NULL);
	synchronize_rcu();

	/* Drop the initial reference. */
	if (obj)
		tbl_put(obj);
}

module_init(incnz_init);
module_exit(incnz_exit);
