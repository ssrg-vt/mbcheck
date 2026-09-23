// SPDX-License-Identifier: GPL-2.0
// P0: W(data) -> rcu_assign_pointer(flag) -> synchronize_rcu
// P1: rcu_read_lock -> R(flag) -> R(data)
// True negative: release store publishes the payload; the reader follows an address dependency.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-01: RCU grace period publication");

struct payload {
	int a;
	int b;
};

static struct payload __rcu *g_flag;

int sync01_publish(void)
{
	struct payload *p;

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->a = 1;			/* W(data) */
	p->b = 2;			/* W(data) */

	rcu_assign_pointer(g_flag, p);	/* release store: publishes the payload */
	synchronize_rcu();
	return 0;
}
EXPORT_SYMBOL_GPL(sync01_publish);

int sync01_consume(void)
{
	struct payload *p;
	int sum = 0;

	rcu_read_lock();
	p = rcu_dereference(g_flag);	/* R(flag) */
	if (p)
		sum = p->a + p->b;	/* R(data), ordered by the dependency */
	rcu_read_unlock();
	return sum;
}
EXPORT_SYMBOL_GPL(sync01_consume);
