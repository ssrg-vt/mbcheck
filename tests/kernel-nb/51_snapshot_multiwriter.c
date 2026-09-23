// SPDX-License-Identifier: GPL-2.0
// 51_snapshot_multiwriter.c — Consistent snapshot with multiple concurrent writers
//
// Non-blocking pattern: CONSISTENT SNAPSHOT (Jayanti, 1993 / Anderson 1994).
//
// A consistent snapshot allows a reader to observe a globally-consistent view
// of N shared variables even while multiple writers update them concurrently.
// The reader repeats the read sequence until it sees the same seqcount on
// both sides (like seqlock), but *multiple independent writers* each protect
// only their own field with their own seqcount.
//
// This pattern is used in:
//   - Linux kernel's timekeeping (struct timekeeper with seqcount)
//   - folly::AtomicStruct<multi-field>
//   - Java's StampedLock optimistic reads
//
// Protocol (double-width seqcount per writer):
//   writer_i():  write_seqcount_begin(sc_i); update field_i;
//                write_seqcount_end(sc_i)
//   reader():    do {
//                  s0 = read_seqcount_begin(sc_0);
//                  s1 = read_seqcount_begin(sc_1);
//                  read field_0, field_1;
//                } while (read_seqcount_retry(sc_0, s0) ||
//                         read_seqcount_retry(sc_1, s1));
//
// The reader must retry if *any* writer was mid-update during the read.
// This is a multi-writer generalisation of litmus 32 (snapshot_seqcount).
//
// Ordering edges of interest:
//   writer_a: kmalloc() → obj_a →{heap}; WRITE_ONCE(g_field_a, obj_a)
//             wrapped in write_seqcount_begin/end(g_sc_a)
//   writer_b: kmalloc() → obj_b →{heap}; WRITE_ONCE(g_field_b, obj_b)
//             wrapped in write_seqcount_begin/end(g_sc_b)
//   reader:   read_seqcount_begin(g_sc_{a,b}); READ_ONCE(g_field_{a,b})
//             read_seqcount_retry(g_sc_{a,b}) → retry loop

#include <linux/init.h>
#include <linux/module.h>
#include <linux/seqlock.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-51: consistent snapshot with multiple independent writers");

struct snap_obj {
	int  id;
	int  value;
};

/* Two independently-written fields, each guarded by its own seqcount. */
static struct snap_obj *g_field_a;
static struct snap_obj *g_field_b;

static seqcount_t g_sc_a = SEQCNT_ZERO(g_sc_a);
static seqcount_t g_sc_b = SEQCNT_ZERO(g_sc_b);

/* Writer A: updates g_field_a under g_sc_a. */
static void snap51_write_a(int val)
{
	struct snap_obj *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return;
	obj->id    = 'A';
	obj->value = val;

	write_seqcount_begin(&g_sc_a);
	WRITE_ONCE(g_field_a, obj);                /* g_field_a →{heap} */
	write_seqcount_end(&g_sc_a);
}

/* Writer B: updates g_field_b under g_sc_b. */
static void snap51_write_b(int val)
{
	struct snap_obj *obj;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);   /* HeapObjVar */
	if (!obj)
		return;
	obj->id    = 'B';
	obj->value = val;

	write_seqcount_begin(&g_sc_b);
	WRITE_ONCE(g_field_b, obj);                /* g_field_b →{heap} */
	write_seqcount_end(&g_sc_b);
}

/* Reader: takes a consistent snapshot of both fields. */
static void snap51_read(int *out_a, int *out_b)
{
	struct snap_obj *pa, *pb;
	unsigned int sa, sb;

	do {
		sa = read_seqcount_begin(&g_sc_a);    /* begin critical window */
		sb = read_seqcount_begin(&g_sc_b);

		pa = READ_ONCE(g_field_a);            /* pa →{heap} */
		pb = READ_ONCE(g_field_b);            /* pb →{heap} */

		if (pa) *out_a = pa->value;
		if (pb) *out_b = pb->value;
	} while (read_seqcount_retry(&g_sc_a, sa) ||
		 read_seqcount_retry(&g_sc_b, sb));   /* retry if any writer interrupted */
}

static int __init snap51_init(void)
{
	/* Two independent writers publish their fields. */
	snap51_write_a(111);
	snap51_write_b(222);
	pr_info("snap51: both writers published\n");
	return 0;
}

static void __exit snap51_exit(void)
{
	struct snap_obj *pa, *pb;
	int va = 0, vb = 0;

	snap51_read(&va, &vb);                     /* consistent snapshot */
	pr_info("snap51: snapshot a=%d b=%d\n", va, vb);

	/* Cleanup: no seqcount needed; no concurrent writers at exit. */
	pa = READ_ONCE(g_field_a);
	pb = READ_ONCE(g_field_b);
	kfree(pa);
	kfree(pb);
}

module_init(snap51_init);
module_exit(snap51_exit);
