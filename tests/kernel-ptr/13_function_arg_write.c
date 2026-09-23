// 13_function_arg_write.c
//
// Pointer written through a function argument (output parameter).
// The callee receives **out and sets *out = kmalloc(...).
// Expected: p -> {heap_object}
//
// Kernel adaptation: kmalloc replaces malloc; kfree in exit.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("13 function arg write – kernel litmus test");

static void alloc_int(int **out)
{
	*out = (int *)kmalloc(sizeof(int), GFP_KERNEL);
}

static int *p;

static int __init mod_init(void)
{
	alloc_int(&p);
	if (!p)
		return -ENOMEM;
	*p = 77;
	pr_info("[13] p=%px *p=%d\n", (void *)p, *p);
	return 0;
}

static void __exit mod_exit(void)
{
	kfree(p);
}

module_init(mod_init);
module_exit(mod_exit);
