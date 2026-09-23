// 03_copy_propagation.c
//
// Copy propagation: q inherits p's points-to set.
// Expected: p -> {x}, q -> {x}
//
// Kernel adaptation: all variables are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("03 copy propagation – kernel litmus test");

static int x = 10;
static int *p;
static int *q;

static int __init mod_init(void)
{
	p = &x;
	q = p;	/* copy: q now points wherever p points */
	pr_info("[03] p=%px q=%px\n", (void *)p, (void *)q);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[03] exit p=%px q=%px\n", (void *)p, (void *)q);
}

module_init(mod_init);
module_exit(mod_exit);
