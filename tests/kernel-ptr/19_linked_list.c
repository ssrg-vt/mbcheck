// 19_linked_list.c
//
// Singly-linked list built on the heap (kmalloc).
// Expected:
//   g_head        -> {heap_node1}
//   node1.next    -> {heap_node2}
//   node2.next    -> {NULL / empty}
//
// Kernel adaptation: g_head is a module-level static so exit can free the list.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("19 linked list – kernel litmus test");

struct Node {
	int          val;
	struct Node *next;
};

static struct Node *g_head;

static int __init mod_init(void)
{
	struct Node *second;

	g_head = (struct Node *)kmalloc(sizeof(struct Node), GFP_KERNEL);
	if (!g_head)
		return -ENOMEM;

	second = (struct Node *)kmalloc(sizeof(struct Node), GFP_KERNEL);
	if (!second) {
		kfree(g_head);
		g_head = NULL;
		return -ENOMEM;
	}

	g_head->val  = 1;
	g_head->next = second;

	second->val  = 2;
	second->next = NULL;

	pr_info("[19] head=%px head->next=%px\n",
		(void *)g_head, (void *)g_head->next);
	return 0;
}

static void __exit mod_exit(void)
{
	if (g_head) {
		kfree(g_head->next);
		kfree(g_head);
	}
}

module_init(mod_init);
module_exit(mod_exit);
