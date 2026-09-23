// 12_return_ptr_static.c
//
// Function returns a pointer to a function-static variable.
// Expected: p -> {static_count}
//
// Kernel adaptation: getCounter and p are module-level; static local persists
// across calls just as in the original test.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("12 return ptr static – kernel litmus test");

static int *getCounter(void)
{
	static int count = 0;

	count++;
	return &count;
}

static int *p;

static int __init mod_init(void)
{
	p = getCounter();
	pr_info("[12] p=%px *p=%d\n", (void *)p, *p);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[12] exit p=%px\n", (void *)p);
}

module_init(mod_init);
module_exit(mod_exit);
