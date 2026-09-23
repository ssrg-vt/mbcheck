// 11_ptr_to_struct.c
//
// Pointer to a struct; accesses go through the pointer.
// Expected: sp -> {obj}
//
// Kernel adaptation: obj and sp are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("11 ptr to struct – kernel litmus test");

struct Point { int x; int y; };

static struct Point obj;
static struct Point *sp;

static int __init mod_init(void)
{
	sp    = &obj;
	sp->x = 3;
	sp->y = 4;
	pr_info("[11] sp=%px x=%d y=%d\n", (void *)sp, sp->x, sp->y);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[11] exit sp=%px\n", (void *)sp);
}

module_init(mod_init);
module_exit(mod_exit);
