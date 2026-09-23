// 10_struct_field_ptr.c
//
// A struct contains a pointer field.
// Expected: s.ptr -> {x}
//
// Kernel adaptation: x and s are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("10 struct field ptr – kernel litmus test");

static int x = 100;

struct S {
	int *ptr;
	int  val;
};

static struct S s;

static int __init mod_init(void)
{
	s.ptr = &x;
	s.val = 1;
	pr_info("[10] s.ptr=%px s.val=%d\n", (void *)s.ptr, s.val);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[10] exit s.ptr=%px\n", (void *)s.ptr);
}

module_init(mod_init);
module_exit(mod_exit);
