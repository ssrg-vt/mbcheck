// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in probe
// P1: R(data) in a sysfs show callback
// True negative: the attribute is created after the state is initialised.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-16: sysfs show after registration");

struct payload {
	int a;
	int b;
};

static struct payload *g_obj;

static ssize_t sum_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	if (!g_obj)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", g_obj->a + g_obj->b);	/* R(data) */
}
static DEVICE_ATTR_RO(sum);

static int sync16_probe(struct platform_device *pdev)
{
	g_obj = devm_kzalloc(&pdev->dev, sizeof(*g_obj), GFP_KERNEL);
	if (!g_obj)
		return -ENOMEM;

	g_obj->a = 1;			/* W(data) */
	g_obj->b = 2;			/* W(data) */

	/* The attribute becomes reachable only after the state is ready. */
	return device_create_file(&pdev->dev, &dev_attr_sum);
}

static void sync16_remove(struct platform_device *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_sum);
	g_obj = NULL;
}

static struct platform_driver sync16_driver = {
	.driver = { .name = "sync16" },
	.probe  = sync16_probe,
	.remove = sync16_remove,
};
module_platform_driver(sync16_driver);
