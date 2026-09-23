// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in fn1
// P1: fn2 calls fn1, then R(data)
// True negative: not two threads: program order does the work.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-20: call-graph subsumption");

static int g_a;
static int g_b;

void sync20_fn1(int v)
{
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
}
EXPORT_SYMBOL_GPL(sync20_fn1);

int sync20_fn2(int v)
{
	sync20_fn1(v);			/* callee, not a peer thread */
	return g_a + g_b;		/* R(data) */
}
EXPORT_SYMBOL_GPL(sync20_fn2);
