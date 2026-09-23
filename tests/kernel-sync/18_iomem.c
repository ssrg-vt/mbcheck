// SPDX-License-Identifier: GPL-2.0
// P0: writel(data), writel(flag)
// P1: readl(flag), readl(data)
// True negative: writel and readl carry the barriers the architecture needs.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-18: MMIO accessor ordering");

#define SYNC18_DATA	0x00
#define SYNC18_FLAG	0x04

static void __iomem *g_base;

void sync18_publish(u32 v)
{
	writel(v, g_base + SYNC18_DATA);	/* W(data) */
	writel(1, g_base + SYNC18_FLAG);	/* W(flag), ordered by writel */
}
EXPORT_SYMBOL_GPL(sync18_publish);

u32 sync18_consume(void)
{
	if (!readl(g_base + SYNC18_FLAG))	/* R(flag) */
		return 0;
	return readl(g_base + SYNC18_DATA);	/* R(data) */
}
EXPORT_SYMBOL_GPL(sync18_consume);

void sync18_attach(void __iomem *base)
{
	g_base = base;
}
EXPORT_SYMBOL_GPL(sync18_attach);
