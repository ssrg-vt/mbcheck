// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in module init
// P1: R(data) in module exit
// True negative: exit cannot run until init has returned.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-03: init/exit temporal serialisation");

struct payload {
	int a;
	int b;
};

static struct payload *g_obj;

static int __init sync03_init(void)
{
	g_obj = kmalloc(sizeof(*g_obj), GFP_KERNEL);
	if (!g_obj)
		return -ENOMEM;

	g_obj->a = 1;			/* W(data) */
	g_obj->b = 2;			/* W(data) */
	return 0;
}

static void __exit sync03_exit(void)
{
	if (g_obj)
		pr_info("sync03: %d\n", g_obj->a + g_obj->b);	/* R(data) */
	kfree(g_obj);
}

module_init(sync03_init);
module_exit(sync03_exit);
