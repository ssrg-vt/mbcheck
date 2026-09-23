/* 22_multi_level_heap.c
 * Two-level heap: pp is a pointer to a pointer variable on the heap.
 * Expected:
 *   pp  -> {heap_ptr_block}
 *   *pp -> {heap_int_block}
 */
#include <stdlib.h>

int **pp;

int main(void)
{
    int *inner = (int *)malloc(sizeof(int));
    pp = (int **)malloc(sizeof(int *));
    *pp  = inner;
    **pp = 42;
    return **pp;
}
