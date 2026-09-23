// 15_function_ptr.c
//
// Function pointer: fp can point to foo or bar.
// Expected in SVF: fp -> {foo_obj, bar_obj}
//
// Kernel adaptation: fp is a module-level static; both targets are static.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("15 function ptr – kernel litmus test");

static int foo(void) { return 1; }
static int bar(void) { return 2; }

typedef int (*FP)(void);

static FP fp;

static int __init mod_init(void)
{
	fp = foo;
	fp = bar;
	pr_info("[15] fp=%px result=%d\n", (void *)fp, fp());
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[15] exit fp=%px\n", (void *)fp);
}

module_init(mod_init);
module_exit(mod_exit);
