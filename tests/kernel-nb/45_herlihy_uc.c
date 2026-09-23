// SPDX-License-Identifier: GPL-2.0
// 45_herlihy_uc.c — Herlihy's Universal Construction (wait-free)
//
// Non-blocking pattern: HERLIHY UNIVERSAL CONSTRUCTION (Herlihy 1991).
//
// The Universal Construction turns any sequential object into a wait-free
// concurrent one, using:
//   1. A shared "consensus" sequence (here: global epoch counter)
//   2. A per-thread "announce" array where threads publish their operation
//   3. A helping protocol: before doing its own work, each thread helps
//      any announced operation at the current epoch
//
// This litmus uses a simplified single-"slot" variant to keep the pointer
// analysis tractable:
//   - Threads publish an operation record to announce[tid]
//   - They CAS the global state pointer (g_state) to the new state
//   - If a thread is slow, the "helping" thread applies the operation on its
//     behalf by CAS-ing g_state
//
// Key APIs: atomic_long_cmpxchg (§1.3), smp_store_release (§2.2),
//           smp_load_acquire (§2.2), cmpxchg on pointer (§2.4)
//
// Ordering edges of interest:
//   announce: kmalloc() → op →{heap}; smp_store_release(&announce[tid], op)
//   help:     smp_load_acquire(&announce[tid]) → op →{heap}
//             cmpxchg(&g_state, old_state, new_state)
//   apply:    new_state →{heap}; g_state →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-45: Herlihy Universal Construction (simplified wait-free)");

#define N_THREADS  2   /* simulated threads */

enum uc_op_type { UC_ADD, UC_RESET };

struct uc_op {
	enum uc_op_type type;
	int             arg;
	int             result;   /* written by the applier */
	atomic_t        done;     /* 0 = pending, 1 = applied */
};

/* The sequential state: a simple integer counter. */
struct uc_state {
	int value;
};

/* Global state pointer (published via CAS). */
static struct uc_state *g_state;

/* Per-thread announce array: each thread publishes its pending operation. */
static struct uc_op *g_announce[N_THREADS];

/* Apply an operation to a state, returning a new state object. */
static struct uc_state *uc_apply(const struct uc_state *s,
				 struct uc_op *op)
{
	struct uc_state *ns;

	ns = kmalloc(sizeof(*ns), GFP_KERNEL);   /* HeapObjVar (new state) */
	if (!ns)
		return NULL;

	switch (op->type) {
	case UC_ADD:
		ns->value     = s->value + op->arg;
		op->result    = ns->value;
		break;
	case UC_RESET:
		ns->value     = 0;
		op->result    = 0;
		break;
	}
	return ns;   /* ns →{heap} */
}

/* Execute an operation wait-free using the universal construction. */
static int uc_execute(int tid, enum uc_op_type type, int arg)
{
	struct uc_op    *my_op, *ann_op;
	struct uc_state *old_state, *new_state;
	int t;

	my_op = kmalloc(sizeof(*my_op), GFP_KERNEL);   /* HeapObjVar */
	if (!my_op)
		return -ENOMEM;

	my_op->type = type;
	my_op->arg  = arg;
	atomic_set(&my_op->done, 0);

	/* Announce my operation. */
	smp_store_release(&g_announce[tid], my_op);    /* g_announce[tid] →{heap} */

	/* Helping phase: try to help others (simplified: just ourselves). */
	for (t = 0; t < N_THREADS; t++) {
		ann_op = smp_load_acquire(&g_announce[t]); /* ann_op →{heap|NULL} */
		if (!ann_op || atomic_read(&ann_op->done))
			continue;

		/*
		 * Help this operation: apply it to the current state.
		 */
		old_state = smp_load_acquire(&g_state);    /* old_state →{heap} */
		new_state = uc_apply(old_state, ann_op);   /* new_state →{heap} */
		if (!new_state)
			continue;

		if (cmpxchg(&g_state, old_state, new_state) == old_state) {
			/* We installed the new state; mark the op as done. */
			atomic_set_release(&ann_op->done, 1);
			kfree(old_state);    /* old_state freed after CAS */
		} else {
			kfree(new_state);    /* someone else got there first */
		}
	}

	/* Wait for my own op to be applied (possibly by a helper). */
	while (!atomic_read_acquire(&my_op->done))
		cpu_relax();

	t = my_op->result;
	kfree(my_op);
	return t;
}

static int __init uc_init(void)
{
	int r;

	g_state = kmalloc(sizeof(*g_state), GFP_KERNEL);  /* HeapObjVar */
	if (!g_state)
		return -ENOMEM;
	g_state->value = 0;

	r = uc_execute(0, UC_ADD,   10);
	pr_info("uc: ADD 10 → %d\n", r);

	r = uc_execute(0, UC_ADD,   5);
	pr_info("uc: ADD 5  → %d\n", r);

	r = uc_execute(1, UC_RESET, 0);
	pr_info("uc: RESET  → %d\n", r);

	return 0;
}

static void __exit uc_exit(void)
{
	struct uc_state *s = smp_load_acquire(&g_state);  /* s →{heap} */

	pr_info("uc: final value=%d\n", s ? s->value : -1);
	kfree(s);
}

module_init(uc_init);
module_exit(uc_exit);
