// SPDX-License-Identifier: GPL-2.0
// P0: spin_lock -> W(data) -> spin_unlock
// P1: spin_lock -> R(data) -> spin_unlock
// True negative: one lock, so the sections are mutually exclusive.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-02: spinlock mutual exclusion");

static DEFINE_SPINLOCK(g_lock);
static int g_a;
static int g_b;

void sync02_write(int v)
{
	spin_lock(&g_lock);
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
	spin_unlock(&g_lock);
}
EXPORT_SYMBOL_GPL(sync02_write);

int sync02_read(void)
{
	int sum;

	spin_lock(&g_lock);
	sum = g_a + g_b;		/* R(data) */
	spin_unlock(&g_lock);
	return sum;
}
EXPORT_SYMBOL_GPL(sync02_read);
