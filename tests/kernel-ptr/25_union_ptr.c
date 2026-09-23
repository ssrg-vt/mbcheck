// 25_union_ptr.c
//
// Union with an int field and a pointer field.
// When the pointer field is used it should point to x.
// Expected: u.pval -> {x}
//
// Kernel adaptation: x and u are module-level statics.
// Note: type-punning through a union is well-defined in C (GNU C used by
// the kernel explicitly allows it).

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("25 union ptr – kernel litmus test");

union U {
	int  ival;
	int *pval;
};

static int x = 5;
static union U u;

static int __init mod_init(void)
{
	u.pval = &x;
	pr_info("[25] u.pval=%px u.ival=%d\n", (void *)u.pval, u.ival);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[25] exit u.pval=%px\n", (void *)u.pval);
}

module_init(mod_init);
module_exit(mod_exit);
