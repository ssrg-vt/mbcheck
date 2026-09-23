// 21_loop_addr.c
//
// Pointer table filled in a loop; each slot points to a different global.
// With field-sensitive analysis each ptrs[i] should be kept separate.
// Expected: ptrs[i] -> {arr[i]}  for i in 0..N-1
//
// Kernel adaptation: arr and ptrs are module-level statics.

#include <linux/init.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("21 loop addr – kernel litmus test");

#define N 5

static int arr[N];
static int *ptrs[N];

static int __init mod_init(void)
{
	int i;

	for (i = 0; i < N; i++)
		ptrs[i] = &arr[i];

	pr_info("[21] ptrs[0]=%px ptrs[4]=%px\n",
		(void *)ptrs[0], (void *)ptrs[N - 1]);
	return 0;
}

static void __exit mod_exit(void)
{
	pr_info("[21] exit ptrs[0]=%px\n", (void *)ptrs[0]);
}

module_init(mod_init);
module_exit(mod_exit);
