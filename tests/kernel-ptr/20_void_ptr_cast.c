// 20_void_ptr_cast.c
//
// void* carries the same abstract object through a bitcast.
// Expected: vp -> {x}, ip -> {x}  (both point to the same object)
//
// Kernel adaptation: x, vp, and ip are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("20 void ptr cast – kernel litmus test");

static int x = 10;
static void *vp;
static int  *ip;

static int __init mod_init(void)
{
	vp = &x;
	ip = (int *)vp;
	pr_info("[20] vp=%px ip=%px *ip=%d\n", vp, (void *)ip, *ip);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[20] exit vp=%px ip=%px\n", vp, (void *)ip);
}

module_init(mod_init);
module_exit(mod_exit);
