// SPDX-License-Identifier: GPL-2.0
// 47_rcu_writer_exclusion.c — RCU with writer–writer exclusion via spinlock
//
// Non-blocking pattern: MULTI-WRITER RCU (Writers serialised; Readers lock-free).
//
// RCU gives readers unconditional lock-free access to the current version of a
// data structure.  Writers must:
//   1. Acquire a mutual-exclusion lock (writer–writer exclusion).
//   2. Copy the current version ("read-copy"), modify the copy.
//   3. Publish the new version via rcu_assign_pointer (release store).
//   4. Wait for all pre-existing RCU read-side critical sections to complete
//      (synchronize_rcu), then free the old version.
//
// This litmus covers the *two-writer* case: both module_init and a helper
// function act as writers; module_exit acts as the final reader/reclaimer.
// The pattern demonstrates writer–writer coordination on top of lock-free
// reader access — a combination absent from litmus 03 (single writer).
//
// Key APIs: spin_lock / spin_unlock (writer serialisation),
//           rcu_assign_pointer (§3 — W-release via WRITE_ONCE+smp_wmb),
//           rcu_dereference (§3 — R-acquire/consume),
//           synchronize_rcu (§3 — waits for all RCU readers to exit).
//
// Ordering edges of interest:
//   writer A: kmalloc() → obj_a →{heap}
//             spin_lock(&g_wlock); rcu_assign_pointer(g_ptr, obj_a); spin_unlock
//   writer B: kmalloc() → obj_b →{heap}
//             spin_lock(&g_wlock); old = rcu_dereference(g_ptr);
//             rcu_assign_pointer(g_ptr, obj_b); spin_unlock
//             synchronize_rcu(); kfree(old)
//   reader:   rcu_read_lock(); p = rcu_dereference(g_ptr) →{heap}; rcu_read_unlock

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-47: multi-writer RCU — writer exclusion via spinlock");

struct rcu_obj {
	int  version;
	char data[16];
};

/* RCU-protected pointer: always points to the current version. */
static struct rcu_obj __rcu *g_ptr;

/* Writer serialisation lock (not needed by readers). */
static DEFINE_SPINLOCK(g_wlock);

/* ---- Writer A: initial publish ------------------------------------------ */
static void rcu47_write_a(void)
{
	struct rcu_obj *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return;
	obj->version = 1;
	__builtin_memcpy(obj->data, "version-one\0\0\0\0\0", 16);

	spin_lock(&g_wlock);
	rcu_assign_pointer(g_ptr, obj);            /* PUBLISH: g_ptr →{heap} */
	spin_unlock(&g_wlock);
}

/* ---- Writer B: read-copy-update ------------------------------------------ */
static void rcu47_write_b(void)
{
	struct rcu_obj *old, *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar (new version) */
	if (!obj)
		return;

	spin_lock(&g_wlock);

	/* Read current version under the writer lock (no RCU read-lock needed
	 * for the writer's own access; we use rcu_dereference_protected). */
	old = rcu_dereference_protected(g_ptr, lockdep_is_held(&g_wlock));

	/* Copy and modify. */
	if (old)
		*obj = *old;
	obj->version = 2;
	__builtin_memcpy(obj->data, "version-two\0\0\0\0\0", 16);

	rcu_assign_pointer(g_ptr, obj);            /* PUBLISH: g_ptr →{heap} (obj) */
	spin_unlock(&g_wlock);

	/* Wait for all readers still on the old version to finish. */
	synchronize_rcu();                         /* RCU grace period */
	kfree(old);                                /* safe to free now */
}

/* ---- Reader: lock-free access -------------------------------------------- */
static void rcu47_read(void)
{
	struct rcu_obj *p;

	rcu_read_lock();
	p = rcu_dereference(g_ptr);                /* CONSUME: p →{heap} */
	if (p)
		pr_info("rcu47: version=%d data=%.16s\n", p->version, p->data);
	rcu_read_unlock();
}

static int __init rcu47_init(void)
{
	/* Two writers run in sequence (simulated); one reader in between. */
	rcu47_write_a();   /* Writer A publishes version 1. */
	rcu47_read();      /* Reader sees version 1 (lock-free). */
	rcu47_write_b();   /* Writer B upgrades to version 2; frees version 1. */
	rcu47_read();      /* Reader sees version 2. */
	pr_info("rcu47: init complete\n");
	return 0;
}

static void __exit rcu47_exit(void)
{
	struct rcu_obj *p;

	/* Final cleanup: retire current pointer. */
	spin_lock(&g_wlock);
	p = rcu_dereference_protected(g_ptr, lockdep_is_held(&g_wlock));
	rcu_assign_pointer(g_ptr, NULL);
	spin_unlock(&g_wlock);

	synchronize_rcu();
	kfree(p);
}

module_init(rcu47_init);
module_exit(rcu47_exit);
