// SPDX-License-Identifier: GPL-2.0
// 15_xchg_ownership.c — Exclusive ownership transfer via xchg
//
// Non-blocking pattern: OWNERSHIP TRANSFER via atomic swap.
//
// xchg atomically swaps the value of a pointer with a new value and returns
// the old value.  This enables a non-blocking "claim ownership" pattern:
//   - Owner stores a pointer.
//   - Claimer xchg(&g_owner, NULL): if the returned value is non-NULL,
//     it now exclusively owns the pointed-to object.
//   - If the returned value is NULL, someone else already claimed it.
//
// This is used in the kernel for:
//   - Handing off struct sk_buff between driver and protocol layers
//   - Work-stealing in work queues (steal a work item with xchg)
//   - One-shot resource release (irq handler claims the resource)
//
// Key API (kernel-memory-ordering-apis.md §2.4):
//   xchg(ptr, val)  — atomic swap, full barrier; returns old value
//
// Folly analogue: folly::AtomicLinkedList's steal() operation.
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → resource      HeapObjVar
//          smp_store_release(&g_owner, resource)  g_owner →{heap}
//   claim: xchg(&g_owner, NULL) → claimed  claimed →{heap|NULL}
//          if (claimed) use + kfree(claimed)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-15: exclusive ownership transfer via xchg");

struct resource_obj {
	int  type;
	int  value;
};

/* g_owner holds the unclaimed resource; NULL if already claimed. */
static struct resource_obj *g_owner;

static int __init xchg_init(void)
{
	struct resource_obj *resource;

	resource = kmalloc(sizeof(*resource), GFP_KERNEL);  /* HeapObjVar */
	if (!resource)
		return -ENOMEM;

	resource->type  = 3;
	resource->value = 0xbeef;

	/*
	 * Publish: release store so the claimer sees fully-initialised data.
	 * g_owner →{heap}
	 */
	smp_store_release(&g_owner, resource);
	pr_info("xchg: resource published at %p\n", resource);
	return 0;
}

static void __exit xchg_exit(void)
{
	struct resource_obj *claimed;

	/*
	 * Atomically claim the resource.
	 * xchg has full-barrier semantics: the store of NULL is ordered after
	 * all prior memory operations, and the load of g_owner is ordered
	 * before any subsequent uses of `claimed`.
	 *
	 * After this: g_owner == NULL, claimed →{heap} (or NULL if already taken).
	 */
	claimed = xchg(&g_owner, (struct resource_obj *)NULL);

	if (claimed) {
		pr_info("xchg: claimed type=%d value=0x%x\n",
			claimed->type, claimed->value);
		kfree(claimed);
	} else {
		pr_info("xchg: resource already claimed\n");
	}
}

module_init(xchg_init);
module_exit(xchg_exit);
