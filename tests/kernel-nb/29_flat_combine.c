// SPDX-License-Identifier: GPL-2.0
// 29_flat_combine.c — Flat combining: delegate operations to a combiner
//
// Non-blocking pattern: FLAT COMBINING (Hendler et al., 2010).
//
// Flat combining reduces synchronisation cost by having threads publish their
// operations to a shared "publication list" and then a single "combiner"
// thread executes all pending operations on the underlying data structure.
// The combiner is chosen by a CAS on a global lock; all other threads spin
// on their publication slot.
//
// Pattern:
//   1. Allocate/reuse a publication record (pub_rec).
//   2. Write the request into pub_rec (WRITE_ONCE), set pub_rec->req = PENDING.
//   3. Link pub_rec into the global publication list (CAS on g_pub_list).
//   4. Try to become combiner: CAS(g_lock, 0, 1).
//      - If winner: scan list, execute all PENDING ops, mark DONE, unlock.
//      - If loser:  spin on pub_rec->req until it becomes DONE.
//   5. Read result from pub_rec->result.
//
// This is the kernel analogue of folly::FlatCombining.  The kernel uses a
// similar pattern in percpu_ref and the futex hash table.
//
// Ordering edges of interest:
//   publish: kmalloc() → pub     HeapObjVar
//            WRITE_ONCE(pub->req, PENDING)  ordered before CAS on list
//            CAS g_pub_list → pub linked
//   combine: scan list → pub →{heap}; execute; smp_store_release(pub->req, DONE)
//   requester: smp_load_acquire(pub->req) == DONE → pub->result →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-29: flat combining — delegate ops via publication list");

#define FC_IDLE    0
#define FC_PENDING 1
#define FC_DONE    2

struct pub_rec {
	atomic_t       state;     /* FC_IDLE / FC_PENDING / FC_DONE */
	int            operand;   /* input */
	int            result;    /* output (written by combiner) */
	struct pub_rec *next;     /* publication list linkage */
};

/* The underlying data structure (a simple counter, protected by combiner). */
static int g_counter;

/* Publication list head (singly-linked, lock-free prepend). */
static struct pub_rec *g_pub_list;

/* Combiner lock: 0 = free, 1 = held by combiner. */
static atomic_t g_lock = ATOMIC_INIT(0);

/* Link pub_rec into the global publication list. */
static void fc_publish(struct pub_rec *pub)
{
	struct pub_rec *old;

	do {
		old       = READ_ONCE(g_pub_list);   /* old →{heap|NULL} */
		pub->next = old;
	} while (cmpxchg(&g_pub_list, old, pub) != old);
	/* g_pub_list →{heap} (pub) */
}

/* Combiner: scan list and execute all PENDING operations. */
static void fc_combine(void)
{
	struct pub_rec *p;

	for (p = READ_ONCE(g_pub_list); p; p = READ_ONCE(p->next)) {  /* p →{heap} */
		if (atomic_read(&p->state) == FC_PENDING) {
			/* Execute the operation on the underlying structure. */
			g_counter += p->operand;
			p->result  = g_counter;

			/* Release store: result visible before DONE. */
			atomic_set_release(&p->state, FC_DONE);   /* DONE */
		}
	}
}

/* Submit an add-to-counter operation via flat combining. */
static int fc_add(int operand)
{
	struct pub_rec *pub;

	pub = kmalloc(sizeof(*pub), GFP_KERNEL);   /* HeapObjVar */
	if (!pub)
		return -ENOMEM;

	atomic_set(&pub->state, FC_IDLE);
	pub->operand = operand;
	pub->result  = 0;
	pub->next    = NULL;

	/* Publish the request. */
	atomic_set_release(&pub->state, FC_PENDING);
	fc_publish(pub);                           /* g_pub_list →{heap} */

	/* Race to become combiner. */
	if (atomic_cmpxchg(&g_lock, 0, 1) == 0) {
		/* We are the combiner. */
		fc_combine();
		atomic_set_release(&g_lock, 0);    /* unlock */
	} else {
		/* Spin until combiner marks our request DONE. */
		while (atomic_read_acquire(&pub->state) != FC_DONE)
			cpu_relax();
	}

	return pub->result;   /* pub →{heap}; result written by combiner */
}

static int __init fc_init(void)
{
	int r1, r2;

	g_counter = 0;
	r1 = fc_add(10);
	r2 = fc_add(5);
	pr_info("fc: r1=%d r2=%d counter=%d\n", r1, r2, g_counter);
	return 0;
}

static void __exit fc_exit(void)
{
	/* Drain the publication list. */
	struct pub_rec *p, *next;

	p = xchg(&g_pub_list, (struct pub_rec *)NULL);  /* p →{heap|NULL} */
	while (p) {                                      /* p →{heap} */
		next = p->next;                          /* next →{heap|NULL} */
		kfree(p);
		p = next;
	}
	pr_info("fc: final counter=%d\n", g_counter);
}

module_init(fc_init);
module_exit(fc_exit);
