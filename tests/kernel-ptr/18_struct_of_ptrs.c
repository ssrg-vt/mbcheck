// 18_struct_of_ptrs.c
//
// Struct whose two fields are pointers to different globals.
// Expected: pair.first -> {x}, pair.second -> {y}
//
// Kernel adaptation: x, y, and pair are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("18 struct of ptrs – kernel litmus test");

static int x = 10, y = 20;

struct PairPtrs {
	int *first;
	int *second;
};

static struct PairPtrs pair;

static int __init mod_init(void)
{
	pair.first  = &x;
	pair.second = &y;
	pr_info("[18] first=%px second=%px\n",
		(void *)pair.first, (void *)pair.second);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[18] exit first=%px second=%px\n",
		(void *)pair.first, (void *)pair.second);
}

module_init(mod_init);
module_exit(mod_exit);
