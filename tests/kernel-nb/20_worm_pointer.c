// SPDX-License-Identifier: GPL-2.0
// 20_worm_pointer.c — Write-once-read-many pointer (immutable after publish)
//
// Non-blocking pattern: WORM (Write-Once-Read-Many) pointer.
//
// An object is allocated and fully initialised, then a pointer to it is
// published exactly once using WRITE_ONCE.  After that the pointer is read-only
// and can be accessed without any synchronisation — all readers observe the
// same, fully-initialised object.
//
// Contrast with message-passing (01-03): WORM has no ongoing producer/consumer
// relationship; the pointer is immutable once set.
//
// The ordering requirement:
//   1. Complete all initialisations of *obj
//   2. smp_wmb()            — store-store: ensure obj writes visible before ptr
//   3. WRITE_ONCE(g_immut, obj)  — publish the pointer
//
// Readers:
//   p = READ_ONCE(g_immut)  — volatile read; if non-NULL, safe to use directly
//   (No per-read barrier needed on the reader side once published on a TSO
//    architecture, but smp_read_barrier_depends() / READ_ONCE is needed for DEC Alpha
//    and is good practice.)
//
// Used in the kernel for: module parameter pointers, firmware blobs, compiled
// BPF programs (after JIT), and crypto algorithm structs.
//
// Folly analogue: folly::once_flag + initialized constant object.
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → obj          HeapObjVar
//          WRITE_ONCE(g_immut, obj)  g_immut →{heap} (volatile store)
//   exit:  READ_ONCE(g_immut) → p   p →{heap} (volatile load)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-20: WORM pointer via WRITE_ONCE + READ_ONCE");

struct immutable_cfg {
	int  version;
	int  flags;
	char label[16];
};

/* Written once at module init; thereafter read-only. */
static const struct immutable_cfg *g_immut;

static int __init worm_init(void)
{
	struct immutable_cfg *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return -ENOMEM;

	/* Fully initialise before publishing. */
	obj->version = 2;
	obj->flags   = 0x7;
	__builtin_memcpy(obj->label, "immutable\0\0\0\0\0\0\0", 16);

	/*
	 * Store-store barrier: all object field writes above must be
	 * completed before the pointer publish below.
	 */
	smp_wmb();

	/*
	 * Publish: volatile store — the pointer is now visible.
	 * After this point g_immut is read-only.
	 * SVF sees: g_immut →{heap} via a plain `store volatile ptr`.
	 */
	WRITE_ONCE(g_immut, obj);                  /* g_immut →{heap} */

	pr_info("worm: published version=%d flags=0x%x\n",
		obj->version, obj->flags);
	return 0;
}

static void __exit worm_exit(void)
{
	const struct immutable_cfg *p;

	/*
	 * Volatile load: SVF sees a direct `load volatile ptr`.
	 * p →{heap}
	 */
	p = READ_ONCE(g_immut);                    /* p →{heap|NULL} */
	if (p) {
		pr_info("worm: version=%d label=%s\n", p->version, p->label);
		kfree(p);
	}
}

module_init(worm_init);
module_exit(worm_exit);
