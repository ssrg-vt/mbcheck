// SPDX-License-Identifier: GPL-2.0
// 25_generation_counter.c — Generation counter (versioned pointer update)
//
// Non-blocking pattern: VERSIONED / GENERATION COUNTER.
//
// A generation counter is an integer incremented monotonically by the writer
// before and after a pointer update.  Readers compare the counter before and
// after reading; if equal and even, they have a consistent snapshot.  This is
// the pointer-sized variant of seqcount without the kernel seqcount_t type.
//
// This pattern appears in:
//   - Linux scheduler's nohz logic (tick_nohz_idle_got_tick)
//   - Kernel time subsystem (timekeeper_lock + tk_core.seq)
//   - folly's AtomicStruct versioning
//
// The update protocol:
//   Writer: fetch_add(1) [odd] → update pointer → fetch_add(1) [even]
//   Reader: loop { load gen (acquire); read ptr; load gen (acquire); retry if changed }
//
// Key APIs:
//   atomic_long_fetch_add(1, &g_gen)      — inc, return old (full barrier, §1.3)
//   smp_load_acquire(&g_gen)              — acquire load of generation
//   smp_store_release(&g_ptr, new)        — release store of pointer
//
// Ordering edges of interest:
//   write: fetch_add (odd) → smp_store_release(&g_ptr, neo) → fetch_add (even)
//   read:  smp_load_acquire(&g_gen) → g_ptr → smp_load_acquire(&g_gen)
//          result: local_ptr →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-25: generation counter (versioned pointer) pattern");

struct gen_record {
	int  id;
	long data;
};

static atomic_long_t     g_gen = ATOMIC_LONG_INIT(0); /* even = stable */
static struct gen_record *g_ptr;

/* Writer: atomically replace g_ptr, using generation counter for readers. */
static void gen_write(int id, long data)
{
	struct gen_record *neo, *old;

	neo = kmalloc(sizeof(*neo), GFP_KERNEL);   /* HeapObjVar */
	if (!neo)
		return;
	neo->id   = id;
	neo->data = data;

	old = g_ptr;

	/* Begin write: generation becomes odd → readers will retry. */
	atomic_long_fetch_add(1, &g_gen);          /* full barrier */

	/* Publish new pointer under the odd generation. */
	smp_store_release(&g_ptr, neo);            /* g_ptr →{heap} */

	/* End write: generation becomes even → readers may proceed. */
	atomic_long_fetch_add(1, &g_gen);          /* full barrier */

	kfree(old);
}

/* Reader: optimistic loop — retries if generation changed during read. */
static struct gen_record *gen_read_snapshot(struct gen_record *buf)
{
	long g1, g2;
	struct gen_record *p;

	do {
		g1 = smp_load_acquire((long *)&g_gen);    /* acquire */
		if (g1 & 1)
			continue;                         /* odd: writer active */

		p = smp_load_acquire(&g_ptr);             /* p →{heap|NULL} */
		if (!p)
			return NULL;

		buf->id   = p->id;
		buf->data = p->data;

		g2 = smp_load_acquire((long *)&g_gen);    /* acquire */
	} while (g1 != g2);

	return buf;  /* consistent snapshot in *buf */
}

static int __init gen_init(void)
{
	gen_write(1, 0xdeadbeef);
	gen_write(2, 0xcafebabe);
	pr_info("gen: two writes done\n");
	return 0;
}

static void __exit gen_exit(void)
{
	struct gen_record snap;

	if (gen_read_snapshot(&snap))              /* snap is a local copy */
		pr_info("gen: id=%d data=0x%lx\n", snap.id, snap.data);

	kfree(g_ptr);
}

module_init(gen_init);
module_exit(gen_exit);
