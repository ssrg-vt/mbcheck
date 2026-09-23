// SPDX-License-Identifier: GPL-2.0
// 13_fetch_add_refcnt.c — Fetch-add reference counting (acquire inc / release dec)
//
// Non-blocking pattern: MANUAL REFERENCE COUNTING via fetch-and-add.
//
// Reference counting is the most common non-blocking reclamation pattern.
// The rules are:
//   get():  atomic_inc(&obj->refcnt)           — relaxed (or acquire)
//   put():  if (atomic_dec_and_test(&obj->refcnt))  — full barrier on last dec
//               → free(obj)                   — safe: refcnt reached zero
//
// The full barrier inside atomic_dec_and_test ensures that all uses of the
// object by this thread are ordered before the free.
//
// The fetch-add variant (atomic_fetch_add) makes the return value (old count)
// available so callers can detect special transitions (e.g., 0→1 resurrection).
//
// Key APIs (kernel-memory-ordering-apis.md §1.1):
//   atomic_fetch_add(i, v)        — add, return OLD value; full barrier
//   atomic_dec_and_test(v)        — dec, test == 0; full barrier
//   atomic_read(v)                — relaxed load (for debugging)
//
// Folly analogue: folly::MakeRef / folly::RefPtr reference counting.
//
// Ordering edges of interest for pointer analysis:
//   make_obj: kmalloc() → obj         HeapObjVar
//             atomic_set(&refcnt, 1)   initial refcount
//   get_obj:  atomic_fetch_add(1,ref) → old  tracks g_obj liveness
//             return g_obj             g_obj →{heap}
//   put_obj:  atomic_dec_and_test → true → kfree(obj)   obj freed

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-13: manual ref-counting via atomic_fetch_add / atomic_dec_and_test");

struct ref_obj {
	atomic_t refcnt;
	int      data;
};

static struct ref_obj *g_obj;

static struct ref_obj *obj_get(void)
{
	int old;

	if (!g_obj)
		return NULL;

	/*
	 * Increment and capture the old count.
	 * If old == 0 the object is already being destroyed — bail.
	 */
	old = atomic_fetch_add(1, &g_obj->refcnt);  /* full barrier */
	if (old == 0) {
		/* Resurrection attempt: undo and signal failure. */
		atomic_dec(&g_obj->refcnt);
		return NULL;
	}
	return g_obj;   /* g_obj →{heap} */
}

static void obj_put(struct ref_obj *obj)
{
	/*
	 * Decrement and test: returns true if refcnt reached 0.
	 * The full barrier ensures all prior uses are ordered before kfree.
	 */
	if (atomic_dec_and_test(&obj->refcnt))  /* full barrier */
		kfree(obj);
}

static int __init fetch_init(void)
{
	struct ref_obj *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return -ENOMEM;

	atomic_set(&obj->refcnt, 1);
	obj->data = 55;
	g_obj = obj;                               /* g_obj →{heap} */

	/* Simulate an extra reference. */
	obj = obj_get();                           /* obj →{heap} */
	if (obj)
		pr_info("fetch_add_refcnt: data=%d refcnt=%d\n",
			obj->data, atomic_read(&obj->refcnt));

	return 0;
}

static void __exit fetch_exit(void)
{
	/* Drop the extra reference taken in init. */
	if (g_obj)
		obj_put(g_obj);

	/* Drop the initial reference: may free the object. */
	if (g_obj)
		obj_put(g_obj);
}

module_init(fetch_init);
module_exit(fetch_exit);
