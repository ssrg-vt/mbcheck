// SPDX-License-Identifier: GPL-2.0
// P0: spin_lock -> W(data) -> spin_unlock
// P1: R(data), no lock held
// True negative: one shared location, so no second access can be seen out of order.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-19: conservative locked-writer/unlocked-reader");

static DEFINE_SPINLOCK(g_lock);
static int g_counter;

void sync19_write(int v)
{
	spin_lock(&g_lock);
	g_counter = v;			/* W(data) */
	spin_unlock(&g_lock);
}
EXPORT_SYMBOL_GPL(sync19_write);

int sync19_read(void)
{
	return READ_ONCE(g_counter);	/* R(data), no lock held */
}
EXPORT_SYMBOL_GPL(sync19_read);
