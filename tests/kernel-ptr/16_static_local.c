// 16_static_local.c
//
// Two calls to a function both return the address of the same static local.
// Expected: p and q should both point to the same single static object.
//
// Kernel adaptation: p and q are module-level statics so the exit handler
// can observe them and prevent dead-store elimination.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("16 static local – kernel litmus test");

static int *getStatic(void)
{
	static int s = 0;

	s++;
	return &s;
}

static int *p;
static int *q;

static int __init mod_init(void)
{
	p = getStatic();
	q = getStatic();
	/* p == q at run time; pts(p) should equal pts(q) */
	pr_info("[16] p=%px q=%px val=%d\n", (void *)p, (void *)q, *p + *q);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[16] exit p=%px q=%px\n", (void *)p, (void *)q);
}

module_init(mod_init);
module_exit(mod_exit);
