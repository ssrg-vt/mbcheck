// 04_flow_overwrite.c
//
// Flow-sensitivity: p first assigned &x, then strongly updated to &y.
// With flow-sensitive analysis the second store dominates.
// Expected: p -> {y}  (x should have no pointers at exit)
//
// Kernel adaptation: all variables are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("04 flow overwrite – kernel litmus test");

static int x = 1;
static int y = 2;
static int *p;

static int __init mod_init(void)
{
	p = &x;	/* first assignment */
	p = &y;	/* strong update: overwrites previous */
	pr_info("[04] p=%px\n", (void *)p);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[04] exit p=%px\n", (void *)p);
}

module_init(mod_init);
module_exit(mod_exit);
