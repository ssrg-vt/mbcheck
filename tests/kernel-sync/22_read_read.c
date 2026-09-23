// SPDX-License-Identifier: GPL-2.0
// P0: R(data) -> smp_store_release(flag)
// P1: smp_load_acquire(flag) -> R(data)
// True negative: nothing writes the payload; two reads cannot be misordered.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-22: reader-reader pair");

static const int g_data = 42;
static int g_flag;

int sync22_side_a(void)
{
	int v = g_data;			/* R(data) */

	smp_store_release(&g_flag, 1);	/* W(flag) */
	return v;
}
EXPORT_SYMBOL_GPL(sync22_side_a);

int sync22_side_b(void)
{
	if (!smp_load_acquire(&g_flag))	/* R(flag) */
		return 0;
	return g_data;			/* R(data) */
}
EXPORT_SYMBOL_GPL(sync22_side_b);
