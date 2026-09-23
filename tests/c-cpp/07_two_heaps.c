/* 07_two_heaps.c
 * Two separate malloc calls should produce two distinct heap objects.
 * Expected: p -> {heap1}, q -> {heap2}  (heap1 != heap2)
 */
#include <stdlib.h>

int *p;
int *q;

int main(void)
{
    p = (int *)malloc(sizeof(int));
    q = (int *)malloc(sizeof(int));
    *p = 1;
    *q = 2;
    return *p + *q;
}
