// SPDX-License-Identifier: GPL-2.0
// 32_snapshot_seqcount.c — Consistent multi-word snapshot via seqcount
//
// Non-blocking pattern: MULTI-WORD CONSISTENT SNAPSHOT.
//
// When multiple independent words (e.g., a pointer + timestamp + flags) must
// be read as a consistent atomic unit, and the writer updates them together,
// a seqcount provides the synchronisation without a lock on the reader side.
//
// This pattern appears in:
//   - Kernel timekeeping (struct timekeeper: xtime + wall_to_monotonic + ...)
//   - Scheduler load tracking (struct sched_avg: util_avg, load_avg, ...)
//   - Network device statistics (multi-word stats snapshot)
//
// Protocol:
//   Writer: write_seqcount_begin → update N words → write_seqcount_end
//   Reader: do { seq=read_seqcount_begin; read N words } while(read_seqcount_retry)
//
// Key distinguishing feature from seqlock_rw (22): this snapshot involves
// a pointer AND multiple scalar words — pointer analysis must track that
// the locally captured pointer g_snap.ptr is derived from the shared heap.
//
// Key APIs: write_seqcount_begin/end, read_seqcount_begin/retry (§1.7)
//
// Ordering edges of interest:
//   write: kmalloc() → neo; write_seqcount_begin; g_ptr=neo; g_ts=...; end
//          g_ptr →{heap}
//   read:  read_seqcount_begin; snap.ptr = g_ptr; retry
//          snap.ptr →{heap} (consistent snapshot)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/seqlock.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-32: multi-word consistent snapshot via seqcount");

struct tracked_obj {
	int  id;
	long value;
};

/* The multi-word shared state. */
static seqcount_t          g_seq    = SEQCNT_ZERO(g_seq);
static struct tracked_obj *g_ptr;
static u64                 g_ts;
static u32                 g_flags;

/* Local snapshot: captures all three words consistently. */
struct snap {
	struct tracked_obj *ptr;
	u64                 ts;
	u32                 flags;
};

/* Writer: atomically update pointer + timestamp + flags under seqcount. */
static void snap_write(struct tracked_obj *neo, u64 ts, u32 flags)
{
	write_seqcount_begin(&g_seq);   /* odd: readers will retry */
	WRITE_ONCE(g_ptr,   neo);       /* g_ptr →{heap} */
	WRITE_ONCE(g_ts,    ts);
	WRITE_ONCE(g_flags, flags);
	write_seqcount_end(&g_seq);     /* even: snapshot is stable */
}

/* Reader: retry until seqcount is even and unchanged. */
static void snap_read(struct snap *out)
{
	unsigned int seq;

	do {
		seq       = read_seqcount_begin(&g_seq);  /* acquire */
		out->ptr  = READ_ONCE(g_ptr);             /* out->ptr →{heap|NULL} */
		out->ts   = READ_ONCE(g_ts);
		out->flags = READ_ONCE(g_flags);
	} while (read_seqcount_retry(&g_seq, seq));    /* retry if writer active */
}

static int __init snap_init(void)
{
	struct tracked_obj *neo;

	neo = kmalloc(sizeof(*neo), GFP_KERNEL);  /* HeapObjVar */
	if (!neo)
		return -ENOMEM;
	neo->id    = 1;
	neo->value = 0xdeadbeef;

	snap_write(neo, 999999999ULL, 0x5);
	pr_info("snap: write done\n");
	return 0;
}

static void __exit snap_exit(void)
{
	struct snap s;

	snap_read(&s);                            /* s.ptr →{heap|NULL} */

	if (s.ptr)
		pr_info("snap: id=%d value=0x%lx ts=%llu flags=0x%x\n",
			s.ptr->id, s.ptr->value, s.ts, s.flags);

	kfree(g_ptr);
}

module_init(snap_init);
module_exit(snap_exit);
