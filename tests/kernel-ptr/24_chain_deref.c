// 24_chain_deref.c
//
// Three-level chain: l1 -> x, l2 -> l1, l3 -> l2.
// Dereferencing l3 three times reaches x.
// Expected:
//   l1 -> {x}
//   l2 -> {l1}
//   l3 -> {l2}
//
// Kernel adaptation: all variables are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("24 chain deref – kernel litmus test");

static int    x  = 42;
static int   *l1;
static int  **l2;
static int ***l3;

static int __init mod_init(void)
{
	l1 = &x;
	l2 = &l1;
	l3 = &l2;
	pr_info("[24] ***l3=%d\n", ***l3);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[24] exit l1=%px l2=%px l3=%px\n",
		(void *)l1, (void *)l2, (void *)l3);
}

module_init(mod_init);
module_exit(mod_exit);
