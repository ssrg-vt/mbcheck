// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in probe
// P1: R(data) in remove
// True negative: the driver core serialises probe and remove for one device.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-11: probe/remove lifecycle");

struct payload {
	int a;
	int b;
};

static struct payload *g_obj;

static int sync11_probe(struct platform_device *pdev)
{
	g_obj = devm_kzalloc(&pdev->dev, sizeof(*g_obj), GFP_KERNEL);
	if (!g_obj)
		return -ENOMEM;

	g_obj->a = 1;			/* W(data) */
	g_obj->b = 2;			/* W(data) */
	return 0;
}

static void sync11_remove(struct platform_device *pdev)
{
	if (g_obj)
		dev_info(&pdev->dev, "sync11: %d\n", g_obj->a + g_obj->b); /* R(data) */
	g_obj = NULL;
}

static struct platform_driver sync11_driver = {
	.driver = { .name = "sync11" },
	.probe  = sync11_probe,
	.remove = sync11_remove,
};
module_platform_driver(sync11_driver);
