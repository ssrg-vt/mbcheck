// SPDX-License-Identifier: GPL-2.0
// 22_seqlock_rw.c — seqlock_t: concurrent readers + exclusive writer
//
// Non-blocking pattern: SEQLOCK (optimistic concurrent readers, non-blocking fast path).
//
// seqlock_t combines a spinlock (for exclusive writers) and a seqcount (for
// optimistic readers).  Readers are *non-blocking* on the fast path: they
// read the seqcount before and after their critical section.  If the count
// is even and unchanged, the read is consistent.  Writers acquire the spin
// lock (exclusive) but do not block readers — readers simply retry.
//
// Pattern from kernel-memory-ordering-apis.md §1.7:
//   write_seqlock(sl)     — spin_lock + seqcount begin (odd)
//   write_sequnlock(sl)   — seqcount end (even) + spin_unlock
//   read_seqlock_excl(sl) — exclusive reader (blocks writers)
//   read_seqretry(sl,seq) — non-blocking retry check
//
// Used in: kernel time-keeping (jiffies, xtime), scheduler runqueues, VDSO.
// Folly analogue: no direct, but matches folly's RWSpinLock read path.
//
// Ordering edges of interest for pointer analysis:
//   write: kmalloc() → record        HeapObjVar
//          g_rec = record             g_rec →{heap}  (inside write_seqlock)
//   read:  read_seqretry loop → local_rec  local_rec →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/seqlock.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-22: seqlock_t concurrent reader / exclusive writer");

struct time_record {
	u64  seconds;
	u32  nanoseconds;
	u32  generation;
};

static DEFINE_SEQLOCK(g_sl);
static struct time_record *g_rec;

/* Writer: update the time record atomically under the seqlock. */
static void seqlock_write(u64 sec, u32 nsec, u32 gen)
{
	write_seqlock(&g_sl);           /* spin_lock + seqcount begin (odd) */

	g_rec->seconds     = sec;
	g_rec->nanoseconds = nsec;
	g_rec->generation  = gen;

	write_sequnlock(&g_sl);         /* seqcount end (even) + spin_unlock */
}

/* Non-blocking optimistic reader: retries on writer interference. */
static void seqlock_read(u64 *sec, u32 *nsec, u32 *gen)
{
	struct time_record *local;
	unsigned int seq;

	do {
		seq = read_seqbegin(&g_sl);       /* load-acquire of seqcount */
		local = g_rec;                    /* local →{heap} */
		*sec  = local->seconds;
		*nsec = local->nanoseconds;
		*gen  = local->generation;
	} while (read_seqretry(&g_sl, seq)); /* retry if writer intervened */
}

static int __init seqlock22_init(void)
{
	struct time_record *rec;

	rec = kmalloc(sizeof(*rec), GFP_KERNEL);   /* HeapObjVar */
	if (!rec)
		return -ENOMEM;

	rec->seconds     = 0;
	rec->nanoseconds = 0;
	rec->generation  = 0;
	g_rec = rec;                               /* g_rec →{heap} */

	/* Simulate a writer update. */
	seqlock_write(1234567890ULL, 500000000U, 1U);
	pr_info("seqlock: write done\n");
	return 0;
}

static void __exit seqlock_exit(void)
{
	u64  sec;
	u32  nsec, gen;

	seqlock_read(&sec, &nsec, &gen);           /* non-blocking read */
	pr_info("seqlock: sec=%llu nsec=%u gen=%u\n", sec, nsec, gen);

	kfree(g_rec);
}

module_init(seqlock22_init);
module_exit(seqlock_exit);
