// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in a __setup handler
// P1: R(data) in a later initcall
// True negative: __setup runs during command-line parsing, before any initcall.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-23: __setup init stub ordering");

static int g_a;
static int g_b;

static int __init sync23_setup(char *str)
{
	g_a = 1;			/* W(data), during command-line parsing */
	g_b = 2;			/* W(data) */
	return 1;
}
__setup("sync23=", sync23_setup);

static int __init sync23_init(void)
{
	pr_info("sync23: %d\n", g_a + g_b);	/* R(data), at initcall time */
	return 0;
}
module_init(sync23_init);
