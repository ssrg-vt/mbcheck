// 05_conditional_branch.c
//
// Pointer assigned on two branches of an if/else.
// At the join point p may point to either x or y.
// Expected: p -> {x, y}
//
// Kernel adaptation: cond is a module parameter (runtime value unknown to
// the static analyser), ensuring both branches are reachable in the CFG.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("05 conditional branch – kernel litmus test");

static int x = 1;
static int y = 2;
static int *p;

static int cond = 1;
module_param(cond, int, 0644);
MODULE_PARM_DESC(cond, "branch selector (0 or 1)");

static int __init mod_init(void)
{
	if (cond)
		p = &x;
	else
		p = &y;
	pr_info("[05] p=%px\n", (void *)p);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[05] exit p=%px\n", (void *)p);
}

module_init(mod_init);
module_exit(mod_exit);
