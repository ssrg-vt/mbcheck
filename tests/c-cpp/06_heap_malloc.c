/* 06_heap_malloc.c
 * Single heap allocation via malloc.
 * Expected: p -> {heap_object}
 */
#include <stdlib.h>

int *p;

int main(void)
{
    p = (int *)malloc(sizeof(int));
    *p = 99;
    free(p);
    return 0;
}
