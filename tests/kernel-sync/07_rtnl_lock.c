// SPDX-License-Identifier: GPL-2.0
// P0: rtnl_lock -> W(data) -> rtnl_unlock
// P1: rtnl_lock -> R(data) -> rtnl_unlock
// True negative: RTNL is a subsystem-global mutex, not visible from the data.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-07: RTNL subsystem-global lock");

static int g_a;
static int g_b;

void sync07_write(int v)
{
	rtnl_lock();
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
	rtnl_unlock();
}
EXPORT_SYMBOL_GPL(sync07_write);

int sync07_read(void)
{
	int sum;

	rtnl_lock();
	sum = g_a + g_b;		/* R(data) */
	rtnl_unlock();
	return sum;
}
EXPORT_SYMBOL_GPL(sync07_read);
