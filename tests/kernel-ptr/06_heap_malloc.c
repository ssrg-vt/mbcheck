// 06_heap_malloc.c
//
// Single heap allocation via kmalloc.
// Expected: p -> {heap_object}
//
// Kernel adaptation: kmalloc replaces malloc; kfree in exit.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("06 heap kmalloc – kernel litmus test");

static int *p;

static int __init mod_init(void)
{
	p = (int *)kmalloc(sizeof(int), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	*p = 99;
	pr_info("[06] p=%px\n", (void *)p);
	return 0;
}

static void __exit mod_exit(void)
{
	kfree(p);
}

module_init(mod_init);
module_exit(mod_exit);
