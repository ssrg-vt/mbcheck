// SPDX-License-Identifier: GPL-2.0
// P0: W(data) -> rcu_assign_pointer(flag) -> synchronize_srcu
// P1: srcu_read_lock -> R(flag) -> R(data) -> srcu_read_unlock
// True negative: as RCU, but the read side may sleep and the grace period is per-domain.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/srcu.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-10: SRCU publication");

struct payload {
	int a;
	int b;
};

DEFINE_STATIC_SRCU(g_srcu);
static struct payload __rcu *g_flag;

int sync10_publish(void)
{
	struct payload *p;

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->a = 1;			/* W(data) */
	p->b = 2;			/* W(data) */

	rcu_assign_pointer(g_flag, p);	/* release store */
	synchronize_srcu(&g_srcu);
	return 0;
}
EXPORT_SYMBOL_GPL(sync10_publish);

int sync10_consume(void)
{
	struct payload *p;
	int idx, sum = 0;

	idx = srcu_read_lock(&g_srcu);
	p = srcu_dereference(g_flag, &g_srcu);	/* R(flag) */
	if (p)
		sum = p->a + p->b;		/* R(data) */
	srcu_read_unlock(&g_srcu, idx);
	return sum;
}
EXPORT_SYMBOL_GPL(sync10_consume);
