// 14_global_multi_fn.c
//
// Global pointer updated by two different functions.
// After calling both, g may point to either x or y.
// Expected: g -> {x, y}
//
// Kernel adaptation: all variables are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("14 global multi fn – kernel litmus test");

static int x = 1;
static int y = 2;
static int *g;

static void setX(void) { g = &x; }
static void setY(void) { g = &y; }

static int __init mod_init(void)
{
	setX();
	setY();
	pr_info("[14] g=%px\n", (void *)g);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[14] exit g=%px\n", (void *)g);
}

module_init(mod_init);
module_exit(mod_exit);
