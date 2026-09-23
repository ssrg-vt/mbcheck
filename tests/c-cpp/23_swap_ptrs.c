/* 23_swap_ptrs.c
 * Classic pointer swap via double pointer.
 * After swap: p -> {y}, q -> {x}
 */
int x = 1, y = 2;

void swap(int **a, int **b)
{
    int *tmp = *a;
    *a = *b;
    *b = tmp;
}

int main(void)
{
    int *p = &x;
    int *q = &y;
    swap(&p, &q);
    return *p + *q;
}
