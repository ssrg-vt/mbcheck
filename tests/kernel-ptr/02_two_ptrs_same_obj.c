// 02_two_ptrs_same_obj.c
//
// Two distinct pointers both targeting the same global object.
// Expected: p -> {x}, q -> {x}
//
// Kernel adaptation: p, q, and x are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("02 two ptrs same obj – kernel litmus test");

static int x = 42;
static int *p;
static int *q;

static int __init mod_init(void)
{
	p = &x;
	q = &x;
	pr_info("[02] p=%px q=%px\n", (void *)p, (void *)q);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[02] exit p=%px q=%px\n", (void *)p, (void *)q);
}

module_init(mod_init);
module_exit(mod_exit);
