// 07_two_heaps.c
//
// Two separate kmalloc calls produce two distinct heap objects.
// Expected: p -> {heap1}, q -> {heap2}  (heap1 != heap2)
//
// Kernel adaptation: kmalloc replaces malloc; kfree in exit.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("07 two heaps – kernel litmus test");

static int *p;
static int *q;

static int __init mod_init(void)
{
	p = (int *)kmalloc(sizeof(int), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	q = (int *)kmalloc(sizeof(int), GFP_KERNEL);
	if (!q) {
		kfree(p);
		return -ENOMEM;
	}
	*p = 1;
	*q = 2;
	pr_info("[07] p=%px q=%px\n", (void *)p, (void *)q);
	return 0;
}

static void __exit mod_exit(void)
{
	kfree(p);
	kfree(q);
}

module_init(mod_init);
module_exit(mod_exit);
