// SPDX-License-Identifier: GPL-2.0
// 42_percpu_int.c — Per-CPU integer (ThreadCachedInt variant with global read)
//
// Non-blocking pattern: PER-CPU INTEGER with global barrier read.
//
// This is a closer analogue to folly::ThreadCachedInt:
//   - Fast path: this_cpu_add (single per-CPU increment, wait-free, no atomic)
//   - Global read: iterate all CPUs + smp_mb() to observe all increments
//
// Unlike litmus 34 (which also caches a *pointer* per-CPU), this litmus
// focuses purely on the counter pattern and the smp_mb() barrier needed to
// get a consistent global snapshot.
//
// The key insight for pointer analysis:
//   per_cpu_ptr(g_pcpu_val, cpu) →{stack/percpu} — NOT a heap edge.
//   But the sum variable is heap-allocated in this variant to showcase
//   the pointer from the fold result back into a caller-visible object.
//
// Key APIs:
//   this_cpu_add(var, n)    — wait-free per-CPU increment        (§2.5)
//   per_cpu(var, cpu)       — access specific CPU's variable     (§2.5)
//   smp_mb()                — full barrier before global fold    (§2.3)
//
// Ordering edges of interest:
//   increment: this_cpu_add(g_pcpu_val, delta) — per-CPU, relaxed
//   global_read: smp_mb(); for_each_possible_cpu → per_cpu(g_pcpu_val, cpu)
//   result: kmalloc() → result →{heap}; result->total = sum; return result

#include <linux/init.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/smp.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-42: per-CPU integer (ThreadCachedInt analogue)");

static DEFINE_PER_CPU(long, g_pcpu_val);

struct pcpu_snapshot {
	long total;
	int  ncpus;
};

/* Wait-free per-CPU increment. */
static void pcpu_int_add(long delta)
{
	this_cpu_add(g_pcpu_val, delta);   /* no atomic, no cache miss */
}

/* Global read: full barrier + sum all CPUs. */
static struct pcpu_snapshot *pcpu_int_read_global(void)
{
	struct pcpu_snapshot *snap;
	int cpu;

	snap = kmalloc(sizeof(*snap), GFP_KERNEL);   /* HeapObjVar */
	if (!snap)
		return NULL;

	snap->total = 0;
	snap->ncpus = 0;

	/*
	 * Full barrier: ensure all prior per-CPU writes (from this or other
	 * CPUs after quiescent synchronisation) are visible before the fold.
	 */
	smp_mb();

	for_each_possible_cpu(cpu) {
		snap->total += per_cpu(g_pcpu_val, cpu);  /* per-CPU read */
		snap->ncpus++;
	}

	return snap;   /* snap →{heap} */
}

static int __init pcpu_int_init(void)
{
	struct pcpu_snapshot *s;

	pcpu_int_add(10);
	pcpu_int_add(5);
	pcpu_int_add(25);

	s = pcpu_int_read_global();                  /* s →{heap} */
	if (s) {
		pr_info("pcpu_int: total=%ld ncpus=%d\n",
			s->total, s->ncpus);
		kfree(s);
	}
	return 0;
}

static void __exit pcpu_int_exit(void)
{
	struct pcpu_snapshot *s = pcpu_int_read_global();  /* s →{heap} */

	if (s) {
		pr_info("pcpu_int: exit total=%ld\n", s->total);
		kfree(s);
	}
}

module_init(pcpu_int_init);
module_exit(pcpu_int_exit);
