// 22_multi_level_heap.c
//
// Two-level heap: g_pp is a pointer to a pointer variable on the heap.
// Expected:
//   g_pp  -> {heap_ptr_block}
//   *g_pp -> {heap_int_block}
//
// Kernel adaptation: kmalloc replaces malloc; g_pp is module-level static for
// proper cleanup in exit.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("22 multi level heap – kernel litmus test");

static int **g_pp;

static int __init mod_init(void)
{
	int *inner;

	inner = (int *)kmalloc(sizeof(int), GFP_KERNEL);
	if (!inner)
		return -ENOMEM;

	g_pp = (int **)kmalloc(sizeof(int *), GFP_KERNEL);
	if (!g_pp) {
		kfree(inner);
		return -ENOMEM;
	}

	*g_pp  = inner;
	**g_pp = 42;
	pr_info("[22] g_pp=%px *g_pp=%px **g_pp=%d\n",
		(void *)g_pp, (void *)*g_pp, **g_pp);
	return 0;
}

static void __exit mod_exit(void)
{
	if (g_pp) {
		kfree(*g_pp);
		kfree(g_pp);
	}
}

module_init(mod_init);
module_exit(mod_exit);
