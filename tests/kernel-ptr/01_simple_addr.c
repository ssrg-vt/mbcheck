// 01_simple_addr.c
//
// Simplest case: one global pointer, one global object.
// Expected: p -> {x}
//
// Kernel adaptation: p and x are module-level statics; init assigns &x to p.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("01 simple addr – kernel litmus test");

volatile static int x = 10;
volatile static int *p;

static int __init mod_init(void)
{
	p = &x;
	pr_info("[01] p=%px\n", (void *)p);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[01] exit p=%px\n", (void *)p);
}

module_init(mod_init);
module_exit(mod_exit);
