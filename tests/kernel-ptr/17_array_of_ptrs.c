// 17_array_of_ptrs.c
//
// Array whose elements are pointers to distinct global objects.
// Expected: ptrs[0] -> {a}, ptrs[1] -> {b}, ptrs[2] -> {c}
//
// Kernel adaptation: a, b, c, and ptrs are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("17 array of ptrs – kernel litmus test");

static int a = 1, b = 2, c = 3;
static int *ptrs[3];

static int __init mod_init(void)
{
	ptrs[0] = &a;
	ptrs[1] = &b;
	ptrs[2] = &c;
	pr_info("[17] ptrs={%px,%px,%px}\n",
		(void *)ptrs[0], (void *)ptrs[1], (void *)ptrs[2]);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[17] exit ptrs[0]=%px\n", (void *)ptrs[0]);
}

module_init(mod_init);
module_exit(mod_exit);
