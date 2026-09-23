// SPDX-License-Identifier: GPL-2.0
// P0: W(data) -> complete(flag)
// P1: wait_for_completion(flag) -> R(data)
// True negative: complete/wait is a release-acquire pair; no smp_* appears in the source.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/completion.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-08: completion handoff");

static DECLARE_COMPLETION(g_done);
static int g_a;
static int g_b;

void sync08_produce(int v)
{
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
	complete(&g_done);		/* signal, carries release */
}
EXPORT_SYMBOL_GPL(sync08_produce);

int sync08_consume(void)
{
	wait_for_completion(&g_done);	/* wait, carries acquire */
	return g_a + g_b;		/* R(data) */
}
EXPORT_SYMBOL_GPL(sync08_consume);
