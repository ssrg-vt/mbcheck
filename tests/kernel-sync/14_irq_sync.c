// SPDX-License-Identifier: GPL-2.0
// P0: disable_irq -> W(data)
// P1: R(data) in the IRQ handler
// True negative: disable_irq waits for any in-flight handler to finish.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-14: IRQ-disable serialisation");

static int g_irq;
static int g_a;
static int g_b;

static irqreturn_t sync14_handler(int irq, void *dev)
{
	int sum = g_a + g_b;		/* R(data) */

	return sum ? IRQ_HANDLED : IRQ_NONE;
}

void sync14_update(int v)
{
	disable_irq(g_irq);		/* waits out any running handler */
	g_a = v;			/* W(data) */
	g_b = v + 1;			/* W(data) */
	enable_irq(g_irq);
}
EXPORT_SYMBOL_GPL(sync14_update);

int sync14_attach(int irq)
{
	g_irq = irq;
	return request_irq(irq, sync14_handler, 0, "sync14", NULL);
}
EXPORT_SYMBOL_GPL(sync14_attach);
