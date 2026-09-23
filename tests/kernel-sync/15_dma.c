// SPDX-License-Identifier: GPL-2.0
// P0: dma_sync_for_device -> W(data)
// P1: dma_sync_for_cpu -> R(data)
// True negative: the DMA sync calls carry the required cache maintenance and barriers.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-15: DMA ownership handoff");

struct payload {
	int a;
	int b;
};

static struct device *g_dev;
static struct payload *g_buf;
static dma_addr_t g_dma;

void sync15_to_device(int v)
{
	dma_sync_single_for_device(g_dev, g_dma, sizeof(*g_buf), DMA_TO_DEVICE);
	g_buf->a = v;			/* W(data) */
	g_buf->b = v + 1;		/* W(data) */
}
EXPORT_SYMBOL_GPL(sync15_to_device);

int sync15_from_device(void)
{
	dma_sync_single_for_cpu(g_dev, g_dma, sizeof(*g_buf), DMA_FROM_DEVICE);
	return g_buf->a + g_buf->b;	/* R(data) */
}
EXPORT_SYMBOL_GPL(sync15_from_device);

int sync15_attach(struct device *dev, struct payload *buf, dma_addr_t dma)
{
	g_dev = dev;
	g_buf = buf;
	g_dma = dma;
	return 0;
}
EXPORT_SYMBOL_GPL(sync15_attach);
