// SPDX-License-Identifier: GPL-2.0
// P0: W(data) in probe
// P1: R(data) in the kref release callback
// True negative: release runs only after the last reference is dropped.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-13: kref release");

struct payload {
	struct kref ref;
	int a;
	int b;
};

static struct payload *g_obj;

static void sync13_release(struct kref *ref)
{
	struct payload *p = container_of(ref, struct payload, ref);

	pr_info("sync13: %d\n", p->a + p->b);	/* R(data) */
	kfree(p);
}

static int sync13_probe(struct platform_device *pdev)
{
	g_obj = kzalloc(sizeof(*g_obj), GFP_KERNEL);
	if (!g_obj)
		return -ENOMEM;

	kref_init(&g_obj->ref);
	g_obj->a = 1;			/* W(data) */
	g_obj->b = 2;			/* W(data) */
	return 0;
}

static void sync13_remove(struct platform_device *pdev)
{
	if (g_obj)
		kref_put(&g_obj->ref, sync13_release);
	g_obj = NULL;
}

static struct platform_driver sync13_driver = {
	.driver = { .name = "sync13" },
	.probe  = sync13_probe,
	.remove = sync13_remove,
};
module_platform_driver(sync13_driver);
