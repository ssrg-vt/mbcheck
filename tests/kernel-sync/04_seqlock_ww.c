// SPDX-License-Identifier: GPL-2.0
// P0: write_seqlock -> W(data) -> write_sequnlock
// P1: write_seqlock -> W(data) -> write_sequnlock
// True negative: the seqlock write side is a spinlock, so writers exclude each other.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/seqlock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-04: seqlock writer-writer exclusion");

static DEFINE_SEQLOCK(g_seq);
static int g_a;
static int g_b;

void sync04_write_one(int v)
{
	write_seqlock(&g_seq);
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
	write_sequnlock(&g_seq);
}
EXPORT_SYMBOL_GPL(sync04_write_one);

void sync04_write_two(int v)
{
	write_seqlock(&g_seq);
	g_a = v * 2;			/* W(data) */
	g_b = v * 2 + 1;		/* W(data) */
	write_sequnlock(&g_seq);
}
EXPORT_SYMBOL_GPL(sync04_write_two);
