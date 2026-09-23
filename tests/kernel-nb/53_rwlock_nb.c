// SPDX-License-Identifier: GPL-2.0
// 53_rwlock_nb.c — Reader–Writer lock with non-blocking fast-path readers
//
// Non-blocking pattern: SCALABLE RW-LOCK (non-blocking readers, blocking writers).
//
// Standard kernel rwlock_t uses atomics internally.  This litmus models the
// *seqlock-free* variant where readers are truly obstruction-free: they use a
// single acquire load of a "writer active" flag and back off only if a writer
// is present.  If no writer is active, readers proceed with zero contention
// (no atomic RMW — only an acquire load).
//
// Pattern:
//   read_lock():   load_acquire(g_writers) == 0 → proceed; else spin
//   read_unlock(): (no action; reads are non-exclusive)
//   write_lock():  fetch_add(g_writers, 1); wait until all readers drain
//                  (g_readers == 0)
//   write_unlock():store_release(g_writers, 0)
//
// The "readers draining" step uses a separate atomic g_readers that each
// reader increments on entry and decrements on exit.  Writers wait for it to
// reach zero.
//
// This pattern covers a combination not present in the existing litmus tests:
//   - Multiple concurrent readers (all non-blocking)
//   - Single writer (blocking only other writers, not readers in flight)
//   - Writer publishes a new heap-allocated object; readers observe it
//
// Ordering edges of interest:
//   write: kmalloc() → g_data →{heap}; smp_store_release(g_data, obj)
//          atomic_fetch_add(g_writers, -1)  → writers gone → readers see obj
//   read:  load_acquire(g_writers) == 0; atomic_fetch_add(g_readers, 1)
//          load_acquire(g_data) → obj →{heap}; atomic_fetch_add(g_readers, -1)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-53: scalable RW-lock — non-blocking fast-path readers");

struct rw_data {
	int  generation;
	char payload[16];
};

/* Shared data pointer — written by writers, read by readers. */
static struct rw_data *g_data;

/* Writer-count and reader-count atomics. */
static atomic_t g_writers = ATOMIC_INIT(0);
static atomic_t g_readers = ATOMIC_INIT(0);

/* ---- Non-blocking reader ------------------------------------------------- */
static void nb_read_lock(void)
{
	/* Spin until no writer is active (obstruction-free read path). */
	while (atomic_read_acquire(&g_writers) != 0)
		cpu_relax();
	/* Announce ourselves as an active reader. */
	atomic_fetch_add(1, &g_readers);
	/* Re-check: if a writer snuck in after our load, back off. */
	while (atomic_read(&g_writers) != 0) {
		atomic_fetch_add(-1, &g_readers);
		while (atomic_read_acquire(&g_writers) != 0)
			cpu_relax();
		atomic_fetch_add(1, &g_readers);
	}
}

static void nb_read_unlock(void)
{
	atomic_fetch_add(-1, &g_readers);               /* announce exit */
}

/* ---- Blocking writer (only blocks other writers, not readers-in-flight) -- */
static void nb_write_lock(void)
{
	/* Announce intent to write (prevents new readers from entering). */
	atomic_fetch_add(1, &g_writers);
	/* Wait for all in-flight readers to drain. */
	while (atomic_read_acquire(&g_readers) != 0)
		cpu_relax();
}

static void nb_write_unlock(void)
{
	atomic_fetch_add(-1, &g_writers);               /* release write intent */
}

/* ---- Writer: publish a new version --------------------------------------- */
static void rw53_write(int gen)
{
	struct rw_data *obj, *old;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);        /* HeapObjVar */
	if (!obj)
		return;
	obj->generation = gen;
	__builtin_memcpy(obj->payload, "gen-data\0\0\0\0\0\0\0\0", 16);

	nb_write_lock();
	old = g_data;
	smp_store_release(&g_data, obj);                /* g_data →{heap} */
	nb_write_unlock();

	kfree(old);  /* previous version no longer reachable by new readers */
}

/* ---- Reader: observe current version ------------------------------------- */
static void rw53_read(void)
{
	struct rw_data *p;

	nb_read_lock();
	p = smp_load_acquire(&g_data);                  /* p →{heap} */
	if (p)
		pr_info("rw53: gen=%d payload=%.8s\n", p->generation, p->payload);
	nb_read_unlock();
}

static int __init rw53_init(void)
{
	rw53_write(1);   /* Writer publishes generation 1. */
	rw53_read();     /* Reader 1 observes gen 1 (non-blocking). */
	rw53_read();     /* Reader 2 observes gen 1 (non-blocking, concurrent). */
	rw53_write(2);   /* Writer upgrades to gen 2. */
	pr_info("rw53: init complete\n");
	return 0;
}

static void __exit rw53_exit(void)
{
	rw53_read();     /* Final reader. */
	kfree(smp_load_acquire(&g_data));
}

module_init(rw53_init);
module_exit(rw53_exit);
