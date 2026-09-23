// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in probe
// P1: cancel_work_sync -> R(data) in cleanup
// True negative: cancel_work_sync waits out the running work item.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-12: bottom-half teardown");

struct payload {
	int a;
	int b;
};

static struct payload *g_obj;

static void sync12_work_fn(struct work_struct *w)
{
	if (g_obj)
		g_obj->a++;
}
static DECLARE_WORK(g_work, sync12_work_fn);

static int sync12_probe(struct platform_device *pdev)
{
	g_obj = devm_kzalloc(&pdev->dev, sizeof(*g_obj), GFP_KERNEL);
	if (!g_obj)
		return -ENOMEM;

	g_obj->a = 1;			/* W(data) */
	g_obj->b = 2;			/* W(data) */
	schedule_work(&g_work);
	return 0;
}

static void sync12_remove(struct platform_device *pdev)
{
	cancel_work_sync(&g_work);	/* drains the bottom half */
	if (g_obj)
		dev_info(&pdev->dev, "sync12: %d\n", g_obj->a + g_obj->b); /* R(data) */
	g_obj = NULL;
}

static struct platform_driver sync12_driver = {
	.driver = { .name = "sync12" },
	.probe  = sync12_probe,
	.remove = sync12_remove,
};
module_platform_driver(sync12_driver);
