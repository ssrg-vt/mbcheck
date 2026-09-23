// 23_swap_ptrs.c
//
// Classic pointer swap via double pointer.
// After swap: g_p -> {y}, g_q -> {x}
//
// Kernel adaptation: g_p and g_q are module-level statics so the exit
// handler can observe them and prevent dead-store elimination.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("23 swap ptrs – kernel litmus test");

static int x = 1, y = 2;

static void swap_ptrs(int **a, int **b)
{
	int *tmp = *a;

	*a = *b;
	*b = tmp;
}

static int *g_p;
static int *g_q;

static int __init mod_init(void)
{
	g_p = &x;
	g_q = &y;
	swap_ptrs(&g_p, &g_q);
	/* after swap: g_p -> y, g_q -> x */
	pr_info("[23] g_p=%px g_q=%px\n", (void *)g_p, (void *)g_q);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[23] exit g_p=%px g_q=%px\n", (void *)g_p, (void *)g_q);
}

module_init(mod_init);
module_exit(mod_exit);
