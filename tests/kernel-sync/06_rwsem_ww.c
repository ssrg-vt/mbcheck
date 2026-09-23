// SPDX-License-Identifier: GPL-2.0
// P0: down_write -> W(data) -> up_write
// P1: down_write -> W(data) -> up_write
// True negative: down_write is exclusive.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/rwsem.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-06: rwsem writer-writer exclusion");

static DECLARE_RWSEM(g_rwsem);
static int g_a;
static int g_b;

void sync06_write_one(int v)
{
	down_write(&g_rwsem);
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
	up_write(&g_rwsem);
}
EXPORT_SYMBOL_GPL(sync06_write_one);

void sync06_write_two(int v)
{
	down_write(&g_rwsem);
	g_a = v * 2;			/* W(data) */
	g_b = v * 2 + 1;		/* W(data) */
	up_write(&g_rwsem);
}
EXPORT_SYMBOL_GPL(sync06_write_two);
