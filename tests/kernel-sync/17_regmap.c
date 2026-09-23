// SPDX-License-Identifier: GPL-2.0
// P0: regmap_write(map, REG, data)
// P1: regmap_read(map, REG, &data)
// True negative: regmap locks internally around every access.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/regmap.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-17: regmap-internal locking");

#define SYNC17_REG	0x10

static struct regmap *g_map;

int sync17_write(unsigned int v)
{
	return regmap_write(g_map, SYNC17_REG, v);	/* W(data) */
}
EXPORT_SYMBOL_GPL(sync17_write);

int sync17_read(unsigned int *out)
{
	return regmap_read(g_map, SYNC17_REG, out);	/* R(data) */
}
EXPORT_SYMBOL_GPL(sync17_read);

void sync17_attach(struct regmap *map)
{
	g_map = map;
}
EXPORT_SYMBOL_GPL(sync17_attach);
