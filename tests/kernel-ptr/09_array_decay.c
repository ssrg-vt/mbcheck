// 09_array_decay.c
//
// Array name decays to a pointer to its first element.
// SVF creates a single abstract object for the whole array.
// Expected: p -> {arr_object}
//
// Kernel adaptation: arr and p are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("09 array decay – kernel litmus test");

static int arr[8];
static int *p;

static int __init mod_init(void)
{
	p = arr;	/* decay: pointer to arr[0] */
	*p    = 7;
	p[3]  = 42;
	pr_info("[09] p=%px arr[3]=%d\n", (void *)p, p[3]);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[09] exit p=%px\n", (void *)p);
}

module_init(mod_init);
module_exit(mod_exit);
