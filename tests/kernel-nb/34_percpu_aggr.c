// SPDX-License-Identifier: GPL-2.0
// 34_percpu_aggr.c — Per-CPU aggregation (ThreadCachedInt / counter sharding)
//
// Non-blocking pattern: PER-CPU COUNTER AGGREGATION.
//
// Per-CPU variables allow each CPU to update its own counter without any
// atomic operation or cache line bouncing.  Reading the global sum requires
// summing all per-CPU values with proper barriers.
//
// This is the kernel's answer to folly::ThreadCachedInt: threads write to
// their local slot (relaxed, no cross-CPU traffic) and a global reader sums
// all slots with appropriate ordering.
//
// The non-blocking property: updates are always wait-free (a single non-atomic
// store to a per-CPU slot).  Reads are lock-free (sum over all CPUs with
// get_cpu_var barriers).
//
// Additionally, this litmus shows a pointer published through a per-CPU
// variable, which is interesting for pointer analysis:
//   each_cpu_ptr(g_pcpu_ptr, cpu) →{heap}  (different heap per CPU)
//
// Key APIs (kernel-memory-ordering-apis.md §2.5):
//   this_cpu_write(var, val)   — relaxed per-CPU store
//   this_cpu_read(var)         — relaxed per-CPU load
//   this_cpu_add(var, val)     — relaxed per-CPU RMW
//   get_cpu_var / put_cpu_var  — preemption-safe access (implies smp_mb)
//
// Ordering edges of interest:
//   write: kmalloc() → obj; this_cpu_write(g_pcpu_ptr, obj)  → per-CPU →{heap}
//   read:  get_cpu_var(g_pcpu_ptr) → p  → p →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/smp.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-34: per-CPU aggregation (ThreadCachedInt analogue)");

struct percpu_data {
	int  cpu_id;
	long contribution;
};

/* Per-CPU counter (integer) */
static DEFINE_PER_CPU(long, g_pcpu_cnt);

/* Per-CPU pointer: each CPU stores a pointer to its own heap object. */
static DEFINE_PER_CPU(struct percpu_data *, g_pcpu_ptr);

/* Relaxed per-CPU increment — wait-free, no cross-CPU traffic. */
static void pcpu_inc(long delta)
{
	this_cpu_add(g_pcpu_cnt, delta);   /* relaxed RMW on local CPU */
}

/* Aggregate the global sum across all CPUs. */
static long pcpu_sum(void)
{
	long total = 0;
	int cpu;

	/* get_cpu_var provides preemption-safe access with an smp_mb. */
	for_each_possible_cpu(cpu)
		total += per_cpu(g_pcpu_cnt, cpu);

	return total;
}

static int __init pcpu_init(void)
{
	struct percpu_data *obj;
	int cpu = smp_processor_id();

	/* Allocate a heap object for the current CPU. */
	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return -ENOMEM;
	obj->cpu_id      = cpu;
	obj->contribution = 100;

	/*
	 * Publish the heap pointer into the per-CPU slot.
	 * this_cpu_write is relaxed but preemption-safe.
	 */
	this_cpu_write(g_pcpu_ptr, obj);           /* per-CPU →{heap} */

	/* Update the per-CPU counter (wait-free). */
	pcpu_inc(10);
	pcpu_inc(20);
	pcpu_inc(30);

	pr_info("pcpu: cpu=%d local_ptr=%p sum=%ld\n",
		cpu, this_cpu_read(g_pcpu_ptr), pcpu_sum());
	return 0;
}

static void __exit pcpu_exit(void)
{
	struct percpu_data *p;
	int cpu = smp_processor_id();

	p = this_cpu_read(g_pcpu_ptr);             /* p →{heap|NULL} */
	if (p) {
		pr_info("pcpu: cpu=%d contribution=%ld total=%ld\n",
			p->cpu_id, p->contribution, pcpu_sum());
		kfree(p);
		this_cpu_write(g_pcpu_ptr, NULL);
	}
}

module_init(pcpu_init);
module_exit(pcpu_exit);
