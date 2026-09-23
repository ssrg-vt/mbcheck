// 08_pointer_to_pointer.c
//
// Pointer-to-pointer: pp -> {p_var}, p -> {x}
// Expected: pp -> {p}, p -> {x}
//
// Kernel adaptation: all variables are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("08 pointer to pointer – kernel litmus test");

static int x = 5;
static int  *p;
static int **pp;

static int __init mod_init(void)
{
	p  = &x;
	pp = &p;
	pr_info("[08] pp=%px p=%px\n", (void *)pp, (void *)p);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[08] exit **pp=%d\n", **pp);
}

module_init(mod_init);
module_exit(mod_exit);
