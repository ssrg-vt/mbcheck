// SPDX-License-Identifier: GPL-2.0
// P0: mutex_lock -> W(data) -> mutex_unlock
// P1: mutex_lock -> R(data) -> mutex_unlock
// True negative: one mutex, so the sections are mutually exclusive.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-05: mutex mutual exclusion");

static DEFINE_MUTEX(g_mutex);
static int g_a;
static int g_b;

void sync05_write(int v)
{
	mutex_lock(&g_mutex);
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
	mutex_unlock(&g_mutex);
}
EXPORT_SYMBOL_GPL(sync05_write);

int sync05_read(void)
{
	int sum;

	mutex_lock(&g_mutex);
	sum = g_a + g_b;		/* R(data) */
	mutex_unlock(&g_mutex);
	return sum;
}
EXPORT_SYMBOL_GPL(sync05_read);
