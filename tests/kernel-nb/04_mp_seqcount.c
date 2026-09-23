// SPDX-License-Identifier: GPL-2.0
// 04_mp_seqcount.c — Message-Passing via seqcount (writer increments sequence counter)
//
// Non-blocking pattern: seqcount-guarded consistent read.
//
// A seqcount is an optimistic-concurrency mechanism: the writer increments a
// counter before and after its critical section (making it odd during the
// write).  The reader samples the counter before and after reading the data;
// if the counter is even and unchanged, the snapshot is consistent.
// Readers *never block*; they simply retry on a torn write.
//
// This pattern shows a pointer g_snap_ptr + payload protected together:
//   Writer: write_seqcount_begin → update ptr + payload → write_seqcount_end
//   Reader: read_seqcount_begin → copy ptr + payload → read_seqcount_retry
//
// Key APIs (from kernel-memory-ordering-apis.md §1.7):
//   write_seqcount_begin(s)   — store-release on seqcount (makes it odd)
//   write_seqcount_end(s)     — store-release on seqcount (makes it even)
//   read_seqcount_begin(s)    — load-acquire, returns seq
//   read_seqcount_retry(s, seq) — load-acquire, true if writer intervened
//
// Folly analogue: no direct equivalent, but matches the seqlock pattern used
//                 in kernel's time-keeping and scheduler.
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → snap             HeapObjVar
//          g_snap_ptr = snap            g_snap_ptr →{heap} (under seqcount)
//   exit:  local_ptr = g_snap_ptr       local_ptr →{heap} (inside retry loop)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/seqlock.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-04: message passing via seqcount-guarded pointer+data");

struct snapshot {
	int   generation;
	long  timestamp;
};

/* Seqcount protects both g_snap_ptr and g_snap_ts together. */
static seqcount_t          g_seq = SEQCNT_ZERO(g_seq);
static struct snapshot    *g_snap_ptr;
static long                g_snap_ts;

static int __init mp04_init(void)
{
	struct snapshot *snap;

	/* [Writer] --------------------------------------------------------- */
	snap = kmalloc(sizeof(*snap), GFP_KERNEL);   /* HeapObjVar */
	if (!snap)
		return -ENOMEM;

	snap->generation = 7;
	snap->timestamp  = 12345678L;

	/*
	 * Begin write section: seqcount becomes odd — readers that sample it
	 * will detect an in-progress write and retry.
	 */
	write_seqcount_begin(&g_seq);

	/* Update the shared pointer and the associated timestamp together. */
	g_snap_ptr = snap;          /* g_snap_ptr →{heap} */
	g_snap_ts  = snap->timestamp;

	/*
	 * End write section: seqcount becomes even — readers will see a
	 * consistent snapshot (or retry if they began while it was odd).
	 */
	write_seqcount_end(&g_seq);

	return 0;
}

static void __exit mp04_exit(void)
{
	struct snapshot *local_ptr;
	long             local_ts;
	unsigned int     seq;

	/* [Reader] — retry loop until we catch an even, stable seqcount ---- */
	do {
		/*
		 * Acquire the current (even) sequence number.  If the writer
		 * is active this returns an odd value and read_seqcount_retry
		 * will force another iteration.
		 */
		seq = read_seqcount_begin(&g_seq);

		/* Read the shared data inside the retry window. */
		local_ptr = g_snap_ptr;    /* local_ptr →{heap} */
		local_ts  = g_snap_ts;

	} while (read_seqcount_retry(&g_seq, seq));

	if (local_ptr)
		pr_info("mp04: gen=%d ts=%ld (snap_ts=%ld)\n",
			local_ptr->generation, local_ptr->timestamp, local_ts);

	kfree(local_ptr);
}

module_init(mp04_init);
module_exit(mp04_exit);
