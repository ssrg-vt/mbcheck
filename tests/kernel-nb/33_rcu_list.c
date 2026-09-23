// SPDX-License-Identifier: GPL-2.0
// 33_rcu_list.c — RCU-protected intrusive linked list (hlist_rcu)
//
// Non-blocking pattern: RCU-PROTECTED INTRUSIVE LIST.
//
// The kernel's hlist_rcu / list_rcu family provides a non-blocking linked
// list where readers traverse without any lock.  Writers use a spinlock for
// mutual exclusion between writers, but readers never block writers.
//
// Insert protocol:
//   spin_lock; hlist_add_head_rcu(node, head); spin_unlock
//   (hlist_add_head_rcu internally does smp_wmb + WRITE_ONCE on ->next)
//
// Delete protocol:
//   spin_lock; hlist_del_rcu(node); spin_unlock; synchronize_rcu; kfree
//
// Lookup protocol (RCU read side — lock-free):
//   rcu_read_lock(); hlist_for_each_entry_rcu(pos, head, member) { ... }
//   rcu_read_unlock();
//
// This litmus uses a simple global hlist_head and manually shows the
// publish/consume pointer flow for a single node to make the analysis crisp.
//
// Key APIs: rcu_assign_pointer, rcu_dereference, synchronize_rcu (§3.4)
//           spin_lock/unlock (§3.1)
//
// Ordering edges of interest:
//   insert: kmalloc() → node →{heap}; rcu_assign_pointer(head->first, node)
//   lookup: rcu_dereference(head->first) → node →{heap}
//   delete: synchronize_rcu(); kfree(node) — node freed after grace period

#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/list.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-33: RCU-protected intrusive hlist (lock-free readers)");

struct rcu_entry {
	int              key;
	int              value;
	struct hlist_node node;
	struct rcu_head  rcu;   /* for call_rcu deferred free */
};

static DEFINE_SPINLOCK(g_list_lock);
static struct hlist_head g_list = HLIST_HEAD_INIT;

/* Writer: insert under spinlock, publish via rcu_assign_pointer internally. */
static int rcu_list_insert(int key, int value)
{
	struct rcu_entry *e;

	e = kmalloc(sizeof(*e), GFP_KERNEL);   /* HeapObjVar */
	if (!e)
		return -ENOMEM;
	e->key   = key;
	e->value = value;

	spin_lock(&g_list_lock);
	/*
	 * hlist_add_head_rcu: does WRITE_ONCE on ->pprev and ->next with
	 * an implicit smp_wmb() before the pointer store, equivalent to
	 * rcu_assign_pointer on the list linkage.
	 */
	hlist_add_head_rcu(&e->node, &g_list);  /* e →{heap} linked */
	spin_unlock(&g_list_lock);

	return 0;
}

/* Reader: traverse without any lock (non-blocking). */
static struct rcu_entry *rcu_list_find(int key)
{
	struct rcu_entry *pos;

	rcu_read_lock();
	/*
	 * hlist_for_each_entry_rcu uses rcu_dereference() internally:
	 * each ->next traversal is an acquire load.
	 */
	hlist_for_each_entry_rcu(pos, &g_list, node) {  /* pos →{heap} */
		if (pos->key == key) {
			rcu_read_unlock();
			return pos;                      /* pos →{heap} */
		}
	}
	rcu_read_unlock();
	return NULL;
}

/* Writer: remove + grace-period + free. */
static void rcu_list_delete(int key)
{
	struct rcu_entry *pos;

	spin_lock(&g_list_lock);
	hlist_for_each_entry_rcu(pos, &g_list, node) {
		if (pos->key == key) {
			hlist_del_rcu(&pos->node);
			spin_unlock(&g_list_lock);
			synchronize_rcu();             /* wait for all readers */
			kfree(pos);                    /* pos →{heap} freed */
			return;
		}
	}
	spin_unlock(&g_list_lock);
}

static int __init rcu_list_init(void)
{
	struct rcu_entry *found;

	rcu_list_insert(1, 100);
	rcu_list_insert(2, 200);
	rcu_list_insert(3, 300);

	found = rcu_list_find(2);              /* found →{heap} */
	if (found)
		pr_info("rcu_list: found key=2 value=%d\n", found->value);

	return 0;
}

static void __exit rcu_list_exit(void)
{
	rcu_list_delete(1);
	rcu_list_delete(2);
	rcu_list_delete(3);
	pr_info("rcu_list: all entries deleted\n");
}

module_init(rcu_list_init);
module_exit(rcu_list_exit);
