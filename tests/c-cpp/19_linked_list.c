/* 19_linked_list.c
 * Singly-linked list built on the heap.
 * Each node->next pointer should point to a distinct heap object.
 * Expected:
 *   head -> {heap_node1}
 *   node1.next -> {heap_node2}
 *   node2.next -> {null / empty}
 */
#include <stdlib.h>

struct Node {
    int          val;
    struct Node *next;
};

int main(void)
{
    struct Node *head   = (struct Node *)malloc(sizeof(struct Node));
    struct Node *second = (struct Node *)malloc(sizeof(struct Node));

    head->val   = 1;
    head->next  = second;

    second->val  = 2;
    second->next = (struct Node *)0;

    return head->next->val;
}
