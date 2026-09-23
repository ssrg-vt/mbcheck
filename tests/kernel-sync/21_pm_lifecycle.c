// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in probe
// P1: R(data) in the suspend callback
// True negative: the PM core does not suspend a device whose probe has not completed.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-21: PM suspend after probe");

struct payload {
	int a;
	int b;
};

static struct payload *g_obj;

static int sync21_probe(struct platform_device *pdev)
{
	g_obj = devm_kzalloc(&pdev->dev, sizeof(*g_obj), GFP_KERNEL);
	if (!g_obj)
		return -ENOMEM;

	g_obj->a = 1;			/* W(data) */
	g_obj->b = 2;			/* W(data) */
	return 0;
}

static int sync21_suspend(struct device *dev)
{
	if (g_obj)
		dev_info(dev, "sync21: %d\n", g_obj->a + g_obj->b);	/* R(data) */
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sync21_pm_ops, sync21_suspend, NULL);

static struct platform_driver sync21_driver = {
	.driver = {
		.name = "sync21",
		.pm   = pm_sleep_ptr(&sync21_pm_ops),
	},
	.probe = sync21_probe,
};
module_platform_driver(sync21_driver);
