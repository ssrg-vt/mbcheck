/* 13_function_arg_write.c
 * Pointer written through a function argument (output parameter).
 * The callee receives **out and sets *out = malloc(...).
 * Expected: p -> {heap_object}
 */
#include <stdlib.h>

void alloc_int(int **out)
{
    *out = (int *)malloc(sizeof(int));
}

int *p;

int main(void)
{
    alloc_int(&p);
    *p = 77;
    return *p;
}
